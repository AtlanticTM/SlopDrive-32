// rp2350_motion -- motion-coprocessor skeleton for the Waveshare RP2350-Zero.
// Constraints:
// - SPI SLAVE only; the S3 is the master and paces on the reported runway
//   (comms/MotionLinkProtocol.h is the one vocabulary, both ends -- T20).
// - The schedule is time-indexed: segments render at their own pace, never
//   faster (sd-dxy ruling). Underrun -> SETTLE at the last endpoint, never
//   extrapolation. ESTOP is handled in the SPI receive IRQ, ahead of the ring.
// - Pin choices here are solder-defined; change them WITH the loom, not before.
// - Output stage is a PIO stepgen: a one-instruction SM (out pins, 2) streams
//   absolute A/B levels at kStateHz; the 20 kHz tick renders trajectory into
//   states. Production and consumption share the crystal, so the joined TX
//   FIFO (8 words = 320 us) never drifts; an underrun HOLDS pins.
#include <Arduino.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/spi.h"

#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

#include "comms/MotionLinkProtocol.h"
#include "comms/RpFlashCore.h"
#include "slopglow/slopglow_core.hpp"

#include <Adafruit_NeoPixel.h>
#include <array>

using namespace motionlink;

// ---- Pins (RP2350-Zero) -----------------------------------------------------
// SPI1 corner cluster (operator loom preference). RP2350 pins carry FIXED
// SPI roles by position (mod 4: RX, CSn, SCK, TX), so within GP26..29+15 the
// legal set is exactly this; SCK cannot land on 29 nor CS on 15.
static constexpr uint8_t PIN_SPI_RX  = 28;  // S3 MOSI (GPIO38) -> here
static constexpr uint8_t PIN_SPI_CS  = 29;  // S3 CS   (GPIO48)
static constexpr uint8_t PIN_SPI_SCK = 26;  // S3 SCK  (GPIO7)
static constexpr uint8_t PIN_SPI_TX  = 27;  // -> S3 MISO (GPIO10)
static constexpr uint8_t PIN_IRQ     = 15;  // -> S3 IRQ (GPIO4), active HIGH
static constexpr uint8_t PIN_STEP    = 7;   // -> S3 GPIO1 (matrix -> drive PUL)
static constexpr uint8_t PIN_DIR     = 8;   // -> S3 GPIO2 (matrix -> drive DIR)
static constexpr uint8_t PIN_WS2812  = 16;  // RP2350-Zero onboard pixel

// ---- Segment ring (SPSC: SPI IRQ produces, stepper ISR consumes) ------------
// Indices are u8 and each side writes only its own; the ring never blocks.
static Segment s_ring[kSegmentDepth];
static volatile uint8_t s_head = 0;   // producer (SPI IRQ)
static volatile uint8_t s_tail = 0;   // consumer (stepper)
static volatile bool s_estop = false;
static volatile uint8_t s_flags = 0;
static volatile uint8_t s_lastSeq = 0;

static inline uint8_t ringDepth() { return uint8_t(s_head - s_tail); }

// ---- Stepper stub: 20 kHz alarm evaluating the schedule ---------------------
// Renders the active segment's quintic Hermite, emits step edges from a float
// accumulator. Position unit = steps at the drive input.
static float s_pos = 0.0f;        // rendered position (steps)
static float s_vel = 0.0f;
static float s_emitted = 0.0f;    // steps actually pulsed out
static uint32_t s_segElapsedUs = 0;
// Segment-entry edge. NOT elapsed==0: the advance carries a sub-tick
// remainder, and a retarget interlude leaves stale elapsed.
static bool s_segFresh = true;
static std::array<float, 6> s_qc{};  // active segment's quintic, normalized u
static float s_segTs = 0.0f;         // active segment duration, seconds
static volatile uint8_t s_state = kStateIdle;

// ---- Retarget state (kOpRetarget: trapezoid seek from live p,v) -------------
// All volatile: SPI IRQ writes, alarm IRQ reads; volatile-to-volatile order
// is preserved and single-word float stores are atomic on the M33.
static volatile bool  s_rtActive = false;
static volatile float s_rtTarget = 0.0f;
static volatile float s_rtVmax   = 0.0f;
static volatile float s_rtAccel  = 0.0f;

// ---- PIO quadrature stepgen -------------------------------------------------
// One-instruction SM (out pins, 2) streams ABSOLUTE A/B levels at kStateHz;
// the tick renders trajectory into 2-bit states, Bresenham-spread. Max one
// transition per state = kStateHz counts/s ceiling (drive input roof 500 kHz).
static constexpr uint32_t kTickUs = 50;   // 20 kHz trajectory tick
static PIO s_qpio = pio0;
static int s_qsm = -1;
static uint8_t  s_qphase = 0;
static uint32_t s_qword = 0;        // 16 states, LSB-first (shift-right OSR)
static uint8_t  s_qbits = 0;
static uint8_t  s_qphaseAtWord = 0; // rollback snapshot: FIFO-full drops a
static float    s_qemitAtWord = 0;  //   whole word, so un-count its motion
static uint32_t s_qdrops = 0;
// The tick owed more steps than one tick can pass. Plans clamp below
// kMaxCountsPerSec against a faster emitter, so a legal plan can never
// reach this: nonzero means an illegal plan, a late tick, or a
// discontinuous reference. Count it, never swallow it (sd-dxy.1.3).
static uint32_t s_emitOverrun = 0;
// Trajectory ticks that arrived late. Plan time advances by tick COUNT,
// so a late tick stretches the timeline while still hitting the
// endpoint -- correct position, wrong tempo (sd-dxy.1.5).
static uint32_t s_lateTicks = 0;
// Renderer speed ceiling in counts per TICK, 0 = unlimited (pre-handshake).
// Pushed as counts/s via kOpSetLimits; consumed ONLY by the emitter as a
// slew cap (emitTowardPos), never by the reference (note above renderTick).
static float    s_maxCountsPerTick = 0.0f;
static uint32_t s_velClamped = 0;   // emitter slew cap engagements
// NO margin above the ceiling: the 1.25x walk lost 20.5 mm of encoder
// agreement in one recovery on 2026-09-02 (the drive follows 1000 mm/s
// content with 0.01 mm deviation and does not follow 1250). Recovery walks
// at the ceiling, however long the gap takes.
static constexpr float kEmitCatchupMargin = 1.0f;
static constexpr uint32_t kStateHz = 400000;
static constexpr uint32_t kStatesPerTick = (kTickUs * kStateHz) / 1000000u;

