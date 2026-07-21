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
//
// D4 (event-driven): TCode callbacks submit MotionIntent via arbiter
// (Core 0 → Core 1 single-slot deferral). PatternEngine emits one intent
// per stroke segment. motorTask processes deferred intents on Core 1.
// No periodic motion tick, no chase loop. ONE COMMAND → ONE PLAN → FAS.

#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>

#include "config_api.h"

#include "AppLog.h"
#include "SystemState.h"
#include "ConfigStore.h"
#include "StatusLeds.h"

#include "range_mapper.h"
#include "Kinematics.h"
#include "PositionTime.h"
#include "freertos/queue.h"
#include <esp_timer.h>
#include <chrono>

#if defined(DRIVER_TMC2160)
#include "TMC2160StepperDriver.h"
#endif

#if defined(DRIVER_AIM_SERVO)
#include "AIMServoDriver.h"
#endif

#include "MotionArbiter.h"
#include "MotionInterpolator.h"
#include "PatternEngine.h"
#include "OssmBleService.h"

#include "TCodeParser.h"
#include "SerialTransport.h"
#include "WebSocketTransport.h"
#include "BleTransport.h"
#include "DongleTransport.h"
#include "TransportManager.h"

#include "WebUI.h"
#include "UiSocket.h"
#include "OtaService.h"

#if defined(FEATURE_RS485_MODBUS)
#include "ServoModbus.h"
#endif

#if defined(BLE_ENABLED)
extern "C" bool bleInUse(void) { return true; }
#endif


// ============================================================================
// Module instances
// ============================================================================

#if defined(DRIVER_TMC2160)
  TMC2160StepperDriver motor;
#elif defined(DRIVER_AIM_SERVO)
  AIMServoDriver motor;
#else
  #error "No motor driver selected. Define DRIVER_TMC2160 or DRIVER_AIM_SERVO in platformio.ini build_flags."
#endif

static SystemState        g_state;
static RangeMapper        mapper;
static PatternEngine      patternEngine(g_state, mapper, motor);
static MotionArbiter      arbiter(g_state, mapper, motor);

// v0.4 on-device interpolator — Core-1-owned cubic motion generator.
// buttplugLinearCmd (Core 0) builds InterpSegments and hands them across via
// g_interp_queue; streamSamplerTask (Core 1) commits them onto the curve and
// samples it at ~1kHz into arbiter.submitStreamSample(). This is the fix for
// the v4 microstutter (C1-continuous cubic instead of per-point FAS re-plan)
// and the v3 slow-speed dropout (live-mode extrapolation). :3
static MotionInterpolator g_interp(0.5f);
static constexpr size_t   INTERP_QUEUE_DEPTH     = 16;
static QueueHandle_t      g_interp_queue         = nullptr;
// After this idle gap with no L0 command the sampler stops feeding FAS and
// yields the motor back to PatternEngine / manual moves.
static constexpr uint32_t STREAM_IDLE_TIMEOUT_MS = 500;

// v0.4 axis state — L0 ("Stroke") is ALWAYS registered.
// Additional axes are registered conditionally when hardware pins exist.
static TCodeAxisState     axisL0("Stroke", {AxisType::Linear, 0}, 0.5f);

static TCodeParser        tcodeParser;
static SerialTransport    serialTransport(tcodeParser);
static WebSocketTransport wsTransport(tcodeParser);
static BleTransport       bleTransport(tcodeParser);
static DongleTransport    dongleTransport(tcodeParser);
static OssmBleService     ossmBleService(g_state, patternEngine, mapper, nullptr);
static TransportManager   transportMgr(g_state, tcodeParser,
                                        serialTransport, wsTransport, bleTransport,
                                        dongleTransport, ossmBleService);

static UiSocket        uiSocket(g_state);
static WebUI webui(g_state, motor, mapper, patternEngine,
                    transportMgr, serialTransport, wsTransport, bleTransport);

