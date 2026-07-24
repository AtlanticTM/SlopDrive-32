#pragma once

// ============================================================================
// SlopSyncHubService — the composition root for the firmware SlopSync hub.
//
// Owns (and wires, in the one order that satisfies the Hub's by-reference
// dependencies): the ESP clock/rng adapters, this device's channel catalog,
// the HubDelegate (which bridges intents onto WebUI::handleCommand → the
// MotionArbiter, honoring the sole-caller rule), the slopsync::Hub itself, and
// the WebSocket port. Spawns ONE FreeRTOS task ("SlopSyncHub", Core 0) that is
// the ONLY thread ever touching the hub or the WS server — see the one-task
// invariant documented in SlopSyncWsTransport.h.
//
// The delegate NEVER commands the motor directly (CLAUDE.md §2 sole-caller):
// every motion-bearing intent becomes a WebUI::handleCommand() call, exactly
// the path the WS UI already uses, which submits to the MotionArbiter.
// ============================================================================

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "SlopSyncCatalog.h"
#include "SlopSyncPlatform.h"
#include "SlopSyncWsTransport.h"
#include "slopsync/hub/hub.hpp"

// Firmware types the service/delegate reference — forward-declared to keep this
// header light (full defs pulled into the .cpp).
struct SystemState;
class WebUI;
class MotionArbiter;
class PatternEngine;

namespace slopdrive {

// ---- Motion-input pacing ring (SlopSync STREAM 0x0084 -> Core-1 sampler) ---
// Roadmap backlog #5 rough-in. Bridges onStreamBundle() — which fires inside
// _hub.update(), i.e. ON the SlopSyncHub task — to taskLoop's own 5 ms tick,
// which drains due entries into the Core-1 sampler queue. Both the producer
// (onStreamBundle) and the consumer (taskLoop) run on the SAME FreeRTOS task
// (see SlopSyncHubService::taskLoop's one-task invariant), so this is
// deliberately lock-free: a mutex here would guard a race that structurally
// cannot happen, and would just be dead weight on the stream hot path.
struct PacingEntry {
    uint64_t due_us = 0;   // device µs, esp_timer_get_time() domain (unwrapped 64-bit)
    float    target = 0.0f;
    float    vel    = 0.0f;
};

class PacingRing {
public:
    static constexpr size_t kCapacity = 64;

    // Pushes one entry. Returns true if the ring was already full and the
    // oldest entry was overwritten to make room (newest wins) — the caller
    // counts that as a drop.
    bool push(const PacingEntry& e) {
        bool overwrote = false;
        if (_count == kCapacity) {
            _tail = (_tail + 1) % kCapacity;  // evict oldest
            overwrote = true;
        } else {
            ++_count;
        }
        _buf[_head] = e;
        _head = (_head + 1) % kCapacity;
        return overwrote;
    }

    // Pops the oldest entry into `out` iff its due_us <= now_us. Entries are
    // pushed in non-decreasing due_us order (STREAM t_off is strictly
    // increasing within a bundle, §5.4, and bundles arrive in order over a
    // stream-ordered WS connection), so checking only the ring's head is
    // sufficient — callers loop this until it returns false to drain
    // everything due on a given tick.
    bool popDue(uint64_t now_us, PacingEntry& out) {
        if (_count == 0 || _buf[_tail].due_us > now_us) return false;
        out = _buf[_tail];
        _tail = (_tail + 1) % kCapacity;
        --_count;
        return true;
    }

private:
    std::array<PacingEntry, kCapacity> _buf{};
    size_t _head = 0, _tail = 0, _count = 0;
};

// ---- The application delegate ----------------------------------------------
// Translates the hub's role-layer callbacks into device actions. Applies +
// clamps intents via WebUI::handleCommand (Ground Truth: echoes the APPLIED,
// post-clamp values the handler reports, never the request).
class SlopDriveHubDelegate final : public slopsync::HubDelegate {
public:
    SlopDriveHubDelegate(SystemState& state, WebUI& webui, MotionArbiter& arbiter, PacingRing& pacingRing)
        : _state(state), _webui(webui), _arbiter(arbiter), _pacingRing(pacingRing) {}

    slopsync::AccessLevel validateToken(std::span<const std::byte> instance_id,
                                        std::span<const std::byte> token, bool hasToken) override;

    slopsync::Result<slopsync::IntentValueMap, slopsync::NackCode> applyIntent(
        uint16_t channel_id, const slopsync::IntentValueMap& requested, slopsync::AccessLevel role,
        bool& cfgChanged) override;

    void onEstop(uint8_t cause, uint8_t origin) override;
    bool canClearEstop() override;