static inline uint8_t phasePins(uint8_t ph) {
    // bit0 = A (GPIO7), bit1 = B (GPIO8); Gray 00, 01, 11, 10.
    constexpr uint8_t lut[4] = {0b00, 0b01, 0b11, 0b10};
    return lut[ph & 3u];
}

// Render this tick's owed transitions into pin states and feed the FIFO.
// Production (20 states / 50 us) equals consumption exactly and both clock
// from the crystal, so FIFO-full is a fault counter, not a design state.
static inline void emitTowardPos() {
    float delta = s_pos - s_emitted;
    const int dirStep = (delta >= 0.0f) ? 1 : -1;
    unsigned n = (unsigned)((delta >= 0.0f) ? delta : -delta);
    if (n > kStatesPerTick) { n = kStatesPerTick; ++s_emitOverrun; }
    // Slew cap: a reference discontinuity (settle re-anchor, sub-jump-guard
    // step) must not slew at the 400 kHz roof the drive drops counts at
    // (-108.75 mm, 2026-08-09). Caps the PULSE RATE only; s_pos stays honest,
    // so the trapezoid and jump guard are untouched -- this is NOT the
    // reverted reference clamp.
    if (s_maxCountsPerTick > 0.0f) {
        const unsigned cap =
            (unsigned)(s_maxCountsPerTick * kEmitCatchupMargin) + 1u;
        if (n > cap) { n = cap; ++s_velClamped; }
    }
    unsigned acc = 0;
    for (unsigned i = 0; i < kStatesPerTick; ++i) {
        acc += n;
        if (acc >= kStatesPerTick) {
            acc -= kStatesPerTick;
            // Inverted pair order = operator direction ruling 2026-08-08:
            // counts-increasing walks toward the HOME wall on this rig. The
            // full software mirror is a separate planned feature (sd-dxy).
            s_qphase = uint8_t((s_qphase + ((dirStep > 0) ? 3u : 1u)) & 3u);
            s_emitted += (float)dirStep;
        }
        s_qword |= (uint32_t)phasePins(s_qphase) << s_qbits;
        s_qbits = uint8_t(s_qbits + 2);
        if (s_qbits == 32u) {
            if (!pio_sm_is_tx_fifo_full(s_qpio, (uint)s_qsm)) {
                pio_sm_put(s_qpio, (uint)s_qsm, s_qword);
            } else {
                ++s_qdrops;
                s_qphase = s_qphaseAtWord;   // those transitions never left
                s_emitted = s_qemitAtWord;
            }
            s_qword = 0;
            s_qbits = 0;
            s_qphaseAtWord = s_qphase;
            s_qemitAtWord = s_emitted;
        }
    }
}

static void quadPioInit() {
    static const uint16_t prog[] = {
        (uint16_t)pio_encode_out(pio_pins, 2),
    };
    static const struct pio_program p = {prog, 1, -1, 0};
    const uint off = pio_add_program(s_qpio, &p);
    s_qsm = (int)pio_claim_unused_sm(s_qpio, true);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, off, off);
    sm_config_set_out_pins(&c, PIN_STEP, 2);   // GPIO7 = A, GPIO8 = B
    sm_config_set_set_pins(&c, PIN_STEP, 2);
    sm_config_set_out_shift(&c, true, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (float)kStateHz);
    pio_sm_init(s_qpio, (uint)s_qsm, off, &c);
    pio_sm_set_consecutive_pindirs(s_qpio, (uint)s_qsm, PIN_STEP, 2, true);
    // Glitch-free handover: park the pins at the current phase BEFORE the
    // funcsel switch, or the drive sees a spurious Gray jump to 00.
    pio_sm_exec(s_qpio, (uint)s_qsm, pio_encode_set(pio_pins, phasePins(s_qphase)));
    pio_gpio_init(s_qpio, PIN_STEP);
    pio_gpio_init(s_qpio, PIN_DIR);
    pio_sm_set_enabled(s_qpio, (uint)s_qsm, true);
}

static struct repeating_timer s_tick;

static void pumpSpiFrames();   // defined with the DMA slave section below

