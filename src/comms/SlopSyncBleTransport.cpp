#if defined(BLE_ENABLED)

#include "SlopSyncBleTransport.h"

#include "SlopSyncDiscoveryWire.h"  // buildFlags() — the byte shared with the UDP responder
#include "sloplog/sloplog.h"

namespace slopdrive {

// ---------------------------------------------------------------------------
// SlopSyncBleTransport
// ---------------------------------------------------------------------------

bool SlopSyncBleTransport::isDroppable(std::span<const std::byte> frame) {
    if (frame.size() < 1) return false;
    const auto t = static_cast<slopsync::FrameType>(frame[0]);
    return t == slopsync::FrameType::STATE || t == slopsync::FrameType::STREAM;
}

bool SlopSyncBleTransport::open() {
    // See SlopSyncAsyncWsTransport::open()'s identical note: do NOT reset the
    // ring here. attachConn() (NimBLE host task, on the connect event) is
    // what resets it, at the one point where there is provably no concurrent
    // producer yet. A reset here would race a HELLO that already arrived
    // between attachConn() and the hub task reaching open().
    _open.store(true, std::memory_order_release);
    return true;
}

void SlopSyncBleTransport::close() {
    _open.store(false, std::memory_order_release);
    _rxHead.store(0, std::memory_order_relaxed);
    _rxTail.store(0, std::memory_order_relaxed);
}

bool SlopSyncBleTransport::write(std::span<const std::byte> frame) {
    if (!_open.load(std::memory_order_acquire) || _notifyChar == nullptr) return false;
    const uint16_t conn = _connHandle.load(std::memory_order_acquire);
    if (conn == kBleNoConn) return false;
    if (frame.empty()) return true;

    const bool droppable = isDroppable(frame);

    // notify()'s own failure IS the "notify queue depth" congestion signal
    // (§13.1) — there is no separate depth counter NimBLE exposes, and a
    // failed enqueue already means "full right now," which is the fact this
    // binding is asked to report. Shed data frames on that signal exactly
    // like the WS transport's Classify policy; never shed control.
    const bool ok = _notifyChar->notify(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), conn);
    if (ok) {
        if (!droppable) _ctrlStallSinceMs = 0;  // control is flowing again
        return true;
    }

    if (droppable) {
        _txDataDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGD_EVERY_MS(5000, "slopsync", "BLE conn#%u shedding data frames (notify congested)",
                       unsigned(conn));
        return false;
    }

    _txCtrlFails.fetch_add(1, std::memory_order_relaxed);

    // Same stall-timer discipline as SlopSyncAsyncWsTransport::write(): a
    // full queue is ordinary backpressure the hub retries through, so only a
    // control frame that stays unsendable for kCtrlStallMs — never reset by
    // inbound traffic — earns a teardown.
    const uint32_t now = millis();
    if (_ctrlStallSinceMs == 0) {
        _ctrlStallSinceMs = now;
        return false;
    }
    if (uint32_t(now - _ctrlStallSinceMs) < kCtrlStallMs) return false;

    SLOGW("slopsync", "BLE conn#%u control frame (type 0x%02X) unsendable for %ums — disconnecting",
          unsigned(conn), unsigned(frame[0]), unsigned(now - _ctrlStallSinceMs));
    _ctrlStallSinceMs = 0;
    if (_server != nullptr) _server->disconnect(conn);
    return false;
}

std::optional<slopsync::FrameBuffer> SlopSyncBleTransport::read() {
    const uint8_t head = _rxHead.load(std::memory_order_relaxed);
    const uint8_t tail = _rxTail.load(std::memory_order_acquire);
    if (head == tail) return std::nullopt;

    slopsync::FrameBuffer fb = _rx[head];
    _rxHead.store(uint8_t((head + 1) % kRxRingDepth), std::memory_order_release);
    return fb;
}