    std::optional<uint8_t> sourceForChannel(uint16_t channel_id) override;
    slopsync::SourceLossPolicy sourcePolicy(uint8_t source_id) override;
    void onDeadmanStop(uint8_t source_id) override;

    void onSessionJoined(uint32_t session_id) override;
    void onSessionLeft(uint32_t session_id) override;

    // 0x0084 motion-input STREAM — decodes samples and pushes them onto the
    // pacing ring. See the .cpp for the full contract (only handles
    // ch::motion_input; anything else is a no-op, matching the base default).
    void onStreamBundle(uint16_t channel_id, uint32_t session_id, const slopsync::BundleView& bundle) override;

private:
    SystemState& _state;
    WebUI& _webui;
    MotionArbiter& _arbiter;
    PacingRing& _pacingRing;
};

// ---- The service -----------------------------------------------------------
class SlopSyncHubService {
public:
    SlopSyncHubService(SystemState& state, WebUI& webui, MotionArbiter& arbiter);

    // Build the port, load persisted pairing tokens, spawn the Core-0 task.
    void init();

    // Optional (additive): wire the PatternEngine so 0x0082 pattern-state can be
    // published from the live engine (Ground Truth). Without it, 0x0082 is
    // silent — every other channel works regardless. Call once from setup().
    void setPatternEngine(PatternEngine* pe) { _patternEngine = pe; }

    // Optional (additive): wire the Core-1 sampler's command queue so drained
    // 0x0084 motion-input pacing-ring entries can reach it (mirrors
    // setPatternEngine). Without it, onStreamBundle still fills the ring but
    // taskLoop has nowhere to send drained commands, so they're just dropped
    // (counted). Call once from setup(), after both g_interp_queue and this
    // service exist.
    void setMotionStreamQueue(QueueHandle_t q) { _motionStreamQueue = q; }

    // Pairing window control (app-facing). openPairing copies the PIN (the hub
    // holds a view of it while the window is open) and lights the SlopGlow
    // Pairing state; closePairing clears both and persists any new tokens.
    void openPairing(const char* pin);
    void closePairing();

    // OTA flash-write safety: park/revive the task so not one byte of XIP/WS
    // work runs during the flash-cache-disabled window (mirrors UiSocket's
    // suspend/resume). OtaService should call these around Update.begin/end.
    void suspendTask();
    void resumeTask();

    slopsync::Hub& hub() { return _hub; }

private:
    static void taskTrampoline(void* arg);
    void taskLoop();

    void syncSafety();          // firmware estop latch <-> hub safety word
    void publishTelemetry();    // cadenced STATE pushes
    void drainMotionStream();   // pop due PacingRing entries -> Core-1 sampler queue

    void loadPairing();         // NVS -> PairingManager at boot
    void savePairing();         // PairingManager -> NVS (skips while OTA active)
    void persistPairingIfChanged();

    // ---- Injected -----------------------------------------------------------
    SystemState& _state;
    WebUI& _webui;
    MotionArbiter& _arbiter;
    PatternEngine* _patternEngine = nullptr;
    QueueHandle_t _motionStreamQueue = nullptr;  // Core-1 SlopMotion sampler queue (g_interp_queue)

    // ---- Owned (declaration order == construction order: catalog/clock/rng/
    //      pacing ring/delegate MUST precede _hub, which binds them by
    //      reference) ---------------------------------------------------------
    EspClock _clock;
    EspRandom _rng;
    slopsync::Catalog32 _catalog;
    PacingRing _pacingRing;
    SlopDriveHubDelegate _delegate;
    slopsync::Hub _hub;
    SlopSyncWsPort _port;

    TaskHandle_t _task = nullptr;

    // ---- Motion-input stream drain bookkeeping ------------------------------
    float _syncPrevTarget = -1.0f;  // previous ENQUEUED target, for the >0.003 move-gate

    // ---- Telemetry cadence bookkeeping --------------------------------------
    uint32_t _lastMotionMs = 0;
    uint32_t _lastSlowMs = 0;
    uint32_t _lastPatternMs = 0;
    uint16_t _lastCfgGen = 0;
    bool _cfgEverSent = false;

    // Pattern-state change detection (last PUBLISHED snapshot).
    bool _patRunning = false;
    uint8_t _patIdx = 0xFF;
    float _patSpeed = -1.0f, _patDepth = -1.0f, _patStroke = -1.0f, _patSensation = -1.0f;

    // ---- Safety sync + pairing ----------------------------------------------
    uint16_t _estopSeq = 0;
    size_t _pairCountCached = 0;
    char _pairPin[8] = {};   // outlives the pairing window (hub holds a view)
};

}  // namespace slopdrive
