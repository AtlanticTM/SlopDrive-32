#pragma once

// ============================================================================
// SlopSyncAsyncWsTransport / SlopSyncAsyncWsPort — the ESP32Async binding for
// the SlopSync hub (SPEC §13.1: real transport adapters live in firmware, NEVER
// in lib/slopsync). Drop-in replacement for the links2004 SlopSyncWsTransport;
// selected at build time by -DSLOPSYNC_WS_ASYNC=1 so the two can be A/B'd on
// otherwise-identical firmware.
//
// ─── WHY THIS EXISTS ────────────────────────────────────────────────────────
// links2004's send path is a SYNCHRONOUS BUSY-WAIT on the caller's task: a
// backed-up client socket blocks the sender until the peer drains or
// WEBSOCKETS_TCP_TIMEOUT expires (already capped 5000 -> 1200 ms here, which
// BOUNDS the stall rather than removing it). Everything the old transport did
// about that — the send-stall mute, kStallEvictMs, the reaper — was scaffolding
// built to survive a transport that can stall a task. AsyncWebSocket gives
// bounded per-client queues that drop or close and NEVER block, which is §9 /
// §10.4 backpressure implemented by the transport instead of compensated for by
// the application. See lib/espasyncwebserver/VENDORED.md.
//
// ─── THE THREADING MODEL (READ THIS BEFORE TOUCHING ANYTHING) ───────────────
// The old transport had a ONE-TASK invariant and was deliberately mutex-free.
// That invariant IS GONE and cannot be recovered: AsyncTCP owns its own task,
// so callbacks arrive there while the hub runs on the SlopSyncHub task.
//
//   AsyncTCP task : onEvent -> attachClient / detachClient / pushRx
//   Hub task      : open / close / write / read, from hub.update()
//
// Two mechanisms make that safe, and they are different mechanisms for the two
// directions — do not collapse them:
//
// RX — a lock-free SPSC ring. ONE producer (AsyncTCP task) advances _rxTail,
//   ONE consumer (hub task) advances _rxHead, and neither ever writes the
//   other's index. The producer fills _rx[tail] and THEN publishes the new tail
//   with a RELEASE store; the consumer reads the tail with an ACQUIRE load and
//   only then touches the slot. That pairing is what makes the frame's bytes
//   visible to the consumer — without it the index could land before the data
//   on a weakly-ordered core and the reader would parse a half-written buffer.
//   No mutex, because a single-producer/single-consumer ring does not need one.
//
// TX — AsyncWebSocket's own locks. _ws_clients_lock guards the client list and
//   _queue_lock guards each client's message queue; the public id-taking
//   methods take both. So the hub task may send while the AsyncTCP task is
//   connecting/disconnecting clients. Verified in the vendored source before
//   adopting it.
//
// NEVER HOLD AN AsyncWebSocketClient*. The AsyncTCP task can destroy a client
// between your lookup and your use. This transport stores the client ID and
// calls the id-taking API (`binary(id,...)`, `availableForWrite(id)`,
// `close(id,...)`), which resolves the id UNDER the lock every time. That is
// the single most important rule in this file.
//
// ─── BACKPRESSURE: A FULL QUEUE IS NOT AN ERROR ─────────────────────────────
// write() returning false already MEANS "not accepted right now; the caller's
// class semantics decide retry vs drop" (§13.1). The hub retries. So a full
// queue during a burst is ORDINARY FLOW CONTROL and the transport's job is to
// say so honestly, not to have an opinion about it.
//
// LEARNED ON HARDWARE, THE EXPENSIVE WAY: the first version of this file tore
// the session down on the first control frame it could not queue. The catalog
// BLOB transfer pushes 57 chunks into a 32-deep queue, so it filled at chunk
// ~16 and killed EVERY session before a client could finish connecting. "The
// queue was full once" and "this client is not draining" are different facts
// and must not share a code path.
//
// What the transport DOES distinguish, using the registry's own control/data/raw
// classification (generated/registry_constants.hpp) rather than a judgement call:
//
//   STATE (0x0B), STREAM (0x0C) = data. Shed EARLY, above kDataQueueHighWater,
//       so a telemetry burst cannot fill the room control frames need. Stale
//       telemetry is worthless — conflation is already the doctrine.
//   everything else = control/raw. Never shed on our own initiative. Refused
//       ones start a STALL TIMER (kCtrlStallMs); the session is torn down only
//       if control stays unsendable that long, which is the one case where a
//       client really is stranded waiting on a reply that will never come.
//       §11.3's loss policy handles the teardown cleanly.
//   EVENT (0x0F) is CONTROL, deliberately. Safety edges are the one thing
//       nobody is allowed to shed (see the 0x000E channel note).
//
// The stall timer is cleared ONLY by a control frame actually going out — never
// by inbound traffic. That is the precise bug that made links2004 fatal: its
// send-stall mute was cleared by ANY inbound byte, so a client that stopped
// reading but kept PINGing re-armed the blocking write forever and took every
// other session down with it (measured: two healthy clients to 0 Hz,
// permanently — see docs/ws-transport-baseline.md).
//
// The policy is selectable — see SlopSyncTxPolicy — because it is a protocol
// semantics decision, not a library one. RATIFIED 2026-07-26: Classify, on the
// evidence of two clean full-suite soaks. See docs/http-plane-retirement.md §6.
// Changing it is an operator decision, not a tuning tweak.
// ============================================================================

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
    // 32, up from links2004's 4 and from this transport's own first cut of 8.
    //
    // SIZED FROM A MEASUREMENT, not a guess: an 18-minute 3-client soak logged
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
    // separates the two by TIME rather than by a single failed attempt.
    // 2 s matches the links2004 port's kStallEvictMs, deliberately: same
    // promise, mechanism that cannot be defeated by the client PINGing.
    static constexpr uint32_t kCtrlStallMs = 2000;

    SlopSyncAsyncWsTransport() = default;

    void bind(AsyncWebSocket* ws) { _ws = ws; }
    void setPolicy(SlopSyncTxPolicy p) { _policy = p; }

    // ---- ITransport (hub task) --------------------------------------------
    bool open() override;
    void close() override;
    bool write(std::span<const std::byte> frame) override;
    std::optional<slopsync::FrameBuffer> read() override;
    slopsync::TransportProperties properties() const override;

    // ---- Driven by the port, on the ASYNCTCP task -------------------------
    void attachClient(uint32_t id);
    void detachClient();
    void pushRx(const uint8_t* data, size_t len);

    // ---- Diagnostics (either task; all relaxed loads) ---------------------
    uint32_t clientId() const { return _clientId.load(std::memory_order_relaxed); }
    uint32_t rxDrops()  const { return _rxDrops.load(std::memory_order_relaxed); }
    uint32_t txDataDrops() const { return _txDataDrops.load(std::memory_order_relaxed); }
    uint32_t txCtrlFails() const { return _txCtrlFails.load(std::memory_order_relaxed); }

