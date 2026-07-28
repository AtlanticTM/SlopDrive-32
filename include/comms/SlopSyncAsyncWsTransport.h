#pragma once

// SlopSyncAsyncWsTransport / SlopSyncAsyncWsPort — ESP32Async WS binding for
// the SlopSync hub.
//
// Constraints:
// - Real transport adapters live in firmware, never in lib/slopsync (SPEC
//   §13.1). THE only WS transport (DOCTRINE.md §9); every S3
//   main-controller env sets -DSLOPSYNC_WS_ASYNC=1 unconditionally.
// - Replaced a synchronous, busy-wait WS server whose backed-up client
//   socket blocked the sender until the peer drained or timed out.
//   AsyncWebSocket's bounded per-client queues drop or close and NEVER
//   block — backpressure (SPEC §9 / §10.4) is implemented by the transport,
//   not compensated for by the application. Rationale + measurements:
//   docs/http-plane-retirement.md; see also lib/espasyncwebserver/VENDORED.md.
//
// ---- Threading model (read before touching anything) ------------------------
// There is NO one-task invariant here, and there cannot be: AsyncTCP owns
// its own task, so callbacks arrive there while the hub runs on the
// SlopSyncHub task.
//   AsyncTCP task : onEvent -> attachClient / detachClient / pushRx
//   Hub task      : open / close / write / read, from hub.update()
// Two different mechanisms make that safe — do not collapse them:
// - RX is a lock-free SPSC ring: ONE producer (AsyncTCP task) advances
//   _rxTail, ONE consumer (hub task) advances _rxHead, and neither ever
//   writes the other's index. The producer fills _rx[tail] and THEN
//   publishes the new tail with a RELEASE store; the consumer reads the tail
//   with an ACQUIRE load and only then touches the slot — that pairing is
//   what makes the frame's bytes visible to the consumer (without it the
//   index could land before the data on a weakly-ordered core and the
//   reader would parse a half-written buffer). No mutex: a single-producer/
//   single-consumer ring does not need one.
// - TX uses AsyncWebSocket's own locks: _ws_clients_lock guards the client
//   list, _queue_lock guards each client's message queue, and the public
//   id-taking methods take both — so the hub task may send while the
//   AsyncTCP task is connecting/disconnecting clients.
// - NEVER HOLD AN AsyncWebSocketClient*. The AsyncTCP task can destroy a
//   client between lookup and use. This transport stores the client ID and
//   calls the id-taking API (`binary(id,...)`, `availableForWrite(id)`,
//   `close(id,...)`), which resolves the id UNDER the lock every time. This
//   is the single most important rule in this file.
//
// ---- Backpressure: a full queue is not an error -----------------------------
// write() returning false MEANS "not accepted right now; the caller's class
// semantics decide retry vs drop" (§13.1) — a full queue during a burst is
// ORDINARY FLOW CONTROL, not an error to have an opinion about. The
// transport DOES distinguish, using the registry's own control/data/raw
// classification (generated/registry_constants.hpp) rather than a judgement
// call:
//   STATE (0x0B), STREAM (0x0C) = data. Shed EARLY, above
//       kDataQueueHighWater, so a telemetry burst cannot fill the room
//       control frames need. Stale telemetry is worthless — conflation is
//       already the doctrine.
//   BLOB_CHUNK (0x1B) = bulk/resumable, its OWN third class (traffic-
//       classification lesson: TRAPS.md T16; heap angle: TRAPS.md T2).
//       Gated on the registry's OWN
//       advertised sender pacing budget (limits::blob_chunks_in_flight,
//       RFC-050) via the same queueLen() check the data class uses — NEVER
//       runs all the way to WS_MAX_QUEUED_MESSAGES before backing off, and
//       NEVER arms the control stall timer below. A resumable chunked
//       transfer legitimately spends several ticks paced behind a slow
//       link; that is not "a client stranded waiting on a reply that will
//       never come" (the ctrlStall timer's actual job), and
//       hub_impl.hpp's pumpBlobTransfer() already retries the same un-sent
//       index next tick with no NACK/teardown of its own. Before this class
//       existed, a 129-chunk catalog transfer pumping 2/tick into a 32-deep
//       queue filled it, and after kCtrlStallMs the same timer meant for a
//       wedged control reply killed the whole session (client-side
//       ConnectionResetError, once a genuine internal-heap-exhaustion PANIC
//       reboot at up to 32 live queued ~260 B buffers on a ~15 KB free
//       budget). Capping in-flight blob buffers at 4 keeps that under ~1 KB
//       instead.
//   everything else = control/raw. Never shed on our own initiative. Refused
//       ones start a STALL TIMER (kCtrlStallMs); the session is torn down
//       only if control stays unsendable that long, which is the one case
//       where a client really is stranded waiting on a reply that will
//       never come. §11.3's loss policy handles the teardown cleanly. The
//       timer is cleared ONLY by a control frame actually going out — never
//       by inbound traffic (a mute that inbound traffic can reset lets a
//       client that stopped reading but kept PINGing hold its session, and
//       every other client's queue room, forever). See
//       docs/ws-transport-baseline.md.
//   EVENT (0x0F) is CONTROL, deliberately — safety edges are the one thing
//       nobody is allowed to shed (see the 0x000E channel note).
// An earlier version of this transport tore a session down on the first
// control frame it could not queue: the catalog BLOB transfer pushes 57
// chunks into a 32-deep queue, filling it around chunk ~16 and killing
// every session before a client could finish connecting. "The queue was
// full once" and "this client is not draining" are different facts and
// must not share a code path.
//
// The policy is selectable (SlopSyncTxPolicy) because it is a protocol
// semantics decision, not a library one — RATIFIED 2026-07-26: Classify, on
// the evidence of two clean full-suite soaks (docs/http-plane-retirement.md
// §6). Changing it is an operator decision, not a tuning tweak.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "config_api.h"
#include "slopsync/hub/hub.hpp"
#include "slopsync/transport/transport.hpp"
#include "slopsync/wire/frame_buffer.hpp"

