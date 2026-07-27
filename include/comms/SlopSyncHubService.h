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
#include "SlopSyncCrypto.h"
#include "SlopSyncPlatform.h"
#include "SlopSyncUiToken.h"
// THE transport. There used to be two behind -DSLOPSYNC_WS_ASYNC so the
// links2004-vs-ESP32Async swap could be A/B'd with one variable moving; the
// A/B is settled (docs/http-plane-retirement.md 2 -- three device reboots and
// a 112-byte heap watermark with the old plane attached, 7/7 clean without)
// and links2004 has been removed from the build entirely. The #else branch
// pointed at a file that no longer exists, so it is gone with it.
#include "SlopSyncAsyncWsTransport.h"
#include "slopsync/hub/hub.hpp"

// Firmware types the service/delegate reference — forward-declared to keep this
// header light (full defs pulled into the .cpp).
struct SystemState;
class WebUI;
class MotionArbiter;
class PatternEngine;
class MotorDriver;
class SlopHttpServer;

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
    // WAVEFORM (0x0085 motion-segment) carries a commanded duration + an
    // EXPLICIT end-velocity presence flag; CHASE (0x0084 motion-input) leaves
    // has_duration false and derives has_end_vel from vel≠0 at push time. Both
    // channels share this ring — the drain builds the slopmotion::Command
    // straight from these fields, so the two paths differ ONLY here at ingress.
    uint32_t duration_us  = 0;
    bool     has_duration = false;
    bool     has_end_vel  = false;
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

    // The oldest entry still in the ring, WITHOUT regard to its due time —
    // nullptr when empty. This is the RFC-008 one-segment LOOKAHEAD: call it
    // straight after a popDue() and it hands back the segment that FOLLOWS the
    // one just popped, which is exactly what the handoff sanity guard needs to
    // bound the popped segment's end velocity. It exists at all because the
    // ring is a SCHEDULER, not a queue: a 0x0085 client sends each segment
    // ~120 ms before its start, so the successor is usually already sitting
    // here when its predecessor comes due.
    //
    // Read-only and non-consuming by design. Nothing about the guard may
    // change what the ring delivers or when.
    const PacingEntry* peekOldest() const {
        return _count == 0 ? nullptr : &_buf[_tail];
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
    SlopDriveHubDelegate(SystemState& state, WebUI& webui, MotionArbiter& arbiter, PacingRing& pacingRing,
                         SlopSyncUiTokenMinter& uiTokens)
        : _state(state), _webui(webui), _arbiter(arbiter), _pacingRing(pacingRing), _uiTokens(uiTokens) {}

    // ---- RFC-011 cfg_gen origin flag ---------------------------------------
    // Set by applyIntent whenever a CONFIG intent actually changed a value —
    // i.e. whenever the HUB already bumped its own cfg_gen for this change. The
    // service reads-and-clears it on the same tick (delegate and service both
    // run on the SlopSyncHub task) so its machine-side detector can tell "the
    // config moved because a client moved it" from "the config moved by itself",
    // and only bumps for the second case. Without this the hub would
    // double-bump every client config-set: harmless but wasteful (every
    // subscriber resyncs), and the whole point of RFC-002 was to stop paying
    // that for nothing.
    bool takeCfgIntentFlag() {
        const bool f = _cfgFromIntent;
        _cfgFromIntent = false;
        return f;
    }

    // ---- The trust ledger, bound AFTER construction -------------------------
    // Deliberately a pointer set by the service rather than a ctor reference:
    // the delegate is bound BY reference into the Hub's constructor, so the
    // delegate must be constructed FIRST and cannot name _hub.pairing() in its
    // own initializer list. init() closes the loop once both exist.
    //
    // A null pointer is a SAFE state, not a broken one: validateToken falls
    // through to `watch`, which is exactly the answer an unpaired client should
    // get. It never fails open.
    void bindPairing(slopsync::PairingManager& pm) { _pairing = &pm; }

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

    // 0x0084 motion-input (chase points) + 0x0085 motion-segment (timed
    // waveform segments) STREAM — decodes samples and pushes them onto the
    // shared pacing ring. See the .cpp for the full contract (handles both
    // stream channels; anything else is a no-op, matching the base default).
    void onStreamBundle(uint16_t channel_id, uint32_t session_id, const slopsync::BundleView& bundle) override;

private:
    SystemState& _state;
    WebUI& _webui;
    MotionArbiter& _arbiter;
    PacingRing& _pacingRing;
    SlopSyncUiTokenMinter& _uiTokens;
    slopsync::PairingManager* _pairing = nullptr;  // see bindPairing()
    bool _cfgFromIntent = false;

public:
    // Set by 0x0105 when a tuning knob actually moved; the service reads and
    // clears it on its 1 Hz tick and persists once. Public because the service
    // (not the delegate) owns NVS.
    bool _smTuneDirty = false;
};

