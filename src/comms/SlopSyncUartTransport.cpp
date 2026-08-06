// SlopSyncUartTransport — the Serial2 ITransport binding for the SlopSync hub.
// Constraints:
//   Build-guarded behind UART_LINK_ENABLED; compiles to nothing otherwise.
//   Serial2 ONLY. Serial1 is the Modbus servo bus.
//   The RX ring resets in resetRing() (new peer session) and close() — never
//   in open(), which would discard a HELLO that already arrived.
//   write() checks availableForWrite() first: HardwareSerial::write() blocks
//   once the TX ring is full, and this runs on the hub task.
//   COBS comes from slopsync/wire/serial_cobs.hpp. Do not reimplement it.
// See: include/comms/SlopSyncUartTransport.h for the wire format and the
//      threading/backpressure argument; transport.md T5.

#if defined(UART_LINK_ENABLED)

#include "SlopSyncUartTransport.h"

#include <cstring>

#include "sloplog/sloplog.h"
#include "slopsync/wire/serial_cobs.hpp"

namespace slopdrive {

// ---- SlopSyncUartTransport --------------------------------------------------

bool SlopSyncUartTransport::open() {
    // DO NOT RESET THE RING HERE (field bug #5, and the identical note in
    // SlopSyncBleTransport::open()). The port pushes the peer's first frame
    // and only then asks for the attach that leads here, so a reset at this
    // point throws away the HELLO that caused the attach in the first place.
    _open.store(true, std::memory_order_release);
    return true;
}

void SlopSyncUartTransport::close() {
    _open.store(false, std::memory_order_release);
    _rxHead.store(0, std::memory_order_relaxed);
    _rxTail.store(0, std::memory_order_relaxed);
}

void SlopSyncUartTransport::resetRing() {
    _rxHead.store(0, std::memory_order_relaxed);
    _rxTail.store(0, std::memory_order_relaxed);
}

bool SlopSyncUartTransport::write(std::span<const std::byte> frame) {
    if (!_open.load(std::memory_order_acquire)) return false;
    if (frame.empty()) return true;
    if (frame.size() > slopsync::kFrameBufferCapacity) {
        // Above the mtu this binding advertises — the hub should never build
        // one, so count it rather than truncating it onto the wire.
        if (_stats) _stats->txDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "UART slot %u TX frame oversized (%u B)", unsigned(_slotId),
                       unsigned(frame.size()));
        return false;
    }

    _txSrc[0] = _slotId;
    std::memcpy(_txSrc + 1, frame.data(), frame.size());

    const size_t n = slopsync::cobsEncode(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(_txSrc), frame.size() + 1),
        std::span<std::byte>(reinterpret_cast<std::byte*>(_txEnc), sizeof(_txEnc)));
    if (n == 0) {  // dst too small — structurally impossible at kUartLinkEncodedMax
        if (_stats) _stats->txDrops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    _txEnc[n] = 0x00;  // the delimiter is the caller's job (serial_cobs.hpp)
    const size_t total = n + 1;

    // THE NON-BLOCKING CHECK. Refusing here is ordinary flow control; the
    // caller's class semantics decide retry vs drop (§13.1). Blocking instead
    // would stall the whole hub tick behind one backed-up UART.
    const int room = Serial2.availableForWrite();
    if (room < 0 || size_t(room) < total) {
        if (_stats) _stats->txDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGD_EVERY_MS(5000, "slopsync", "UART TX full — slot %u frame refused", unsigned(_slotId));
        return false;
    }

    Serial2.write(_txEnc, total);
    if (_stats) _stats->framesTx.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::optional<slopsync::FrameBuffer> SlopSyncUartTransport::read() {
    const uint8_t head = _rxHead.load(std::memory_order_relaxed);
    const uint8_t tail = _rxTail.load(std::memory_order_acquire);
    if (head == tail) return std::nullopt;  // at most one frame per call (§13.1)

    slopsync::FrameBuffer fb = _rx[head];
    _rxHead.store(uint8_t((head + 1) % kRxRingDepth), std::memory_order_release);
    return fb;
}

slopsync::TransportProperties SlopSyncUartTransport::properties() const {
    slopsync::TransportProperties p;
    // A whole frame fits: COBS framing imposes no MTU of its own, so the only
    // ceiling is the FrameBuffer the hub already hands across this boundary.
    p.mtu = uint16_t(slopsync::kFrameBufferCapacity);
    p.ordered = true;   // one byte stream, one direction: strictly ordered
    p.reliable = true;  // a wired point-to-point UART; loss is line noise, not policy
    p.congestion = slopsync::CongestionSignal::QueueWatermark;  // TX ring occupancy
    return p;
}

void SlopSyncUartTransport::pushRx(const uint8_t* data, size_t len) {
    if (len == 0 || len > slopsync::kFrameBufferCapacity) {
        if (_stats) _stats->rxDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "UART RX frame dropped (len=%u)", unsigned(len));
        return;
    }

    const uint8_t tail = _rxTail.load(std::memory_order_relaxed);
    const uint8_t next = uint8_t((tail + 1) % kRxRingDepth);
    if (next == _rxHead.load(std::memory_order_acquire)) {
        // Full: the NEWEST frame is the one that loses, so the ring keeps the
        // oldest un-consumed frames intact rather than shredding a sequence.
        if (_stats) _stats->rxDrops.fetch_add(1, std::memory_order_relaxed);
        SLOGW_EVERY_MS(2000, "slopsync", "UART RX ring full (slot %u) — frame dropped",
                       unsigned(_slotId));
        return;
    }

    _rx[tail] = slopsync::FrameBuffer::from(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len));
    // Fill the slot, THEN publish the index — see the ordering argument in
    // SlopSyncAsyncWsTransport.h.
    _rxTail.store(next, std::memory_order_release);
}

