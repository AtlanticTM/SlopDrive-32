// SlopDrive-32 — main.cpp: composition root, wires modules and creates tasks.
// Constraints:
//   All logic lives in system/, motion/, comms/, ui/. This file only declares
//   module instances, wires them in setup(), creates FreeRTOS tasks with
//   correct core pinning, and idles in loop().
//   Core 1 (real-time): motorTask and PatternEngine's own task.
//   Core 0 (system): commsTask, httpTask.
//   Event-driven: SlopSync callbacks submit intents via the arbiter (Core 0
//   -> Core 1 deferral queues); PatternEngine emits one intent per stroke
//   segment; motorTask drains them and forwards over the motion link.
//   No periodic motion tick, no chase loop, no S3-side sampler: the RP2350
//   holds the plan (docs/rp-motion-port.md).
// See: architecture.md (motion doctrine, sole-caller, dual-core
//   task separation).

#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <new>              // placement new (SlopSync hub goes into PSRAM)
#include "esp_heap_caps.h"
#include "esp_system.h"     // esp_reset_reason() — boot-time crash/reboot diagnostics

#include "config_api.h"

#include "AppLog.h"          // bridge only: applogBegin/applogDrain (SlopLog sinks)
#include "BootHeap.h"        // per-init-stage heap attribution (shared with SlopSyncHubService)
#include "CrashRing.h"       // last-words ring: begin/crumb/heapSample
#include "OomHook.h"        // failed-alloc hook: names the allocation, not the victim
#include "HeapTrace.h"      // DIAGNOSTIC: alloc/free history (SLOPSYNC_HEAP_BISECT only)
#include "HeapWatch.h"      // DIAGNOSTIC: hardware watchpoint 0 (SLOPSYNC_HEAP_BISECT only)
#include "sloplog/sloplog.h"
#include "SystemState.h"
#include "ConfigStore.h"
#include "MotionPassthrough.h"
#include "SlopGlowBoard.h"

#include "range_mapper.h"
#include "Kinematics.h"
#include "freertos/queue.h"
#include <esp_timer.h>

#if defined(DRIVER_AIM_SERVO)
#include "MlinkServoDriver.h"
#include "MachineConfig.h"
#endif

#include "MotionArbiter.h"
// Engine ENUM ORDINALS only (Mode, PlanKind): the RP holds the engine, and
// the ordinals ride the status frame verbatim. Transcribing them here would
// be the T20 hand-copied vocabulary.
#include <slopmotion/slopmotion.hpp>
#include "PatternEngine.h"

#include "WifiLink.h"
#include "SlopSyncHubService.h"

#include "WebUI.h"
// WebUI.h only forward-declares SlopHttpServer; commsTask's stall reap calls
// through it (abortBlockedClient), so this TU needs the complete type.
#include "ui/SlopHttpServer.h"
#include "OtaService.h"

#if defined(SD32_MODBUS_TOOLS)
#include "ServoModbus.h"
#endif

#if defined(SD32_MODBUS_TOOLS) && defined(DRIVER_AIM_SERVO)
#include "EncoderValidator.h"
#endif

#if defined(BLE_ENABLED)
extern "C" bool bleInUse(void) { return true; }
#endif


// ---- Module instances -------------------------------------------------------

// The Modbus bus carries motor configuration and the encoder audit only
// (architecture.md section 1); SD32_MODBUS_TOOLS drops both from the image.
#if defined(SD32_MODBUS_TOOLS)
static ServoModbus     servoModbus(Serial1, /* addr */ 1);
#endif

// The ONE motion backend (architecture.md section 1): the RP2350 over the SPI
// link, driven by quadrature. The drive is saved in encoder-follow (0x19=2),
// so nothing on the S3 renders pulses.
// TODO(sd-pln): one-time drive programming moves to the web flasher, which is
// what a production build without the Modbus bus needs.
#if defined(DRIVER_AIM_SERVO)
  MlinkServoDriver  motor;
#else
  #error "No motor driver selected. Define DRIVER_AIM_SERVO in platformio.ini build_flags."
#endif

static SystemState        g_state;
static RangeMapper        mapper;
static PatternEngine      patternEngine(g_state, mapper, motor);
static MotionArbiter      arbiter(g_state, mapper, motor);

// Plan-adoption trace (diagnosis, sd-tki), rebuilt on kEvtPlanAdopted. Core 1
// queues a POD record per adopted plan -- never a formatted log on the motion
// core (T27) -- and httpTask formats it into the `plan` tag. Drop-if-full;
// the drop count rides the line so a gap is visible.
struct PlanTrace {
    uint32_t due_us;       // MASTER microseconds, converted by the driver
    int32_t  late_us;      // pull time behind the anchor
    float    target;       // normalized plan end
    uint32_t duration_us;  // 0 = a hold
    uint8_t  cmd_seq;      // frame that caused it, 0 = the slave's own settle
    uint8_t  kind_after;
    uint8_t  mode_after;
};
static constexpr size_t   kPlanTraceDepth = 64;
static QueueHandle_t      g_plan_trace_queue     = nullptr;
static uint32_t           g_plan_trace_drops     = 0;   // Core 1 writes, httpTask reads

static WifiLink           wifiLink(g_state);

static WebUI webui(g_state, motor, mapper, patternEngine);

// SlopSync hub — the ecosystem sync plane (binary WS :SLOPSYNC_WS_PORT).
// Lives in PSRAM: as a BSS static its ~100 KB reservation starved internal
// heap to 13 KB free / 1.6 KB min — the WebUI page-serve death. The S3 has
// 8 MB of PSRAM for exactly this; placement-new'd there in setup(). Its
// FreeRTOS task stack stays internal (created inside init(), default heap).
static slopdrive::SlopSyncHubService* slopSyncHub = nullptr;

// WiFi OTA path (firmware + LittleFS bundle). Owns the shared safety gate for
// both ArduinoOTA (espota) and the HTTP /api/ota endpoints. Serviced from the
// Core-0 httpTask only — never the motion-critical core.
static OtaService      otaService(g_state, arbiter, patternEngine);

#if defined(UART_LINK_ENABLED)
// Binds the bridge control channel's OTA ops to OtaService without the comms
// layer including a system header. Stateless: the state lives in OtaService.
struct SerialOtaSink final : slopdrive::SlopSyncUartPort::IOtaSink {
    uint8_t otaBegin(uint8_t target, uint32_t size) override {
        return otaService.otaSerialBegin(target, size);
    }
    uint8_t otaData(uint16_t seq, const uint8_t* d, size_t n) override {
        return otaService.otaSerialData(seq, d, n);
    }
    uint8_t otaEnd(uint32_t crc) override { return otaService.otaSerialEnd(crc); }
    void    otaAbort(uint8_t reason) override { otaService.otaSerialAbort(reason); }
    uint8_t  otaAbortReason() const override { return otaService.otaSerialAbortReason(); }
    uint16_t otaNextSeq() const override { return otaService.otaSerialNextSeq(); }
    bool     otaInFlight() const override { return otaService.otaSerialInFlight(); }
};
static SerialOtaSink g_serialOtaSink;

