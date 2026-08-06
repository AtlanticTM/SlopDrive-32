#pragma once

// SlopSyncUartTransport / SlopSyncUartPort — the point-to-point SERIAL binding
// for the SlopSync hub (SPEC §13.5), carrying the C5 comms bridge's relayed
// sessions (docs/c5-comms-offload.md Phase 2).
//
// Constraints:
//   SPEC §13.4: real transport adapters live in firmware, NEVER in
//   lib/slopsync. This file is the third such adapter, after
//   SlopSyncAsyncWsTransport (the high-throughput plane) and
//   SlopSyncBleTransport (whose SHAPE this one copies).
//
//   Compiles to nothing unless -DUART_LINK_ENABLED is set — the same
//   self-exclusion idiom SlopSyncBleTransport.h uses for -DBLE_ENABLED.
//
//   Serial2 ONLY, TX = GPIO 43, RX = GPIO 44. Serial1 is the Modbus servo bus
//   and must never be touched from here (docs/c5-comms-offload.md §8 item 5).
//
// ---- Wire format (SPEC §13.5, and it must match src/c5_probe/main.cpp) ------
//   COBS( [slot_id][slopsync frame] ) + 0x00
// One UART carries up to kSlots independent sessions, so a slot byte rides
// INSIDE the COBS envelope — keeping it outside would put a raw byte next to
// the delimiter discipline for no gain. The 0x00 delimiter is OUTSIDE the
// codec by design (serial_cobs.hpp's own header note): delimiter-finding is
// this transport's job, COBS (de)framing is the library's. Nothing here
// reimplements COBS.
//
// ---- Threading model (read before touching anything) ------------------------
// UNLIKE the WS and BLE ports, this one has NO second task: there is no
// AsyncTCP task and no NimBLE host task behind it. pumpRx() drains
// Serial2 from SlopSyncUartPort::loop(), which the hub task calls — so the
// RX ring's producer and consumer are the SAME task, and the deferred
// attach/detach flags resolve on the task that set them.
//
// The SPSC ring and the deferred-attach discipline are kept anyway, byte for
// byte identical to the BLE port:
//   - The memory ordering (producer publishes _rxTail with RELEASE, consumer
//     loads it with ACQUIRE and advances _rxHead with RELEASE) is the correct
//     discipline the moment a producer ever moves off this task, and it costs
//     nothing measurable on a 5 ms tick.
//   - Deferred attach/detach keeps hub.attachTransport()/detachTransport()
//     calls at ONE site per port, in ONE order (detach before attach), which
//     is what TRAPS T5 / field bug #5 is actually about.
// Three ports that behave differently under teardown is a worse outcome than
// three atomics that are currently uncontended.
//
// RX MUST STAY IN TASK CONTEXT — this is not a style preference. The whole
// SlopSyncHubService is placement-new'd into PSRAM (main.cpp, heap_caps_malloc
// with MALLOC_CAP_SPIRAM), so every buffer below lives behind the flash cache.
// An ISR that touches PSRAM during a cache-disabled window (flash write, OTA)
// faults. Do NOT install a UART ISR/event-task producer for this ring without
// first moving the ring into internal RAM.
//
// ---- Backpressure -----------------------------------------------------------
// write() returning false means "not accepted right now" (§13.1) — ORDINARY
// FLOW CONTROL, not an error. HardwareSerial::write() BLOCKS once the TX ring
// buffer fills, which would stall the hub task inside hub.update(), so free
// space is checked FIRST and the frame is refused rather than waiting. There
// is deliberately no per-class shedding and no control-stall teardown here
// (unlike the WS transport): TX backpressure on this link is a property of the
// ONE shared UART, not of any single session, so tearing a session down over
// it would punish the wrong client. A peer that has genuinely gone away stops
// sending, and the RX-idle sweep in loop() is what reaps it.
//
// See:
//   SlopSyncBleTransport.h — the port/transport shape this file mirrors
//   SlopSyncAsyncWsTransport.h — the full memory-ordering argument for the ring
//   transport.md T5 -- why transport callbacks never touch hub state

#if defined(UART_LINK_ENABLED)

#include <Arduino.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "comms/BridgeProtocol.h"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/wire/frame_buffer.hpp"