namespace slopdrive {

// How a send that cannot be queued is handled. Default is Classify.
//
//   CloseAlways  — any un-queueable frame closes the session. Simplest, and
//                  silent loss becomes structurally impossible; but a slow
//                  client that only backed up on TELEMETRY loses its session
//                  over something that did not matter.
//   Classify     — shed data frames; control frames refuse honestly and only
//                  tear down after kCtrlStallMs of continuous failure.
//   DropAlways   — never tear down. ONLY for measuring how bad the loss would
//                  be; it will strand a client waiting on a lost reply, so it
//                  is not a shipping configuration.
enum class SlopSyncTxPolicy : uint8_t { CloseAlways = 0, Classify = 1, DropAlways = 2 };

class SlopSyncAsyncWsTransport final : public slopsync::ITransport {
public:
    // 32, up from this transport's own first cut of 8 — SIZED FROM A
    // MEASUREMENT, not a guess: an 18-minute 3-client soak logged
    // `rx_ring_full: 2` at depth 8 -- rare, but it means two inbound frames were
    // dropped for want of a slot, and the operator's whole ask for this
    // transport is that clients do not lose things.
    //
    // WHY IT IS CHEAP: the ring is kFrameBufferCapacity (512 B) per slot, and
    // the whole SlopSyncHubService -- this port and every transport inside it --
    // is placement-new'd into PSRAM (see main.cpp, heap_caps_malloc with
    // MALLOC_CAP_SPIRAM). So 5 transports x 32 x 512 B = 82 KB comes out of 8 MB
    // of PSRAM, not out of the ~60 KB internal heap we are fighting for. Costing
    // this against internal RAM would have been the wrong trade; costing it
    // against PSRAM is free.
    //
    // PSRAM IS LEGAL HERE, and that is not automatic: the producer is the
    // AsyncTCP TASK, not an ISR. Anything an ISR touches must stay in internal
    // RAM, because PSRAM sits behind the flash cache and an ISR that touches it
    // during a cache-disabled window (flash write, OTA) faults. Do not move
    // ISR-reachable state here by analogy with this.
    //
    // Depth is NOT a substitute for draining: if the hub ever stops keeping up,
    // a deeper ring only delays the drop and hides the real problem. 2 drops in
    // 18 minutes says the drain is healthy and the ring was simply one size too
    // small for burst jitter.
    static constexpr uint8_t kRxRingDepth = 32;