// Diag-archive pull for the C5 bridge (sd-0gy): one bounded batch per request,
// stateless -- the cursor rides the wire, so an abandoned pull costs nothing.
struct SerialDiagSource final : slopdrive::SlopSyncUartPort::IDiagSource {
    size_t diagRead(uint32_t from, const char* tag, char* buf, size_t cap,
                    uint32_t& next, bool& done) override {
        DiagRead rd(tag, from);
        size_t used = 0, n = 0;
        while (used < cap && (n = rd.next(buf + used, cap - used)) > 0) used += n;
        next = rd.cursor();
        done = rd.finished();
        return used;
    }
};
static SerialDiagSource g_serialDiagSource;
#endif

#if defined(SD32_MODBUS_TOOLS) && defined(DRIVER_AIM_SERVO)
// Report-only encoder audit: reads servoModbus telemetry against the link's
// rendered position, never commands anything. Lives on httpTask Core 0.
static EncoderValidator encoderValidator(servoModbus, motor);
#endif



// ---- Task stack census ------------------------------------------------------
//
// Our five task stacks are 38,912 B of a ~256 KB heap; a stack sized by guess is
// pure waste, and the high-water mark is the only honest way to size one.
// uxTaskGetSystemState() reports EVERY task, IDF's own included, so WiFi/lwIP/
// NimBLE stacks show up in the same budget — it needs
// CONFIG_FREERTOS_USE_TRACE_FACILITY, which this framework's sdkconfig sets.
//
// ESP-IDF reports the mark in BYTES REMAINING (vanilla FreeRTOS reports words) —
// so a SMALL number is the dangerous one, and the reclaimable slack is
// (stack size - peak use), which this figure is the tail of.
//
// Only meaningful once tasks have done real work: never call this from setup().
// httpTask owns the trigger, so this needs no synchronization.
//
// A HIGH-WATER MARK ONLY COVERS PATHS ACTUALLY EXERCISED SINCE BOOT — the whole
// reason this re-reports instead of printing once. An idle machine never runs
// the sampler's planner path, and httpTask's deepest path is OTA, whose peak is
// unobservable by construction because the flash is followed by a reboot. So
// after the first full census, ONLY REGRESSIONS print: a task that has gone
// deeper than last reported is news, everything else is noise in a 44-line ring.
// DO NOT size a stack from a census that has not seen motion and a page serve.
// File-scope, not function-local: a function-local static gets a lazy-init
// guard and an __cxa_atexit registration, which is the field bug TRAPS records
// for /uitoken. Nothing here needs either.
static bool     s_census_done = false;
static uint32_t s_census_last_ms = 0;

// Deepest-seen-so-far per task, keyed by name. Linear scan at 0.1 Hz over a
// couple dozen entries costs nothing and avoids owning task handles.
struct StackSeen {
    char     name[16];
    uint32_t free_min;   // smallest remaining-bytes seen = deepest use
};
static StackSeen s_seen[32];
static size_t    s_seen_count = 0;

// Returns true if this reading is deeper than anything reported for `name`.
static bool stackWentDeeper(const char* name, uint32_t free_now) {
    for (size_t i = 0; i < s_seen_count; ++i) {
        if (strncmp(s_seen[i].name, name, sizeof(s_seen[i].name) - 1) == 0) {
            if (free_now < s_seen[i].free_min) {
                s_seen[i].free_min = free_now;
                return true;
            }
            return false;
        }
    }
    if (s_seen_count < (sizeof(s_seen) / sizeof(s_seen[0]))) {
        StackSeen& e = s_seen[s_seen_count++];
        snprintf(e.name, sizeof(e.name), "%s", name);
        e.free_min = free_now;
    }
    return true;   // first sighting always reports
}