private:
    // Is this frame type shed-able under backpressure? Byte 0 of the header is
    // the frame type (wire/frame_header.hpp), so this needs no parsing.
    static bool isDroppable(std::span<const std::byte> frame);

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

    std::atomic<uint32_t> _rxDrops{0};
    std::atomic<uint32_t> _txDataDrops{0};
    std::atomic<uint32_t> _txCtrlFails{0};
};

// The server + its N transport slots. Owns the AsyncWebSocket handler and
// bridges connect/disconnect to hub.attach/detachTransport.
//
// NOTE ON THE HTTP SERVER: AsyncWebSocket is an AsyncWebHandler, so it needs an
// AsyncWebServer to live in. This port owns its OWN AsyncWebServer on
// SLOPSYNC_WS_PORT (82) rather than sharing the WebUI's, which keeps the swap
// like-for-like with the links2004 topology (WebUI on :80, SlopSync on :82) and
// keeps a WebUI stall from ever being able to touch the protocol plane.
class SlopSyncAsyncWsPort {
public:
    static constexpr uint8_t kSlots = slopsync::kHubMaxSessions + 1;

    SlopSyncAsyncWsPort();

    void begin(slopsync::Hub* hub);
    // Kept for signature parity with the links2004 port so the service task is
    // identical either way. AsyncTCP needs no pumping — this only sweeps
    // bookkeeping, and it is a no-op most ticks.
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

    // ---- THE HUB IS TOUCHED ONLY FROM THE HUB TASK (field bug #5) ----------
    // onEvent() runs on the AsyncTCP task. It used to call
    // hub.attachTransport()/detachTransport() directly from there, which put a
    // SECOND task inside the hub's slot table while the hub task was walking
    // it. Hub::update() null-checks slot.transport once at the top of its walk
    // and dereferences it several calls deeper in pumpStatePacing(); the
    // AsyncTCP task nulling it in that window is a LoadProhibited panic on
    // Core 0, and rapid connect/disconnect (slopsoak's `churn`) is exactly the
    // workload that lands in it.
    //
    // So onEvent now only RECORDS intent in these flags and loop() -- which
    // runs on the hub task -- performs the actual attach/detach. The header's
    // own rule ("everything touched here is either atomic or owned by
    // AsyncWebSocket's own locks") was always right; calling into the hub
    // simply was not compatible with it.
    //
    // NOTE the links2004 port did not have this bug and could not have: its
    // loop() was pumped BY the hub task, so its callbacks already ran there.
    // Going async moved the callbacks to another task and the invariant went
    // with them, silently.
    std::atomic<bool> _wantAttach[kSlots]{};
    std::atomic<bool> _wantDetach[kSlots]{};
};

}  // namespace slopdrive
