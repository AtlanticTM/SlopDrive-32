// SlopDrive-32 — main.cpp
//
// Thin composition root. All logic lives in the modules under
// system/, motion/, comms/, and ui/. This file only:
//   1. Declares module instances.
//   2. Wires them together in setup().
//   3. Creates FreeRTOS tasks with correct core pinning.
//   4. Idles in loop().
//
// Core assignment (.clinerules §2):
//   Core 1 (real-time):  motorTask, Generator task
//   Core 0 (system):     commsTask, httpTask

#include <Arduino.h>
#include <LittleFS.h>

#include "config_api.h"
#include "AppLog.h"
#include "SystemState.h"
#include "ConfigStore.h"

#include "range_mapper.h"
#include "Kinematics.h"
#include "PositionTime.h"
#include "freertos/queue.h"
#include <chrono>

#if defined(DRIVER_TMC2160)
#include "TMC2160StepperDriver.h"
#endif

#include "Generator.h"

#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "TransportManager.h"

#include "WebUI.h"


// ============================================================================
// Module instances — one big orgy of objects :3
// Driver selected at compile time via build flag.
// ============================================================================

#if defined(DRIVER_TMC2160)
  TMC2160StepperDriver motor;
#else
  #error "No motor driver selected. Define DRIVER_TMC2160 (or a future driver flag) in platformio.ini build_flags."
#endif

static SystemState        g_state;
static RangeMapper        mapper;
static Generator          generator(g_state, mapper, motor);

static TCodeParser        tcodeParser;
static SerialTransport    serialTransport(tcodeParser);
static WebSocketTransport wsTransport(tcodeParser);
static BleTransport       bleTransport(tcodeParser);
static TransportManager   transportMgr(g_state, tcodeParser,
                                        serialTransport, wsTransport, bleTransport);

static WebUI webui(g_state, motor, mapper, generator,
                   transportMgr, serialTransport, wsTransport, bleTransport);


// ============================================================================
// Waypoint queue — Core 0 pushes, Core 1 pops. The ONLY cross-core motion
// handoff. Core 0 never touches the stepper object after setup(). :3
//
// 8 slots = 80ms of buffer at 100Hz TCode. Deep enough to absorb a WiFi
// jitter spike; shallow enough that a stale burst doesn't play out after
// the stream stops. If the queue fills, the newest waypoint is dropped —
// a 10ms gap at 100Hz, completely imperceptible. :3
// ============================================================================
static constexpr size_t WAYPOINT_QUEUE_DEPTH = 8;
static QueueHandle_t    g_waypoint_queue     = nullptr;


// ============================================================================
// Glue callbacks — the good boys sitting between TCode and the motor :3
// ============================================================================

// Called by TCodeParser on every valid L0 linear command.
// Core 0 ONLY: measure cadence, build PositionTime, push to queue.
// No motor calls, no kinematics, no FAS touching. Pure data handoff. :3
static void buttplugLinearCmd(float position, uint32_t duration_ms) {
    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) {
        g_state.resume_start_ms = millis();
        return;
    }

    g_state.last_intiface_ms = millis();

    // ---- Cadence measurement (Core 0 only) ----------------------------------
    // Feeds the Hz display and the auto-duration fallback. EMA filter smooths
    // out the occasional WiFi hiccup so the Hz chip doesn't flicker. :3
    uint32_t now = millis();
    if (g_state.last_cmd_ms != 0) {
        uint32_t gap = now - g_state.last_cmd_ms;
        if (gap > 0 && gap < 1000) {
            if (g_state.measured_interval_ms <= 0.0f)
                g_state.measured_interval_ms = (float)gap;
            else
                g_state.measured_interval_ms =
                    0.7f * g_state.measured_interval_ms + 0.3f * (float)gap;
        }
    }
    g_state.last_cmd_ms = now;

    // Auto Duration: substitute measured gap ONLY when the host sent no I-param.
    // If the host sent an explicit interval, honour it — overriding it was the
    // "jitter half a mm" bug. Trust the host. :3
    if (duration_ms == 0 && g_state.auto_duration && g_state.measured_interval_ms > 1.0f)
        duration_ms = (uint32_t)(g_state.measured_interval_ms + 0.5f);

    // Stash raw mapped demand for the UI motion graph (Core 0 only). :3
    g_state.commanded_raw_mm = mapper.intensityToPosition(position);

    // ---- Build waypoint and push to queue -----------------------------------
    // Non-blocking (0 timeout): if the queue is full, drop this waypoint.
    // Core 0 NEVER blocks waiting for Core 1. The motion consumer on Core 1
    // will drain the queue at its own pace. :3
    PositionTime pt;
    pt.position     = (uint8_t)constrain((int)(position * 100.0f), 0, 100);
    pt.inTime       = (uint16_t)constrain((int)duration_ms, 0, 65535);
    pt.has_set_time = true;
    pt.setTime      = std::chrono::steady_clock::now();

    xQueueSend(g_waypoint_queue, &pt, 0);
}

