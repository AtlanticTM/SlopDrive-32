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
#include <new>              // placement new (SlopSync hub goes into PSRAM)
#include "esp_heap_caps.h"

#include "config_api.h"

#include "AppLog.h"          // bridge only: applogBegin/applogDrain (SlopLog sinks)
#include "sloplog/sloplog.h"
#include "SystemState.h"
#include "ConfigStore.h"
#include "SlopGlowBoard.h"

#include "range_mapper.h"
#include "Kinematics.h"
#include "freertos/queue.h"
#include <esp_timer.h>

#if defined(DRIVER_AIM_SERVO)
#include "AIMServoDriver.h"
#include "MotorProxy.h"
#include "MachineConfig.h"
#if defined(FEATURE_RS485_MODBUS)
#include "ModbusServoDriver.h"
#endif
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
#include "SlopSyncHubService.h"

#include "WebUI.h"
#include "UiSocket.h"
#include "OtaService.h"

#if defined(FEATURE_RS485_MODBUS)
#include "ServoModbus.h"
#endif

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
#include "EncoderValidator.h"
#endif

#if defined(BLE_ENABLED)
extern "C" bool bleInUse(void) { return true; }
#endif


// ============================================================================
// Module instances
// ============================================================================

// ServoModbus — moved ABOVE the motor-driver block (Phase 2) so
// ModbusServoDriver can take it by reference at construction. Was previously
// declared further down, right before its own #include block; the transport
// object itself doesn't care where it's declared, but the new Modbus motor
// driver needs a live ServoModbus& to bind to. :3
#if defined(FEATURE_RS485_MODBUS)
static ServoModbus     servoModbus(Serial1, /* addr */ 1);
#endif

// Runtime-selectable motion backend (Phase 2 — MotorProxy/plan.md "Design").
// g_motion_backend (below) picks FAS vs. Modbus; the pick is read from NVS as
// early as possible in setup() and applied via motor.bind() before ANY other
// module touches `motor`. Every other module (patternEngine, arbiter, webui,
// encoderValidator) still captures MotorDriver& motor — the proxy — at
// static-init time exactly as before; only the bind target is now runtime-
// selectable instead of compile-time-fixed.
#if defined(DRIVER_AIM_SERVO)
  AIMServoDriver    fasMotor;
#if defined(FEATURE_RS485_MODBUS)
  ModbusServoDriver mbMotor(servoModbus);
#endif
  MotorProxy        motor;
#else
  #error "No motor driver selected. Define DRIVER_AIM_SERVO in platformio.ini build_flags."
#endif

// 0 = FAS step/dir (default), 1 = Modbus direct drive. Set once, early in
// setup(), from machineBackendLoad() — read-only after that point until the
// next reboot (backend switch is strict reboot-to-apply, see WebUI.cpp
// POST /api/machine/commit). AIM servo backend only. :3
static uint8_t g_motion_backend = 0;

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
static OssmBleService     ossmBleService(g_state, patternEngine, mapper);
static TransportManager   transportMgr(g_state, tcodeParser,
                                        serialTransport, wsTransport, bleTransport,
                                        dongleTransport, ossmBleService);

static UiSocket        uiSocket(g_state);
static WebUI webui(g_state, motor, mapper, patternEngine,
                    transportMgr, serialTransport, wsTransport, bleTransport);

// SlopSync hub — the ecosystem sync plane (binary WS :SLOPSYNC_WS_PORT).
// Lives in PSRAM: as a BSS static its ~100 KB reservation starved internal
// heap to 13 KB free / 1.6 KB min — the WebUI page-serve death. The S3 has
// 8 MB of PSRAM for exactly this; placement-new'd there in setup(). Its
// FreeRTOS task stack stays internal (created inside init(), default heap).
static slopdrive::SlopSyncHubService* slopSyncHub = nullptr;

// WiFi OTA path (firmware + LittleFS bundle). Owns the shared safety gate for
// both ArduinoOTA (espota) and the HTTP /api/ota endpoints. Serviced from the
// Core-0 httpTask only — never the motion-critical core. :3
static OtaService      otaService(g_state, arbiter, patternEngine, uiSocket);