    // Queue occupancy at or above which a DATA frame is shed early rather than
    // handed to AsyncWebSocket. Kept below WS_MAX_QUEUED_MESSAGES so control
    // frames still have room after telemetry has started shedding — the whole
    // point of classifying is lost if data fills the queue control needs.
    static constexpr size_t kDataQueueHighWater = (WS_MAX_QUEUED_MESSAGES * 3) / 4;

    // How long a CONTROL frame may stay un-sendable before the session is torn
    // down. A full queue is NOT an error -- see write() -- it is ordinary flow
    // control, and a burst (57 catalog BLOB_CHUNKs into a 32-deep queue) fills
    // it every single time. What is NOT ordinary is a queue that never drains,
    // because then a client is waiting on a reply that will never come. This
    // separates the two by TIME rather than by a single failed attempt. Long
    // enough to absorb the catalog BLOB burst; cannot be reset by client PING
    // traffic (see write()'s stall-timer logic), so a wedged client cannot
    // hold a session open indefinitely.
    static constexpr uint32_t kCtrlStallMs = 2000;

    // §10.3 hysteresis windows for pollCongestionLevel()'s watermark bands —
    // the SPEC's own numbers, not tuned locally.
    static constexpr uint32_t kCongestedSustainMs = 1000;
    static constexpr uint32_t kRecoveredSustainMs = 5000;

    SlopSyncAsyncWsTransport() = default;

    void bind(AsyncWebSocket* ws) { _ws = ws; }
    void setPolicy(SlopSyncTxPolicy p) { _policy = p; }

    // ---- ITransport (hub task) ----------------------------------------------
    bool open() override;
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<slopsync::FrameBuffer> read() override;
    slopsync::TransportProperties properties() const override;

    // ---- Driven by the port, on the ASYNCTCP task ---------------------------
    void attachClient(uint32_t id);
    void detachClient();
    void pushRx(const uint8_t* data, size_t len);

    // ---- Diagnostics (either task; all relaxed loads) -----------------------
    uint32_t clientId() const { return _clientId.load(std::memory_order_relaxed); }
    uint32_t rxDrops()  const { return _rxDrops.load(std::memory_order_relaxed); }
    uint32_t txDataDrops() const { return _txDataDrops.load(std::memory_order_relaxed); }
    uint32_t txCtrlFails() const { return _txCtrlFails.load(std::memory_order_relaxed); }
    // Held-not-dropped: a BLOB_CHUNK refused because the in-flight budget
    // (limits::blob_chunks_in_flight) or the queue itself is full. Never
    // lost — pumpBlobTransfer() retries the same index — so this is a pacing
    // counter, not a loss counter, and deliberately not folded into txDataDrops.
    uint32_t txBlobHolds() const { return _txBlobHolds.load(std::memory_order_relaxed); }

    // ---- §10.3 congestion classification, HUB TASK ONLY ---------------------
    // Feeds Hub::setCongestionLevel(ITransport&, level) — see hub.hpp: "real
    // bindings feed their native signals through the same choke point". This
    // was the missing wire (LEDGER "Morning ruling batch" item 1): without
    // it, congestionLevel sits at 0 forever and pumpStatePacing()'s
    // shedDecision()/ConflateHard coalescing never engages on real hardware,
    // no matter how backed up the socket gets.
    //
    // Reuses signals this transport already tracks; no new queue, no
    // allocation:
    //   severe (2): a CONTROL/never-shed frame is CURRENTLY stalled
    //     (_ctrlStallSinceMs != 0) -- §10.4's own definition of "the
    //     never-shed queue itself can't drain".
    //   congested (1) / clear (0): the data queue watermark, hysteresis per
    //     §10.3 -- sustained > 50% for 1 s -> congested; < 20% for 5 s ->
    //     recovered. Between the two bands, the level holds (no flapping on
    //     one noisy sample).
    // Caller (SlopSyncAsyncWsPort::loop()) polls this once per attached slot
    // per tick and republishes the result into the hub.
    uint8_t pollCongestionLevel(uint32_t nowMs);

private:
    // Is this frame type shed-able under backpressure? Byte 0 of the header is
    // the frame type (wire/frame_header.hpp), so this needs no parsing.
    static bool isDroppable(std::span<const std::byte> frame);
    // Is this a BLOB_CHUNK — bulk/resumable, its own third backpressure class
    // (see the header comment block above). Never true at the same time as
    // isDroppable().
    static bool isBlobChunk(std::span<const std::byte> frame);

