// SlopDrive-32 — main.cpp: composition root, wires modules and creates tasks.
// Constraints:
//   All logic lives in system/, motion/, comms/, ui/. This file only declares
//   module instances, wires them in setup(), creates FreeRTOS tasks with
//   correct core pinning, and idles in loop().
//   Core 1 (real-time): motorTask, streamSamplerTask, PatternEngine's own
//   task, servoBusTask (Modbus backend only).
//   Core 0 (system): commsTask, httpTask.
//   D4 event-driven: SlopSync callbacks submit MotionIntent via the
//   arbiter (Core 0 -> Core 1 deferral queue); PatternEngine emits one
//   intent per stroke segment; motorTask drains deferred intents on Core 1.
//   No periodic motion tick, no chase loop — ONE COMMAND -> ONE PLAN -> FAS.
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
#include "AIMServoDriver.h"
#include "MlinkServoDriver.h"
#include "MotorProxy.h"
#include "MachineConfig.h"
#if defined(FEATURE_RS485_MODBUS)
#include "ModbusServoDriver.h"
#endif
#endif

#include "MotionArbiter.h"
#include <slopmotion/slopmotion.hpp>
#include "PatternEngine.h"

#include "WifiLink.h"
#include "SlopSyncHubService.h"

#include "WebUI.h"
// WebUI.h only forward-declares SlopHttpServer; commsTask's stall reap calls
// through it (abortBlockedClient), so this TU needs the complete type.
#include "ui/SlopHttpServer.h"
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


// ---- Module instances -------------------------------------------------------

// ServoModbus must be declared above the motor-driver block: ModbusServoDriver
// takes it by reference at construction, so the reference must already exist.
// The transport object itself has no ordering requirement of its own.
#if defined(FEATURE_RS485_MODBUS)
static ServoModbus     servoModbus(Serial1, /* addr */ 1);
#endif

// Runtime-selectable motion backend (Phase 2, via MotorProxy indirection).
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
  // The drive is SAVED in quadrature (0x19=2, 2026-08-07): the RP2350 is the
  // only pulse source, so the pulse backend (0) binds mlink, not FAS. FAS
  // stays compiled for a future step/dir drive; it cannot move THIS one.
  MlinkServoDriver  mlinkMotor;
  MotorProxy        motor;
#else
  #error "No motor driver selected. Define DRIVER_AIM_SERVO in platformio.ini build_flags."
#endif

// 0 = FAS step/dir (default), 1 = Modbus direct drive. Set once, early in
// setup(), from machineBackendLoad() — read-only after that point until the
// next reboot (backend switch is strict reboot-to-apply; written by the
// motion_backend setting, 0x3030 key 5). AIM servo backend only.
static uint8_t g_motion_backend = 0;

static SystemState        g_state;
static RangeMapper        mapper;
static PatternEngine      patternEngine(g_state, mapper, motor);
static MotionArbiter      arbiter(g_state, mapper, motor);

// SlopMotion — Core-1-owned jerk-limited motion core (docs/canon doctrine
// §SlopMotion). The SlopSync ingress (Core 0) builds slopmotion::Commands
// and hands them across via g_interp_queue; streamSamplerTask (Core 1)
// plans them (quintic waveform / Ruckig chase + guard) and samples the plan
// at ~1kHz into arbiter.submitStreamSample().
// ~3.4 KB object — plain BSS static is fine (measured on xtensa).
static slopmotion::Engine g_slopmotion({}, 0.5f);
static constexpr size_t   INTERP_QUEUE_DEPTH     = 16;
static QueueHandle_t      g_interp_queue         = nullptr;
// After this idle gap with no L0 command the sampler stops feeding FAS and
// yields the motor back to PatternEngine / manual moves.
static constexpr uint32_t STREAM_IDLE_TIMEOUT_MS = 500;

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

