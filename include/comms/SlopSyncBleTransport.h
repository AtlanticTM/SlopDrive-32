#pragma once

// SlopSyncBleTransport / SlopSyncBlePort — the NimBLE GATT binding for the
// SlopSync hub
//
// Constraints:
//   SPEC §13.4: real transport adapters live in firmware, NEVER in
//   lib/slopsync. RFC-043: BLE GATT is the hardware-hub CONFORMANCE FLOOR
//   (infrastructure-free control + discovery + future WiFi provisioning);
//   WebSocket stays the preferred high-throughput path. Clients SHOULD
//   auto-upgrade BLE->WS once connected (WELCOME's ws_port/ipv4, §6.3).
//
//   Compiles to nothing unless -DBLE_ENABLED is set (the same self-exclusion
//   idiom SlopSyncAsyncWsTransport.h documents for -DSLOPSYNC_WS_ASYNC) — an
//   env without the flag never needs NimBLE-Arduino on its include path.
//
//   Threading model (read this before touching anything; mirrors
//   SlopSyncAsyncWsTransport.h's own header note almost exactly):
//   NimBLE-Arduino runs its host stack on its OWN FreeRTOS task ("NimBLEHost"
//   / the Bluedroid/NimBLE port's controller-host task), so every
//   NimBLEServerCallbacks/NimBLECharacteristicCallbacks method below fires on
//   THAT task, concurrently with the SlopSyncHub task running hub.update().
//   TRAPS T5 applies verbatim: these callbacks NEVER touch slopsync::Hub or
//   session state directly — they only ever (a) push into a lock-free SPSC
//   ring, or (b) set an atomic "intent" flag that SlopSyncBlePort::loop()
//   (called from the hub task, exactly like SlopSyncAsyncWsPort::loop())
//   resolves into an actual hub.attachTransport()/detachTransport() call.
//   The hub is single-task BY DESIGN; this file keeps that true for BLE
//   exactly as SlopSyncAsyncWsTransport.h keeps it true for WS.
//
//   RX — the identical lock-free SPSC ring pattern as the WS transport: ONE
//   producer (the NimBLE host task, from onWrite) advances the tail with a
//   RELEASE store; ONE consumer (the hub task, from read()) advances the
//   head with an ACQUIRE load. See SlopSyncAsyncWsTransport.h's own comment
//   for the full memory-ordering argument — it applies unchanged here.
//
//   TX — NimBLECharacteristic::notify() is the library's own non-blocking
//   send (it enqueues onto the host's outgoing event queue and returns
//   success/failure immediately; it does not wait for the radio). A failed
//   notify() IS this binding's congestion signal (§13.1 "notify queue
//   depth") — there is no separate depth counter to read, so a failure is
//   treated as "the queue is full right now," which is the only fact §13.1
//   asks a binding to report.
//
//   Why 2 concurrent connections (not more, not fewer): a hardware hub's BLE
//   role here is "phone finds and provisions the machine, then upgrades to
//   WS" (§13.1) — it is not the high-throughput motion plane (that is WS).
//   Two lets one phone hold a BLE session mid BLE->WS migration while a
//   second BLE-only client (a different phone, a diagnostic tool) is still
//   served, without carrying the RAM/GATT-slot cost of matching
//   kHubMaxSessions+1. Raising this later is a one-line constant change plus
//   NimBLE's own CONFIG_BT_NIMBLE_MAX_CONNECTIONS ceiling (default 3,
//   comfortably above this).
//
// See:
//   SlopSyncAsyncWsTransport.h — the WS-side twin this file mirrors throughout
//   TRAPS.md T5 — why transport callbacks never touch hub state directly

#if defined(BLE_ENABLED)

#include <NimBLEDevice.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/wire/frame_buffer.hpp"

namespace slopdrive {

// ---- registry.yaml `ble_identity` (RFC-046 item 1) --------------------------
// NOT emitted by tools/gen_registry_header.py (the codegen has no CBOR-key
// schema for a GATT UUID string) — transcribed here verbatim per Phase E's
// documented fallback (see SlopSyncDiscoveryWire.h's own note on the same
// pattern for `udp_discovery`/`ble_adv_flags`). The first three groups spell
// "SLOP"/"SY"/"NC" in ASCII, deliberately, per the registry's own comment.
inline constexpr const char* kBleServiceUuid = "534C4F50-5359-4E43-8000-000000000001";
inline constexpr const char* kBleWriteCharUuid = "534C4F50-5359-4E43-8000-000000000002";   // c2h
inline constexpr const char* kBleNotifyCharUuid = "534C4F50-5359-4E43-8000-000000000003";  // h2c

// 0xFFFF: the Bluetooth SIG's own "for testing" company identifier —
// SlopSync has no assigned company id, and this is the smallest legitimate
// AD structure that can carry one custom byte (2-byte company id + payload).
// It rides the SCAN RESPONSE, not the primary advertisement: the legacy
// 31-byte advertising budget is already spent by the service UUID + shortened
// name (27B), with no room for a 5B Manufacturer-Specific-Data record
// alongside them (TRAPS T14; §13.4's own byte-budget note).
inline constexpr uint16_t kBleMfgCompanyId = 0xFFFF;

// Sentinel: no connection bound to this slot. Matches NimBLE's own
// BLE_HS_CONN_HANDLE_NONE value; defined locally so this header does not need
// a host-stack-internal include for one constant.
inline constexpr uint16_t kBleNoConn = 0xFFFF;

class SlopSyncBleTransport final : public slopsync::ITransport {
public:
    // Modest on purpose: BLE's whole role here is control-plane + discovery
    // (§13.1's "phone-direct control" path), not the dense motion stream WS
    // carries. A control/catalog burst is a handful of frames, not the
    // 32-deep bursts the WS transport's own kRxRingDepth was sized against.
    static constexpr uint8_t kRxRingDepth = 16;