static void dumpTaskStacks() {
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    const size_t bytes = size_t(count) * sizeof(TaskStatus_t);
    // PSRAM by preference — a diagnostic must not take internal RAM away from
    // the thing it is measuring. The fallback keeps this working on a board
    // with no PSRAM at all.
    TaskStatus_t* snap = static_cast<TaskStatus_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (snap == nullptr) {
        snap = static_cast<TaskStatus_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    if (snap == nullptr) {
        SLOGW("sys", "stack census skipped — no room for %u tasks", unsigned(count));
        return;
    }
    const UBaseType_t got = uxTaskGetSystemState(snap, count, nullptr);
    for (UBaseType_t i = 0; i < got; ++i) {
        const uint32_t free_bytes = unsigned(snap[i].usStackHighWaterMark);
        if (!stackWentDeeper(snap[i].pcTaskName, free_bytes)) continue;
        SLOGI("sys", "stack %-10s core=%d free=%u B",
              snap[i].pcTaskName,
              (snap[i].xCoreID == tskNO_AFFINITY) ? -1 : int(snap[i].xCoreID),
              unsigned(free_bytes));
    }
    heap_caps_free(snap);
}

// ---- FreeRTOS Tasks ---------------------------------------------------------

// ---- Motion-link return path (Core 1) --------------------------------------
// Pulled events become the anomaly feed and the plan strip; status becomes
// the interp_* telemetry. Engine anomaly kinds ride below kEvtLinkBase and
// index kSmAnomalyNames by ordinal; link kinds get their own names.
// See docs/rp-motion-port.md, "Telemetry after the port".
static const char* const kLinkEventNames[] = {
    "cmd_gated", "cfg_tag_unknown", "clock_step", "plan_adopted",
};
static constexpr uint8_t kLinkEventNameCount =
    uint8_t(sizeof(kLinkEventNames) / sizeof(kLinkEventNames[0]));

static void drainMotionLink() {
    const motionlink::StatusV2& st = motor.status();
    const Window win = mapper.effectiveWindow();
    const float span_mm = win.max_mm - win.min_mm;
    const float span_counts = span_mm * AIM_STEPS_PER_MM;

    // ---- events ------------------------------------------------------------
    motionlink::EventRecord ev;
    char nmbuf[8];
    while (motor.popEvent(ev)) {
        if (ev.kind == motionlink::kEvtPlanAdopted) {
            const MlinkServoDriver::PlanEvent p = motor.lastPlan();
            PlanTrace tr{};
            tr.due_us      = p.due_master_us;
            tr.late_us     = p.late_us;
            tr.target      = ev.target;
            tr.duration_us = p.duration_us;
            tr.cmd_seq     = ev.cmd_seq;
            tr.kind_after  = st.plan_kind;
            tr.mode_after  = st.mode;
            if (g_plan_trace_queue && xQueueSend(g_plan_trace_queue, &tr, 0) != pdTRUE)
                ++g_plan_trace_drops;
            // The strip's start is the position the plan was adopted FROM.
            // Sampled here rather than at t_us: the pull is within one poll of
            // the adoption, and the alternative is a second position history.
            if (span_mm > 0.01f) {
                const float pos_mm = motor.getPosition();
                g_state.interp_start_pos = (pos_mm - win.min_mm) / span_mm;
            }
            g_state.interp_end_pos     = ev.target;
            g_state.interp_duration_us = p.duration_us;
            continue;
        }

        // COUNT FIRST, UNCONDITIONALLY: the log line below is throttled, so
        // the counters are the only lossless record.
        g_state.sm_anomalies = g_state.sm_anomalies + 1;
        if (ev.kind < SystemState::SM_ANOM_KINDS)
            g_state.sm_anom_kind[ev.kind] = g_state.sm_anom_kind[ev.kind] + 1;
        // Hand the EDGE to Core 0 for the 0x0089 EVENT channel: the SlopSync
        // hub is single-task by invariant and that task is on Core 0, so an
        // anomaly crosses as data and the hub turns it into a frame.
        {
            SystemState::SmAnomalyRec rec;
            rec.t_us   = ev.t_us;
            rec.seq    = ev.seq;
            rec.kind   = ev.kind;
            rec.target = ev.target;
            rec.detail = ev.detail;
            g_state.smAnomalyPush(rec);
        }
        // Bound from each table's own size, never a literal. An unknown kind
        // prints its ORDINAL so a stale table names the number to add.
        const char* nm;
        if (ev.kind >= motionlink::kEvtLinkBase) {
            const uint8_t li = uint8_t(ev.kind - motionlink::kEvtLinkBase);
            if (li < kLinkEventNameCount) nm = kLinkEventNames[li];
            else { snprintf(nmbuf, sizeof(nmbuf), "?L%u", unsigned(li)); nm = nmbuf; }
        } else if (ev.kind < kSmAnomalyNameCount) {
            nm = kSmAnomalyNames[ev.kind];
        } else {
            snprintf(nmbuf, sizeof(nmbuf), "?%u", (unsigned)ev.kind);
            nm = nmbuf;
        }
        // Log the KIND CHANGING, not every event: a stream that outruns the
        // engine emits one kind continuously, and a flat throttle turns that
        // into a permanent drip that says nothing new after the first line.
        static uint8_t last_kind = 0xFF;
        static uint32_t last_kind_ms = 0;
        const uint32_t anom_now = millis();
        if (ev.kind != last_kind || (anom_now - last_kind_ms) >= 10000u) {
            last_kind = ev.kind;
            last_kind_ms = anom_now;
            SLOGI("motion", "rp %s target=%.3f detail=%.3f (total %lu)",
                  nm, (double)ev.target, (double)ev.detail,
                  (unsigned long)g_state.sm_anomalies);
        }
    }

    // ---- status -> plan strip ---------------------------------------------
    if (span_mm > 0.01f) {
        const float pos_mm = motor.getPosition();
        g_state.interp_cur_pos = (pos_mm - win.min_mm) / span_mm;
        // Native counts are NEGATED vs mm, so the reported velocity flips
        // sign on the way into the normalized frame.
        g_state.interp_cur_vel =
            span_counts > 1.0f ? -st.vel / span_counts : 0.0f;
    }
    const MlinkServoDriver::PlanEvent p = motor.lastPlan();
    g_state.interp_elapsed_us = p.valid ? uint32_t(micros() - p.due_master_us) : 0;
    g_state.interp_active   = st.state == motionlink::kStateRunning;
    g_state.interp_style    = st.mode;
    g_state.interp_live_mode = st.mode == uint8_t(slopmotion::Mode::Chase);
    g_state.interp_grad_mode =
        st.plan_kind == uint8_t(slopmotion::PlanKind::Quintic) ||
        st.plan_kind == uint8_t(slopmotion::PlanKind::Cubic);
    g_state.sm_mode      = st.mode;
    g_state.sm_plan_kind = st.plan_kind;
}

// Core 1 — real-time: homing, deferred-intent consumer, motion-link service
static void motorTask(void* /*param*/) {
    bool homing_started = false;
    // Deferred intents wake this task instead of waiting out the 1 ms poll:
    // the poll still runs the homing, link and glide duties below.
    arbiter.setConsumerTask(xTaskGetCurrentTaskHandle());
    while (true) {
        // E-stop
        if (g_state.estop_requested.exchange(false)) {
            arbiter.emergencyStop();
            homing_started = false;
            g_state.homing_in_progress = false;
            g_state.homed = false;
            SLOGW("sys", "E-Stop handled — shaft is soft, waiting for orders~ :3");
        }
        // Homing
        else if (g_state.homing_in_progress) {
            if (!motor.isHoming() && !homing_started) {
                arbiter.armHoming(true);   // free window on the wire first
                motor.home();
                arbiter.armHoming(false);
                homing_started = true;
            }
            if (!motor.isHoming() && homing_started) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                homing_started = false;
                if (g_state.homed) {
                    g_state.resume_start_ms = millis();
                    // Item 3 (fw 2.1.76): flag the fresh measurement for a
                    // Core-0 NVS persist (SlopSyncHubService's 1 Hz tick) and
                    // wake the 0x0081 on-change publisher so `measured_stroke`
                    // reaches clients promptly instead of waiting for some
                    // unrelated config edit to bump cfg_gen next.
                    g_state.stroke_measured_pending = true;
                    g_state.cfg_gen.fetch_add(1, std::memory_order_relaxed);
                    SLOGI("sys", "System is now homed and ready to pound :3");
                } else {
                    SLOGW("sys", "Homing failed — no current-spike stall found in the search sweep. Check motor wiring/power.");
                }
            }
        } else {
            homing_started = false;
            // The driver drops homed on its own (an RP restart re-zeroes the
            // count); the system flag follows it, or policy keeps a window in
            // a frame that no longer exists.
            if (g_state.homed && !motor.isHomed()) {
                g_state.homed = false;
                SLOGW("sys", "homed dropped by the motion processor -- re-home");
            }
            if (!g_state.homed) {
                if (motor.checkPushToHome()) {
                    g_state.homed = true;
                    SLOGI("sys", "System homed via push-to-home and ready :3");
                }
            }
        }
        // The SPI link has exactly one owner. While an RP2350 image is being
        // written the OTA path drives the bus from commsTask, and motion is
        // stopped by the same gate, so nothing on this task may touch the wire
        // (sd-4k1.3). That covers the arbiter's config push and the event pull
        // below too: both reach the bus through this task, which IS the link's
        // owner, so they would ship frames into a second master.
        const bool rp_flashing = otaService.rpFlashActive() ||
                                 MlinkServoDriver::standoffRequested();
        // Acknowledge here, BEFORE any bus use in this pass: the flash path
        // waits for this before its first frame (RpFlashLink::ensureBus).
        MlinkServoDriver::ackStandoff(rp_flashing);
        if (!rp_flashing) motor.update();
        // Window glide (sd-ey0): runtime window edits slew at the USER
        // (gentle) limit instead of re-mapping every target in one sample.
        // Goal writes race in from Core 0 (applySettings); a one-tick torn
        // min/max pair only mis-aims the glide for 1 ms and self-corrects.
        {
            static uint32_t s_lastTickMs = millis();
            const uint32_t nowMs = millis();
            const bool gliding = mapper.getMinMm() != mapper.getGoalMinMm() ||
                                 mapper.getMaxMm() != mapper.getGoalMaxMm();
            mapper.tick(float(nowMs - s_lastTickMs) * 1e-3f,
                        fmaxf(20.0f, g_state.config.user_max_speed_mm_s));
            s_lastTickMs = nowMs;
            // Glide completion bumps cfg_gen for the 0x0081 broadcast (the
            // engine's limits push is per-tick and tracks the glide itself).
            if (gliding && mapper.getMinMm() == mapper.getGoalMinMm() &&
                mapper.getMaxMm() == mapper.getGoalMaxMm())
                g_state.cfg_gen.fetch_add(1, std::memory_order_relaxed);
        }
#if defined(SD32_HEADLESS)
        // Headless sole writer of the position atomics: WebUI's 240 Hz sampler
        // (the normal owner, see WebUI::telemetryTimerCb) never starts here,
        // and SlopSync 0x0080 + the stream rising-edge seed read them. One
        // sample, both uses (same rule as the sampler).
        {
            const float actual_mm = motor.getPosition();
            g_state.actual_position_mm.store(actual_mm, std::memory_order_relaxed);
            g_state.measured_position_mm.store(
                motor.hasActualPosition() ? motor.getActualPosition() : actual_mm,
                std::memory_order_relaxed);
            // The odometer (0x1020) mirror: the driver feeds it per RP status.
            if (const SessionOdometer* o = motor.odometer()) {
                g_state.session_distance_mm.store(o->distanceMm(), std::memory_order_relaxed);
                g_state.stroke_count.store(o->strokes(), std::memory_order_relaxed);
                g_state.live_speed_mm_s.store(o->liveMmS(), std::memory_order_relaxed);
                g_state.max_speed_mm_s.store(o->peakMmS(), std::memory_order_relaxed);
            }
        }
#endif
        // Core 0 -> Core 1 deferred intents, then the link's return path.
        if (!rp_flashing) {
            arbiter.processDeferred();
            drainMotionLink();
        }
        // SlopGlow liveness: this pulse is what keeps the status LEDs
        // animating. If this loop dies, the lights freeze — by design.
        if (auto* hb = slopglowMotorHeartbeat()) hb->pulse();
        // Yields exactly like the 1 ms delay it replaces, minus the wait when
        // a Core-0 intent is already queued (MotionArbiter::setConsumerTask).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}

// Core 0 — services all active transports, reports inbound rate
// Permanent Core-0 stall watchdog shared by commsTask + httpTask. Logs WHICH
// Core-0 call blocked, and for how long, once it returns. The heartbeat lives
// at the tail of httpTask, so any blocked step freezes the breath; this names
// the offender in the web log ([STALL] ...) instead of guesswork. Near-zero
// cost — only two millis() reads per step, logs only when a step exceeds the
// threshold.
#define STALL_LOG_MS 120u

// ---- Core-0 step beacon: a stall you can ACT on, not just read about -------
// TIME_STEP only ever measured a step AFTER it returned, so an in-flight stall
// was invisible while it mattered. httpTask now PUBLISHES the step it is
// entering and clears it on exit; commsTask (2 ms cadence, and never blocked
// by httpTask) watches the beacon and can cancel the offender.
// Single writer (httpTask), single reader (commsTask), two plain atomics — no
// lock, and nothing here may block: the whole point is that the writer is
// already stuck when the reader acts.
// 0 == no step in flight. millis() can legitimately BE 0 for one tick at boot,
// so the publish floors it to 1 rather than lying about being idle.
static std::atomic<uint32_t>     s_stepStartMs{0};
static std::atomic<const char*>  s_stepName{nullptr};

#define TIME_STEP(call, name) do {                                            \
        uint32_t _s0 = millis();                                              \
        s_stepName.store(name, std::memory_order_relaxed);                    \
        s_stepStartMs.store(_s0 ? _s0 : 1u, std::memory_order_release);       \
        call;                                                                 \
        s_stepStartMs.store(0, std::memory_order_release);                    \
        uint32_t _dt = millis() - _s0;                                        \
        if (_dt > STALL_LOG_MS) SLOGW("sys", name " blocked %lums", (unsigned long)_dt); \
    } while (0)

// DISABLED (2026-07-31) — measured, does not work, and was actively harmful.
// Set >0 to re-arm; 0 disables the reap entirely.
//
// WHY IT IS OFF, from the live log rather than from reasoning:
//   [1326] Core-0 step http:ui.update blocked >800ms - canceled client fd=57
//   [1335] http:ui.update blocked 10015ms     <- shutdown() did NOT wake it
//   [1345] ... canceled client fd=57         <- same fd, every 800ms, forever
// Three defects, in order of severity:
//   1. shutdown() does not cancel the block. The premise was that
//      handleClient() parks in a socket read; the 10 015 ms step says
//      otherwise (two chained 5 s WebServer timeouts). The stuck client is
//      not reachable this way.
//   2. A LEGITIMATE full page serve measures ~900 ms of blocking, so an
//      800 ms deadline canceled real browser loads -- GET / failed with
//      "connection closed" every time. It cost responsiveness and bought
//      nothing.
//   3. The latch reset whenever the step ended, so it re-reaped the same fd
//      indefinitely.
// The general lesson, which outlives this code: legitimate-slow (~900 ms) and
// stuck (~10 s) are within 10x on this server, so NO fixed deadline separates
// them. That is why Apache's mod_reqtimeout gates on a byte RATE, and why the
// real fix is an event-driven server that never blocks a serve slot at all.
static constexpr uint32_t kCore0ReapMs = 0;
static void commsTask(void* /*param*/) {
    uint32_t last_report_ms   = 0;
    // Latches so one stall produces one reap, not one every 2 ms until the
    // blocked step notices. Cleared when the beacon goes idle.
    bool     reaped_this_step = false;
    while (true) {
        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            last_report_ms = now;
#if !defined(SD32_HEADLESS)
            wifiLink.pollWifiLink();
            wifiLink.superviseWifi();   // re-scan + re-pin if link dropped
#endif
        }

        // ---- Reap a Core-0 step that is STILL blocked ----------------------
        // THE HUB OUTRANKS ANY CLIENT. A connection that cannot be served must
        // never be allowed to hold the only serve slot until the watchdog
        // reboots the machine — it gets canceled instead. This is the half
        // IdleGuardWebServer::dropIdleCapture() cannot reach: it runs BETWEEN
        // handleClient() calls, and a partial request never returns to it.
        // Fires once per stall (the beacon is republished per step, and the
        // reap only re-arms after the step clears).
        {
            const uint32_t stepAt = s_stepStartMs.load(std::memory_order_acquire);
            if (kCore0ReapMs != 0 && stepAt != 0 && (millis() - stepAt) > kCore0ReapMs) {
                if (!reaped_this_step) {
                    reaped_this_step = true;
                    const char* nm = s_stepName.load(std::memory_order_relaxed);
#if defined(SD32_HEADLESS)
                    const int fd = -1;
#else
                    const int fd = webui.server()->abortBlockedClient();
#endif
                    crashring::crumb("http-reap");
                    SLOGW("sys", "Core-0 step %s blocked >%lums — canceled client fd=%d",
                          nm ? nm : "?", (unsigned long)kCore0ReapMs, fd);
                }
            } else if (stepAt == 0) {
                reaped_this_step = false;
            }
        }

        if (auto* hb = slopglowCommsHeartbeat()) hb->pulse();  // SlopGlow liveness (Core 0)
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Core 0 — HTTP server + status LEDs
// Each step is wrapped in TIME_STEP (the Core-0 stall watchdog, defined above
// commsTask). The heartbeat is at the tail, so a blocked step freezes the
// breath; the [STALL] web-log line names which call blocked.
static void httpTask(void* param) {
    WebUI* ui = static_cast<WebUI*>(param);
    while (true) {
        TIME_STEP(ui->update(),           "http:ui.update");
        // M5c: the :81 telemetry WebSocket is gone — see transport.md T8 (never
        // stream to a wedged WebSocket client under a shared lock). The
        // replacement never blocks: ESP32Async's write() queues or refuses
        // rather than waiting, so a stuck client can never delay the
        // heartbeat, HTTP, or OTA that share this task.
        TIME_STEP(otaService.handle(),    "http:ota.handle");
#if defined(SD32_MODBUS_TOOLS)
        // Sole owner of the bus: ServoModbus is not thread-safe to poll from
        // two tasks, and configuration plus the encoder audit have no realtime
        // deadline, so Core 0 carries them.
        {
            TIME_STEP(servoModbus.update(),   "http:servoModbus");
            // While reg 0x00 reads 1 the drive IGNORES its pulse input, so the
            // RP2350 clocks quadrature into a deaf drive and the machine
            // silently does not move. No Modbus write clears 0x00 on this
            // drive (sd-opb): reporting it is the whole fix, the operator
            // power-cycles the drive.
            const ServoTelemetry st = servoModbus.getTelemetry();
            if (st.valid && st.enabled) {
                SLOGW_EVERY_MS(30000, "servobus",
                               "drive reg 0x00 = 1: step/dir input IGNORED, motion will not "
                               "move. POWER-CYCLE THE DRIVE, Modbus cannot clear this.");
            }
        }
#endif
#if defined(SD32_MODBUS_TOOLS) && defined(DRIVER_AIM_SERVO)
        TIME_STEP(encoderValidator.update(), "http:encValidator");
#endif
        TIME_STEP(applogDrain(),          "http:logDrain");   // SlopLog ring -> web/serial sinks
        // Planner trace consumer: format Core 1's POD records here, bounded per
        // tick so a burst cannot own httpTask. Kind/mode names index the enums
        // in slopmotion.hpp (append-only there).
        if (g_plan_trace_queue) {
            static const char* const kKind[] = {"none", "quintic", "ruckig", "cubic"};
            static const char* const kMode[] = {"idle", "waveform", "chase", "settle"};
            PlanTrace tr;
            for (int n = 0; n < 8 && xQueueReceive(g_plan_trace_queue, &tr, 0) == pdTRUE; ++n) {
                SLOGI("plan", "due=%lu late=%ld tgt=%.4f dur=%ums seq=%u -> %s/%s%s",
                      (unsigned long)tr.due_us, (long)tr.late_us, (double)tr.target,
                      unsigned(tr.duration_us / 1000u), unsigned(tr.cmd_seq),
                      kKind[tr.kind_after < 4 ? tr.kind_after : 0],
                      kMode[tr.mode_after < 4 ? tr.mode_after : 0],
                      tr.cmd_seq ? "" : " (slave-initiated)");
            }
            static uint32_t s_dropsSeen = 0;
            if (g_plan_trace_drops != s_dropsSeen) {
                s_dropsSeen = g_plan_trace_drops;
                SLOGW("plan", "trace queue dropped %lu records", (unsigned long)s_dropsSeen);
            }
        }
#if defined(SD32_MODBUS_TOOLS)
        // Plain bool read, safe whichever core owns the bus. The LED renders
        // false as Motion/Degraded: an unpowered drive blinks, unhomed sits.
        g_state.servo_bus_ready = servoModbus.isReady();
#endif
        slopglowUpdate(g_state);
        // Heap health beacon: free / low-water / largest-block. maxblock is
        // the one that kills big allocations (LittleFS streams, WS buffers)
        // long before free hits zero — fragmentation shows up there first.
        SLOGI_EVERY_MS(10000, "sys", "heap free=%u min=%u maxblock=%u psram=%u",
                       unsigned(ESP.getFreeHeap()), unsigned(ESP.getMinFreeHeap()),
                       unsigned(ESP.getMaxAllocHeap()), unsigned(ESP.getFreePsram()));
        // One-shot memory census at 30 s. Stack high-water is meaningless until
        // the tasks have run, and putting the boot-heap table and the stack
        // table in ONE known moment is what makes both fetchable from a 44-line
        // ring — scattered across boot they recycle before anyone reads them.
        if (!s_census_done && millis() > 30000) {
            s_census_done = true;
            bootheap::report();
        }
        // Re-scanned every 30 s forever, but dumpTaskStacks() prints ONLY tasks
        // that have gone deeper than last reported — so a bench session that
        // exercises motion names the stacks it actually grew, and a quiet
        // machine stays silent. This is what makes a stack trim defensible.
        if (s_census_done && (millis() - s_census_last_ms) >= 30000) {
            s_census_last_ms = millis();
            dumpTaskStacks();
        }
        // Crash-ring watermark: three u32 stores per tick — cheap enough for
        // 100 Hz, and the ring's post-mortem value depends on it being fresh.
        crashring::heapSample(ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        // Drain the failed-alloc hook HERE, on a task, because the hook itself
        // may have fired in an ISR and must not log from there. One line per
        // distinct failure; a storm collapses to its newest record plus a count.
        oomhook::Record oom;
        if (oomhook::take(oom)) {
            SLOGE("heap", "ALLOC FAILED #%u: %u B caps=0x%04X task=%s fn=%s "
                          "(free_int=%u largest_int=%u)",
                  unsigned(oom.count), unsigned(oom.size), unsigned(oom.caps),
                  oom.task[0] ? oom.task : "isr/unknown",
                  oom.fn[0] ? oom.fn : "?",
                  unsigned(oom.free_int), unsigned(oom.largest_int));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Decodes esp_reset_reason()'s enum to its name for the boot log — the only
// diagnostic a spontaneous, unlogged-cause reboot leaves behind.
static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_UNKNOWN:    return "UNKNOWN";
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default:                 return "UNRECOGNIZED";
    }
}

// ---- setup() — ordered wiring only ------------------------------------------

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin(&g_state);
    SLOGI("boot", "=== SlopDrive-32 v2.0 — D4 event-driven ===");
    // AppLog.cpp's WebRingSink partitions /api/log into a 44-line Trace/Debug/
    // Info ring that the 10 s heap beacon alone recycles in well under a
    // minute, and a 16-line Warn+ ring that ordinary churn never touches.
    // POWERON/SW are the two EXPECTED reasons (cold boot, esp_restart() after
    // an OTA); anything else is exactly the "why did it reboot" question this
    // line exists to answer, so it goes in the ring that survives long enough
    // for someone to actually read it.
    {
        const esp_reset_reason_t reason = esp_reset_reason();
        const char* name = resetReasonName(reason);
        const bool expected = (reason == ESP_RST_POWERON || reason == ESP_RST_SW);
        if (expected) {
            SLOGI("boot", "Reset reason: %s", name);
        } else {
            SLOGW("boot", "Reset reason: %s (unexpected)", name);
        }
        // Recover the previous boot's last words (RTC-noinit crash ring) and
        // re-arm it for this boot. Must run after applogBegin() so the
        // recovered report lands in a readable ring, and before anything that
        // drops crumbs.
        crashring::begin(name, !expected);
    // Immediately after the ring: the hook's only durable side effect is a
    // crumb, so the ring must already be armed to receive it.
    oomhook::begin();
    }
#if SERIAL_CONTROL_MODE
    SLOGI("boot", "USB Serial is boot-log + rescue path only — SlopSync (WiFi) is the control plane.");
#endif

#if defined(DRIVER_AIM_SERVO)
    // No backend to select: one motion backend, bound at construction.
    // TODO(sd-pln): the web flasher is where one-time drive programming goes.
    pinMode(AIM_PIN_STEP, OUTPUT); digitalWrite(AIM_PIN_STEP, LOW);
    pinMode(AIM_PIN_DIR,  OUTPUT); digitalWrite(AIM_PIN_DIR,  LOW);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    // Runtime steps/mm — load the persisted motor steps/rev (drive reg 0x0B
    // mirror) BEFORE any motion math runs. Reprogramming steps/rev from the
    // Configure pane updates this live (+ forces a re-home) — no reboot.
    aimGeometryInit();
#endif

    bootheap::mark("entry");

    if (LittleFS.begin(true))
        SLOGI("boot", "LittleFS mounted");
    else
        SLOGE("boot", "LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");
    bootheap::mark("littlefs");

    ConfigStore::load(g_state, mapper, motor);

    motor.init();
    motor.applyDriverConfig(g_state.driver);
    bootheap::mark("config+motor");

    slopglowInit();
    // Motor is bound and configured above; the boot rainbow drops this bit
    // now and waits on the hub for Link + Session.
    slopglowEngine().markReady(slopglow::System::Motion);
    bootheap::mark("slopglow");

#if defined(SD32_HEADLESS)
    // sd-4v9: no WiFi init, no HTTP OTA, no web server. Every duty they held
    // rides the C5 bridge (OTA, diag, token mint) or SlopSync. esp_wifi_init
    // never runs, so its runtime buffers never allocate -- that reclaim is
    // the point, and the boot heap beacon below is the measurement.
    // fw version in the boot log is the C-8 verification stamp now that
    // /api/capabilities is gone -- read it via the C5's /api/diag/boot.
    SLOGI("boot", "HEADLESS build fw %s: WiFi/HTTP/ArduinoOTA not started (sd-4v9)",
          FIRMWARE_VERSION);
    bootheap::mark("wifi");
    webui.init();
    bootheap::mark("webui");
#else
    bool wifi_ok = wifiLink.setupWiFi();
    bootheap::mark("wifi");

    webui.init();
    bootheap::mark("webui");


    // OTA — only meaningful when WiFi actually came up. ArduinoOTA needs the
    // network stack; the HTTP endpoints ride the WebServer webui.init() just
    // started. Register both here so a network flash works the moment we boot.
    if (wifi_ok) {
        otaService.begin(MDNSServiceName, SECRET_OTA_PASSWORD);
        otaService.registerHttpRoutes(webui.server());
        SLOGI("boot", "OTA ready — hostname '%s', fw %s", MDNSServiceName, FIRMWARE_VERSION);
#if defined(SLOPSYNC_HEAP_BISECT)
        // DIAGNOSTIC (LEDGER THE QUEUE #0). Read by host polling during the
        // corruption reproduction; the last good poll before the device dies is
        // the evidence, since a panic inside the allocator runs none of our
        // hooks and the PSRAM record buffer does not survive the reboot.
        // 64 records, not 256: the buffer must be INTERNAL RAM (see
        // HeapTrace.cpp) and that is the scarce resource. 64 x ~88 B is ~5.6 KB,
        // affordable against ~41 KB free, and the host polls continuously so
        // depth matters less than the buffer working at all.
        heaptrace::begin(64);
        // HTTPMethod:: qualified on purpose: this TU sees BOTH the sync
        // WebServer's HTTPMethod and ESPAsyncWebServer's request-method enum,
        // so a bare HTTP_GET is ambiguous here (it is not in OtaService.cpp,
        // which only sees the sync one).
        webui.server()->on("/api/heaptrace", HTTPMethod::HTTP_GET, []() {
            // Heap-allocated on purpose: this is a diagnostic route on
            // httpTask's 8 KB stack, and the dump is far too big for it.
            const size_t cap = 12288;
            char* buf = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (buf == nullptr) {
                webui.server()->send(503, "application/json", "{\"error\":\"no psram for dump\"}");
                return;
            }
            heaptrace::dumpJson(buf, cap, 64);
            webui.server()->send(200, "application/json", buf);
            heap_caps_free(buf);
        });
        // Watchpoint readout + selftest. GET reports what is armed; ?selftest=1
        // deliberately CRASHES the device (that is the pass condition — see
        // HeapWatch.h constraint 4). httpTask is Core 0, which is the core every
        // suspect in this hunt runs on, so the selftest proves the watchpoint on
        // the core that matters.
        webui.server()->on("/api/heapwatch", HTTPMethod::HTTP_GET, []() {
            if (webui.server()->arg("selftest") == "1") {
                webui.server()->send(200, "application/json",
                                     "{\"selftest\":\"firing\",\"expect\":\"Watchpoint 0 triggered\"}");
                delay(150);   // let the response actually leave before we panic
                heapwatch::selftest();
                return;
            }
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "{\"armed_at\":\"0x%08x\",\"armed_size\":%u,\"arms\":%u,"
                     "\"quarantined\":%u,\"evicted\":%u,"
                     "\"selftest_failed\":%s,\"core\":%d}",
                     unsigned(uintptr_t(heapwatch::armedAt())), unsigned(heapwatch::armedSize()),
                     unsigned(heapwatch::arms()), unsigned(heapwatch::quarantined()),
                     unsigned(heapwatch::quarantineEvicted()),
                     heapwatch::selftestFailed() ? "true" : "false",
                     int(xPortGetCoreID()));
            webui.server()->send(200, "application/json", buf);
        });
        SLOGW("boot", "DIAGNOSTIC BUILD: /api/heaptrace + /api/heapwatch live "
                      "(watchpoint 0, comprehensive poisoning)");
#endif
    } else {
        SLOGW("boot", "OTA skipped — WiFi down at boot (serial rescue path only)");
    }
#endif  // SD32_HEADLESS
    bootheap::mark("ota");

#if defined(SD32_MODBUS_TOOLS)
    Serial1.begin(19200, SERIAL_8N1, AIM_PIN_485_RX, AIM_PIN_485_TX);
    servoModbus.init();
    webui.setServoModbus(servoModbus);
#if defined(DRIVER_AIM_SERVO)
    webui.setEncoderValidator(encoderValidator);

    // ---- Drive baud: 115200 in BOTH backends --------------------------------
    // Operator ruling: the drive stays at 115200 and FAS-mode telemetry runs at
    // the same speed, so the encoder readback and the Configure pane behave
    // identically whichever backend booted. Runs BEFORE the reg-0x0B e-gear
    // adoption below so that read lands at the FINAL baud. reprogramBaud() is
    // a no-op when the drive already answered at 115200.
    if (servoModbus.isReady() && servoModbus.baud() == 19200) {
        if (servoModbus.reprogramBaud(115200)) {
            SLOGI("boot", "drive reprogrammed 19200 -> 115200 (OSSM-RS magic sequence) :3");
        } else {
            SLOGW("boot", "19200 -> 115200 reprogram FAILED, staying at 19200 (everything "
                  "still works, just a tighter bus budget).");
        }
    }

    // NEVER write reg 0x00 on the step/dir backend, not even 0. Measured live
    // 2026-08-07: the boot-time releaseMotionArm() wrote 0, the drive latched
    // 1, and the machine booted deaf to step/dir every time (sd-opb one-way
    // door -- a bare write arms it). The exit is 506-then-0, now inside
    // releaseMotionArm(); the httpTask poll warns while 0x00 reads 1. Nothing
    // arms the door on this backend, so nothing here needs to release it.

    // Ramp-register reconcile. Read first, write only on a mismatch, so an
    // unchanged setting never arms the drive. See docs/drive-accel-register.md.
    if (servoModbus.isReady()) {
        const uint16_t want = machineAccelRegLoad();
        g_state.servo_accel_reg_ovr = want;
        switch (servoModbus.reconcileAccelReg(want)) {
            case ServoModbus::AccelCommit::AlreadyMatches:
                break;
            case ServoModbus::AccelCommit::SavedUnarmed:
                SLOGI("boot", "drive ramp 0x03 -> %u, saved unarmed. step/dir still live :3",
                      (unsigned)want);
                break;
            case ServoModbus::AccelCommit::SavedArmed:
                SLOGE("boot", "drive ramp 0x03 -> %u REQUIRED ARMING. The drive now IGNORES "
                      "step/dir and ONLY a drive power cycle fixes it. POWER-CYCLE THE 36V "
                      "RAIL. uhoh :c", (unsigned)want);
                break;
            case ServoModbus::AccelCommit::Failed:
                SLOGW("boot", "drive ramp 0x03 -> %u FAILED, drive keeps its own value.",
                      (unsigned)want);
                break;
        }
        // Populate the register mirror once at boot. Without it the 0x03
        // readback reads 0 until the first write, and a readout showing 0 for
        // "not measured yet" is indistinguishable from a real 0.
        servoModbus.requestConfigScan();
    }

    // Ground Truth for geometry: the drive's own e-gear register (0x0B, saved
    // in ITS EEPROM by the programmer) is the authority on steps/rev. Adopt it
    // at boot whenever the drive answers — this heals the NVS-mirror-lost case
    // where the firmware would otherwise boot at the 800 default while the
    // drive physically needs 1600 pulses/rev, silently halving every commanded
    // millimeter until the mismatch is noticed. Machine is unhomed at this
    // point, so the forced re-home semantics of a steps/rev change are free.
    if (servoModbus.isReady()) {
        uint16_t drive_spr = 0;
        if (servoModbus.readRegisterBlocking(0x0B, drive_spr) &&
            drive_spr >= 50 && drive_spr <= 32767) {
            if (drive_spr != aimMotorStepsPerRev()) {
                SLOGI("boot", "Boot geometry: drive reg 0x0B says %u steps/rev, NVS mirror had %u — adopting the drive's value",
                      (unsigned)drive_spr, (unsigned)aimMotorStepsPerRev());
                aimSetMotorStepsPerRev(drive_spr, /*persist=*/true);
            }
        } else {
            SLOGW("boot", "Boot geometry: could not read drive reg 0x0B — keeping NVS/default steps/rev");
        }
    }
#endif
#endif

    // D4: init the arbiter — sole caller of motor for positioning
    arbiter.init();
    // Seed the dual limit sets from the persisted fields (migrated from legacy
    // on first boot after upgrade — ConfigStore fills both from the old
    // max_speed/accel if the new keys haven't been written yet).
    arbiter.setUserSpeedLimit(g_state.config.user_max_speed_mm_s);
    arbiter.setUserAccelLimit(g_state.config.user_max_accel_mm_s2);
    arbiter.setInputSpeedLimit(g_state.config.input_max_speed_mm_s);
    // The offboard renderer gets the SAME ceiling the arbiter clamps to. Kept
    // adjacent on purpose: two ceilings that can disagree is how the RP ended
    // up commanding >1900 mm/s against a 900 mm/s setting. Max of BOTH sets:
    // manual point moves run at USER limits, and a ceiling below any legal
    // plan pins the emitter slew cap into steady-state lag.
    motor.setRenderCeiling(fmaxf(g_state.config.user_max_speed_mm_s,
                                 g_state.config.input_max_speed_mm_s));
    arbiter.setInputAccelLimit(g_state.config.input_max_accel_mm_s2);

    // Wire PatternEngine to the arbiter so it submits intents instead of
    // calling motor directly.
    patternEngine.setArbiter(&arbiter);

    // Wire WebUI to the arbiter so the UI's applySettings can dispatch
    // live limit-set updates directly through the sole caller.
    webui.setArbiter(&arbiter);

    g_plan_trace_queue = xQueueCreate(kPlanTraceDepth, sizeof(PlanTrace));
    configASSERT(g_plan_trace_queue != nullptr);

    // Create FreeRTOS tasks. Every creation is checked — a boot-critical task
    // that fails to spin up under heap pressure (motorTask IS the homing +
    // e-stop servicer) must halt loudly, not boot a device that silently can't
    // home, e-stop, or move. Same configASSERT discipline as the queue
    // creations above.
    // End of the single-task boot phase: from here logs are ring-buffered and
    // drained by httpTask (immediate synchronous drain is only safe pre-tasks).
    sloplog::logger().setImmediateDrain(false);

    BaseType_t task_ok;
    // motorTask: Core 1, priority 3 — homing + D4 deferred-intent consumer
    task_ok = xTaskCreatePinnedToCore(motorTask, "Motor", 4096, nullptr, 3, nullptr, 1);
    configASSERT(task_ok == pdPASS);
    // 4096: measured 1,784 B peak in the fw 2.1.90 stack census (2.3x headroom).
    // Safe to size from that census specifically because this task's whole job —
    // WiFi supervision, scan, reconnect — HAD run by then. Sampler and Motor are
    // deliberately NOT trimmed on the same data: their deep paths are motion and
    // homing, which an idle bench boot never exercises.
    task_ok = xTaskCreatePinnedToCore(commsTask, "Comms", 4096, nullptr, 2, nullptr, 0);
    configASSERT(task_ok == pdPASS);
    task_ok = xTaskCreatePinnedToCore(httpTask, "HTTP", 8192, &webui, 1, nullptr, 0);
    configASSERT(task_ok == pdPASS);
    patternEngine.init();    // creates its own Core 1 task
    // Covers every task stack above plus patternEngine's own: 38,912 B of
    // declared stack for our five, and the stack census names the slack.
    bootheap::mark("tasks");

    // SlopSync hub last: WiFi is up, arbiter/webui/patternEngine are wired.
    // Placement-new into PSRAM (see the declaration comment); refuses to
    // start rather than eat internal RAM if PSRAM is somehow absent.
    {
        void* mem = heap_caps_malloc(sizeof(slopdrive::SlopSyncHubService),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem != nullptr) {
            // `motor` by reference: the catalog's 0x0087 power channel is
            // FEATURE-GATED on the driver's own hasCurrentSensor()/
            // hasPowerMonitor(), and that is read inside the constructor
            // before the hub computes the catalog etag. Safe here — this runs
            // long after motor.bind() picked the backend.
            slopSyncHub = new (mem) slopdrive::SlopSyncHubService(g_state, webui, arbiter, motor);
            // Separate from init(): the catalog is built HERE, and its cost has
            // to be distinguishable from the transports' (see LEDGER's 110 KB).
            bootheap::mark("ss:ctor");
            slopSyncHub->setPatternEngine(&patternEngine);
#if defined(UART_LINK_ENABLED)
            // OTA over the bridge control channel, NOT over SlopSync (RFC-057).
            slopSyncHub->setOtaSink(&g_serialOtaSink);
            // Diag archive pull, same channel, same reasoning (sd-0gy).
            slopSyncHub->setDiagSource(&g_serialDiagSource);
#endif
            slopSyncHub->init();
#if !defined(SD32_HEADLESS)
            // RFC-029 §4: GET /uitoken on the SHARED WebServer — HTTP escapee #2,
            // and it has to be HTTP because its whole security property is the
            // browser's same-origin policy, which cannot exist in-band. The
            // headless build mints over the bridge instead (sd-ykg.2).
            slopSyncHub->attachHttpRoutes(webui.server());
#endif
            SLOGI("slopsync", "hub service in PSRAM (%u B)",
                  unsigned(sizeof(slopdrive::SlopSyncHubService)));
        } else {
            SLOGE("slopsync", "no PSRAM block for hub service — SlopSync DISABLED this boot");
        }
    }
    bootheap::mark("slopsync");
    SLOGI("sys", "post-slopsync heap free=%u maxblock=%u psram free=%u",
          unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()),
          unsigned(ESP.getFreePsram()));
    bootheap::report();

#if HOMING_DISABLED
    g_state.homed = true;
    motor.forceHomeState(true);
    SLOGW("boot", "!!! HOMING DISABLED — bench-test build only. Remove -DHOMING_DISABLED for real hardware.");
#endif

    SLOGI("boot", "System ready — push that thick shaft all the way in to home, or use the web UI :3");
}


// ---- loop() — idle ----------------------------------------------------------

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}