// REVERTED 2026-08-09. A rate limit on the RENDERED position is NOT a safety
// net -- it sits in the path of every move. Clamping s_pos makes it lag its own
// trajectory, and the lag then feeds two mechanisms that assume it does not:
// the retarget trapezoid keeps accelerating because dist never shrinks, and the
// segment-start teleport guard sees a reference that drifted for a legitimate
// reason. Live result: audible mechanical cracking. The REFERENCE is never
// rate-limited. The EMITTER is (emitTowardPos slew cap, 2026-08-09): capping
// pulse rate leaves s_pos honest and both mechanisms above intact, and lag
// shows up as counted residue instead of lost drive counts.
static void renderTick() {
    // Frame pump FIRST, and unconditionally: an ESTOP frame must act this
    // tick, and a latched estop must still process its kOpClear.
    pumpSpiFrames();

    if (s_rtActive) {
        // Trapezoid seek from live (p, v): accelerate toward the target,
        // capped at vmax AND at the brake parabola so it lands at v=0.
        constexpr float dt = 1e-6f * float(kTickUs);
        const float dist = s_rtTarget - s_pos;
        const float dir = (dist >= 0.0f) ? 1.0f : -1.0f;
        const float adist = dist * dir;
        float vTo = s_vel * dir;   // signed velocity TOWARD the target
        if (adist <= 1.0f && vTo <= s_rtAccel * dt * 4.0f) {
            s_pos = s_rtTarget;
            s_vel = 0.0f;
            s_state = kStateIdle;
            // Un-latch on landing: a latched retarget keeps the full trapezoid
            // + emitter running every tick forever, and that load starves the
            // SPI IRQ into ~20% torn frames (measured 2026-08-07).
            s_rtActive = false;
        } else {
            const float vBrake = sqrtf(2.0f * s_rtAccel * adist);
            float vLim = (s_rtVmax < vBrake) ? s_rtVmax : vBrake;
            vTo += s_rtAccel * dt;
            if (vTo > vLim) vTo = vLim;
            s_vel = dir * vTo;
            s_pos += s_vel * dt;
            s_state = kStateRunning;
        }
        return;
    }

    if (ringDepth() == 0) {
        if (s_state == kStateRunning) {
            // Underrun: SETTLE. Hold the last endpoint; never extrapolate.
            s_state = kStateSettled;
            s_flags |= kFlagUnderran;
            s_vel = 0.0f;
            s_segElapsedUs = 0;
            s_segFresh = true;
        }
        return;
    }

    const Segment& seg = s_ring[s_tail % kSegmentDepth];
    if (s_segFresh) {
        s_segFresh = false;
        // A p0 off the emitted position is WALKED at the ceiling, whatever its
        // size (counted in s_velClamped, visible as residue). A teleport is a
        // permanent calc/physical divergence: the 4096-count walk limit
        // teleported a 20 mm gap after a script seek on 2026-09-02 and the
        // encoder validator logged exactly that as lost steps (sd-dxy.1.7).
        // The only teleport left is the one with no ceiling to walk under
        // (nothing pushed yet), because an uncapped slew shoots.
        const float jump = seg.p0 - s_emitted;
        if (jump > 64.0f || jump < -64.0f) {
            const bool walkable = s_maxCountsPerTick > 0.0f;
            if (!walkable) {
                s_emitted += jump;
                s_flags |= kFlagJumped;
            }
        }
        // Quintic Hermite coefficients over normalized u. Accel knots ride
        // kOpSegment2 (a C1 chain steps accel at every knot: a 100 Hz torque
        // notch the servo renders as texture); legacy kOpSegment lands a=0.
        s_segTs = float(seg.duration_us) * 1e-6f;
        const float V0 = seg.v0 * s_segTs, V1 = seg.v1 * s_segTs;
        const float A0 = seg.a0 * s_segTs * s_segTs;
        const float A1 = seg.a1 * s_segTs * s_segTs;
        const float R1 = seg.p1 - seg.p0 - V0 - 0.5f * A0;
        const float R2 = V1 - V0 - A0;
        const float R3 = A1 - A0;
        s_qc = {seg.p0, V0, 0.5f * A0,
                10.0f * R1 - 4.0f * R2 + 0.5f * R3,
                -15.0f * R1 + 7.0f * R2 - R3,
                6.0f * R1 - 3.0f * R2 + 0.5f * R3};
    }
    s_state = kStateRunning;
    s_segElapsedUs += kTickUs;
    const float T = float(seg.duration_us);
    float t = float(s_segElapsedUs);
    if (t >= T) t = T;
    const float u = (T > 0.0f) ? (t / T) : 1.0f;
    s_pos = ((((s_qc[5] * u + s_qc[4]) * u + s_qc[3]) * u + s_qc[2]) * u +
             s_qc[1]) * u + s_qc[0];
    s_vel = (s_segTs > 0.0f)
                ? ((((5.0f * s_qc[5] * u + 4.0f * s_qc[4]) * u +
                     3.0f * s_qc[3]) * u + 2.0f * s_qc[2]) * u + s_qc[1]) /
                      s_segTs
                : 0.0f;


    if (s_segElapsedUs >= seg.duration_us) {
        s_tail = uint8_t(s_tail + 1);
        s_segFresh = true;
        // Carry the sub-tick remainder into the next segment: durations are
        // arbitrary us now, and zeroing here would leak up to one tick of
        // timeline per segment boundary.
        s_segElapsedUs -= seg.duration_us;
    }
    return;
}

// ONE emit call site, on purpose. emitTowardPos() used to be called only from
// the ticks that RENDERED, so every other exit -- ring empty, settle, retarget
// landing -- silently abandoned whatever (s_pos - s_emitted) the emitter still
// owed. That was unflagged, uncounted step loss, once per stroke (sd-dxy.1.2).
// Do NOT push this call back down into the branches: a fourth branch will be
// added one day and it will not get one.
// Tick liveness for the watchdog feed in loop(): a tick that stops advancing
// must reboot the coprocessor, not leave the motor frozen mid-plan.
static volatile uint32_t s_tickCount = 0;

static bool stepperTick(struct repeating_timer*) {
    // Tick lateness census. Plan time advances by tick COUNT, not wall clock,
    // so a tick that never ran is trajectory that silently never happened.
    static uint32_t s_lastTickUs = 0;
    const uint32_t tnow = time_us_32();
    if (s_lastTickUs != 0 && (tnow - s_lastTickUs) > (kTickUs + kTickUs / 2u))
        ++s_lateTicks;
    s_lastTickUs = tnow;
    ++s_tickCount;

    // Frame pump FIRST, and unconditionally: an ESTOP frame must act this
    // tick, and a latched estop must still process its kOpClear.
    pumpSpiFrames();
    if (s_estop) { s_state = kStateEstop; return true; }   // hold: no motion

    renderTick();
    emitTowardPos();
    return true;
}

// ---- Firmware update over the link (sd-4k1.3) -------------------------------
// A/B slot write fed by the S3, which is fed by the C5 bridge's OTA surface.
// Rollback is the RP2350 bootrom's TRY BEFORE YOU BUY, not ours: the freshly
// written slot is entered with a FLASH UPDATE boot, which runs it ONCE, and an
// image that never calls rom_explicit_buy() is abandoned at the next ordinary
// boot. A hand-rolled two-slot loader would put the one component that cannot
// be updated over the link on the link's critical path; this puts nothing
// there. See include/comms/RpFlashCore.h and dev board sd-4k1.3.
// The TBYB bit lives in the IMAGE_DEF and only the -tbyb build variant carries
// it (tools/rp2350_tbyb.py): the bootrom SKIPS a TBYB image on a normal boot,
// so the plain image is the USB rescue image and the variant is link-only.
// Rollback triggers on a REBOOT of an unbought image, never on a hang, which
// is what the watchdog below is for: a hung loop or a hung tick reboots, and
// an image that never proved the link reverts.
//
// The RP image version. This constant is its ONE home (C-1); the S3 reads it
// with kOpFlashVersion, which is what makes C-8 verification possible without
// a bench trip. Bump it with every image that goes out over the link.
static constexpr char kRpFwVersion[] = "0.1.2-rp";
static constexpr uint32_t kWatchdogMs = 8000;   // hardware max is 8388
static_assert(sizeof(kRpFwVersion) <= kFlashVersionBytes,
              "version string does not fit the status tail");