// NOTE: servoModbus itself now lives further up (right above the motor-driver
// block) so ModbusServoDriver can bind to it — see the comment there. :3

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
// Report-only FAS-vs-encoder cross-check — reads servoModbus telemetry + the
// motor's step counter, never commands anything. Lives on httpTask Core 0. :3
static EncoderValidator encoderValidator(servoModbus, motor);
#endif



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
        static uint32_t interp_drops = 0;
        interp_drops++;
        SLOGW_EVERY_MS(2000, "sys", "Interp queue FULL — %lu TCode segment(s) dropped; sampler stalled?",
                       (unsigned long)interp_drops);
    }

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
            SLOGW("sys", "E-Stop handled — shaft is soft, waiting for orders~ :3");
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
                    SLOGI("sys", "System is now homed and ready to pound :3");
                } else {
                    SLOGW("sys", "Homing failed — endstop not found. Check wiring.");
                }
            }
        } else {
            homing_started = false;
            if (!g_state.homed) {
                if (motor.checkPushToHome()) {
                    g_state.homed = true;
                    SLOGI("sys", "System homed via push-to-home and ready :3");
                }
            }
        }
        motor.update();
        // D4: process any Core 0 → Core 1 deferred intents
        arbiter.processDeferred();
        // SlopGlow liveness: this pulse is what keeps the status LEDs
        // animating. If this loop dies, the lights freeze — by design.
        if (auto* hb = slopglowMotorHeartbeat()) hb->pulse();
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
        if (_dt > STALL_LOG_MS) SLOGW("sys", name " blocked %lums", (unsigned long)_dt); \
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
                SLOGD("sys", "rx=%u frames/s", per_sec);
            transportMgr.pollWifiLink();
            transportMgr.superviseWifi();   // re-scan + re-pin if link dropped
        }

        if (auto* hb = slopglowCommsHeartbeat()) hb->pulse();  // SlopGlow liveness (Core 0)
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
        // In Modbus motion-backend mode the bus is serviced from Core 1
        // (servoBusTask) instead — single-owner rule, ServoModbus is not
        // thread-safe to poll from two tasks. In FAS mode (the default,
        // backend 0) nothing changes: httpTask keeps servicing it here
        // exactly like before Phase 2. :3
        if (g_motion_backend == 0) {
            TIME_STEP(servoModbus.update(),   "http:servoModbus");
        }
#endif
#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
        TIME_STEP(encoderValidator.update(), "http:encValidator");
#endif
        TIME_STEP(ossmBleService.update(),"http:ossmBle");
        // Serial-log gating follows LIVE serial TCode traffic (not the old
        // compile-time flag): Intiface streaming -> serial sink mutes; idle ->
        // full logs return. Self-healing in both directions.
        applogSerialDedicated(serialTransport.isActive());
        TIME_STEP(applogDrain(),          "http:logDrain");   // SlopLog ring -> web/serial sinks
        slopglowUpdate(g_state);
        // Heap health beacon: free / low-water / largest-block. maxblock is
        // the one that kills big allocations (LittleFS streams, WS buffers)
        // long before free hits zero — fragmentation shows up there first.
        SLOGI_EVERY_MS(10000, "sys", "heap free=%u min=%u maxblock=%u psram=%u",
                       unsigned(ESP.getFreeHeap()), unsigned(ESP.getMinFreeHeap()),
                       unsigned(ESP.getMaxAllocHeap()), unsigned(ESP.getFreePsram()));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