    AsyncWebSocket* _ws = nullptr;
    SlopSyncTxPolicy _policy = SlopSyncTxPolicy::Classify;

    // 0 = unbound. AsyncWebSocket client ids start at 1 (_cNextId(1)).
    std::atomic<uint32_t> _clientId{0};
    std::atomic<bool> _open{false};

    // SPSC ring: producer = AsyncTCP task (tail), consumer = hub task (head).
    slopsync::FrameBuffer _rx[kRxRingDepth]{};
    std::atomic<uint8_t> _rxHead{0};
    std::atomic<uint8_t> _rxTail{0};

    // millis() of the first control frame we could not queue, 0 = none
    // outstanding. Written on the hub task only.
    uint32_t _ctrlStallSinceMs = 0;

    // pollCongestionLevel() hysteresis state. Hub task only (same as
    // _ctrlStallSinceMs) -- no atomics needed.
    uint8_t _congestionLevel = 0;
    uint32_t _aboveSinceMs = 0;  // 0 = not currently above the congested watermark
    uint32_t _belowSinceMs = 0;  // 0 = not currently below the recovered watermark

    std::atomic<uint32_t> _rxDrops{0};
    std::atomic<uint32_t> _txDataDrops{0};
    std::atomic<uint32_t> _txCtrlFails{0};
    std::atomic<uint32_t> _txBlobHolds{0};
};

// The server + its N transport slots. Owns the AsyncWebSocket handler and
// bridges connect/disconnect to hub.attach/detachTransport.
//
// NOTE ON THE HTTP SERVER: AsyncWebSocket is an AsyncWebHandler, so it needs an
// AsyncWebServer to live in. This port owns its OWN AsyncWebServer on
// SLOPSYNC_WS_PORT (82) rather than sharing the WebUI's (:80) — isolation
// means a WebUI stall can never touch the protocol plane.
class SlopSyncAsyncWsPort {
public:
    static constexpr uint8_t kSlots = slopsync::kHubMaxSessions + 1;

    SlopSyncAsyncWsPort();

    void begin(slopsync::Hub* hub);
    // AsyncTCP needs no pumping; this only drains deferred attach/detach and
    // sweeps bookkeeping (see the .cpp), and is a no-op most ticks.
    void loop();

    void setPolicy(SlopSyncTxPolicy p);

private:
    void onEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len);
    int  slotForClient(uint32_t id) const;   // -1 = none
    void detachSlot(int slot);

    AsyncWebServer _http;
    AsyncWebSocket _ws;
    slopsync::Hub* _hub = nullptr;
    SlopSyncAsyncWsTransport _slots[kSlots];
    // Written on the AsyncTCP task, read on the hub task -> atomic.
    std::atomic<bool> _attached[kSlots]{};

    // ---- THE HUB IS TOUCHED ONLY FROM THE HUB TASK (field bug #5) -----------
    // onEvent() runs on the AsyncTCP task. Calling hub.attachTransport()/
    // detachTransport() from there races Hub::update()'s slot walk on the hub
    // task: it null-checks slot.transport once, then dereferences it deeper in
    // pumpStatePacing(), so AsyncTCP nulling it mid-walk is a LoadProhibited
    // panic on Core 0 — rapid connect/disconnect (slopsoak's `churn`) hits it
    // reliably. So onEvent only RECORDS intent in these flags; loop() (hub
    // task) performs the actual attach/detach. See TRAPS.md T5.
    std::atomic<bool> _wantAttach[kSlots]{};
    std::atomic<bool> _wantDetach[kSlots]{};
};

}  // namespace slopdrive