slopsync::TransportProperties SlopSyncBleTransport::properties() const {
    slopsync::TransportProperties p;
    // §13.4: "each <= ATT_MTU - 3". Ask the server for THIS connection's
    // negotiated MTU live rather than caching it — cheap, and avoids needing
    // a third cross-task field just to shadow a value NimBLE already tracks.
    // Pre-negotiation (or if the connection has already dropped), fall back
    // to the legacy default ATT_MTU (23) - 3 = 20, the honest worst case.
    uint16_t attMtu = 23;
    const uint16_t conn = _connHandle.load(std::memory_order_relaxed);
    if (_server != nullptr && conn != kBleNoConn) {
        const uint16_t negotiated = _server->getPeerMTU(conn);
        if (negotiated > 0) attMtu = negotiated;
    }
    p.mtu = uint16_t(attMtu > 3 ? attMtu - 3 : 0);
    p.ordered = true;    // one ATT bearer per connection: a strict byte stream
    p.reliable = false;  // h2c rides NOTIFY (unacked); the weaker of the pair governs (§13.1)
    p.congestion = slopsync::CongestionSignal::NotifyQueueDepth;
    return p;
}

void SlopSyncBleTransport::attachConn(uint16_t connHandle) {
    // NimBLE host task. Mirrors SlopSyncAsyncWsTransport::attachClient(): the
    // ring resets HERE, at the arrival of a NEW connection, the one point
    // where there is provably no concurrent producer yet.
    _rxHead.store(0, std::memory_order_relaxed);
    _rxTail.store(0, std::memory_order_relaxed);
    _ctrlStallSinceMs = 0;
    _rxDrops.store(0, std::memory_order_relaxed);
    _txDataDrops.store(0, std::memory_order_relaxed);
    _txCtrlFails.store(0, std::memory_order_relaxed);
    _connHandle.store(connHandle, std::memory_order_release);
}

void SlopSyncBleTransport::detachConn() {
    _connHandle.store(kBleNoConn, std::memory_order_release);
}