#if defined(MOTION_PASSTHROUGH_BENCH)
// ---- RP2350 bench SPI master (sd-dxy bring-up) ------------------------------
// Drop-in pinout (docs/rp2350-wiring.md): the matrix remaps roles, the silk
// lies. Manual CS; the slave answers each transaction with the status it
// preloaded after the PREVIOUS one, so the first read is discarded.
#include <SPI.h>
#include "comms/MotionLinkProtocol.h"
namespace mlink {
constexpr int8_t kSck = 7, kMiso = 10, kMosi = 38, kCs = 48, kIrq = 4;
SPIClass s_spi(FSPI);
uint8_t s_seq = 0;
bool s_begun = false;

void begin() {
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    pinMode(kIrq, INPUT_PULLDOWN);
    s_spi.begin(kSck, kMiso, kMosi, -1);
    s_begun = true;
    SLOGI("mlink", "bench SPI master up: SCK=%d MISO=%d MOSI=%d CS=%d IRQ=%d, %lu Hz",
          kSck, kMiso, kMosi, kCs, kIrq, (unsigned long)motionlink::kSpiHz);
}

// One fixed-size transaction: send a frame, read back the slave's preload.
// Stamps the outgoing CRC; the caller never does.
void xfer(uint8_t (&out)[motionlink::kFrameBytes],
          uint8_t (&in)[motionlink::kFrameBytes]) {
    static_assert(motionlink::kSpiMode == 1, "PL022 slave needs CPHA=1");
    // >=60 us since the last transaction: the slave block-resets its SPI to
    // flush TX after every frame, and clocking into that window tears frames.
    static uint32_t s_lastEndUs = 0;
    const uint32_t sinceUs = micros() - s_lastEndUs;
    if (sinceUs < 60) delayMicroseconds(60 - sinceUs);
    motionlink::crcStamp(out);
    s_spi.beginTransaction(SPISettings(motionlink::kSpiHz, MSBFIRST, SPI_MODE1));
    digitalWrite(kCs, LOW);
    s_spi.transferBytes(out, in, motionlink::kFrameBytes);
    digitalWrite(kCs, HIGH);
    s_spi.endTransaction();
    s_lastEndUs = micros();
}

// Build-and-send for payloadless ops (estop, clear, ping).
void sendOp(uint8_t op) {
    uint8_t out[motionlink::kFrameBytes] = {op, ++s_seq};
    uint8_t back[motionlink::kFrameBytes] = {};
    xfer(out, back);
}

// 10 Hz ping; 0.5 Hz status line into the diag archive (tag mlink).
void tick() {
    if (!s_begun) return;
    static uint32_t lastMs = 0;
    const uint32_t now = millis();
    if (now - lastMs < 100) return;
    lastMs = now;

    uint8_t out[motionlink::kFrameBytes] = {motionlink::kOpPing, ++s_seq};
    uint8_t in[motionlink::kFrameBytes] = {};
    xfer(out, in);

    uint16_t runway = uint16_t(in[2]) | uint16_t(uint16_t(in[3]) << 8);
    float pos = 0, vel = 0;
    memcpy(&pos, &in[6], 4);
    memcpy(&vel, &in[10], 4);
    SLOGI_EVERY_MS(2000, "mlink",
                   "rp2350 hex[0..15]: %02x %02x %02x %02x %02x %02x %02x %02x "
                   "%02x %02x %02x %02x %02x %02x %02x %02x irq=%d",
                   in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7],
                   in[8], in[9], in[10], in[11], in[12], in[13], in[14], in[15],
                   int(digitalRead(kIrq)));
    (void)runway; (void)pos; (void)vel;

    // Trust = CRC: act only on a proven status frame. A rebooting or torn
    // slave reads as depth 0, and blind-feeding on that overfilled the ring
    // twice (2026-08-06).
    const bool frameSane = motionlink::crcOk(in);

    // Machine estop punches through the bench path. Repeat kOpEstop until the
    // echoed state confirms it (repetition survives a dropped frame); leave
    // the hold only after the machine unlatches.
    if (g_state.estop_latched) {
        if (!frameSane || in[0] != motionlink::kStateEstop)
            sendOp(motionlink::kOpEstop);
        return;
    }
    if (frameSane && in[0] == motionlink::kStateEstop) {
        sendOp(motionlink::kOpClear);
        return;
    }