// ---- The service -----------------------------------------------------------
class SlopSyncHubService {
public:
    // `motor` is taken by reference (not wired later like the PatternEngine)
    // because the catalog is built INSIDE the member-initializer list and its
    // FEATURE GATING — whether 0x0087 power is advertised at all — is read from
    // the driver right there. A setter would be too late: the etag is computed
    // when _hub is constructed, and a catalog that grew a channel afterwards
    // would be a different catalog under the same etag. Construct this service
    // AFTER motor.bind() picks the backend (main.cpp already does).
    SlopSyncHubService(SystemState& state, WebUI& webui, MotionArbiter& arbiter, MotorDriver& motor);

    // Build the port, load the persisted trust ledger, spawn the Core-0 hub
    // task and the low-priority signing task.
    void init();

    // RFC-029 §4: register GET /uitoken on the shared WebServer. Separate from
    // init() because the HTTP server belongs to WebUI and is only guaranteed to
    // exist after WebUI::begin(). Call once from setup(). Skipping it simply
    // means no browser-borne credential path exists — everything else works.
    void attachHttpRoutes(SlopHttpServer* server);

    // Shared-space kill switch for the browser credential path (persisted).
    void setUiTokenEnabled(bool on) { _uiTokens.setEnabled(on); }
    bool uiTokenEnabled() const { return _uiTokens.enabled(); }

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
    static void signTaskTrampoline(void* arg);
    void signTaskLoop();

    void syncSafety();          // firmware estop latch <-> hub safety word
    void publishTelemetry();    // cadenced STATE pushes
    void drainMotionStream();   // pop due PacingRing entries -> Core-1 sampler queue
    void publishAnomalies();    // Core-1 anomaly ring -> 0x0089 motion-anomaly EVENTs
    void drainLogBridge();      // RFC-017: SlopLog SPSC ring -> 0x0008 log EVENTs
    void pumpSigning();         // M4c: hub <-> signing task, both directions
    void pumpConfigGeneration();// RFC-011: machine-side config change -> cfg_gen

    void loadPairing();         // NVS -> PairingManager at boot (ledger + legacy migration)
    void savePairing();         // PairingManager -> NVS (skips while OTA active)
    void persistPairingIfChanged();

    // ---- Injected -----------------------------------------------------------
    SystemState& _state;
    WebUI& _webui;
    MotionArbiter& _arbiter;
    MotorDriver& _motor;   // power/thermal/energy telemetry + the 0x0087 feature gate
    PatternEngine* _patternEngine = nullptr;
    QueueHandle_t _motionStreamQueue = nullptr;  // Core-1 SlopMotion sampler queue (g_interp_queue)

    // ---- Owned (declaration order == construction order: catalog/clock/rng/
    //      pacing ring/delegate MUST precede _hub, which binds them by
    //      reference) ---------------------------------------------------------
    EspClock _clock;
    EspRandom _rng;
    EspCrypto _crypto;               // MUST precede _hub — bound by reference
    SlopSyncUiTokenMinter _uiTokens; // MUST precede _delegate — bound by reference
    slopsync::Catalog32 _catalog;
    PacingRing _pacingRing;
    SlopDriveHubDelegate _delegate;
    slopsync::Hub _hub;
    SlopSyncAsyncWsPort _port;

    TaskHandle_t _task = nullptr;

    // ---- M4c deferred signing (RFC-029 item 1) ------------------------------
    // THE ONE-TASK INVARIANT IS WHY THESE QUEUES EXIST. takePendingSignJob() and
    // submitSignature() are Hub methods, so they may ONLY be called from the hub
    // task — but the sign itself is 30-80 ms of uninterruptible ECP that would
    // put a 6-16 tick hole in every other client's motion stream. So the hub
    // task does both Hub calls and the queues carry the WORK across:
    //
    //   hub task:  takePendingSignJob() -> _signReqQ -> (sign task) -> _signResQ
    //              -> submitSignature()
    //
    // Depth 2: one session can have at most one outstanding signature and the
    // hub caps sessions at 4, but a queue this shallow simply back-pressures
    // (the job stays flagged in the hub slot and is taken on a later tick),
    // which is strictly better than a deep queue full of signatures for sessions
    // that died while waiting.
    struct SignRequest {
        uint32_t session_id = 0;
        uint8_t message[slopsync::kHubSigMaterialBytes] = {};
    };
    struct SignResult {
        uint32_t session_id = 0;
        uint8_t len = 0;
        uint8_t sig[slopsync::kTrustSigMaxBytes] = {};
    };
    QueueHandle_t _signReqQ = nullptr;
    QueueHandle_t _signResQ = nullptr;
    TaskHandle_t _signTask = nullptr;
    uint32_t _signsDone = 0;