    // How long a CONTROL frame may stay un-sendable (every notify() fails)
    // before the connection is dropped — identical reasoning and identical
    // value to SlopSyncAsyncWsTransport::kCtrlStallMs: a full queue is
    // ordinary backpressure the hub retries through; a queue that NEVER
    // drains means the peer is gone in every way that matters.
    static constexpr uint32_t kCtrlStallMs = 2000;

    SlopSyncBleTransport() = default;

    // Wires the shared h2c characteristic and the server (needed for
    // getPeerMTU()/disconnect()) — every slot points at the SAME
    // characteristic; NimBLE's per-connection CCCD state is what makes one
    // characteristic instance serve multiple centrals safely.
    void bind(NimBLECharacteristic* notifyChar, NimBLEServer* server) {
        _notifyChar = notifyChar;
        _server = server;
    }

    // ---- ITransport (hub task) ----------------------------------------------
    bool open() override;
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<slopsync::FrameBuffer> read() override;
    slopsync::TransportProperties properties() const override;

    // ---- Driven by the port, on the NimBLE host task ------------------------
    void attachConn(uint16_t connHandle);
    void detachConn();
    void pushRx(const uint8_t* data, size_t len);

    // ---- Diagnostics (either task; all relaxed loads) -----------------------
    uint16_t connHandle() const { return _connHandle.load(std::memory_order_relaxed); }
    uint32_t rxDrops() const { return _rxDrops.load(std::memory_order_relaxed); }
    uint32_t txDataDrops() const { return _txDataDrops.load(std::memory_order_relaxed); }
    uint32_t txCtrlFails() const { return _txCtrlFails.load(std::memory_order_relaxed); }

private:
    // Same registry-classification rule as SlopSyncAsyncWsTransport::isDroppable
    // (byte 0 of the header is the frame type; only STATE/STREAM are data).
    static bool isDroppable(std::span<const std::byte> frame);

    NimBLECharacteristic* _notifyChar = nullptr;  // shared h2c characteristic
    NimBLEServer* _server = nullptr;              // for getPeerMTU()/disconnect()

    std::atomic<uint16_t> _connHandle{kBleNoConn};
    std::atomic<bool> _open{false};

    // SPSC ring: producer = NimBLE host task (tail), consumer = hub task (head).
    slopsync::FrameBuffer _rx[kRxRingDepth]{};
    std::atomic<uint8_t> _rxHead{0};
    std::atomic<uint8_t> _rxTail{0};

    // millis() of the first control frame that failed to notify(), 0 = none
    // outstanding. Written on the hub task only (inside write()).
    uint32_t _ctrlStallSinceMs = 0;

    std::atomic<uint32_t> _rxDrops{0};
    std::atomic<uint32_t> _txDataDrops{0};
    std::atomic<uint32_t> _txCtrlFails{0};
};

// The GATT server + its (small, fixed) set of connection slots. Owns the
// NimBLEServer/Service/Characteristics and the advertising payload, and
// bridges NimBLE connect/disconnect/write callbacks to
// hub.attach/detachTransport — the BLE-specific twin of SlopSyncAsyncWsPort.
class SlopSyncBlePort final : public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
public:
    static constexpr uint8_t kSlots = 2;  // see the file header's "why 2" note

    void begin(slopsync::Hub* hub, const char* fullName, const char* shortName);

    // Called from the hub task's own 5 ms tick (mirrors SlopSyncAsyncWsPort::
    // loop()): drains deferred attach/detach. NimBLE needs no per-tick
    // service pump otherwise — its host task runs itself.
    void loop();

    // Diffs against the currently-advertised flags and only touches the
    // radio (rebuild + restart advertising) on an actual change — "cheap,
    // event-driven, no polling loop" per the task brief. Safe to call every
    // tick from the hub's existing 1 Hz slow block (like
    // pumpPresencePairingWindow already does for SlopGlow); the diff check
    // itself costs two bool compares.
    void updateAdvertising(bool pairingWindowOpen, bool wsAvailable);

private:
    // ---- NimBLEServerCallbacks (NimBLE host task) ---------------------------
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override;
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override;

    // ---- NimBLECharacteristicCallbacks (NimBLE host task) -------------------
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override;

    void refreshAdvertisingData(uint8_t flags);
    int slotForConn(uint16_t connHandle) const;  // -1 = none

    slopsync::Hub* _hub = nullptr;
    NimBLEServer* _server = nullptr;
    NimBLECharacteristic* _writeChar = nullptr;
    NimBLECharacteristic* _notifyChar = nullptr;
    const char* _fullName = "";
    const char* _shortName = "";

    SlopSyncBleTransport _slots[kSlots];
    // Written on the NimBLE host task, read on the hub task -> atomic. Same
    // deferred attach/detach discipline as SlopSyncAsyncWsPort (field bug #5:
    // calling hub.attachTransport()/detachTransport() off the hub task races
    // Hub::update()'s slot walk).
    std::atomic<bool> _attached[kSlots]{};
    std::atomic<bool> _wantAttach[kSlots]{};
    std::atomic<bool> _wantDetach[kSlots]{};

    // Last flags byte actually pushed onto the radio — the diff state
    // updateAdvertising() compares against. 0xFF (an impossible value: only
    // bits 0-1 are ever legal) forces the first call to always publish.
    uint8_t _lastAdvFlags = 0xFF;
};

}  // namespace slopdrive

#endif  // BLE_ENABLED