// The inactive half of the A/B pair, resolved from the bootrom's partition
// table. capacity() == 0 is the honest report when no partition table has been
// written yet (the one-time picotool step in build-test-deploy.md): the S3
// then reports kFlashDetailNoSlot instead of erasing something.
struct BootromSlot final : rpflash::IFlashSink {
    uint32_t base  = 0;   // flash storage offset of the target slot
    uint32_t bytes = 0;

    // Layout assumption, guaranteed by the documented picotool command: the
    // A/B pair is partitions 0 and 1. Anything else reports "no slot" rather
    // than guessing which region is safe to erase.
    void resolve() {
        base = bytes = 0;
        boot_info_t info;
        if (!rom_get_boot_info(&info) || info.partition < 0) return;
        const int b = rom_get_b_partition(0);
        if (b < 0) return;
        int target;
        if (info.partition == 0) target = b;
        else if (info.partition == b) target = 0;
        else return;
        uint32_t buf[4] = {};
        const int rc = rom_get_partition_table_info(
            buf, 4,
            PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION |
                (uint32_t(target) << 24));
        if (rc != 3) return;
        // PICOBIN partition-location word: first sector in bits 12:0, last
        // sector in bits 25:13, both inclusive and sector-granular. The
        // defining header (pico-sdk common/boot_picobin_headers/include/boot/
        // picobin.h, PICOBIN_PARTITION_LOCATION_*) is NOT on the arduino-pico
        // include path (lib/core_inc.txt carries boot_bootrom_headers only), so
        // the two shifts are restated here rather than reached for through a
        // relative path into the framework package.
        const uint32_t loc = buf[1];
        const uint32_t first = (loc & 0x1FFFu) * FLASH_SECTOR_SIZE;
        const uint32_t last = (((loc >> 13) & 0x1FFFu) + 1) * FLASH_SECTOR_SIZE;
        if (last <= first) return;
        base = first;
        bytes = last - first;
    }

    uint32_t capacity() const override { return bytes; }

    // Interrupts off and the other core parked for the erase and the program:
    // the framework's own EEPROM commit uses exactly this sequence, and
    // idleOtherCore() is a no-op when core 1 was never started. The 20 kHz tick
    // stops for the duration, which is the BACKPRESSURE the master paces on --
    // `want` cannot advance while a write is owed, so a stalled ack means
    // "busy", never "lost" (T33 rule 2 wears this shape here).
    bool writeSector(uint32_t off, const uint8_t* data, uint32_t n) override {
        if (bytes == 0 || n == 0 || off + FLASH_SECTOR_SIZE > bytes) return false;
        const uint32_t whole = n & ~(FLASH_PAGE_SIZE - 1u);
        noInterrupts();
        rp2040.idleOtherCore();
        flash_range_erase(base + off, FLASH_SECTOR_SIZE);
        if (whole != 0) flash_range_program(base + off, data, whole);
        rp2040.resumeOtherCore();
        interrupts();
        if (n > whole) {
            // flash_range_program takes whole pages only; pad the tail with
            // the erased value so the unwritten remainder reads back as 0xFF.
            static uint8_t tail[FLASH_PAGE_SIZE];
            memset(tail, 0xFF, sizeof(tail));
            memcpy(tail, data + whole, n - whole);
            noInterrupts();
            rp2040.idleOtherCore();
            flash_range_program(base + off + whole, tail, FLASH_PAGE_SIZE);
            rp2040.resumeOtherCore();
            interrupts();
        }
        return true;
    }

    // Read back through the NON-CACHED XIP window: the cache still holds the
    // pre-erase contents of anything just written, and a verify that reads the
    // cache is a verify of nothing.
    bool read(uint32_t off, uint8_t* out, uint32_t n) override {
        if (bytes == 0 || off + n > bytes) return false;
        memcpy(out,
               (const void*)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + base + off),
               n);
        return true;
    }
};

static BootromSlot s_flashSlot;
static rpflash::Receiver s_flashRx(s_flashSlot);
static volatile bool s_flashMode = false;
// end() reads the whole slot back, so it runs in loop(), never in the frame
// path. The frame only records the request.
static volatile bool s_flashEndReq = false;
static volatile uint32_t s_flashEndCrc = 0;
static volatile bool s_flashVerReq = false;
static uint32_t s_flashLastMs = 0;

// ---- Status frame, preloaded before every transaction -----------------------
static uint8_t s_statusBuf[kFrameBytes];

static uint16_t runwayMs() {
    // Remaining time in the active segment plus every queued one.
    uint32_t us = 0;
    const uint8_t depth = ringDepth();
    for (uint8_t i = 0; i < depth; ++i)
        us += s_ring[uint8_t(s_tail + i) % kSegmentDepth].duration_us;
    if (depth != 0 && us >= s_segElapsedUs) us -= s_segElapsedUs;
    uint32_t ms = us / 1000u;
    return ms > 0xFFFF ? 0xFFFF : uint16_t(ms);
}

// The version string overlays the telemetry tail for ONE reply, on request.
// Motion telemetry is meaningless while the S3 is asking what image this is,
// and the frame has no spare bytes to grow into.
static void stampVersion() {
    memset(&s_statusBuf[kFlashStatusOffVersion], 0, kFlashVersionBytes);
    memcpy(&s_statusBuf[kFlashStatusOffVersion], kRpFwVersion,
           sizeof(kRpFwVersion) - 1);
}