namespace slopdrive {

// 4 Mbaud. The old 4-Mbaud conviction was the dead RX ISR, not the link; see
// transport.md T33 and the RX-ring-sizing section for the retest numbers.
// Divides both crystals exactly (40/10, 48/12); 5 Mbaud does not divide the
// C5's. Both ends read their own constant -- change BOTH or the link is down.
inline constexpr uint32_t kUartLinkBaud = 4000000;
// PCB-verified and carrying live traffic both directions (2026-08-06).
// S3 TX 43 -> C5 RX IO12; C5 TX IO11 -> S3 RX 44.
inline constexpr int8_t kUartLinkTxPin = 43;
inline constexpr int8_t kUartLinkRxPin = 44;

// 2 Mbaud at 8N1 is 200 B/ms, so a 5 ms tick takes ~1,000 B. Both buffers
// MUST be sized before begin(): HardwareSerial will not resize a running UART.
inline constexpr size_t kUartLinkTxBufferBytes = 8192;
// RX outlasts the DRAIN INTERVAL, not the frame: 32 KB = 164 ms, ~32 ticks.
// Doubled from 16 KB and KEPT after the baud revert: an OTA burst (one credit
// window sent back to back) is the case that loses frames, and headroom there
// costs 16 KB of a heap with 137 KB free.
inline constexpr size_t kUartLinkRxBufferBytes = 32768;

// The COBS *body*: one slot byte + one whole frame (header + payload).
inline constexpr size_t kUartLinkBodyMax = 1 + slopsync::kFrameBufferCapacity;
// Worst-case COBS expansion is 1 byte per 254 of source (serial_cobs.hpp:30-33),
// plus the caller-appended 0x00 delimiter.
inline constexpr size_t kUartLinkEncodedMax =
    kUartLinkBodyMax + kUartLinkBodyMax / 254 + 1 + 1;

// One counter block shared by the port and every slot — the link is one wire,
// so per-slot TX numbers would only ever be re-summed by whoever reads them.
// Relaxed everywhere: these are diagnostics, never control flow.
struct SlopSyncUartLinkStats {
    std::atomic<uint32_t> framesTx{0};
    std::atomic<uint32_t> framesRx{0};
    // Refused because the TX ring had no room, or because the frame was
    // oversized. NOT an error — see the header's backpressure note.
    std::atomic<uint32_t> txDrops{0};
    // Inbound frames lost: RX ring full, oversized accumulation, or a slot id
    // outside kSlots.
    std::atomic<uint32_t> rxDrops{0};
    // COBS rejected the region between two delimiters (or it decoded to fewer
    // than the 2 bytes a slot byte + a frame needs). Line noise and resync
    // land here, NOT in rxDrops.
    std::atomic<uint32_t> decodeErrors{0};
    // Raw bytes off Serial2, counted before framing. Distinguishes "no wire"
    // from "wire but no valid frames" — both otherwise read as framesRx == 0.
    std::atomic<uint32_t> rawBytesRx{0};
};

class SlopSyncUartTransport final : public slopsync::ITransport {
public:
    // Depth is set by the BURST the producer can deliver in one pass, NOT by
    // the steady rate. `SlopSyncUartPort::poll()` drains the whole Serial2 RX
    // buffer before the hub consumes anything, and that buffer holds roughly
    // 350 frames at this frame size — so the elastic feeding this ring is far
    // deeper than the ring itself, and everything past the depth is shredded.
    //
    // The old value was 16, copied from SlopSyncBleTransport with the note "the
    // drain is a 5 ms tick and one client's inbound burst is a handful of
    // frames". That reasoning does not survive the serial producer: at ~63
    // frames/s inbound, a mere 250 ms hub-task hiccup (an NVS write after
    // homing, a flash operation) buffers exactly 16 frames and every frame
    // after that is lost. Measured live 2026-08-01 as repeated
    // "UART RX ring full (slot 1)" while a client was connected, and lost
    // frames are lost SlopSync sequence — the session breaks.
    //
    // 64 buys ~1 s of hub-stall tolerance at that rate. Cost is PSRAM, not
    // internal heap: 64 x sizeof(FrameBuffer) x kSlots.
    //
    // NOT a complete fix. The structural answer is BACK-PRESSURE: poll() should
    // stop pulling new chunks at a delimiter boundary while a slot's ring is
    // near full and leave the bytes in the Serial2 buffer, which is the right
    // place to hold them. Depth only widens the window.
    static constexpr uint8_t kRxRingDepth = 64;