// WiFi OTA path (firmware + LittleFS bundle). Owns the shared safety gate for
// both ArduinoOTA (espota) and the HTTP /api/ota endpoints. Serviced from the
// Core-0 httpTask only — never the motion-critical core. :3
static OtaService      otaService(g_state, arbiter, patternEngine, uiSocket);

#if defined(FEATURE_RS485_MODBUS)
static ServoModbus     servoModbus(Serial1, /* addr */ 1);
#endif


// LEGACY: waypoint queue — retained behind LEGACY_STREAM_PIPELINE for A/B bring-up.
// D4 replaces this with single-slot deferral via MotionArbiter.
static constexpr size_t WAYPOINT_QUEUE_DEPTH = 8;
static QueueHandle_t    g_waypoint_queue     = nullptr;


// ============================================================================
// Glue callbacks — D4: submit MotionIntent to arbiter (Core 0 → Core 1)
// ============================================================================

static void buttplugLinearCmd(float position, uint32_t duration_ms,
                              float slope, bool hasSlope, bool hasDuration) {
    if (!g_state.homed) return;
    if (g_state.paused || g_state.manual_override) {
        g_state.resume_start_ms = millis();
        return;
    }

    // New-stream soft start — stamp resume so safeSpeedCap eases the first move
    {
        uint32_t now0 = millis();
        if (g_state.last_intiface_ms == 0 ||
            (now0 - g_state.last_intiface_ms) > 2000)
            g_state.resume_start_ms = now0;
    }

    // Cadence measurement (Core 0 only) — EMA filter. The interpolator derives
    // its own live-mode timing from segment arrival, but we still keep this for
    // the WebUI rate readout and the auto-duration fallback below.
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
    g_state.last_cmd_ms      = now;
    g_state.last_intiface_ms = now;   // marks the stream active for the sampler

    // Yield-on-MOTION stamp: only a packet that moves the commanded target
    // counts as "the stream is driving". Keep-alive packets repeating the same
    // position hold the sampler (recentPacket) but no longer suppress a
    // user-started pattern indefinitely.
    {
        static float s_prev_stream_pos = -1.0f;
        if (s_prev_stream_pos < 0.0f || fabsf(position - s_prev_stream_pos) > 0.003f) {
            g_state.last_intiface_move_ms = now;
            s_prev_stream_pos = position;
        }
    }

    // Raw target telemetry (pre-planner) — mapped into the stroke window.
    g_state.commanded_raw_mm = mapper.intensityToPosition(position);

    // Build a POD InterpSegment and hand it to the Core-1 sampler. The
    // interpolator owns ALL curve shaping (v3 live extrapolation, v4 Hermite
    // gradient); main.cpp only translates parser output into a segment.
    //   targetPos   : normalized 0..1 magnitude (interp window == TCode 0..1)
    //   durationUs  : I<ms> * 1000 (0 when absent → interp derives from cadence)
    //   endSlope    : RAW wire G — the interpolator applies /1000 → dp/dtau,
    //                 byte-for-byte with TempestMAx's Axis::setCubic.
    //   isLivePoint : bare high-rate v3 point (no I, no G)
    InterpSegment seg;
    seg.targetPos   = position;
    seg.durationUs  = duration_ms * 1000UL;
    seg.endSlope    = slope;
    seg.hasSlope    = hasSlope;
    seg.hasDuration = hasDuration;
    seg.isLivePoint = (!hasSlope && !hasDuration);
    // Live motion-command path: a full queue means the Core-1 sampler is
    // stalled or overloaded and this segment is LOST. Count it and log
    // (rate-limited) — a silently-dropped TCode segment is a motion glitch
    // with no trace otherwise. :3
    if (g_interp_queue && xQueueSend(g_interp_queue, &seg, 0) != pdTRUE) {
        static uint32_t interp_drops = 0, last_drop_log_ms = 0;
        interp_drops++;
        uint32_t now_drop = millis();
        if (now_drop - last_drop_log_ms > 2000) {
            last_drop_log_ms = now_drop;
            APPLOGF("Interp queue FULL — %lu TCode segment(s) dropped; sampler stalled?",
                    (unsigned long)interp_drops);
        }
    }

    // LEGACY fallback — keep the waypoint queue alive for A/B testing
#if defined(LEGACY_STREAM_PIPELINE)
    PositionTime pt;
    pt.position     = (uint8_t)constrain((int)(position * 100.0f), 0, 100);
    pt.inTime       = (uint16_t)constrain((int)duration_ms, 0, 65535);
    pt.has_set_time = true;
    pt.setTime      = std::chrono::steady_clock::now();
    if (xQueueSend(g_waypoint_queue, &pt, 0) != pdTRUE) {
        static uint32_t wp_last_drop_log_ms = 0;
        uint32_t now_wp = millis();
        if (now_wp - wp_last_drop_log_ms > 2000) {
            wp_last_drop_log_ms = now_wp;
            APPLOG("Waypoint queue FULL — legacy waypoint dropped");
        }
    }
#endif
}