// Flash-mode overlay: `pos` and `vel` carry no meaning with motion held, so
// the flash fields reuse their bytes (offsets from MotionLinkProtocol.h --
// neither end transcribes a number, T20).
static void preloadFlashStatus() {
    memset(s_statusBuf, 0, kFrameBytes);
    s_statusBuf[0] = kStateFlash;
    s_statusBuf[1] = s_flags;
    s_statusBuf[5] = s_lastSeq;
    rpflash::writeFlashStatus(s_statusBuf, s_flashRx.want(), s_flashRx.result(),
                              s_flashRx.detail());
    s_statusBuf[14] = 0xA5;
    s_statusBuf[15] = s_lastSeq;
    stampVersion();
    crcStamp(s_statusBuf);
    digitalWrite(PIN_IRQ, LOW);
}

static void preloadStatus() {
    if (s_flashMode) { preloadFlashStatus(); return; }
    const uint16_t rw = runwayMs();
    s_statusBuf[0] = s_state;
    s_statusBuf[1] = s_flags;
    // JUMPED self-clears once reported: sticky, it logged only the FIRST
    // teleport of a session and hid every later one (2026-08-09 drift hunt).
    s_flags &= uint8_t(~kFlagJumped);
    s_statusBuf[2] = uint8_t(rw);
    s_statusBuf[3] = uint8_t(rw >> 8);
    s_statusBuf[4] = ringDepth();
    s_statusBuf[5] = s_lastSeq;
    memcpy(&s_statusBuf[6], (const void*)&s_pos, 4);
    memcpy(&s_statusBuf[10], (const void*)&s_vel, 4);
    s_statusBuf[14] = 0xA5;          // alignment signature (bench)
    s_statusBuf[15] = s_lastSeq;     // seq duplicate for offset hunting
    // Renderer truth (sd-dxy.1.1). `pos` above is COMMANDED; these say what the
    // emitter actually did. Offsets come from MotionLinkProtocol.h -- both ends
    // read the same constants, neither transcribes a number (T20).
    const float emitted = s_emitted;
    memcpy(&s_statusBuf[kStatusOffEmitted], &emitted, 4);
    const uint16_t qd = sat16(s_qdrops), ov = sat16(s_emitOverrun),
                   lt = sat16(s_lateTicks);
    memcpy(&s_statusBuf[kStatusOffQDrops], &qd, 2);
    memcpy(&s_statusBuf[kStatusOffEmitOverrun], &ov, 2);
    memcpy(&s_statusBuf[kStatusOffLateTicks], &lt, 2);
    const uint16_t vc = sat16(s_velClamped);
    memcpy(&s_statusBuf[kStatusOffVelClamped], &vc, 2);
    if (s_flashVerReq) { s_flashVerReq = false; stampVersion(); }
    crcStamp(s_statusBuf);
    // Feed-me line: the producer paces on this, not on polling cadence.
    digitalWrite(PIN_IRQ, (rw < kRunwayLowMs && !s_estop) ? HIGH : LOW);
}

// ---- Frame processor (SPI IRQ context: short, no allocation) ----------------
static uint32_t s_badCrc = 0;
static uint32_t s_torn = 0;
// Segment dedup: the master resends a segment with the SAME seq until the
// status seq echo acks it, so a lost-ack resend of a segment that DID land
// must be dropped here, not queued twice. >255 = none seen.
static volatile uint16_t s_lastSegSeq = 0xFFFF;