    SlopSyncUartTransport() = default;

    // Called once from the port's begin(). The slot id is the byte that rides
    // inside the COBS envelope; the stats block is the port's, shared.
    void bind(uint8_t slotId, SlopSyncUartLinkStats* stats) {
        _slotId = slotId;
        _stats = stats;
    }

    // ---- ITransport (hub task) ----------------------------------------------
    bool open() override;
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<slopsync::FrameBuffer> read() override;
    slopsync::TransportProperties properties() const override;

    // ---- Driven by the port -------------------------------------------------
    // Empties the ring. Call ONLY at the arrival of a new peer session, where
    // there is provably no in-flight frame to lose — never from open().
    void resetRing();
    void pushRx(const uint8_t* data, size_t len);

    uint8_t slotId() const { return _slotId; }

private:
    uint8_t _slotId = 0;
    SlopSyncUartLinkStats* _stats = nullptr;
    std::atomic<bool> _open{false};

    // SPSC ring: producer advances the tail, consumer advances the head, and
    // neither ever writes the other's index.
    slopsync::FrameBuffer _rx[kRxRingDepth]{};
    std::atomic<uint8_t> _rxHead{0};
    std::atomic<uint8_t> _rxTail{0};

    // TX scratch as MEMBERS, not locals: write() is reached deep inside
    // hub.update() and ~1 KB of stack down there is not free (TRAPS T1). In
    // PSRAM with the rest of the service, where it is.
    uint8_t _txSrc[kUartLinkBodyMax]{};
    uint8_t _txEnc[kUartLinkEncodedMax]{};
};

// The Serial2 link and its fixed set of session slots. Owns the byte stream,
// the delimiter splitter and the decode scratch, and bridges "a slot went
// quiet" / "a slot spoke for the first time" to hub.attach/detachTransport —
// the serial twin of SlopSyncBlePort.
class SlopSyncUartPort {
public:
    // Matches src/c5_probe/main.cpp's own kSlots, which must not disagree.
    static constexpr uint8_t kSlots = uint8_t(slopsync::kHubMaxSessions) + 1;

    // A serial link has no connect/disconnect events: the bridge closes a
    // WebSocket without telling us, so RX SILENCE IS THE ONLY DETACH SIGNAL
    // that exists. Clients PING inside their granted deadman window (~2 s), so
    // Bridge control vocabulary lives in comms/BridgeProtocol.h, included by
    // BOTH ends. Never restate a value from it here (T20).
    static_assert(bridge::kSlots == kSlots,
                  "bridge::kSlots is stale: slopsync::kHubMaxSessions moved. "
                  "Fix BridgeProtocol.h and reflash the C5 -- the bridge drops "
                  "any slot >= its own kSlots, silently.");   // [op][slot]

    // BACKSTOP ONLY. The real detach signal is bridge::kOpSlotClosed, which the
    // bridge sends the instant a WebSocket actually closes.
    //
    // This was 10 s, and at 10 s it was a BUG: silence cannot tell
    // "disconnected" from "connected but idle", so a client that simply had
    // nothing to say — MFP between actions, a paused user — had its transport
    // torn out from under a live session. SPEC §11.3 / RFC-042 are explicit
    // that a quiet session is marked STALE and its slot, session_id and grants
    // are RETAINED so it can reattach; this sweep was destroying exactly what
    // the protocol says to keep. Observed live 2026-08-01 as repeated MFP
    // dropouts ~12 s after the client went quiet.
    //
    // Kept at a long value purely so a bridge that dies without sending the
    // close (power loss, panic) cannot strand a slot forever. Anything short
    // enough to be useful as a primary signal is short enough to reap live
    // sessions — that is the whole lesson.
    static constexpr uint32_t kSlotIdleMs = 120000;