// ---- Bridge control channel -------------------------------------------------

void SlopSyncUartPort::sendBridge(const uint8_t* payload, size_t len) {
    uint8_t src[16];
    uint8_t enc[24];
    if (len + 1 > sizeof(src)) return;
    src[0] = bridge::kSlot;
    std::memcpy(src + 1, payload, len);

    const size_t n = slopsync::cobsEncode(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(src), len + 1),
        std::span<std::byte>(reinterpret_cast<std::byte*>(enc), sizeof(enc)));
    if (n == 0) return;
    enc[n] = 0x00;
    // Same refuse-never-block discipline as the per-slot path: a status frame
    // is a diagnostic, and stalling the hub tick to deliver one is worse than
    // losing it. The C5 re-reads state from the next status anyway.
    const int room = Serial2.availableForWrite();
    if (room < 0 || size_t(room) < n + 1) return;
    Serial2.write(enc, n + 1);
}

void SlopSyncUartPort::handleBridgeOp(const uint8_t* body, size_t n) {
    if (n < 2) return;
    const uint8_t op = body[1];

    if (op == bridge::kOpSlotClosed) {
        if (n < 3) return;
        const uint8_t s = body[2];
        if (s < kSlots && _active[s]) {
            SLOGI("slopsync", "UART slot %u closed by bridge -- detach deferred", unsigned(s));
            _wantDetach[s].store(true, std::memory_order_release);
        }
        return;
    }

    if (!_otaSink) return;
    uint8_t state = bridge::kOtaIdle;

    switch (op) {
    case bridge::kOpOtaBegin: {
        if (n < 7) return;
        const uint32_t size = uint32_t(body[3]) | (uint32_t(body[4]) << 8) |
                              (uint32_t(body[5]) << 16) | (uint32_t(body[6]) << 24);
        state = _otaSink->otaBegin(body[2], size);
        break;
    }
    case bridge::kOpOtaData: {
        if (n < 4) return;
        const uint16_t seq = uint16_t(body[2]) | uint16_t(uint16_t(body[3]) << 8);
        // A gap must ack IMMEDIATELY: that status is the only thing that tells
        // the sender to retransmit. Otherwise ack on a window, since per-chunk
        // would halve throughput.
        const bool gap = (seq != _otaSink->otaNextSeq());
        state = _otaSink->otaData(seq, body + 4, n - 4);
        if (state != bridge::kOtaFailed && !gap &&
            (seq % bridge::kOtaAckEvery) != bridge::kOtaAckEvery - 1) return;
        break;
    }
    case bridge::kOpOtaEnd: {
        if (n < 6) return;
        const uint32_t crc = uint32_t(body[2]) | (uint32_t(body[3]) << 8) |
                             (uint32_t(body[4]) << 16) | (uint32_t(body[5]) << 24);
        state = _otaSink->otaEnd(crc);
        break;
    }
    case bridge::kOpOtaAbort:
        if (n < 3) return;
        _otaSink->otaAbort(body[2]);
        state = bridge::kOtaFailed;
        break;
    default:
        return;   // unknown op: ops are append-only, so an older peer is normal
    }

    const uint16_t want = _otaSink->otaNextSeq();
    const uint8_t msg[5] = {bridge::kOpOtaStatus, state, _otaSink->otaAbortReason(),
                            uint8_t(want & 0xFF), uint8_t(want >> 8)};
    sendBridge(msg, sizeof(msg));
}