static void processFrame(uint8_t* data, size_t len) {
    // Whole verified frames only: a torn or corrupted frame is DROPPED, never
    // partially parsed. The master's credit loop re-sends what the status
    // never acknowledged; estop is repeated until the echoed state confirms.
    if (len != kFrameBytes ||
        !crcOk(std::span<const uint8_t, kFrameBytes>(data, kFrameBytes))) {
        ++s_badCrc;
        return;
    }
    s_lastSeq = data[1];
    // A firmware write owns the board. Every motion op is refused for its
    // duration, so a stale master frame cannot start a move into a half-
    // written slot; the estop latched by kOpFlashBegin holds the pins.
    if (s_flashMode && data[0] < kOpFlashBegin) return;
    switch (data[0]) {
        case kOpEstop:   // ahead of the ring, by design
            s_estop = true;
            s_rtActive = false;
            s_head = s_tail = 0;
            s_segElapsedUs = 0;
            s_segFresh = true;
            s_lastSegSeq = 0xFFFF;
            break;
        case kOpClear:
            s_estop = false;
            s_rtActive = false;   // never resume a pre-estop target
            s_head = s_tail = 0;
            s_segElapsedUs = 0;
            s_segFresh = true;
            s_flags = 0;
            s_state = kStateIdle;
            s_lastSegSeq = 0xFFFF;
            break;
        case kOpSegment: {
            if (len < 2 + kSegmentWireBytes) break;
            if (data[1] == s_lastSegSeq) break;   // lost-ack resend duplicate
            if (ringDepth() >= kSegmentDepth) { s_flags |= kFlagOverflow; break; }
            Segment seg;
            memcpy(&seg.duration_us, data + 2, 4);
            memcpy(&seg.p0, data + 6, 4);
            memcpy(&seg.v0, data + 10, 4);
            memcpy(&seg.p1, data + 14, 4);
            memcpy(&seg.v1, data + 18, 4);
            seg.a0 = 0.0f;
            seg.a1 = 0.0f;
            s_ring[s_head % kSegmentDepth] = seg;
            s_head = uint8_t(s_head + 1);
            s_lastSegSeq = data[1];               // only a QUEUED seq dedups
            s_flags &= uint8_t(~kFlagUnderran);
            s_rtActive = false;   // segments reclaim the renderer
            break;
        }
        case kOpSegment2: {
            if (len < 2 + kSegment2WireBytes) break;
            if (data[1] == s_lastSegSeq) break;   // lost-ack resend duplicate
            if (ringDepth() >= kSegmentDepth) { s_flags |= kFlagOverflow; break; }
            Segment seg;
            memcpy(&seg.duration_us, data + 2, 4);
            memcpy(&seg.p0, data + 6, 4);
            memcpy(&seg.v0, data + 10, 4);
            memcpy(&seg.a0, data + 14, 4);
            memcpy(&seg.p1, data + 18, 4);
            memcpy(&seg.v1, data + 22, 4);
            memcpy(&seg.a1, data + 26, 4);
            s_ring[s_head % kSegmentDepth] = seg;
            s_head = uint8_t(s_head + 1);
            s_lastSegSeq = data[1];               // only a QUEUED seq dedups
            s_flags &= uint8_t(~kFlagUnderran);
            s_rtActive = false;   // segments reclaim the renderer
            break;
        }
        case kOpSetPos: {   // standstill zero-set (homing)
            if (len < 2 + 4) break;
            float np;
            memcpy(&np, data + 2, 4);
            s_rtActive = false;
            s_head = s_tail = 0;
            s_segElapsedUs = 0;
            s_segFresh = true;
            // Flush forgets the seq, else the next real segment reads as a dup.
            s_lastSegSeq = 0xFFFF;
            s_pos = np;
            s_emitted = np;
            // Rollback snapshots too, or a later FIFO-full rollback would
            // restore a pre-home reference.
            s_qemitAtWord = np;
            s_qphaseAtWord = s_qphase;
            s_vel = 0.0f;
            s_state = kStateIdle;
            break;
        }
        case kOpSetLimits: {
            if (len < 2 + 4) break;
            float cps;
            memcpy(&cps, data + 2, 4);
            // Reject nonsense rather than latching it: a bad ceiling here
            // silently throttles every move the machine will ever make.
            // Enforced at the EMITTER only (slew cap in emitTowardPos);
            // the reference stays unclamped -- REVERTED note above renderTick.
            if (cps > 0.0f && cps < 2.0e6f)
                s_maxCountsPerTick = cps * (float(kTickUs) * 1e-6f);
            break;
        }
        case kOpRetarget: {
            if (len < 2 + 12) break;
            float t = 0, v = 0, a = 0;
            memcpy(&t, data + 2, 4);
            memcpy(&v, data + 6, 4);
            memcpy(&a, data + 10, 4);
            if (!(a > 0.0f) || !(v > 0.0f)) break;   // rejects NaN too
            if (v > kMaxCountsPerSec) v = kMaxCountsPerSec;
            s_rtTarget = t;
            s_rtVmax   = v;
            s_rtAccel  = a;
            s_head = s_tail;        // last command wins: drop queued segments
            s_segElapsedUs = 0;
            s_segFresh = true;
            // Flush forgets the seq, else the next real segment reads as a dup.
            s_lastSegSeq = 0xFFFF;
            s_rtActive = true;
            break;
        }
        case kOpFlashBegin: {
            if (len < 2 + 4) break;
            uint32_t sz = 0;
            memcpy(&sz, data + 2, 4);
            // ONE gate, the same one estop uses: motion is held for the whole
            // transfer rather than a second flash-only interlock.
            s_estop = true;
            s_rtActive = false;
            s_head = s_tail = 0;
            s_segElapsedUs = 0;
            s_segFresh = true;
            s_lastSegSeq = 0xFFFF;
            s_state = kStateEstop;
            s_flashSlot.resolve();
            s_flashRx.begin(sz);
            s_flashEndReq = false;
            s_flashLastMs = millis();
            s_flashMode = true;
            break;
        }
        case kOpFlashData: {
            if (!s_flashMode || len < kFlashChunkOffset) break;
            const uint32_t off = uint32_t(data[2]) |
                                 (uint32_t(data[3]) << 8) |
                                 (uint32_t(data[4]) << 16);
            // Staging only. The erase and the program happen in loop(): a
            // receive path that writes flash cannot also service the link.
            s_flashRx.data(off, data + kFlashChunkOffset, data[5]);
            break;
        }
        case kOpFlashEnd: {
            if (!s_flashMode || len < 2 + 4) break;
            uint32_t crc = 0;
            memcpy(&crc, data + 2, 4);
            s_flashEndCrc = crc;
            s_flashEndReq = true;   // loop() verifies: end() reads the slot
            break;
        }
        case kOpFlashAbort:
            s_flashRx.abort();
            s_flashEndReq = false;
            s_flashMode = false;
            break;
        case kOpFlashStatus:
            break;                  // the preloaded status is the whole answer
        case kOpFlashVersion:
            s_flashVerReq = true;
            break;
        default:   // kOpPing and future ops: status answers regardless
            break;
    }
}

// ---- Raw PL022 slave, DMA-drained -------------------------------------------
// The IRQ collector raced an 8-byte FIFO with an ~8 us entry budget; any rare
// us-scale latency source overran it silently (~1 torn frame per kiloframe
// under motion load, every mitigation tried). DMA lands bytes in a ring with
// no latency budget at all; the 20 kHz tick pumps completed frames, so ESTOP
// still acts within 50 us. TX is a per-frame 32-byte DMA paced by the DREQ.
static uint8_t s_rxFrame[kFrameBytes];
static int s_dmaRx = -1;
static int s_dmaTx = -1;
static uint8_t __attribute__((aligned(256))) s_rxRing[256];
static uint32_t s_rxRead = 0;      // ring offset of the next unparsed byte
static uint32_t s_frames = 0;

// spi_init's RESETS-block reset is the ONLY thing that clears a PL022 TX
// FIFO -- an SSE cycle does not (measured 2026-08-07: +8 reply shift,
// self-sustaining). Never revert this to an SSE toggle.
static void spiConfigure() {
    spi_init(spi1, kSpiHz);
    spi_set_slave(spi1, true);
    static_assert(kSpiMode == 1, "PL022 slave needs CPHA=1 for held-low CS");
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
}

static void armRx() {
    dma_channel_abort(s_dmaRx);
    s_rxRead = 0;
    dma_channel_set_write_addr(s_dmaRx, s_rxRing, false);
    dma_channel_set_trans_count(s_dmaRx, 0x0FFFFFFFu, true);
}

static void armTx() {
    dma_channel_abort(s_dmaTx);
    dma_channel_set_read_addr(s_dmaTx, s_statusBuf, false);
    dma_channel_set_trans_count(s_dmaTx, kFrameBytes, true);
}