    // ---- RFC-011 machine-side cfg_gen detector ------------------------------
    // The last PUBLISHED value of every field 0x0081 carries. A machine-side
    // change (physical control, boot adoption, an internal recalculation) has no
    // other way to reach the protocol's generation counter, and a client's
    // `precondition` CAS silently passing against config that already moved is
    // the failure this closes.
    struct CfgSnapshot {
        float window_min = 0, window_max = 0;
        float user_speed = 0, user_accel = 0;
        float input_speed = 0, input_accel = 0;
        float max_rail = 0, input_jerk = 0;
        bool valid = false;
    } _cfgSnap;

    // ---- Motion-input stream drain bookkeeping ------------------------------
    float _syncPrevTarget = -1.0f;  // previous ENQUEUED target, for the >0.003 move-gate

    // ---- Telemetry cadence bookkeeping --------------------------------------
    uint32_t _lastMotionMs = 0;
    uint32_t _lastSlowMs = 0;
    uint32_t _lastPatternMs = 0;
    uint32_t _lastPlanMs = 0;    // 0x0086 plan-strip, 45 Hz
    uint32_t _lastPowerMs = 0;   // 0x0087 power, 2 Hz (grant-capped at 10)
    uint16_t _lastCfgGen = 0;
    bool _cfgEverSent = false;
    // 0x008A machine-modes (M5b): the LAST PUBLISHED bytes, not the last known
    // values. Diffing what subscribers actually hold is what makes the
    // on-change trigger unable to disagree with them — see publishTelemetry.
    std::array<std::byte, 4> _lastModes{};
    bool _modesEverSent = false;
    // 0x008B/0x008C/0x008D slopmotion tuning — last PUBLISHED bytes, same
    // reason as _lastModes: diffing what subscribers hold cannot disagree with
    // what they hold.
    std::array<std::byte, 18> _lastSmLim{};
    std::array<std::byte, 20> _lastSmChase{};
    std::array<std::byte, 21> _lastSmWav{};
    bool _smLimEverSent = false;
    bool _smChaseEverSent = false;
    bool _smWavEverSent = false;
    // 0x0087 is only PUBLISHED when it is also DECLARED — a publishState() to a
    // channel absent from the catalog is refused anyway, but skipping the work
    // keeps the driver reads off the tick on a machine with no sensor.
    bool _hasPowerChannel = false;
    bool _hasDieTemp = false;
    bool _planEverSent = false;

    // Pattern-state change detection (last PUBLISHED snapshot). _patMask is in
    // here because the RFC-009 enabled_mask is part of the snapshot and moves
    // on homed/e-stop transitions the pattern PARAMETERS do not see — leaving
    // it out would let a client keep the whole card enabled through an e-stop.
    bool _patRunning = false;
    uint8_t _patIdx = 0xFF;
    uint8_t _patMask = 0xFF;
    float _patSpeed = -1.0f, _patDepth = -1.0f, _patStroke = -1.0f, _patSensation = -1.0f;

    // ---- Trust-ledger NVS scratch (RFC-029 item 3) --------------------------
    // ONE buffer for both directions. It lives HERE, as a member, rather than as
    // a function-local static, for one specific reason: this whole service is
    // placement-new'd into PSRAM, so a member costs nothing from the internal
    // heap the WebUI and the network stack compete for — and this codebase has
    // already killed the WebUI once by starving that heap. 1.9 KB is also far
    // too much to put on any task stack.
    //
    // No concurrency to guard: loadPairing() runs in init(), before the hub task
    // exists; savePairing() runs only on the hub task afterwards.
    uint8_t _ledgerBlob[slopsync::limits::trust_ledger_max_bytes] = {};

    // ---- RFC-017 log bridge -------------------------------------------------
    // The serial handoff RE-BINDS to the first log-channel GRANT (RFC-017), so
    // once an in-band subscriber is receiving the log, USB serial demotes to
    // Warn+ exactly as it does today for the first /api/log serve. One-shot.
    bool _logGrantSeen = false;
    uint32_t _logPublished = 0;

    // ---- Safety sync + pairing ----------------------------------------------
    uint16_t _estopSeq = 0;
    size_t _pairCountCached = 0;
    char _pairPin[8] = {};   // outlives the pairing window (hub holds a view)
};

}  // namespace slopdrive