// ---- SlopSyncUartPort -------------------------------------------------------

void SlopSyncUartPort::begin(slopsync::Hub* hub) {
    _hub = hub;
    for (uint8_t i = 0; i < kSlots; ++i) _slots[i].bind(i, &_stats);

    // Buffers BEFORE begin(): HardwareSerial refuses to resize a running UART
    // (it logs an error and returns 0), which would silently leave the default
    // 256-byte RX buffer in place — ~1 ms of runway at 2 Mbaud.
    Serial2.setRxBufferSize(kUartLinkRxBufferBytes);
    Serial2.setTxBufferSize(kUartLinkTxBufferBytes);
    // XTAL, not APB: better baud accuracy, immune to frequency scaling.
    Serial2.setClockSource(UART_CLK_SRC_XTAL);
    Serial2.begin(kUartLinkBaud, SERIAL_8N1, kUartLinkRxPin, kUartLinkTxPin);
    // Never park the hub task in readBytes(); the Arduino default is 1000 ms.
    Serial2.setTimeout(0);

    SLOGI("slopsync", "UART link up (Serial2 %lu baud, TX=%d RX=%d, %u slots)",
          (unsigned long)kUartLinkBaud, int(kUartLinkTxPin), int(kUartLinkRxPin), unsigned(kSlots));
}