static inline uint32_t rxWriteOff() {
    return (uint32_t)dma_channel_hw_addr(s_dmaRx)->write_addr
         - (uint32_t)s_rxRing;
}

// Frame pump -- called ONLY from stepperTick (single consumer of s_rxRead).
// A clean 32/32 exchange leaves the TX FIFO drained by the master, so the
// happy path re-arms both DMAs with NO block reset; the reset survives only
// in the tear path, where stale TX bytes genuinely need flushing.
static void pumpSpiFrames() {
    const uint32_t avail = (rxWriteOff() - s_rxRead) & 255u;
    if (avail >= kFrameBytes) {
        for (uint32_t i = 0; i < kFrameBytes; i++)
            s_rxFrame[i] = s_rxRing[(s_rxRead + i) & 255u];
        s_rxRead = (s_rxRead + kFrameBytes) & 255u;
        processFrame(s_rxFrame, kFrameBytes);
        s_frames++;
        preloadStatus();
        armTx();
        // Refill the RX count during the >=200 us inter-frame gap, long
        // before its 268 MB budget runs dry. Only with no partial pending:
        // armRx resets the read pointer.
        if (dma_channel_hw_addr(s_dmaRx)->transfer_count < (1u << 16) &&
            ((rxWriteOff() - s_rxRead) & 255u) == 0) {
            armRx();
        }
    } else if (avail != 0 && gpio_get(PIN_SPI_CS)) {
        // CS idle-high with a partial frame = torn. Discard it, flush the
        // stale TX reply by block reset, re-arm both sides.
        ++s_torn;
        dma_channel_abort(s_dmaTx);
        dma_channel_abort(s_dmaRx);
        spiConfigure();
        preloadStatus();
        armRx();
        armTx();
    }
}

static void spiSlaveBegin() {
    gpio_set_function(PIN_SPI_RX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_TX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_CS, GPIO_FUNC_SPI);
    spiConfigure();
    s_dmaRx = dma_claim_unused_channel(true);
    {
        dma_channel_config c = dma_channel_get_default_config(s_dmaRx);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, 8);     // 256-byte write ring
        channel_config_set_dreq(&c, spi_get_dreq(spi1, false));
        dma_channel_configure(s_dmaRx, &c, s_rxRing,
                              &spi_get_hw(spi1)->dr, 0, false);
    }
    s_dmaTx = dma_claim_unused_channel(true);
    {
        dma_channel_config c = dma_channel_get_default_config(s_dmaTx);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, spi_get_dreq(spi1, true));
        dma_channel_configure(s_dmaTx, &c, &spi_get_hw(spi1)->dr,
                              s_statusBuf, 0, false);
    }
    preloadStatus();
    armRx();
    armTx();
}

// ---- SlopGlow on the onboard WS2812 -----------------------------------------
static Adafruit_NeoPixel s_px(1, PIN_WS2812, NEO_GRB + NEO_KHZ800);
struct PixelOut final : slopglow::IGlowOutput {
    slopglow::Rgb c{};
    size_t pixelCount() const override { return 1; }
    void set(size_t, slopglow::Rgb v) override { c = v; }
    void show() override {
        // Core gamma (ONE curve fleet-wide, not Adafruit's near-twin), then
        // brightness in DUTY space: pre-gamma dimming quantizes to a handful
        // of codes (measured on the C5, 2026-08-06). Adafruit setBrightness
        // is that same pre-gamma trap, which is why it goes unused.
        auto s = [](uint8_t v) {
            // 9/256 ~ 3.5% duty: bench-distance peripheral vision (operator-
            // tuned 2026-08-08; 30% was a butt-wiggle/LED mixup).
            return uint8_t((uint16_t(slopglow::gamma8(v)) * 9u) >> 8);
        };
        s_px.setPixelColor(0, s(c.r), s(c.g), s(c.b));
        s_px.show();
    }
};
static PixelOut s_pixel;
static slopglow::GlowEngine s_glow(s_pixel);
static slopglow::HeartbeatSource* s_glowHb = nullptr;

// ---- Firmware update: the task-context half (sd-4k1.3) ----------------------

// TRY BEFORE YOU BUY. A flash-update boot runs this image ONCE; unless it buys
// itself, the next ordinary boot returns to the other slot. The proof required
// is the LINK -- one CRC-valid frame from the S3 means this image can be
// commanded. An image that cannot be commanded must not be able to keep
// itself, and that is the whole of the rollback.
static void flashBuyIfProven() {
    static bool s_bought = false;
    if (s_bought || s_frames == 0) return;
    s_bought = true;
    if (rom_get_last_boot_type() != BOOT_TYPE_FLASH_UPDATE) return;
    // rom_explicit_buy erases and rewrites the sector holding the flag, so it
    // needs 4 KiB of word-aligned scratch. Called under the same
    // interrupts-off, other-core-parked window every flash write here uses
    // rather than through flash_safe_execute: this sketch never starts core 1,
    // so the SDK helper's multicore lockout has nothing to hand off to.
    static uint8_t __attribute__((aligned(4))) s_buyScratch[4096];
    rom_explicit_buy_fn buy =
        (rom_explicit_buy_fn)rom_func_lookup(ROM_FUNC_EXPLICIT_BUY);
    if (buy == nullptr) return;
    noInterrupts();
    rp2040.idleOtherCore();
    const int rc = buy(s_buyScratch, sizeof(s_buyScratch));
    rp2040.resumeOtherCore();
    interrupts();
    Serial.printf("[flash] tbyb buy rc=%d fw=%s\n", rc, kRpFwVersion);
}

// A master that dies mid-transfer must not latch motion off forever: that is
// the latched-gate class this project has been bitten by twice (sd-emy). The
// running image is untouched by an abort, so timing out costs nothing.
static constexpr uint32_t kFlashIdleAbortMs = 10000;