static void buttplugStop() {
    // DSTOP = stop moving now. Mark the stream idle so the Core-1 sampler stops
    // feeding FAS and releases the motor, then force-stop FAS. hardStop() keeps
    // homed + stream state — DSTOP is "stop moving," not "cut power forever."
    g_state.last_intiface_ms      = 0;
    g_state.last_intiface_move_ms = 0;
    arbiter.hardStopMotion();
}


// ============================================================================
// WIFI sideband command — set secondary credentials over USB serial
// ============================================================================
//
// `WIFI <ssid> <password>` arrives here (un-tokenised tail) via the TCodeParser
// WifiCmdCallback hook. This is the recovery path for a rig on an unknown
// network: the operator plugs in USB, opens a serial monitor, types the creds,
// and reboots — setupWiFi() then tries these NVS-stored creds as its second
// stage. We split on the FIRST space: everything before is the SSID, the rest
// (which may itself contain spaces) is the password. `WIFI CLEAR` wipes the
// stored secondary creds. The reply goes back on the same serial hose. :3
static void handleWifiCmd(const char* args) {
    if (!args || args[0] == '\0') {
        Serial.print("WIFI ERR empty\n");
        return;
    }

    // `WIFI CLEAR` — wipe stored secondary creds.
    if (strncasecmp(args, "CLEAR", 5) == 0 && (args[5] == '\0' || args[5] == ' ')) {
        ConfigStore::clearWifiCreds(g_state);
        Serial.print("WIFI OK cleared\n");
        return;
    }

    // Split SSID (first token) from password (remainder after first space).
    const char* sp = strchr(args, ' ');
    if (!sp) {
        Serial.print("WIFI ERR need <ssid> <password>\n");
        return;
    }
    char ssid[33];
    size_t ssid_len = (size_t)(sp - args);
    if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
        Serial.print("WIFI ERR ssid length\n");
        return;
    }
    memcpy(ssid, args, ssid_len);
    ssid[ssid_len] = '\0';

    const char* pass = sp + 1;
    while (*pass == ' ') pass++;   // skip extra spaces between ssid and pass
    if (*pass == '\0') {
        Serial.print("WIFI ERR empty password\n");
        return;
    }

    ConfigStore::saveWifiCreds(g_state, ssid, pass);
    Serial.printf("WIFI OK saved SSID='%s' — reboot to connect\n", ssid);
}


// ============================================================================
// FreeRTOS Tasks
// ============================================================================