void SlopSyncBleTransport::pushRx(const uint8_t* data, size_t len) {
    // Producer side (NimBLE host task). One GATT write == one SlopSync frame
    // (no ATT-level fragmentation is attempted here — see the file header's
    // note on §5.6/§18-22; a client sending a frame that does not fit its own
    // negotiated MTU is a client bug this transport does not try to repair).
    if (len == 0 || len > slopsync::kFrameBufferCapacity) {
        _rxDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "BLE RX frame dropped (len=%u)", unsigned(len));
        return;
    }

    const uint8_t tail = _rxTail.load(std::memory_order_relaxed);
    const uint8_t next = uint8_t((tail + 1) % kRxRingDepth);
    if (next == _rxHead.load(std::memory_order_acquire)) {
        _rxDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "BLE RX ring full — frame dropped");
        return;
    }

    _rx[tail] = slopsync::FrameBuffer::from(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len));
    _rxTail.store(next, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// SlopSyncBlePort
// ---------------------------------------------------------------------------

void SlopSyncBlePort::begin(slopsync::Hub* hub, const char* fullName, const char* shortName) {
    _hub = hub;
    _fullName = fullName;
    _shortName = shortName;

    NimBLEDevice::init(fullName);
    // Request MTU >= 250 + data-length extension BEFORE catalog transfer
    // (§13.4). This is a preference the stack negotiates with each peer, not
    // a guarantee — properties()/mtu above always reports what actually got
    // negotiated for a given connection, never this request.
    NimBLEDevice::setMTU(250);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(this);

    NimBLEService* service = _server->createService(NimBLEUUID(kBleServiceUuid));
    _writeChar = service->createCharacteristic(NimBLEUUID(kBleWriteCharUuid),
                                                NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    _writeChar->setCallbacks(this);
    _notifyChar = service->createCharacteristic(NimBLEUUID(kBleNotifyCharUuid), NIMBLE_PROPERTY::NOTIFY);

    for (auto& slot : _slots) slot.bind(_notifyChar, _server);

    // NimBLEService::start() is a deliberate no-op in this NimBLE-Arduino
    // release (services start when the server/advertising does) — omitted
    // rather than kept as a deprecated call that does nothing.

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->enableScanResponse(true);
    refreshAdvertisingData(discovery::buildFlags(false, false));
    adv->start();

    SLOGI("slopsync", "BLE GATT port up (service %s, %u slots, name '%s')", kBleServiceUuid,
          unsigned(kSlots), fullName);
}

void SlopSyncBlePort::refreshAdvertisingData(uint8_t flags) {
    _lastAdvFlags = flags;

    // Legacy 31-byte advertising budget (BLE_HS_ADV_MAX_SZ, §13.4): Flags(3)
    // + 128-bit Complete Service UUID(18) + shortened name "SD32"(6) = 27B —
    // no room left in THIS payload for the flags byte's 5-byte
    // Manufacturer-Specific-Data record (TRAPS T14: a prior version put the
    // MSD here too, 32B total, and NimBLEAdvertisementData::addData() silently
    // rejected it while every earlier record still fit and made it to air).
    // The flags byte therefore rides the SCAN RESPONSE instead, alongside the
    // full name: complete name "SlopDrive-32"(14) + MSD(5) = 19B, comfortably
    // under budget. Every setter below returns bool; failures are logged at
    // WARN (never silently dropped, per TRAPS T14).
    NimBLEAdvertisementData advData;
    bool advOk = true;
    advOk &= advData.setFlags(0x06);  // LE General Discoverable + BR/EDR Not Supported (standard combo)
    advOk &= advData.setCompleteServices(NimBLEUUID(kBleServiceUuid));
    advOk &= advData.setShortName(_shortName);  // AD type 0x08 — NOT setName() (which defaults to complete/0x09)
    if (!advOk) {
        SLOGW("slopsync", "BLE advertisement payload build failed — an AD record did not fit the 31-byte legacy budget");
    }

    NimBLEAdvertisementData scanData;
    bool scanOk = true;
    scanOk &= scanData.setName(_fullName);  // complete name (0x09) rides the scan response (§13.4)
    // Manufacturer-specific data: company id (2B, LE) + our one flags byte —
    // see the header's kBleMfgCompanyId doc for why this AD structure and
    // not a 128-bit Service Data one. Rides the SCAN RESPONSE (TRAPS T14),
    // not the advertisement — see the budget note above.
    const char mfg[3] = {char(uint8_t(kBleMfgCompanyId)), char(uint8_t(kBleMfgCompanyId >> 8)), char(flags)};
    scanOk &= scanData.setManufacturerData(std::string(mfg, sizeof(mfg)));
    if (!scanOk) {
        SLOGW("slopsync", "BLE scan-response payload build failed — ble_adv_flags byte is NOT on the air this cycle");
    }

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!adv->setAdvertisementData(advData)) {
        SLOGW("slopsync", "ble_gap_adv_set_data rejected the advertisement payload");
    }
    if (!adv->setScanResponseData(scanData)) {
        SLOGW("slopsync", "ble_gap_adv_rsp_set_data rejected the scan-response payload");
    }
}

void SlopSyncBlePort::updateAdvertising(bool pairingWindowOpen, bool wsAvailable) {
    const uint8_t flags = discovery::buildFlags(pairingWindowOpen, wsAvailable);
    if (flags == _lastAdvFlags) return;  // event-driven: touch the radio only on an actual change

    refreshAdvertisingData(flags);
    // Force the new payload out now rather than waiting for NimBLE's next
    // natural rebuild — a stop+start on a state-change event (pairing window
    // opening/closing, WiFi going up/down) is cheap; this is not a polling
    // loop touching the radio every tick.
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::getAdvertising()->start();
    SLOGI("slopsync", "BLE advertising flags -> 0x%02X (pairing=%d ws=%d)", unsigned(flags),
          int(pairingWindowOpen), int(wsAvailable));
}

int SlopSyncBlePort::slotForConn(uint16_t connHandle) const {
    if (connHandle == kBleNoConn) return -1;
    for (int i = 0; i < int(kSlots); ++i) {
        if (_slots[i].connHandle() == connHandle) return i;
    }
    return -1;
}

void SlopSyncBlePort::onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
    // ***** NimBLE HOST TASK, NOT THE HUB TASK. ***** See the header's class
    // doc — only ever record intent here; loop() (hub task) does the actual
    // hub.attachTransport() call.
    const uint16_t handle = connInfo.getConnHandle();
    int slot = -1;
    for (int i = 0; i < int(kSlots); ++i) {
        if (_slots[i].connHandle() == kBleNoConn) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        // Hub is full (kSlots reached) — refuse cleanly rather than accepting
        // a connection we cannot serve, same posture as the WS port's
        // "no free slot" close(). The link-layer connection already exists by
        // the time onConnect fires (BLE has no earlier veto point); disconnect
        // immediately.
        SLOGW("slopsync", "no free BLE slot for conn#%u — disconnecting", unsigned(handle));
        server->disconnect(handle);
        return;
    }
    _slots[slot].attachConn(handle);
    _wantAttach[slot].store(true, std::memory_order_release);
    SLOGI("slopsync", "BLE conn#%u claimed slot %d (attach deferred to hub task)", unsigned(handle), slot);

    // Keep advertising while a slot remains, so a SECOND concurrent client
    // can still find and connect to this hub.
    bool anyFree = false;
    for (int i = 0; i < int(kSlots); ++i) {
        if (_slots[i].connHandle() == kBleNoConn) anyFree = true;
    }
    if (anyFree) NimBLEDevice::startAdvertising();
}