    // Bench wiggle: keep two segments queued so the motor visibly moves.
    // SMALL ON PURPOSE: this path bypasses the MotionArbiter (no homed gate,
    // no limit clamp), so the amplitude must be safe from ANY carriage
    // position. Scale only at the bench, drive supervised.
    // Units are DRIVE INPUT COUNTS: quadrature at gear 4/1 (0x19=2 saved
    // 2026-08-07), so 8192 counts/motor-rev. 2048 counts is ~10 mm.
    constexpr float kWiggleSteps = 2048.0f;
    constexpr uint32_t kWiggleUs = 2000000u;
    // Ack-gated alternation: the endpoint flip commits only when the slave
    // echoes the segment's seq (a torn frame leaves the echo on the ping's
    // seq). Flipping at send time desynced the pattern on every silent drop
    // and the next leg started 600 steps from the machine -- the
    // burst-to-reversal bug (2026-08-07).
    static bool outward = true;
    static bool pending = false;
    static uint8_t pendingSeq = 0;
    if (pending && frameSane) {
        if (in[5] == pendingSeq) outward = !outward;
        pending = false;   // no echo: dropped, the same leg goes out again
    }
    if (frameSane && in[4] < 2 && !pending) {
        motionlink::Segment seg{kWiggleUs,
                                outward ? 0.0f : kWiggleSteps, 0.0f,
                                outward ? kWiggleSteps : 0.0f, 0.0f};
        uint8_t sout[motionlink::kFrameBytes] = {motionlink::kOpSegment, ++s_seq};
        pendingSeq = s_seq;
        pending = true;
        memcpy(&sout[2], &seg.duration_us, 4);
        memcpy(&sout[6], &seg.p0, 4);
        memcpy(&sout[10], &seg.v0, 4);
        memcpy(&sout[14], &seg.p1, 4);
        memcpy(&sout[18], &seg.v1, 4);
        uint8_t back[motionlink::kFrameBytes] = {};
        xfer(sout, back);
    }
}
}  // namespace mlink
#endif

// servoModbus itself is declared above the motor-driver block so
// ModbusServoDriver can bind to it — see the comment there.

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
// Report-only FAS-vs-encoder cross-check — reads servoModbus telemetry + the
// motor's step counter, never commands anything. Lives on httpTask Core 0.
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
            if (!g_state.homed) {
                if (motor.checkPushToHome()) {
                    g_state.homed = true;
                    SLOGI("sys", "System homed via push-to-home and ready :3");
                }
            }
        }
        motor.update();
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
            // Speed EMA, the WebUI sampler's other duty: ~16 ms time constant
            // at this loop's ~1 ms tick; jitter in the tick washes into the EMA.
            static float last_pos = actual_mm;
            static float spd_ema = 0.0f;
            spd_ema += 0.06f * (fabsf(actual_mm - last_pos) * 1000.0f - spd_ema);
            last_pos = actual_mm;
            g_state.live_speed_mm_s.store(spd_ema, std::memory_order_relaxed);
            if (spd_ema > g_state.max_speed_mm_s.load(std::memory_order_relaxed))
                g_state.max_speed_mm_s.store(spd_ema, std::memory_order_relaxed);
        }
#endif
        // D4: process any Core 0 -> Core 1 deferred intents
        arbiter.processDeferred();
        // SlopGlow liveness: this pulse is what keeps the status LEDs
        // animating. If this loop dies, the lights freeze — by design.
        if (auto* hb = slopglowMotorHeartbeat()) hb->pulse();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Core 1 — real-time: SlopMotion sampler. Plans Core-0 commands on the