static void flashServiceLoop() {
    using namespace slopglow;
    // The receiver's task-context half. The write parks the tick and with it
    // the frame pump, so `want` stops advancing and the master backs off --
    // that stall IS the flow control on this hop, not a symptom.
    const uint32_t want = s_flashRx.want();
    if (s_flashRx.pending()) s_flashRx.service();
    if (s_flashEndReq && !s_flashRx.pending()) {
        s_flashEndReq = false;
        if (s_flashRx.end(s_flashEndCrc)) {
            preloadStatus();   // the master reads kFlashDone before the reboot
            Serial.printf("[flash] verified %u B at 0x%06lx, entering it\n",
                          unsigned(s_flashRx.size()),
                          (unsigned long)s_flashSlot.base);
            // BOOT_TYPE_FLASH_UPDATE is the same value as the picoboot
            // REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE (bootrom_constants.h says
            // so in as many words); that header is the one of the pair on the
            // arduino-pico include path. The reboot is asynchronous, so the
            // return code is the only report of a refusal, and 200 ms of delay
            // is what lets the master read kFlashDone first.
            if (rom_reboot(BOOT_TYPE_FLASH_UPDATE, 200, s_flashSlot.base, 0) != 0)
                s_flashRx.fail(kFlashDetailBuyFail);   // the running image stands
        }
    }
    // Flash mode is left by the master's abort, never on our own success:
    // a self-clear here would swap the status overlay out from under the very
    // read that reports the result.
    if (want != s_flashRx.want() || s_flashRx.pending()) s_flashLastMs = millis();
    if (millis() - s_flashLastMs > kFlashIdleAbortMs) {
        Serial.printf("[flash] idle %u ms -- aborting, running image intact\n",
                      unsigned(millis() - s_flashLastMs));
        s_flashRx.abort();
        s_flashMode = false;
    }
    s_glow.set(System::Flash, Status::Working);
    s_glowHb->pulse();
    s_glow.update(millis());
}


#if defined(MLINK_WIRE_PROBE)
// ---- Wire probe (no scope on the bench): counts what actually arrives -------
// SPI is NOT initialized in this build; the pads are plain inputs. CS falls
// are counted by interrupt (10 Hz, easy); SCK/MOSI are 8 MHz bursts, so a
// tight poll just answers "any activity this second".
static volatile uint32_t s_csFalls = 0;
static void csIsr() { s_csFalls = s_csFalls + 1; }

void setup() {
    Serial.begin(115200);
    pinMode(PIN_SPI_CS, INPUT);
    pinMode(PIN_SPI_SCK, INPUT);
    pinMode(PIN_SPI_RX, INPUT);
    pinMode(PIN_IRQ, OUTPUT);
    digitalWrite(PIN_IRQ, HIGH);   // keep the S3's irq=1 liveness tell
    attachInterrupt(digitalPinToInterrupt(PIN_SPI_CS), csIsr, FALLING);
    s_px.begin();
}

void loop() {
    const uint32_t c0 = s_csFalls;
    bool sck = false, mosi = false;
    const uint32_t t0 = millis();
    while (millis() - t0 < 1000) {
        if (digitalRead(PIN_SPI_SCK)) sck = true;
        if (digitalRead(PIN_SPI_RX)) mosi = true;
    }
    const uint32_t falls = s_csFalls - c0;
    Serial.printf("[probe] cs_falls=%lu/s sck_activity=%d mosi_activity=%d\n",
                  (unsigned long)falls, int(sck), int(mosi));
    // Pixel verdict: green = CS arriving, red = silent bus.
    s_px.setPixelColor(0, falls ? 0 : 40, falls ? 40 : 0, 0);
    s_px.show();
}
#else
void setup() {
    Serial.begin(115200);   // USB CDC status; printf is the Zero's only log
    pinMode(PIN_IRQ, OUTPUT);
    digitalWrite(PIN_IRQ, LOW);
    quadPioInit();   // owns PIN_STEP/PIN_DIR from here on (A/B via PIO)

    s_px.begin();
    // Engine brightness stays 255; the adapter dims post-gamma in duty space.
    // Rainbow until the S3 speaks: an unwired or dead link never fakes ready.
    s_glow.requireReady(uint8_t(1u << uint8_t(slopglow::System::Link)));
    s_glowHb = s_glow.addHeartbeat(500);

    spiSlaveBegin();

    add_repeating_timer_us(-int32_t(kTickUs), stepperTick, nullptr, &s_tick);
    // Hardware watchdog, fed from loop() only while the tick advances. Every
    // legitimate stall sits far under 8 s: a 4 KB sector erase+program is
    // ~50 ms and rom_explicit_buy rewrites one sector.
    watchdog_enable(kWatchdogMs, true);
}

void loop() {
    using namespace slopglow;
    {
        static uint32_t s_fedAt = 0;
        const uint32_t t = s_tickCount;
        if (t != s_fedAt) { s_fedAt = t; watchdog_update(); }
    }
    flashBuyIfProven();
    if (s_flashMode) { flashServiceLoop(); return; }
    if (s_lastSeq != 0 || s_state != kStateIdle) s_glow.markReady(System::Link);
    s_glow.set(System::Safety, s_estop ? Status::Urgent : Status::Nominal);
    s_glow.set(System::Motion,
               s_state == kStateRunning   ? Status::Working
               : (s_flags & kFlagUnderran) ? Status::Degraded
                                           : Status::Nominal);
    // Frames, tears and the status snapshot are ALL the tick pump's business
    // now: one consumer, IRQ context, 50 us cadence. loop() only observes.
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();
        Serial.printf("[mlink] lastSeq=%u badCrc=%lu torn=%lu qdrops=%lu "
                      "frames=%lu dmaRemain=%lu sspsr=0x%02lx cs=%d\n",
                      unsigned(s_lastSeq), (unsigned long)s_badCrc,
                      (unsigned long)s_torn, (unsigned long)s_qdrops,
                      (unsigned long)s_frames,
                      (unsigned long)dma_channel_hw_addr(s_dmaRx)->transfer_count,
                      (unsigned long)spi_get_hw(spi1)->sr,
                      int(gpio_get(PIN_SPI_CS)));
    }
    s_glowHb->pulse();
    s_glow.update(millis());
    delay(2);
}
#endif  // MLINK_WIRE_PROBE