// Called by TCodeParser on DSTOP — pull out cleanly. :3
static void buttplugStop() {
    if (motor.isHomed()) motor.hardStop();
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

// Core 1 — real-time: homing state machine + per-tick motor maintenance.
// This is the dom core — it keeps the shaft disciplined and on time. :3
//
// The handler core (Core 0) sets flags (homing_in_progress, estop_requested)
// to issue orders. We detect those flags here and call ALL motor hardware
// operations from Core 1, where the FastAccelStepper engine lives. No cross-
// core racing on the stepper object — the handler asks, the dom core does. :3
//
// Homing is fully self-contained: motor.home() spawns its own FreeRTOS task
// that runs the sweep, polls the endstop, does the backoff, and sets the homed
// flag when done. motorTask just kicks it off once and watches isHoming() go
// false. No polling — the homing task IS the loop. :3
static void motorTask(void* /*param*/) {
    bool homing_started = false;
    while (true) {
        // ---- E-stop: the red button got slapped. Cut power NOW. :3 ----------
        if (g_state.estop_requested) {
            motor.stop();
            homing_started = false;
            g_state.estop_requested = false;
            APPLOG("E-Stop handled — shaft is soft, waiting for orders~ :3");
        }
        // ---- Homing in progress: spawn the homing task ONCE, then watch
        //      isHoming() go false when the task finishes. :3 -----------------
        else if (g_state.homing_in_progress) {
            if (!motor.isHoming() && !homing_started) {
                motor.home();
                homing_started = true;
            }
            if (!motor.isHoming() && homing_started) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                homing_started = false;
                if (g_state.homed)
                    APPLOG("System is now homed and ready to pound :3");
                else
                    APPLOG("Homing failed — endstop not found. Check wiring.");
            }
        } else {
            // ---- Push-to-home: shove the carriage into the endstop manually. :3
            homing_started = false;
            if (!g_state.homed) {
                if (motor.checkPushToHome()) {
                    g_state.homed = true;
                    APPLOG("System homed via push-to-home and ready :3");
                }
            }
        }
        motor.update();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Core 0 — system: services all active transports and reports inbound rate.
// This is the handler core — takes all the dirty talk (TCode) and passes
// orders to the dom core without breaking a sweat. :3
static void commsTask(void* /*param*/) {
    uint32_t last_report_ms   = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        // In SER mode skip WebSocket networking — the RX FIFO needs every
        // microsecond we can give it. :3
        if (g_state.getTransport() == TransportMode::SER) {
            serialTransport.poll();
        } else {
            wsTransport.run();
        }

        // Once-per-second rate diagnostic — feeds the Hz chip in the UI. :3
        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            uint32_t frames  = tcodeParser.rxFrameCount;
            uint32_t per_sec = frames - last_frame_count;
            last_frame_count = frames;
            last_report_ms   = now;
            g_state.measured_hz = (uint16_t)per_sec;
            if (wsTransport.isConnected() || serialTransport.isActive())
                APPLOGF("[RATE] rx=%u frames/s", per_sec);
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Core 0 — system: HTTP server (delegates entirely to WebUI::update()). :3
static void httpTask(void* param) {
    WebUI* ui = static_cast<WebUI*>(param);
    while (true) {
        ui->update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// motionConsumerTask — Core 1, priority 4
// ============================================================================
//
// The ONLY task that touches the motor driver after setup(). Pops PositionTime
// waypoints from g_waypoint_queue, runs the OSSM kinematic pipeline, and
// dispatches to FAS via motor.streamToSteps(). All FAS calls happen on Core 1
// — same core as the FAS engine and ISR. No cross-core racing. :3
//
// The absolute timeline (best) self-corrects each cycle: it advances by
// inTime regardless of actual wall-clock drift, so the playback rate stays
// locked to the content's intended tempo even if individual waypoints arrive
// a few ms late. OSSM calls this the "self-correcting clock." :3
//
// Direction gating: if a new waypoint reverses direction while the motor is
// still mid-stroke, we put it back at the front of the queue and wait 1ms.
// This prevents commanding a reversal while the motor is mid-acceleration —
// FAS would need a longer brake ramp than the remaining distance, causing
// overshoot. We wait for the stroke to complete, THEN reverse. :3
//
// Raise-only acceleration guard: once moving, acceleration can only increase.
// Lowering it mid-flight means FAS's braking ramp is suddenly longer than the
// remaining distance — overshoot. We take the max of the back-calculated accel
// and the current FAS accel. Stay firm, never go limp early. :3
static void motionConsumerTask(void* /*param*/) {
    // Absolute timeline anchor — advances by inTime each cycle. :3
    auto best = std::chrono::steady_clock::now();

    PositionTime lastPt;
    lastPt.position     = 50;
    lastPt.inTime       = 250;
    lastPt.has_set_time = false;

    // ---- One-deep carry buffer (requeue-free, high-rate safe) ---------------
    // We always keep AT MOST one un-dispatched packet in a local "carry" slot
    // (carryPt). Each loop pops exactly ONE fresh packet from the queue and
    // never pushes anything back — so the queue ordering can't be scrambled and
    // nothing is ever dropped by a failed requeue. This is what broke at 60Hz:
    // the old xQueueSendToFront collided with the producer and silently lost a
    // waypoint every cycle once the 8-slot queue filled. Gone now. :3
    //
    // The rule:
    //   - carryPt holds the packet we're about to dispatch.
    //   - We pop the NEXT packet (incoming). If carryPt had NO time (inTime==0),
    //     we steal its duration from (incoming.setTime - carryPt.setTime) — the
    //     OSSM position-only trick. If carryPt already had a real I-param (the
    //     golden MFP path), we leave it exactly as-is: zero added behavior.
    //   - Dispatch carryPt, then promote incoming -> carryPt for next loop.
    //
    // Net latency: exactly one packet of lookahead (~16ms at 60Hz), the minimum
    // possible to measure a duration. Timed packets are NOT slowed — they get
    // their own I-param honored; the one-packet hold is just pipeline depth. :3
    bool         haveCarry = false;
    PositionTime carryPt    = {};

    while (true) {
        PositionTime incoming;

        // Block up to 50ms waiting for the next waypoint.
        bool gotNew = (xQueueReceive(g_waypoint_queue, &incoming, pdMS_TO_TICKS(50)) == pdTRUE);

        if (!haveCarry) {
            // Prime the pipeline with the first packet, then loop to fetch the
            // one after it so we can measure a duration if needed. :3
            if (gotNew) { carryPt = incoming; haveCarry = true; }
            continue;
        }

        // We have a carried packet to dispatch this cycle.
        PositionTime pt = carryPt;

        if (gotNew) {
            // Only derive timing if the carried packet lacked an explicit one.
            // Timed packets (MFP golden path) keep their own I-param untouched.
            if (pt.inTime == 0 && pt.has_set_time && incoming.has_set_time) {
                int32_t gap_ms = (int32_t)std::chrono::duration_cast<
                    std::chrono::milliseconds>(incoming.setTime - pt.setTime).count();
                if (gap_ms > 0 && gap_ms < 1000) pt.inTime = (uint16_t)gap_ms;
            }
            carryPt = incoming;          // promote for next loop, no requeue
        } else {
            // Stream went quiet (50ms). Flush the carried packet and empty the
            // pipeline so we re-prime cleanly when the stream resumes. :3
            haveCarry = false;
        }

        // Last-resort duration floor for a still-timeless carried packet
        // (e.g. stream stalled right after a position-only command). :3
        if (pt.inTime == 0)
            pt.inTime = (g_state.measured_interval_ms > 1.0f)
                      ? (uint16_t)(g_state.measured_interval_ms + 0.5f)
                      : 100;

        // ---- Direction gating: REMOVED ----------------------------------------
        // We previously held reversal waypoints in the queue and retried every
        // 1ms until the motor stopped. At fast speeds (100Hz, 10ms intervals)
        // the motor needs 3–5ms to decelerate — that's 3–5 retries × 1ms each
        // = 3–5ms of dead time at EVERY reversal. At high speed that eats 30–50%
        // of the interval budget, producing the flat-topped peaks and notched
        // valleys visible in the motion graph. :3
        //
        // The raise-only acceleration guard (getLiveAcceleration() below) already
        // prevents overshoot by ensuring FAS never gets a softer brake ramp than
        // it's currently running. That guard IS the correct protection. The
        // direction gate was belt-and-suspenders that added more latency than it
        // prevented overshoot. FAS handles mid-flight reversals cleanly when the
        // raise-only guard is in place — it re-plans from current velocity with
        // the same or higher acceleration. No gate needed. :3
        //
        // This matches OSSM's actual behavior: OSSM's direction gate only fires
        // when the queue has a reversal AND the motor is still running the
        // previous stroke. In OSSM's single-task architecture that's a rare edge
        // case. In our queued architecture it fires on EVERY reversal because the
        // queue always has the next waypoint ready before the motor finishes the
        // current one. Removing it makes our behavior match OSSM's intent. :3

        float timeSeconds = pt.inTime / 1000.0f;

        // ---- OSSM latency compensation --------------------------------------
        // Uses the RX timestamp to measure actual transport lag and adjusts
        // timeSeconds up to 25% shorter to resynchronize if we're falling behind.
        // bufTarget = 100ms: we aim to stay ~100ms ahead of the playback clock.
        // If we're behind, shorten the current segment to catch up. :3
        if (pt.has_set_time) {
            const int16_t bufTarget = 100;  // ms — target buffer depth
            int16_t currentBuffer = (int16_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - best).count();
            int16_t lag = (int16_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    pt.setTime - best).count();

            // If lag is wildly out of range (stream restart, clock jump), re-anchor.
            if (lag < 0 || lag > bufTarget * 10) {
                best = pt.setTime;
                lag  = 0;
            }
            best += std::chrono::milliseconds(pt.inTime);

            // Offset: positive = we're ahead (slow down), negative = behind (speed up).
            // Cap the speedup at 25% of the segment duration. :3
            int16_t offset = bufTarget - currentBuffer;
            if (offset < 0) {
                int16_t maxSpeedup = -(int16_t)(pt.inTime / 4);
                if (offset < maxSpeedup) offset = maxSpeedup;
            }
            timeSeconds += offset / 1000.0f;
        } else {
            best = std::chrono::steady_clock::now();
        }

        lastPt = pt;

        if (timeSeconds <= 0.01f) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }

        // ---- Map TCode 0–100 to native steps --------------------------------
        // 0 = fully retracted (min_mm), 100 = fully inserted (max_mm).
        // Coordinate system: endstop = 0 steps, front = negative steps.
        // The negative sign converts mm→steps in the correct direction. :3
        float   frac         = pt.position / 100.0f;
        float   target_mm    = mapper.getMinMm() + frac * (mapper.getMaxMm() - mapper.getMinMm());
        int32_t target_steps = -motor.mmToNative(target_mm);

        int32_t min_steps = -motor.mmToNative(mapper.getMaxMm());
        int32_t max_steps = -motor.mmToNative(mapper.getMinMm());

        // ---- OSSM kinematic pipeline ----------------------------------------
        // getCurrentPosition() returns last COMMANDED step count (open-loop).
        // Identical to OSSM's behavior — correct for open-loop servos. :3
        int32_t current_steps = motor.mmToNative(motor.getPosition());
        // getPosition() returns mm (positive), convert to steps with sign:
        current_steps = -(int32_t)(motor.getPosition() * STEPS_PER_MM);

        uint32_t speed_limit = (uint32_t)(g_state.config.max_speed_mm_s    * STEPS_PER_MM);
        uint32_t accel_limit = (uint32_t)(g_state.config.acceleration_mm_s2 * STEPS_PER_MM);

        kinematics::PlanResult plan = kinematics::planTrapezoid(
            current_steps,
            target_steps,
            timeSeconds,
            speed_limit,
            accel_limit,
            min_steps,
            max_steps
        );

        // ---- Raise-only acceleration guard (OSSM-exact) ---------------------
        // Once moving, acceleration can only increase. Lowering it mid-flight
        // means FAS needs a longer brake ramp than the remaining distance —
        // the motor overshoots the peak and lurches. :3
        //
        // CRITICAL: compare against stepper->getAcceleration() — the LIVE FAS
        // value — NOT the config ceiling. This is exactly what OSSM does:
        //   requiredAccel = max(stepper->getAcceleration(), requiredAccel);
        //
        // The bug: we were comparing against g_state.config.acceleration_mm_s2
        // (a constant). At the peak of a fast stroke, planTrapezoid back-
        // calculates a huge requiredAccel (tiny distance, short time), which
        // gets clamped to accel_limit inside the planner. Our guard then
        // compared the clamped value against the same constant — they matched,
        // no raise happened. But FAS was running at whatever accel the PREVIOUS
        // waypoint set, which could be higher. We were silently lowering FAS's
        // active accel at the exact moment it needed maximum braking authority
        // to stop at the peak. That's the grittiness. :3
        uint32_t finalAccel = plan.accel_steps_s2;
        if (motor.isMoving()) {
            uint32_t liveAccel = motor.getLiveAcceleration();
            if (liveAccel > finalAccel) finalAccel = liveAccel;
        }

        // ---- Dispatch to FAS (non-blocking retarget) ------------------------
        // All FAS calls happen on Core 1 — same core as the FAS engine. Safe.
        motor.streamToSteps(plan.target_steps, plan.speed_steps_s, finalAccel);

        // ---- Telemetry (atomic write — no mutex needed on S3) ---------------
        // Core 0's telemetry timer reads this with memory_order_relaxed. :3
        float actual_mm = motor.nativeToMm(-plan.target_steps);
        g_state.actual_position_mm.store(actual_mm, std::memory_order_relaxed);
        g_state.commanded_target_mm = actual_mm;

        // OSSM-exact pacing: yield 1ms after every dispatch. This matches
        // OSSM streaming.cpp line 142: vTaskDelay(1).
        //
        // This is NOT just a scheduler yield — it is a REQUIRED pacing delay.
        // Without it, motionConsumerTask drains the entire queue in a tight loop
        // (all 8 waypoints) before FAS has moved even one step. Every call to
        // motor.getPosition() returns the same stale position because FAS hasn't
        // had CPU time to execute. So current_steps is identical for all 8
        // waypoints, distance ≈ 0 for waypoints 2–8, and the planner generates
        // near-zero speed commands — the motor barely twitches. That's the flat
        // green line. :3
        //
        // 1ms gives FAS one tick to start executing the move and update its
        // internal position counter before we plan the next waypoint. At 100Hz
        // (10ms intervals) this costs 10% of the budget — acceptable because
        // without it the motor doesn't move at all. OSSM accepts this same cost.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}


// ============================================================================
// setup() — ordered wiring only
// ============================================================================

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    APPLOG("=== SlopDrive-32 v2.0 ===");
#if SERIAL_CONTROL_MODE
    applog("Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    applog("Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

    // Mount filesystem (web assets served from LittleFS).
    if (LittleFS.begin(true))
        APPLOG("LittleFS mounted");
    else
        APPLOG("LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");

    // Load persisted settings (or factory defaults on first boot).
    ConfigStore::load(g_state, mapper, motor);

    // Sync the persisted Intiface-compat flag into the parser's live static so
    // the very first TCode frame after boot gets decoded with the right scale.
    // Without this the parser would default to OFF (MFP decode) until the user
    // touched the toggle — and an Intiface user's first strokes would land
    // shallow. Push it in now so we're balls-deep from frame one. :3
    TCodeParser::intifaceCompat = g_state.intiface_compat;

    // Motor hardware init + apply saved driver config.
    motor.init();
    motor.applyDriverConfig(g_state.driver);

    // WiFi + mDNS (prerequisite for WS transport and the web UI).
    transportMgr.setupWiFi();

    // Register all HTTP routes and start the web server.
    webui.init();

    // Wire TCode parser callbacks to the motion layer.
    tcodeParser.onLinearRampTo(buttplugLinearCmd);
    tcodeParser.onLinearStop(buttplugStop);

    // Start the WS TCode server (always up — MultiFunPlayer can connect). :3
    wsTransport.begin();

    // Bring up the saved transport (WS / SER / BT).
    transportMgr.applyTransport(g_state.getTransport());

    // ---- Waypoint queue — must exist before any task that touches it --------
    // Created here, before task creation, so both Core 0 (buttplugLinearCmd)
    // and Core 1 (motionConsumerTask) see a valid handle from their first tick.
    // configASSERT panics on OOM — if this fails, nothing else matters. :3
    g_waypoint_queue = xQueueCreate(WAYPOINT_QUEUE_DEPTH, sizeof(PositionTime));
    configASSERT(g_waypoint_queue != nullptr);

    // ---- Create FreeRTOS tasks with correct core pinning (.clinerules §2) ----
    //   Core 1 (real-time):  motorTask, motionConsumerTask, Generator
    //   Core 0 (system):     commsTask, httpTask
    xTaskCreatePinnedToCore(motorTask,           "Motor",    4096, nullptr, 3, nullptr, 1);
    // motionConsumerTask: priority 4 — above motorTask (3), below FAS ISR.
    // Owns ALL FAS dispatch after setup(). Core 0 never touches motor again. :3
    xTaskCreatePinnedToCore(motionConsumerTask,  "MotionCon", 4096, nullptr, 4, nullptr, 1);
    // Stack bumped to 6144: feedLine() allocates a 256-byte local buf on every
    // call, and under a disconnect burst it can be called many times per tick.
    // 4096 was tight enough to overflow under that load — 6144 gives headroom. :3
    xTaskCreatePinnedToCore(commsTask,           "Comms",    6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(httpTask,            "HTTP",     4096, &webui,  1, nullptr, 0);

    generator.init();    // creates its own task on Core 1, priority 2

#if HOMING_DISABLED
    // >>>> HOMING DISABLED: force homed flags for bench/remote testing only.
    // Re-enable homing before running on real hardware. <<<<
    g_state.homed = true;
    motor.forceHomeState(true);
    APPLOG("!!! HOMING DISABLED — bench-test build only. Remove -DHOMING_DISABLED for real hardware.");
#endif

    APPLOG("System ready — push that thick shaft all the way in to home, or use the web UI :3");
}


// ============================================================================
// loop() — idle; all work is done in FreeRTOS tasks
// ============================================================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}