// slopmotion::Engine (quintic waveform / Ruckig chase + guard, docs/canon
// doctrine §SlopMotion), samples the plan at ~1kHz, and feeds the arbiter's
// stream fast-path (submitStreamSample). Publishes telemetry for the WebUI
// overlay. Only drives motion while a SlopSync motion-input stream is
// recently active — otherwise it yields the motor to PatternEngine / manual
// moves.
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
        // re-command doesn't drop-then-reacquire.
        bool recentPacket = g_state.last_intiface_ms != 0 &&
                            (now_ms - g_state.last_intiface_ms < STREAM_IDLE_TIMEOUT_MS);
        bool interpBusy   = g_slopmotion.isBusy(nowUs);

        // Trailing hold measured from MOVE-END, not packet arrival. While the
        // curve is gliding we keep stamping lastMotionMs; once it settles to a
        // hold we keep the motor for one more STREAM_IDLE_TIMEOUT_MS window.
        // This is the "finish the move, THEN hold ~500 ms" requirement — a
        // long final segment (e.g. 933 ms) no longer releases with 0 ms trailing
        // hold just because the last packet arrived >500 ms ago. recentPacket
        // still covers the between-packets case on a live stream.
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

        // Rising edge: seed the engine at the actual current position so a
        // new stream starts from where the shaft really is (no stale plan).
        if (streamActive && !wasActive) {
            float span      = mapper.getMaxMm() - mapper.getMinMm();
            float actual_mm = g_state.actual_position_mm.load(std::memory_order_relaxed);
            float norm      = (span > 0.01f) ? (actual_mm - mapper.getMinMm()) / span : 0.5f;
            g_slopmotion.resetAt(constrain(norm, 0.0f, 1.0f), nowUs);
        }

        // Push the live tuning (POST /api/slopmotion, Core 0) into the engine.
        // Ceilings derive from the mm-domain INPUT limit set over the stroke
        // window (1 normalized unit == the window span), overridable for bench
        // tuning. ALL THREE derive the same way as of fw 2.1.47 — jerk used to
        // be a bare normalized constant, which made the PHYSICAL jerk ceiling
        // shrink as the operator narrowed the window and silently bound fast
        // segments. It is a persisted mm-domain limit now, like its siblings.
        // Same-core with commit() — no lock. Runs every tick: 12 scalar copies.
        {
            slopmotion::Config smCfg;
            const float span = mapper.getMaxMm() - mapper.getMinMm();
            const float vovr = g_state.sm_tune_vmax_ovr;
            const float aovr = g_state.sm_tune_amax_ovr;
            const float jovr = g_state.sm_tune_jmax_ovr;
            smCfg.limits.vmax = vovr > 0.0f ? vovr
                : (span > 1.0f ? g_state.config.input_max_speed_mm_s  / span : 3.0f);
            smCfg.limits.amax = aovr > 0.0f ? aovr
                : (span > 1.0f ? g_state.config.input_max_accel_mm_s2 / span : 30.0f);
            smCfg.limits.jmax = jovr > 0.0f ? jovr
                : (span > 1.0f ? g_state.config.input_max_jerk_mm_s3  / span : 500.0f);
            smCfg.chase_feedforward = g_state.sm_tune_chase_ff;
            smCfg.chase_accel_ff    = g_state.sm_tune_chase_aff;
            smCfg.chase_ff_gain     = g_state.sm_tune_chase_gain;
            smCfg.chase_lookahead   = g_state.sm_tune_chase_look;
            smCfg.chase_dense_us    = g_state.sm_tune_dense_us;
            // Infeasible-segment policy: a 3-WAY map, not a boolean. This runs
            // every tick, so whatever it writes IS the engine's policy — a
            // narrower map here silently overrides the engine's own default
            // (that was the fw 2.1.49 bug: Reshape was unreachable because the
            // boolean map could only produce Scale or Stretch). An out-of-range
            // stored value falls through to the ENGINE default (smCfg is a
            // fresh default-constructed Config), never to an arbitrary policy.
            switch (g_state.sm_tune_infeas_policy) {
                case 0: smCfg.infeasible_policy = slopmotion::InfeasiblePolicy::Stretch; break;
                case 1: smCfg.infeasible_policy = slopmotion::InfeasiblePolicy::Scale;   break;
                case 2: smCfg.infeasible_policy = slopmotion::InfeasiblePolicy::Reshape; break;
                case 3: smCfg.infeasible_policy =
                            slopmotion::InfeasiblePolicy::PrioritizeAmplitude; break;
                case 4: smCfg.infeasible_policy =
                            slopmotion::InfeasiblePolicy::PrioritizeSmooth;    break;
                case 5: smCfg.infeasible_policy = slopmotion::InfeasiblePolicy::Blend; break;
                default: /* leave slopmotion::Config's own default in place */  break;
            }
            smCfg.infeasible_scale_margin = g_state.sm_tune_infeas_margin;
            smCfg.infeasible_reshape_steps = g_state.sm_tune_reshape_steps;
            smCfg.settle_grace_us          = g_state.sm_tune_settle_grace_us;
            smCfg.chase_aim_accel_extrap  = g_state.sm_tune_aim_extrap;
            // Budgeted-policy spend limits + alpha-search depth (slopmotion
            // 0.8.0). Inert unless infeasible_policy is one of the two budgeted
            // ones; the engine clamps both budgets to [0,1] and the step count
            // to [1,10] itself, so pushing whatever the HTTP side stored is
            // safe — the clamp on the POST side exists to keep the echo honest,
            // not to protect the engine.
            smCfg.infeasible_smooth_budget    = g_state.sm_tune_smooth_budget;
            smCfg.infeasible_amplitude_budget = g_state.sm_tune_amp_budget;
            smCfg.infeasible_blend_steps      = g_state.sm_tune_blend_steps;
            // Curve family for waveform-segment reconstruction. Same shape as
            // the policy map above and for the same reason: this runs EVERY
            // TICK, so whatever it writes IS the engine's policy, and an
            // out-of-range stored value must fall through to slopmotion's own
            // default (smCfg is freshly default-constructed) rather than being
            // cast blindly into a family nobody selected.
            switch (g_state.sm_tune_curve_policy) {
                case 0: smCfg.curve_policy = slopmotion::CurvePolicy::FollowClient; break;
                case 1: smCfg.curve_policy = slopmotion::CurvePolicy::ForceC1;      break;
                case 2: smCfg.curve_policy = slopmotion::CurvePolicy::ForceC2;      break;
                default: /* leave slopmotion::Config's own default in place */      break;
            }
            // DC centering of a degraded band (slopmotion 0.5.0). The engine
            // clamps the gain itself; clamping on the POST side too just keeps
            // the /api/slopmotion echo honest about what Core 1 pushed.
            smCfg.wave_centering      = g_state.sm_tune_centering;
            smCfg.wave_centering_gain = g_state.sm_tune_centering_gain;
            // RFC-008 handoff sanity guard (0 = off). The guard itself only
            // engages when the INGRESS supplied a one-segment lookahead
            // (SlopSyncHubService::drainMotionStream), so this knob is the
            // aggressiveness dial + off switch, never the arming condition.
            smCfg.handoff_chord_factor = g_state.sm_tune_handoff_k;
            g_slopmotion.setConfig(smCfg);
            g_state.sm_eff_vmax = smCfg.limits.vmax;
            g_state.sm_eff_amax = smCfg.limits.amax;
            g_state.sm_eff_jmax = smCfg.limits.jmax;
        }

        // Drain the Core-0 -> Core-1 command handoff; each commit is ONE plan.
        // Plan time is the software-double cost — benched right here, where it
        // runs, and surfaced via GET /api/slopmotion (docs/canon doctrine
        // §SlopMotion part-2 gate).
        slopmotion::Command cmd;
        while (xQueueReceive(g_interp_queue, &cmd, 0) == pdTRUE) {
            const uint32_t t0 = (uint32_t)esp_timer_get_time();
            g_slopmotion.commit(cmd, nowUs);
            const uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
            g_state.sm_plan_us_last = dt;
            if (dt > g_state.sm_plan_us_max) g_state.sm_plan_us_max = dt;
            g_state.sm_plan_us_avg = g_state.sm_plan_us_avg <= 0.0f
                ? (float)dt
                : 0.9f * g_state.sm_plan_us_avg + 0.1f * (float)dt;
            // Joint census (sd-ar3 notch hunt). gap = this chord's DUE time vs
            // the previous chord's due+duration: sender-schedule contiguity,
            // now that anchored commits absorb release jitter. late = arrival
            // behind due; growth here means the client's send lead eroding.
            static uint64_t s_prevEndUs = 0;
            const uint64_t due = cmd.has_anchor ? cmd.anchor_us : nowUs;
            const int64_t gap_us = s_prevEndUs
                ? int64_t(due) - int64_t(s_prevEndUs) : 0;
            s_prevEndUs = due + cmd.duration_us;
            SLOGI("smplan", "tgt=%.3f T=%lums G=%c vf=%.3f gap=%+ld late=%ld us",
                  (double)cmd.target,
                  (unsigned long)(cmd.duration_us / 1000u),
                  cmd.has_end_vel ? 'y' : 'n', (double)cmd.end_vel,
                  (long)gap_us, (long)(int64_t(nowUs) - int64_t(due)));
        }

        if (streamActive) {
            float pos = g_slopmotion.positionAt(nowUs);
            float vel = g_slopmotion.velocityAt(nowUs);
            arbiter.submitStreamSample(pos, vel);

            // Publish telemetry for the WebUI planned-path overlay. Field
            // mapping onto the legacy interp_* slots (honest approximations;
            // proper SlopMotion telemetry lands with the WebUI refactor):
            // live_mode = chasing, grad_mode = quintic plan, style = Mode.
            slopmotion::Snapshot d = g_slopmotion.snapshot(nowUs);
            g_state.interp_start_pos   = d.start;
            g_state.interp_end_pos     = d.target;
            g_state.interp_cur_pos     = d.pos;
            g_state.interp_cur_vel     = d.vel;
            g_state.interp_duration_us = (uint32_t)(d.duration_s * 1e6f);
            g_state.interp_elapsed_us  = (uint32_t)(d.elapsed_s * 1e6f);
            g_state.interp_live_mode   = (d.mode == (uint8_t)slopmotion::Mode::Chase);
            g_state.interp_grad_mode   = (d.plan_kind == (uint8_t)slopmotion::PlanKind::Quintic ||
                                          d.plan_kind == (uint8_t)slopmotion::PlanKind::Cubic);
            g_state.interp_style       = d.mode;
            g_state.interp_active      = true;
            g_state.sm_mode      = d.mode;
            g_state.sm_plan_kind = d.plan_kind;
            g_state.sm_plans     = d.plans;
            g_state.sm_failures  = d.failures;
        } else if (wasActive) {
            g_state.interp_active = false;
        }

        // Drain the engine's anomaly ring: count (lossless), log (throttled),
        // and forward each edge to Core 0 for the SlopSync 0x0089 EVENT feed.
        {
            slopmotion::Anomaly ev;
            char nmbuf[8];
            while (g_slopmotion.popAnomaly(ev)) {
                // COUNT FIRST, UNCONDITIONALLY. The human log line below is
                // throttled (a 14-event replan burst would otherwise spam the
                // ring), so the counters are the only lossless record — they
                // must never sit behind a throttle or a log-level gate.
                g_state.sm_anomalies = g_state.sm_anomalies + 1;
                if (ev.kind < SystemState::SM_ANOM_KINDS)
                    g_state.sm_anom_kind[ev.kind] = g_state.sm_anom_kind[ev.kind] + 1;
                // Hand the EDGE to Core 0 for the 0x0089 motion-anomaly EVENT
                // channel. We cannot publish from here: the slopsync Hub is
                // single-task by invariant and that task is on Core 0, so the
                // anomaly crosses as data (SPSC ring in SystemState) and the
                // SlopSyncHub task turns it into a frame. Bounded, non-
                // blocking, allocation-free — safe on the 1 kHz sampler.
                // UNGATED by the ring's own name table on purpose: an unknown
                // kind still deserves to reach a subscriber (the catalog's
                // option labels stop at the last named kind, so a client shows
                // the ordinal — the same self-identifying behavior the log
                // line's "?<n>" gives, rather than silence).
                {
                    SystemState::SmAnomalyRec rec;
                    rec.t_us   = (uint32_t)(ev.t_us & 0xFFFFFFFFull);
                    rec.seq    = ev.seq;
                    rec.kind   = ev.kind;
                    rec.target = ev.target;
                    rec.detail = ev.detail;
                    g_state.smAnomalyPush(rec);
                }
                // Bound from the shared table's own size, never a literal — an
                // engine that grows a kind must not silently become "?" again.
                const char* nm;
                if (ev.kind < kSmAnomalyNameCount) {
                    nm = kSmAnomalyNames[ev.kind];
                } else {
                    // Unknown kind prints its ORDINAL ("?6"), so a stale table
                    // names the number to add rather than swallowing it.
                    snprintf(nmbuf, sizeof(nmbuf), "?%u", (unsigned)ev.kind);
                    nm = nmbuf;
                }
                // Info, not Debug: Debug sits below the default
                // SLOPLOG_COMPILE_LEVEL floor, so this line compiled out of
                // every stock build — the anomaly feed was invisible on the
                // device by construction.
                //
                // Log the KIND CHANGING, not every anomaly. A stream that
                // outruns the engine emits the same kind continuously, and the
                // old flat 1 Hz throttle turned that into a permanent 1/s
                // drip that said nothing new after the first line. The full
                // per-event feed lives on channel 0x0089 and in the
                // sm_anom_kind[] counters — this is the human mirror, so it
                // fires on transitions plus a 10 s "still happening" pulse.
                static uint8_t last_kind = 0xFF;
                static uint32_t last_kind_ms = 0;
                const uint32_t anom_now = millis();
                if (ev.kind != last_kind || (anom_now - last_kind_ms) >= 10000u) {
                    last_kind = ev.kind;
                    last_kind_ms = anom_now;
                    SLOGI("motion", "slopmotion %s target=%.3f detail=%.3f (total %lu)",
                          nm, (double)ev.target, (double)ev.detail,
                          (unsigned long)g_state.sm_anomalies);
                }
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
#if defined(FEATURE_RS485_MODBUS)
        // In Modbus motion-backend mode the bus is serviced from Core 1
        // (servoBusTask) instead — single-owner rule, ServoModbus is not
        // thread-safe to poll from two tasks. In FAS mode (the default,
        // backend 0) httpTask keeps servicing it here.
        if (g_motion_backend == 0) {
            TIME_STEP(servoModbus.update(),   "http:servoModbus");
            // While reg 0x00 reads 1 the drive IGNORES step/dir, so FAS emits
            // pulses into a deaf drive and the machine silently does not move.
            // No Modbus write clears 0x00 on this drive (sd-opb) -- reporting
            // it is the whole fix; the operator power-cycles the drive.
            const ServoTelemetry st = servoModbus.getTelemetry();
            if (st.valid && st.enabled) {
                SLOGW_EVERY_MS(30000, "servobus",
                               "drive reg 0x00 = 1: step/dir input IGNORED, motion will not "
                               "move. POWER-CYCLE THE DRIVE, Modbus cannot clear this.");
            }
        }
#endif
#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
        TIME_STEP(encoderValidator.update(), "http:encValidator");
#endif
        TIME_STEP(applogDrain(),          "http:logDrain");   // SlopLog ring -> web/serial sinks
#if defined(MOTION_PASSTHROUGH_BENCH)
        mlink::tick();   // 10 Hz ping; status lands in /api/diag/mlink
#endif
#if defined(FEATURE_RS485_MODBUS)
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

#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
// Core 1 — Modbus bus service task, ONLY created when g_motion_backend == 1.
// Guarded on DRIVER_AIM_SERVO too (not just FEATURE_RS485_MODBUS) because the
// body below now calls mbMotor.executorTick() — mbMotor only EXISTS inside
// the DRIVER_AIM_SERVO branch above. g_motion_backend can only ever be 1
// there too, so this doesn't lose any real configuration, just keeps a
// hypothetical FEATURE_RS485_MODBUS-without-DRIVER_AIM_SERVO build compiling.
// Phase 3: setpoint-first priority. Every 2ms
// tick, mbMotor.executorTick() runs FIRST — it only sends a setpoint from an
// IDLE bus (StreamedSetpointExecutor::onTick), so a setpoint due this tick
// always gets first crack at the wire. servoModbus.update() runs SECOND and
// spends whatever's left of the tick on Configure write-queue drain / config
// scan / telemetry-and-encoder poll rotation — its own internal spacing
// constants (POLL_INTERVAL_MS etc.) already keep that traffic from hogging
// the bus. A poll already in flight when a setpoint comes due can delay that
// setpoint by up to ~1 transaction (~4ms @19200, less @115200) — acceptable
// jitter for this phase; reprogramBaud(115200) below shrinks it.
static void servoBusTask(void* /*param*/) {
    while (true) {
        mbMotor.executorTick(esp_timer_get_time());
        servoModbus.update();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#endif


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
    // Runtime motion-backend selection (Phase 2 — see MotorProxy.h's static-init-trap note).
    // Read NVS ("machcfg"/backend) and bind the proxy to a concrete driver as
    // early as physically possible — BEFORE aimGeometryInit(), BEFORE
    // ConfigStore::load(), BEFORE motor.init()/applyDriverConfig(), before
    // ANY call through `motor` at all. Every one of those eventually reaches
    // MotorProxy::d(), which configASSERTs non-null — an unbound proxy is a
    // boot-order bug that must halt loudly, not limp along silently. NVS
    // itself is safe to read this early: the Arduino core's own startup
    // brings up nvs_flash_init() before setup() ever runs, well before
    // LittleFS.begin() below.
    g_motion_backend = machineBackendLoad();
#if defined(FEATURE_RS485_MODBUS)
    if (g_motion_backend == 1) {
        motor.bind(mbMotor);
        SLOGI("boot", "Motion backend: MODBUS direct-drive (skeleton mode — no motion until Phase 3)");
    } else {
#if defined(MOTION_PASSTHROUGH_BENCH)
        // Bench wiggle owns the SPI link; two masters would fight. Arbiter
        // motion is inert on this env and that is the point of the bench.
        motor.bind(fasMotor);
        SLOGI("boot", "Motion backend: FAS bound but INERT (bench wiggle owns the mlink)");
#else
        motor.bind(mlinkMotor);
        SLOGI("boot", "Motion backend: mlink -> RP2350 quadrature (drive saved 0x19=2)");
#endif
    }
#else
    motor.bind(mlinkMotor);
    SLOGI("boot", "Motion backend: mlink -> RP2350 quadrature (FEATURE_RS485_MODBUS not compiled)");
#endif
    webui.setMachineBackend(g_motion_backend);

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

#if defined(FEATURE_RS485_MODBUS)
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

    // Re-apply the driver config now that the bus is actually up: the earlier
    // motor.applyDriverConfig() call (right after motor.init()) ran BEFORE
    // servoModbus.init(), so in Modbus mode its queued register writes (output
    // state, torque clamp 0x18) were dropped by the !_ready guard. FAS mode is
    // untouched — its applyDriverConfig is a no-op either way.
    if (g_motion_backend == 1 && servoModbus.isReady()) {
        // SAME ordering trap as applyDriverConfig, and why nothing moved at fw
        // 2.4.3: ModbusServoDriver::init() runs BEFORE servoModbus.init(), so
        // its arm call hit the !_ready guard and did nothing. Arm HERE.
        servoModbus.armMotionControl();
        motor.applyDriverConfig(g_state.driver);
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
    arbiter.setInputAccelLimit(g_state.config.input_max_accel_mm_s2);

    // Wire PatternEngine to the arbiter so it submits intents instead of
    // calling motor directly.
    patternEngine.setArbiter(&arbiter);

    // Wire WebUI to the arbiter so the UI's applySettings can dispatch
    // live limit-set updates directly through the sole caller.
    webui.setArbiter(&arbiter);

    // SlopMotion command queue — Core 0 (SlopSync ingress) → Core 1 sampler
    g_interp_queue = xQueueCreate(INTERP_QUEUE_DEPTH, sizeof(slopmotion::Command));
    configASSERT(g_interp_queue != nullptr);

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
    // streamSamplerTask: Core 1, priority 4 — SlopMotion sampler. 16 KB stack:
    // commit() nests Ruckig temporaries (InputParameter 328 B + Trajectory
    // 2.2 KB per frame, measured on xtensa) — the 4 KB stack that fit the
    // cubic is exactly the cpp-safety.md T1 stack-bomb class waiting to recur.
    task_ok = xTaskCreatePinnedToCore(streamSamplerTask, "Sampler", 16384, nullptr, 4, nullptr, 1);
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
#if defined(FEATURE_RS485_MODBUS) && defined(DRIVER_AIM_SERVO)
    // servoBusTask: Core 1, priority 5 — ONLY when Modbus is the active
    // backend. Boot-order note: servoModbus.init() already ran earlier in
    // setup() (in the FEATURE_RS485_MODBUS block above, well before we get
    // here), so the bus is already probed/ready before this task starts
    // polling it — verified by reading through setup() top to bottom, not
    // assumed. In FAS mode (g_motion_backend == 0, the default) this task is
    // never created at all; httpTask keeps servicing servoModbus as it always
    // has (see the guard in httpTask above).
    if (g_motion_backend == 1) {
        task_ok = xTaskCreatePinnedToCore(servoBusTask, "ServoBus", 4096, nullptr, 5, nullptr, 1);
        configASSERT(task_ok == pdPASS);
    }
#endif

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
            slopSyncHub->setMotionStreamQueue(g_interp_queue);  // 0x0084 motion-input -> Core-1 sampler
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

#if defined(MOTION_PASSTHROUGH_BENCH)
    // RP2350 loom bring-up (sd-dxy): steal the drive pins for the matrix
    // route LAST, so no driver re-grabs them. Bench flag only; FAS still
    // believes it owns these pins, so do not command FAS motion here.
    motionPassthroughEnable();
    mlink::begin();
    SLOGW("boot", "!!! MOTION_PASSTHROUGH_BENCH: drive pins belong to the RP2350, FAS is a bystander.");
#endif

    SLOGI("boot", "System ready — push that thick shaft all the way in to home, or use the web UI :3");
}


// ---- loop() — idle ----------------------------------------------------------

void loop() {
    vTaskDelay(pdMS_TO_TICKS(100));
}