void SlopSyncUartPort::pumpRx() {
    // TASK CONTEXT, NOT AN ISR -- every buffer here is in PSRAM (see header).
    // BULK read, not byte-at-a-time: HardwareSerial::read() takes the UART
    // mutex per call, ~1,000 per 5 ms tick at 2 Mbaud, and the drain could not
    // keep up (measured: 64 B frames 100%, 242/504 B only 68-74%).
    // BOUNDED BY BUDGET, not by the wire being slower than the drain. One full
    // RX buffer per tick is unreachable in normal traffic; the cap exists so a
    // saturating peer cannot hold the hub task in this loop indefinitely.
    size_t budget = kUartLinkRxBufferBytes;
    uint8_t chunk[512];
    while (budget != 0) {
        const int avail = Serial2.available();
        if (avail <= 0) break;
        size_t want = size_t(avail) > sizeof(chunk) ? sizeof(chunk) : size_t(avail);
        if (want > budget) want = budget;
        const size_t got = Serial2.readBytes(chunk, want);
        if (got == 0) break;
        budget -= got;
        // RAW byte count, before any framing. This is the ONLY way to tell
        // "nothing is on the wire" (bad/absent wiring, wrong pins) apart from
        // "bytes arrive but do not decode" (baud, framing, or slot mismatch).
        // Without it both look identical: framesRx == 0.
        _stats.rawBytesRx.fetch_add(uint32_t(got), std::memory_order_relaxed);

        for (size_t ci = 0; ci < got; ++ci) {
        const uint8_t b = chunk[ci];

        if (b != 0x00) {
            if (_rxOverflow) continue;  // discarding until the next delimiter
            if (_rxLen < sizeof(_rxAcc)) {
                _rxAcc[_rxLen++] = b;
            } else {
                _rxOverflow = true;
                _rxLen = 0;
                _stats.rxDrops.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        // Delimiter. Everything since the last one is exactly one COBS body.
        const size_t bodyLen = _rxLen;
        const bool wasOverflow = _rxOverflow;
        _rxLen = 0;
        _rxOverflow = false;
        if (wasOverflow || bodyLen == 0) continue;  // resync / idle-line run of zeros

        uint8_t out[kUartLinkBodyMax];
        const auto r = slopsync::cobsDecode(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(_rxAcc), bodyLen),
            std::span<std::byte>(reinterpret_cast<std::byte*>(out), sizeof(out)));
        if (!r.isOk()) {
            _stats.decodeErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const size_t n = r.value();
        if (n < 2) {  // a slot byte with no frame behind it
            _stats.decodeErrors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const uint8_t slot = out[0];

        // Bridge control, not a session frame: [0xFF][op][args]. The bridge is
        // the only thing that knows a WebSocket actually closed, so this is how
        // it says so — see kSlotIdleMs for why inferring it from silence was
        // wrong.
        if (slot == bridge::kSlot) {
            handleBridgeOp(out, n);
            continue;
        }

        if (slot >= kSlots) {
            _stats.rxDrops.fetch_add(1, std::memory_order_relaxed);
            SLOGW_EVERY_MS(2000, "slopsync", "UART RX for unknown slot %u", unsigned(slot));
            continue;
        }

        if (!_active[slot]) {
            // A new peer session on this slot. The ring resets HERE — the one
            // point where there is provably nothing in flight to lose — and
            // never in open(), which runs later, after the frame below is
            // already queued.
            _active[slot] = true;
            _slots[slot].resetRing();
            _wantAttach[slot].store(true, std::memory_order_release);
            SLOGI("slopsync", "UART slot %u active (attach deferred to hub task)", unsigned(slot));
        }
        _lastRxMs[slot] = millis();
        _slots[slot].pushRx(out + 1, n - 1);
        _stats.framesRx.fetch_add(1, std::memory_order_relaxed);
        }   // for (ci) — one byte of the bulk-read chunk
    }       // while (available) — refill the chunk
}

void SlopSyncUartPort::loop() {
    pumpRx();

#if defined(UART_LINK_BRINGUP_HEARTBEAT)
    // BRING-UP DIAGNOSTIC ONLY — remove once the link is proven.
    // Emits a COBS-framed 4-byte payload on slot 0 once a second so the S3->C5
    // direction can be tested WITHOUT a session existing. Needed because
    // rawBytesRx==0 alone cannot distinguish "wrong pins", "TX/RX swapped" and
    // "trace not connected": each direction has to be observed on its own.
    {
        static uint32_t lastBeat = 0;
        const uint32_t nowBeat = millis();
        if (uint32_t(nowBeat - lastBeat) >= 1000) {
            lastBeat = nowBeat;
            const uint8_t payload[5] = {0x00, 0xDE, 0xAD, 0xBE, 0xEF};  // [slot][marker]
            uint8_t enc[16];
            const size_t n = slopsync::cobsEncode(
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(payload), sizeof(payload)),
                std::span<std::byte>(reinterpret_cast<std::byte*>(enc), sizeof(enc)));
            if (n > 0 && size_t(Serial2.availableForWrite()) >= n + 1) {
                Serial2.write(enc, n);
                Serial2.write(uint8_t(0x00));
                _stats.framesTx.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
#endif

    // BACKSTOP sweep only — the bridge's kOpSlotClosed is the real detach
    // signal. This fires at kSlotIdleMs and exists solely for a bridge that
    // died without getting the close out. Reaching it at all means the close
    // notification was lost, which is worth saying at WARN.
    const uint32_t now = millis();
    for (uint8_t i = 0; i < kSlots; ++i) {
        if (_active[i] && uint32_t(now - _lastRxMs[i]) > kSlotIdleMs) {
            SLOGW("slopsync",
                  "UART slot %u silent %ums — BACKSTOP detach (no bridge close received)",
                  unsigned(i), unsigned(now - _lastRxMs[i]));
            _wantDetach[i].store(true, std::memory_order_release);
        }
    }

    // DETACH FIRST — identical ordering reason to SlopSyncBlePort::loop(): a
    // peer that appeared and vanished between two ticks has both flags set,
    // and "nothing was ever attached, so there is nothing to tear down" is the
    // only honest resolution.
    for (uint8_t i = 0; i < kSlots; ++i) {
        if (_wantDetach[i].exchange(false, std::memory_order_acq_rel)) {
            _wantAttach[i].store(false, std::memory_order_release);
            if (_attached[i].exchange(false, std::memory_order_acq_rel)) {
                if (_hub) _hub->detachTransport(_slots[i]);
            }
            _slots[i].close();  // idempotent; clears a ring the hub never drained
            _active[i] = false;
            continue;
        }
        if (_wantAttach[i].exchange(false, std::memory_order_acq_rel)) {
            if (_hub && _hub->attachTransport(_slots[i])) {
                _attached[i].store(true, std::memory_order_release);
                SLOGI("slopsync", "UART slot %u attached", unsigned(i));
            } else {
                // Hub full. Drop the slot back to idle so the peer's next
                // frame retries rather than wedging the slot forever; there is
                // no link-layer connection to refuse, so this is all we can do.
                SLOGW("slopsync", "hub refused UART slot %u", unsigned(i));
                _slots[i].close();
                _active[i] = false;
            }
        }
    }
}

}  // namespace slopdrive

#endif  // UART_LINK_ENABLED