    // Bridge OTA ops are handed OUT to the composition root rather than the
    // transport reaching into system services (architecture.md 1). Each returns
    // a bridge::OtaState; the port reports it back on the control channel.
    struct IOtaSink {
        virtual ~IOtaSink() = default;
        virtual uint8_t otaBegin(uint8_t target, uint32_t size) = 0;
        virtual uint8_t otaData(uint16_t seq, const uint8_t* d, size_t n) = 0;
        virtual uint8_t otaEnd(uint32_t crc) = 0;
        virtual void    otaAbort(uint8_t reason) = 0;
        virtual uint8_t otaAbortReason() const = 0;
        virtual uint16_t otaNextSeq() const = 0;
        // True only while a SERIAL transfer is in flight, never for HTTP.
        virtual bool otaInFlight() const = 0;
    };
    void setOtaSink(IOtaSink* sink) { _otaSink = sink; }

    // Diag archive pull source (kOpDiagReq), same injection pattern as
    // IOtaSink: comms/ never reaches into system/.
    struct IDiagSource {
        virtual ~IDiagSource() = default;
        // Fill buf with whole lines starting at cursor `from`, tag-filtered
        // (empty tag = all). Sets next (the following request's from) and done.
        virtual size_t diagRead(uint32_t from, const char* tag,
                                char* buf, size_t cap,
                                uint32_t& next, bool& done) = 0;
    };
    void setDiagSource(IDiagSource* src) { _diagSource = src; }

    // The hub task's ota_active guard skips this port; it MUST NOT while the
    // OTA is the one arriving here. Safe because a serial OTA's flash writes
    // run on the hub task too, so drain and write are serialized.
    bool otaInFlight() const { return _otaSink && _otaSink->otaInFlight(); }

    void begin(slopsync::Hub* hub);

    // Hub task, from the same 5 ms tick that drives the other ports: drain the
    // UART, then resolve deferred detach/attach.
    void loop();

    const SlopSyncUartLinkStats& stats() const { return _stats; }

private:
    // Delimiter-split + COBS decode + push into the addressed slot's ring.
    void pumpRx();

    // [kSlot][op][args] out to the bridge. Payloads here are single-digit
    // bytes, so unlike the per-slot frame path this needs no member scratch.
    void sendBridge(const uint8_t* payload, size_t len);
    // Dispatch one decoded [kSlot][op][args] body.
    void handleBridgeOp(const uint8_t* body, size_t n);

    IOtaSink* _otaSink = nullptr;
    IDiagSource* _diagSource = nullptr;
    // One diag batch, filled and sent whole inside one kOpDiagReq dispatch.
    // 2 KB fits the 8 KB TX buffer outright, so sendBridge never has to wait.
    // Costs PSRAM, not internal RAM: the port lives inside the PSRAM-resident
    // hub service.
    char _diagBuf[2048]{};
    // sendBridge encode scratch. Sized for the largest bridge frame (241 B
    // kOpDiagData) plus COBS expansion; PSRAM with the rest of this object.
    uint8_t _bridgeSrc[256]{};
    uint8_t _bridgeEnc[264]{};
    slopsync::Hub* _hub = nullptr;
    SlopSyncUartTransport _slots[kSlots];

    // Same deferred attach/detach discipline as SlopSyncBlePort (field bug #5,
    // TRAPS T5): nothing calls the hub except loop(), detach before attach.
    std::atomic<bool> _attached[kSlots]{};
    std::atomic<bool> _wantAttach[kSlots]{};
    std::atomic<bool> _wantDetach[kSlots]{};

    // Hub task only — no atomics, single writer and single reader.
    bool _active[kSlots]{};        // slot has a live peer session
    uint32_t _lastRxMs[kSlots]{};  // millis() of its last inbound frame

    // Bytes accumulated since the last 0x00. _rxOverflow suppresses
    // accumulation until the next delimiter, so an oversized run resyncs
    // cleanly instead of decoding its own tail as a fresh frame.
    uint8_t _rxAcc[kUartLinkEncodedMax]{};
    size_t _rxLen = 0;
    bool _rxOverflow = false;

    SlopSyncUartLinkStats _stats;
};

}  // namespace slopdrive

#endif  // UART_LINK_ENABLED