// Core 1 — real-time: homing + D4 deferred-intent consumer
static void motorTask(void* /*param*/) {
    bool homing_started = false;
    while (true) {
        // E-stop
        if (g_state.estop_requested.exchange(false)) {
            arbiter.emergencyStop();
            homing_started = false;
            g_state.homing_in_progress = false;
            g_state.homed = false;
            APPLOG("E-Stop handled — shaft is soft, waiting for orders~ :3");
        }
        // Homing
        else if (g_state.homing_in_progress) {
            if (!motor.isHoming() && !homing_started) {
                motor.home();
                homing_started = true;
            }
            if (!motor.isHoming() && homing_started) {
                g_state.homing_in_progress = false;
                g_state.homed = motor.isHomed();
                homing_started = false;
                if (g_state.homed) {
                    g_state.resume_start_ms = millis();
                    APPLOG("System is now homed and ready to pound :3");
                } else {
                    APPLOG("Homing failed — endstop not found. Check wiring.");
                }
            }
        } else {
            homing_started = false;
            if (!g_state.homed) {
                if (motor.checkPushToHome()) {
                    g_state.homed = true;
                    APPLOG("System homed via push-to-home and ready :3");
                }
            }
        }
        motor.update();
        // D4: process any Core 0 → Core 1 deferred intents
        arbiter.processDeferred();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Core 1 — real-time: v0.4 interpolator sampler. Commits Core-0 segments onto
// the MotionInterpolator, samples its cubic at ~1kHz, and feeds the arbiter's
// stream fast-path (submitStreamSample). Publishes interp telemetry for the
// WebUI overlay. Only drives motion while a TCode stream is recently active —
// otherwise it yields the motor to PatternEngine / manual moves. :3
static void streamSamplerTask(void* /*param*/) {
    TickType_t lastWake     = xTaskGetTickCount();
    bool       wasActive    = false;
    uint32_t   lastMotionMs = 0;   // wall-clock of the last tick the curve was gliding
    while (true) {
        uint64_t nowUs  = (uint64_t)esp_timer_get_time();
        uint32_t now_ms = millis();

        bool gatesOk = g_state.homed && !g_state.paused && !g_state.manual_override &&
                       !g_state.estop_requested.load(std::memory_order_relaxed);
        // A stream is "active" if a packet arrived recently OR the interpolator
        // still has an in-flight segment to render. The second clause is the fix
        // for the sparse-v4 freeze: v4 lands points ~700-1000 ms apart — longer
        // than STREAM_IDLE_TIMEOUT_MS — so a packet-cadence-only gate declared
        // the stream idle BETWEEN every point and froze motion partway through a
        // move (e.g. ~499 ms into a 933 ms stroke). Gating on interp.isBusy()
        // lets the SEGMENT decide when a move is done: we keep sampling through
        // the whole cubic, and only start the idle countdown once the curve has
        // genuinely settled to a hold. The recent-packet clause still holds the
        // motor for the timeout AFTER the last move completes so a same-position
        // re-command doesn't drop-then-reacquire. :3
        bool recentPacket = g_state.last_intiface_ms != 0 &&
                            (now_ms - g_state.last_intiface_ms < STREAM_IDLE_TIMEOUT_MS);
        bool interpBusy   = g_interp.isBusy(nowUs);

        // Trailing hold measured from MOVE-END, not packet arrival. While the
        // curve is gliding we keep stamping lastMotionMs; once it settles to a
        // hold we keep the motor for one more STREAM_IDLE_TIMEOUT_MS window. This
        // is the operator's requested \"finish the move, THEN hold ~500 ms\" — a
        // long final segment (e.g. 933 ms) no longer releases with 0 ms trailing
        // hold just because the last packet arrived >500 ms ago. recentPacket
        // still covers the between-packets case on a live stream. :3
        if (interpBusy) lastMotionMs = now_ms;
        bool postMoveHold = lastMotionMs != 0 &&
                            (now_ms - lastMotionMs < STREAM_IDLE_TIMEOUT_MS);

        // Pattern reclaim: a USER-STARTED pattern takes the machine back once
        // the stream stops actually driving (no target movement for 1.5s and
        // no in-flight curve). Keep-alive packets alone no longer pin the
        // sampler — last active driver wins, both directions.
        bool intifaceDriving = g_state.last_intiface_move_ms != 0 &&
                               (now_ms - g_state.last_intiface_move_ms < 1500);
        bool patternClaims = g_state.pattern_running && !intifaceDriving && !interpBusy;

        bool streamActive = gatesOk && !patternClaims &&
                            (interpBusy || recentPacket || postMoveHold);

        // Rising edge: seed the interpolator at the actual current position so a
        // new stream starts from where the shaft really is (no stale segment).
        if (streamActive && !wasActive) {
            float span      = mapper.getMaxMm() - mapper.getMinMm();
            float actual_mm = g_state.actual_position_mm.load(std::memory_order_relaxed);
            float norm      = (span > 0.01f) ? (actual_mm - mapper.getMinMm()) / span : 0.5f;
            g_interp.reset(constrain(norm, 0.0f, 1.0f));
        }

        // Push the live WebUI overshoot-clamp toggle into the interpolator so a
        // change takes effect on the next committed segment. Cheap bool store on
        // the same core that reads it in commit() — no lock needed.
        g_interp.setClampOvershoot(g_state.interp_clamp_overshoot);

        // Drain the Core-0 → Core-1 segment handoff, committing each onto the curve.
        InterpSegment seg;
        while (xQueueReceive(g_interp_queue, &seg, 0) == pdTRUE) {
            g_interp.commit(seg, nowUs);
        }

        if (streamActive) {
            float pos = g_interp.positionAt(nowUs);
            float vel = g_interp.velocityAt(nowUs);
            arbiter.submitStreamSample(pos, vel);

            // Publish interp telemetry for the WebUI planned-path overlay.
            InterpDebug d = g_interp.snapshot(nowUs);
            g_state.interp_start_pos   = d.startPos;
            g_state.interp_end_pos     = d.endPos;
            g_state.interp_cur_pos     = d.curPos;
            g_state.interp_cur_vel     = d.curVel;
            g_state.interp_duration_us = d.durationUs;
            g_state.interp_elapsed_us  = d.elapsedUs;
            g_state.interp_live_mode   = d.liveMode;
            g_state.interp_grad_mode   = d.gradMode;
            g_state.interp_style       = d.style;
            g_state.interp_active      = true;
        } else if (wasActive) {
            g_state.interp_active = false;
        }

        // Drain the interpolator's Core-1-local anomaly ring into the cross-core
        // ring for the WebUI's 0x05 ANOMALY feed. Done unconditionally (not gated
        // on streamActive) so a decel-overrun fired as a stream ends still lands.
        // popAnomaly() is a cheap FIFO pop; the portMUX section is only entered
        // when there's actually something to publish. :3
        {
            InterpAnomaly ev;
            while (g_interp.popAnomaly(ev)) {
                portENTER_CRITICAL(&g_state.anom_mux);
                g_state.anom_ring[g_state.anom_write % SystemState::ANOM_CAP] = ev;
                g_state.anom_write++;
                portEXIT_CRITICAL(&g_state.anom_mux);
            }
        }

        wasActive = streamActive;
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1));
    }
}