// Core 1 — Modbus bus service task, ONLY created when g_motion_backend == 1.
// Guarded on DRIVER_AIM_SERVO too (not just FEATURE_RS485_MODBUS) because the
// body below now calls mbMotor.executorTick() — mbMotor only EXISTS inside
// the DRIVER_AIM_SERVO branch above. g_motion_backend can only ever be 1
// there too, so this doesn't lose any real configuration, just keeps a
// hypothetical FEATURE_RS485_MODBUS-without-DRIVER_AIM_SERVO build compiling.
// Phase 3 (plan.md "Task/core layout"): setpoint-first priority. Every 2ms
// tick, mbMotor.executorTick() runs FIRST — it only sends a setpoint from an
// IDLE bus (StreamedSetpointExecutor::onTick), so a setpoint due this tick
// always gets first crack at the wire. servoModbus.update() runs SECOND and
// spends whatever's left of the tick on Configure write-queue drain / config
// scan / telemetry-and-encoder poll rotation — its own internal spacing
// constants (POLL_INTERVAL_MS etc.) already keep that traffic from hogging
// the bus. A poll already in flight when a setpoint comes due can delay that
// setpoint by up to ~1 transaction (~4ms @19200, less @115200) — acceptable
// jitter for this phase; reprogramBaud(115200) below shrinks it. :3
static void servoBusTask(void* /*param*/) {
    while (true) {
        mbMotor.executorTick(esp_timer_get_time());
        servoModbus.update();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#endif


// ============================================================================
// setup() — ordered wiring only
// ============================================================================

void setup() {
    Serial.begin(SERIAL_CONTROL_BAUD);
    applogBegin();
    SLOGI("boot", "=== SlopDrive-32 v2.0 — D4 event-driven ===");
#if SERIAL_CONTROL_MODE
    SLOGI("boot", "Serial control mode ON: USB Serial is dedicated to Intiface TCode.");
    SLOGI("boot", "Add a 'Serial' device in Intiface pointing at this COM port.");
#endif

#if defined(DRIVER_AIM_SERVO)
    // Runtime motion-backend selection (Phase 2 — plan.md "Static-init trap").
    // Read NVS ("machcfg"/backend) and bind the proxy to a concrete driver as
    // early as physically possible — BEFORE aimGeometryInit(), BEFORE
    // ConfigStore::load(), BEFORE motor.init()/applyDriverConfig(), before
    // ANY call through `motor` at all. Every one of those eventually reaches
    // MotorProxy::d(), which configASSERTs non-null — an unbound proxy is a
    // boot-order bug that must halt loudly, not limp along silently. NVS
    // itself is safe to read this early: the Arduino core's own startup
    // brings up nvs_flash_init() before setup() ever runs, well before
    // LittleFS.begin() below. :3
    g_motion_backend = machineBackendLoad();
#if defined(FEATURE_RS485_MODBUS)
    if (g_motion_backend == 1) {
        motor.bind(mbMotor);
        SLOGI("boot", "Motion backend: MODBUS direct-drive (skeleton mode — no motion until Phase 3)");
    } else {
        motor.bind(fasMotor);
        SLOGI("boot", "Motion backend: FAS step/dir");
    }
#else
    // Modbus feature not compiled into this build at all — always FAS,
    // regardless of what a stale NVS value might say (machineBackendLoad()
    // already clamps to 0 in this case too — belt and suspenders). :3
    motor.bind(fasMotor);
    SLOGI("boot", "Motion backend: FAS step/dir (FEATURE_RS485_MODBUS not compiled)");
#endif
    webui.setMachineBackend(g_motion_backend);

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
        SLOGI("boot", "LittleFS mounted");
    else
        SLOGE("boot", "LittleFS mount FAILED - upload filesystem image (pio run -t uploadfs)");

    ConfigStore::load(g_state, mapper, motor);

    TCodeParser::intifaceCompat = g_state.intiface_compat;

    motor.init();
    motor.applyDriverConfig(g_state.driver);

    slopglowInit();

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
        SLOGI("boot", "OTA ready — hostname '%s', fw %s", MDNSServiceName, FIRMWARE_VERSION);
    } else {
        SLOGW("boot", "OTA skipped — WiFi down at boot (serial rescue path only)");
    }

#if defined(FEATURE_RS485_MODBUS)
    Serial1.begin(19200, SERIAL_8N1, AIM_PIN_485_RX, AIM_PIN_485_TX);
    servoModbus.init();
    webui.setServoModbus(servoModbus);
#if defined(DRIVER_AIM_SERVO)
    webui.setEncoderValidator(encoderValidator);

    // ---- Modbus-mode baud auto-config (Phase 3) ------------------------------
    // OSSM-RS-style: the drive boots factory-19200 every power-cycle (we NEVER
    // save baud to its EEPROM — see ServoModbus::reprogramBaud() doc — so a
    // power-cycle always recovers factory state no matter what we did last
    // session). Only worth the reprogram+rebaud latency when Modbus is the
    // ACTIVE backend — FAS mode only needs telemetry, and its dual-baud probe
    // already finds a drive at either speed. Runs BEFORE the reg-0x0B e-gear
    // adoption below so that read (and everything else in this boot) lands at
    // the FINAL baud, not the ephemeral 19200 the probe started at. :3
    if (g_motion_backend == 1 && servoModbus.isReady() && servoModbus.baud() == 19200) {
        if (servoModbus.reprogramBaud(115200)) {
            SLOGI("boot", "Modbus mode: drive reprogrammed 19200 -> 115200 (OSSM-RS magic sequence) :3");
        } else {
            SLOGW("boot", "Modbus mode: 19200 -> 115200 reprogram FAILED — staying at 19200 "
                  "(motion still works, just tighter bus budget per plan.md).");
        }
    }

    // Re-apply the driver config now that the bus is actually up: the earlier
    // motor.applyDriverConfig() call (right after motor.init()) ran BEFORE
    // servoModbus.init(), so in Modbus mode its queued register writes (output
    // state, torque clamp 0x18) were dropped by the !_ready guard. FAS mode is
    // untouched — its applyDriverConfig is a no-op either way. :3
    if (g_motion_backend == 1 && servoModbus.isReady()) {
        motor.applyDriverConfig(g_state.driver);
    }

    // Ground Truth for geometry: the drive's own e-gear register (0x0B, saved
    // in ITS EEPROM by the programmer) is the authority on steps/rev. Adopt it
    // at boot whenever the drive answers — this heals the NVS-mirror-lost case
    // where the firmware would otherwise boot at the 800 default while the
    // drive physically needs 1600 pulses/rev, silently halving every commanded
    // millimetre until the mismatch is noticed. Machine is unhomed at this
    // point, so the forced re-home semantics of a steps/rev change are free. :3
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
        SLOGW("boot", "WiFi down at boot — falling back to USB serial transport. "
              "Send 'WIFI <ssid> <password>' over serial, then reboot.");
        g_state.setTransport(TransportMode::SER);
    }

    wsTransport.begin();

    transportMgr.applyTransport(g_state.getTransport());

    // v0.4 interpolator segment queue — Core 0 (buttplugLinearCmd) → Core 1 sampler
    g_interp_queue = xQueueCreate(INTERP_QUEUE_DEPTH, sizeof(InterpSegment));
    configASSERT(g_interp_queue != nullptr);

    // Create FreeRTOS tasks. Every creation is checked — a boot-critical task
    // that fails to spin up under heap pressure (motorTask IS the homing +
    // e-stop servicer) must halt loudly, not boot a device that silently can't
    // home, e-stop, or move. Same configASSERT discipline as the queue
    // creations above. :3
    // End of the single-task boot phase: from here logs are ring-buffered and
    // drained by httpTask (immediate synchronous drain is only safe pre-tasks).
    sloplog::logger().setImmediateDrain(false);

    BaseType_t task_ok;
    // motorTask: Core 1, priority 3 — homing + D4 deferred-intent consumer
    task_ok = xTaskCreatePinnedToCore(motorTask, "Motor", 4096, nullptr, 3, nullptr, 1);
    configASSERT(task_ok == pdPASS);
    // streamSamplerTask: Core 1, priority 4 — v0.4 interpolator cubic sampler
    task_ok = xTaskCreatePinnedToCore(streamSamplerTask, "Sampler", 4096, nullptr, 4, nullptr, 1);
    configASSERT(task_ok == pdPASS);
    task_ok = xTaskCreatePinnedToCore(commsTask, "Comms", 6144, nullptr, 2, nullptr, 0);
    configASSERT(task_ok == pdPASS);
    task_ok = xTaskCreatePinnedToCore(httpTask, "HTTP", 8192, &webui, 1, nullptr, 0);
    configASSERT(task_ok == pdPASS);
#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
    // servoBusTask: Core 1, priority 5 — ONLY when Modbus is the active
    // backend. Boot-order note: servoModbus.init() already ran earlier in
    // setup() (in the FEATURE_RS485_MODBUS block above, well before we get
    // here), so the bus is already probed/ready before this task starts
    // polling it — verified by reading through setup() top to bottom, not
    // assumed. In FAS mode (g_motion_backend == 0, the default) this task is
    // never created at all; httpTask keeps servicing servoModbus as it always
    // has (see the guard in httpTask above). :3
    if (g_motion_backend == 1) {
        task_ok = xTaskCreatePinnedToCore(servoBusTask, "ServoBus", 4096, nullptr, 5, nullptr, 1);
        configASSERT(task_ok == pdPASS);
    }
#endif

    ossmBleService.init();
    patternEngine.init();    // creates its own Core 1 task

    // SlopSync hub last: WiFi is up, arbiter/webui/patternEngine are wired.
    // Placement-new into PSRAM (see the declaration comment); refuses to
    // start rather than eat internal RAM if PSRAM is somehow absent.
    {
        void* mem = heap_caps_malloc(sizeof(slopdrive::SlopSyncHubService),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (mem != nullptr) {
            slopSyncHub = new (mem) slopdrive::SlopSyncHubService(g_state, webui, arbiter);
            slopSyncHub->setPatternEngine(&patternEngine);
            slopSyncHub->init();
            SLOGI("slopsync", "hub service in PSRAM (%u B)",
                  unsigned(sizeof(slopdrive::SlopSyncHubService)));
        } else {
            SLOGE("slopsync", "no PSRAM block for hub service — SlopSync DISABLED this boot");
        }
    }
    SLOGI("sys", "post-slopsync heap free=%u maxblock=%u psram free=%u",
          unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()),
          unsigned(ESP.getFreePsram()));

#if HOMING_DISABLED
    g_state.homed = true;
    motor.forceHomeState(true);
    SLOGW("boot", "!!! HOMING DISABLED — bench-test build only. Remove -DHOMING_DISABLED for real hardware.");
#endif

    SLOGI("boot", "System ready — push that thick shaft all the way in to home, or use the web UI :3");
}


// ============================================================================
// loop() — idle
// ============================================================================

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}