void SlopSyncBlePort::onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo, int reason) {
    const uint16_t handle = connInfo.getConnHandle();
    const int slot = slotForConn(handle);
    if (slot >= 0) {
        SLOGI("slopsync", "BLE conn#%u gone (slot %d, reason %d) — detach deferred", unsigned(handle),
              slot, reason);
        // Flag only — clientId/connHandle deliberately NOT cleared here yet
        // (mirrors SlopSyncAsyncWsPort::onEvent's WS_EVT_DISCONNECT note):
        // leaving the slot occupied until the hub task finishes teardown
        // stops a fresh connect from reusing a slot the hub still believes
        // is live.
        _wantDetach[slot].store(true, std::memory_order_release);
    }
    // A slot just freed (or is about to) — resume advertising so the next
    // client can connect. Harmless if already advertising.
    NimBLEDevice::startAdvertising();
}

void SlopSyncBlePort::onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) {
    if (characteristic != _writeChar) return;  // only one write characteristic exists, but be explicit
    const int slot = slotForConn(connInfo.getConnHandle());
    if (slot < 0) return;  // a write from a connection we have not (yet) attached — drop
    const auto& value = characteristic->getValue();
    _slots[slot].pushRx(value.data(), value.length());
}

void SlopSyncBlePort::loop() {
    // Hub task, exactly like SlopSyncAsyncWsPort::loop(). NimBLE's host task
    // needs no pumping from here; this only drains deferred attach/detach.
    //
    // DETACH FIRST — identical ordering reason to the WS port: a connection
    // that connected and vanished between two ticks has both flags set, and
    // "nothing was ever attached, so there is nothing to tear down" is the
    // only honest resolution.
    for (int i = 0; i < int(kSlots); ++i) {
        if (_wantDetach[i].exchange(false, std::memory_order_acq_rel)) {
            _wantAttach[i].store(false, std::memory_order_release);
            if (_attached[i].exchange(false, std::memory_order_acq_rel)) {
                if (_hub) _hub->detachTransport(_slots[i]);
            }
            _slots[i].detachConn();
            continue;
        }
        if (_wantAttach[i].exchange(false, std::memory_order_acq_rel)) {
            const uint16_t handle = _slots[i].connHandle();
            if (_hub && _hub->attachTransport(_slots[i])) {
                _attached[i].store(true, std::memory_order_release);
                SLOGI("slopsync", "BLE conn#%u attached to slot %d", unsigned(handle), i);
            } else {
                SLOGW("slopsync", "hub refused BLE conn#%u — disconnecting", unsigned(handle));
                _slots[i].detachConn();
                if (_server != nullptr) _server->disconnect(handle);
            }
        }
    }
}

}  // namespace slopdrive

#endif  // BLE_ENABLED