// Core 0 — services all active transports, reports inbound rate
// Permanent Core-0 stall watchdog shared by commsTask + httpTask. Logs WHICH
// Core-0 call blocked, and for how long, once it returns. The heartbeat lives
// at the tail of httpTask, so any blocked step freezes the breath; this names
// the offender in the web log ([STALL] ...) instead of guesswork. Near-zero
// cost — only two millis() reads per step, logs only when a step exceeds the
// threshold. Keep it: it's how the WS-loop-on-httpTask freeze was found. :3
#define STALL_LOG_MS 120u
#define TIME_STEP(call, name) do {                                            \
        uint32_t _s0 = millis(); call; uint32_t _dt = millis() - _s0;         \
        if (_dt > STALL_LOG_MS) APPLOGF("[STALL] " name " blocked %lums", (unsigned long)_dt); \
    } while (0)
static void commsTask(void* /*param*/) {
    uint32_t last_report_ms   = 0;
    uint32_t last_frame_count = 0;
    while (true) {
        TransportMode activeMode = g_state.getTransport();
        // DIAG: commsTask is prio 2 (> httpTask prio 1) — a block here preempts
        // and freezes the heartbeat too. Time the transport poll + wifi supervise.
        if (activeMode == TransportMode::SER) {
            TIME_STEP(serialTransport.poll(), "comms:serial.poll");
        } else if (activeMode == TransportMode::DONGLE) {
            TIME_STEP(dongleTransport.poll(), "comms:dongle.poll");
        } else {
            TIME_STEP(wsTransport.run(), "comms:wsTransport.run");
        }

        uint32_t now = millis();
        if (now - last_report_ms >= 1000) {
            uint32_t frames  = tcodeParser.rxFrameCount;
            uint32_t per_sec = frames - last_frame_count;
            last_frame_count = frames;
            last_report_ms   = now;
            g_state.measured_hz = (uint16_t)per_sec;
            if (wsTransport.isConnected() || serialTransport.isActive() || dongleTransport.isActive())
                APPLOGF("[RATE] rx=%u frames/s", per_sec);
            transportMgr.pollWifiLink();
            transportMgr.superviseWifi();   // re-scan + re-pin if link dropped
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Core 0 — HTTP server + OSSM BLE + Status LEDs
// Each step is wrapped in TIME_STEP (the Core-0 stall watchdog, defined above
// commsTask). The heartbeat is at the tail, so a blocked step freezes the
// breath; the [STALL] web-log line names which call blocked.
static void httpTask(void* param) {
    WebUI* ui = static_cast<WebUI*>(param);
    while (true) {
        TIME_STEP(ui->update(),           "http:ui.update");
        // NOTE: uiSocket.update() (_ws.loop()) was MOVED to UiSocket::senderTask.
        // Servicing the telemetry WebSocket here let a wedged/half-open client's
        // blocking socket ops (up to WEBSOCKETS_TCP_TIMEOUT × connected sockets,
        // ~2.6s observed) freeze this task — and with it the heartbeat + HTTP +
        // OTA on Core 0. Isolating WS onto the sender task means a stuck client
        // can only delay telemetry, never the heartbeat/HTTP/motion. :3
        TIME_STEP(otaService.handle(),    "http:ota.handle");
#if defined(FEATURE_RS485_MODBUS)
        TIME_STEP(servoModbus.update(),   "http:servoModbus");
#endif
        TIME_STEP(ossmBleService.update(),"http:ossmBle");
        statusLedsUpdate(g_state);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// LEGACY_STREAM_PIPELINE: old waypoint-queue consumer (off by default)
// D4 replaces this with event-driven MotionArbiter.
#if defined(LEGACY_STREAM_PIPELINE)
static void motionConsumerTask(void* /*param*/) {
    auto best = std::chrono::steady_clock::now();

    PositionTime lastPt;
    lastPt.position     = 50;
    lastPt.inTime       = 250;
    lastPt.has_set_time = false;

    bool         haveCarry = false;
    PositionTime carryPt    = {};

    while (true) {
        PositionTime incoming;
        bool gotNew = (xQueueReceive(g_waypoint_queue, &incoming, pdMS_TO_TICKS(50)) == pdTRUE);

        if (!haveCarry) {
            if (gotNew) { carryPt = incoming; haveCarry = true; }
            continue;
        }

        PositionTime pt = carryPt;

        if (gotNew) {
            if (pt.inTime == 0 && pt.has_set_time && incoming.has_set_time) {
                int32_t gap_ms = (int32_t)std::chrono::duration_cast<
                    std::chrono::milliseconds>(incoming.setTime - pt.setTime).count();
                if (gap_ms > 0 && gap_ms < 1000) pt.inTime = (uint16_t)gap_ms;
            }
            carryPt = incoming;
        } else {
            haveCarry = false;
        }

        if (pt.inTime == 0)
            pt.inTime = (g_state.measured_interval_ms > 1.0f)
                      ? (uint16_t)(g_state.measured_interval_ms + 0.5f)
                      : 100;

        float timeSeconds = pt.inTime / 1000.0f;

        if (pt.has_set_time) {
            const int16_t bufTarget = 100;
            int16_t currentBuffer = (int16_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - best).count();
            int16_t lag = (int16_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    pt.setTime - best).count();
            if (lag < 0 || lag > bufTarget * 10) {
                best = pt.setTime;
                lag  = 0;
            }
            best += std::chrono::milliseconds(pt.inTime);

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

        float frac      = pt.position / 100.0f;
        float target_mm = mapper.getMinMm() + frac * (mapper.getMaxMm() - mapper.getMinMm());

        // Sole-caller rule (CLAUDE.md §2): even the legacy A/B pipeline submits
        // through the MotionArbiter — it no longer plans + dispatches to the
        // driver itself, which bypassed every e-stop/pause/override/window
        // gate. This task runs on Core 1, so direct submit() is allowed; the
        // arbiter derives the trapezoid from the deadline exactly like the old
        // kinematics::planTrapezoid path did. :3
        MotionIntent intent = {};
        intent.source      = MotionSource::TCODE_STREAM;
        intent.target_mm   = target_mm;
        intent.deadline_ms = (uint32_t)(timeSeconds * 1000.0f);
        arbiter.submit(intent);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif // LEGACY_STREAM_PIPELINE


// ============================================================================
// setup() — ordered wiring only
// ============================================================================

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    APPLOG("=== SlopDrive-32 v2.0 — D4 event-driven ===");
#if SERIAL_CONTROL_MODE
    applog("Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    applog("Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

#if defined(DRIVER_AIM_SERVO)
    pinMode(AIM_PIN_STEP, OUTPUT); digitalWrite(AIM_PIN_STEP, LOW);
    pinMode(AIM_PIN_DIR,  OUTPUT); digitalWrite(AIM_PIN_DIR,  LOW);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(400000);
    // Runtime steps/mm — load the persisted motor steps/rev (drive reg 0x0B
    // mirror) BEFORE any motion math runs. Reprogramming steps/rev from the
    // Configure pane updates this live (+ forces a re-home) — no reboot. :3
    aimGeometryInit();
#endif

    if (LittleFS.begin(true))
        APPLOG("LittleFS mounted");
    else
        APPLOG("LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");

    ConfigStore::load(g_state, mapper, motor);

    TCodeParser::intifaceCompat = g_state.intiface_compat;

    motor.init();
    motor.applyDriverConfig(g_state.driver);

    statusLedsInit();

    bool wifi_ok = transportMgr.setupWiFi();

    webui.init();

    uiSocket.setTelemetryRing(webui._telemetry_ring, &webui._telemetry_seq, &webui._telemetry_mux);
    uiSocket.setMotorDriver(&motor);
    uiSocket.setWebUI(&webui);
    webui.setUiSocket(&uiSocket);   // Health-tab /api/clients enumerate + kick
    uiSocket.init();

    // OTA — only meaningful when WiFi actually came up. ArduinoOTA needs the
    // network stack; the HTTP endpoints ride the WebServer webui.init() just
    // started. Register both here so a network flash works the moment we boot.
    if (wifi_ok) {
        otaService.begin(MDNSServiceName, SECRET_OTA_PASSWORD);
        otaService.registerHttpRoutes(webui.server());
        APPLOGF("[OTA] ready — hostname '%s', fw %s", MDNSServiceName, FIRMWARE_VERSION);
    } else {
        APPLOG("[OTA] skipped — WiFi down at boot (serial rescue path only)");
    }

#if defined(FEATURE_RS485_MODBUS)
    Serial1.begin(19200, SERIAL_8N1, AIM_PIN_485_RX, AIM_PIN_485_TX);
    servoModbus.init();
    webui.setServoModbus(servoModbus);
#endif

    // D4: init the arbiter — sole caller of motor for positioning
    arbiter.init();
    // Seed the dual limit sets from the persisted fields (migrated from legacy
    // on first boot after upgrade — ConfigStore fills both from the old
    // max_speed/accel if the new keys haven't been written yet).
    arbiter.setUserSpeedLimit(g_state.config.user_max_speed_mm_s);
    arbiter.setUserAccelLimit(g_state.config.user_max_accel_mm_s2);
    arbiter.setInputSpeedLimit(g_state.config.input_max_speed_mm_s);
    arbiter.setInputAccelLimit(g_state.config.input_max_accel_mm_s2);

    // Wire PatternEngine to the arbiter so it submits intents instead of
    // calling motor directly.
    patternEngine.setArbiter(&arbiter);

    // Wire WebUI to the arbiter so the UI's applySettings can dispatch
    // live limit-set updates directly through the sole caller.
    webui.setArbiter(&arbiter);

    // Register L0 axis with the parser (v0.4 multi-axis model)
    tcodeParser.registerAxis(&axisL0);

    // Wire TCode parser callbacks
    tcodeParser.onLinearRampTo(buttplugLinearCmd);
    tcodeParser.onLinearStop(buttplugStop);
    tcodeParser.onWifiCmd(handleWifiCmd);

    // WiFi-dependent transport fallback: if WiFi never associated at boot and
    // the persisted transport is WS (which needs the network), drop to USB
    // serial so the machine stays controllable. The operator can then send
    // `WIFI <ssid> <password>` over serial to store creds and reboot. :3
    if (!wifi_ok && g_state.getTransport() == TransportMode::WS) {
        APPLOG("WiFi down at boot — falling back to USB serial transport. "
               "Send 'WIFI <ssid> <password>' over serial, then reboot.");
        g_state.setTransport(TransportMode::SER);
    }

    wsTransport.begin();

    transportMgr.applyTransport(g_state.getTransport());

    // Waypoint queue — retained for LEGACY_STREAM_PIPELINE A/B
    g_waypoint_queue = xQueueCreate(WAYPOINT_QUEUE_DEPTH, sizeof(PositionTime));
    configASSERT(g_waypoint_queue != nullptr);

    // v0.4 interpolator segment queue — Core 0 (buttplugLinearCmd) → Core 1 sampler
    g_interp_queue = xQueueCreate(INTERP_QUEUE_DEPTH, sizeof(InterpSegment));
    configASSERT(g_interp_queue != nullptr);

    // Create FreeRTOS tasks. Every creation is checked — a boot-critical task
    // that fails to spin up under heap pressure (motorTask IS the homing +
    // e-stop servicer) must halt loudly, not boot a device that silently can't
    // home, e-stop, or move. Same configASSERT discipline as the queue
    // creations above. :3
    BaseType_t task_ok;
    // motorTask: Core 1, priority 3 — homing + D4 deferred-intent consumer
    task_ok = xTaskCreatePinnedToCore(motorTask, "Motor", 4096, nullptr, 3, nullptr, 1);
    configASSERT(task_ok == pdPASS);
    // streamSamplerTask: Core 1, priority 4 — v0.4 interpolator cubic sampler
    task_ok = xTaskCreatePinnedToCore(streamSamplerTask, "Sampler", 4096, nullptr, 4, nullptr, 1);
    configASSERT(task_ok == pdPASS);
    // motionConsumerTask: Core 1, priority 4 — ONLY compiled in LEGACY mode
#if defined(LEGACY_STREAM_PIPELINE)
    task_ok = xTaskCreatePinnedToCore(motionConsumerTask, "MotionCon", 4096, nullptr, 4, nullptr, 1);
    configASSERT(task_ok == pdPASS);
#endif
    task_ok = xTaskCreatePinnedToCore(commsTask, "Comms", 6144, nullptr, 2, nullptr, 0);
    configASSERT(task_ok == pdPASS);
    task_ok = xTaskCreatePinnedToCore(httpTask, "HTTP", 8192, &webui, 1, nullptr, 0);
    configASSERT(task_ok == pdPASS);

    ossmBleService.setWaypointQueue(g_waypoint_queue);
    ossmBleService.init();
    patternEngine.init();    // creates its own Core 1 task

#if HOMING_DISABLED
    g_state.homed = true;
    motor.forceHomeState(true);
    APPLOG("!!! HOMING DISABLED — bench-test build only. Remove -DHOMING_DISABLED for real hardware.");
#endif

    APPLOG("System ready — push that thick shaft all the way in to home, or use the web UI :3");
}


// ============================================================================
// loop() — idle
// ============================================================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}