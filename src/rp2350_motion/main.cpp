// rp2350_motion -- the motion processor: it OWNS the plan (Waveshare RP2350-Zero).
// Constraints:
// - GLUE ONLY. The engine, the config image, the command queue, the event ring
//   and the render hand-off live in include/comms/RpMotionCore.h, which is
//   hardware-free and host-tested. This file is the SPI DMA slave, the PIO
//   stepgen, the flash receiver, the watchdog and the LEDs.
// - SPI SLAVE only; comms/MotionLinkProtocol.h is the one vocabulary, both
//   ends (T20). Commands arrive as ANCHORED INTENTS and are evaluated here
//   (architecture.md section 2, three-board split); nothing is pre-rendered on
//   the S3 and there is no runway to starve.
// - CORE SPLIT. Core 0 runs the 20 kHz tick: it pumps frames, evaluates the
//   PUBLISHED render plan and feeds the emitter. Core 1 runs the engine:
//   commit() is milliseconds and must never sit in the tick's way. The SPI IRQ
//   only decodes and enqueues.
// - The emitter slew cap is a FAULT DETECTOR: it counts (s_velClamped) and
//   never shapes the reference. The reference is never rate-limited.
// - ESTOP is handled in the frame path, ahead of everything, and acts within
//   one 50 us tick.
// - Pin choices here are solder-defined; change them WITH the loom, not before.
// - Output stage is a PIO stepgen: a one-instruction SM (out pins, 2) streams
//   absolute A/B levels at kStateHz; the tick renders trajectory into states.
//   Production and consumption share the crystal, so the joined TX FIFO
//   (8 words = 320 us) never drifts; an underrun HOLDS pins.
// See: docs/rp-motion-port.md, dev board sd-4k1.4.
#include <Arduino.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/resets.h"
#include "hardware/spi.h"
#include "hardware/timer.h"

#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"

#include "comms/MotionLinkProtocol.h"
#include "comms/RpFlashCore.h"
#include "comms/RpMotionCore.h"
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

// ---- The motion core --------------------------------------------------------
// Two slopmotion engines (the live one and republish()'s shadow) plus the
// queue and the rings, in .bss. The RP2350's 520 KB of SRAM carries it; nothing
// here allocates after construction.
static rpmotion::Core s_core;

// Rendered reference and what the emitter actually pulsed. `s_pos` is written
// by the tick and read by the status preload, both on core 0.
static float s_pos = 0.0f;        // counts, evaluated from the published plan
static float s_vel = 0.0f;        // counts/s
static float s_emitted = 0.0f;    // counts actually pulsed out
static uint8_t s_lastState = kStateIdle;

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
// Trajectory ticks that arrived late. Plan time is read from the clock, so a
// late tick is a sample that never happened rather than a stretched timeline.
static uint32_t s_lateTicks = 0;
// Renderer speed ceiling in counts per TICK, 0 = unlimited (pre-handshake).
// Pushed as counts/s via kOpSetLimits; consumed ONLY by the emitter as a slew
// cap, never by the reference. A rate limit on the RENDERED position is not a
// safety net: it sits in the path of every move and makes the reference lag its
// own trajectory (audible mechanical cracking, 2026-08-09). Capping the PULSE
// RATE leaves the reference honest and shows lag as counted residue instead.
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
static inline void __not_in_flash_func(emitTowardPos)() {
    float delta = s_pos - s_emitted;
    const int dirStep = (delta >= 0.0f) ? 1 : -1;
    unsigned n = (unsigned)((delta >= 0.0f) ? delta : -delta);
    if (n > kStatesPerTick) { n = kStatesPerTick; ++s_emitOverrun; }
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



// Tick liveness for the watchdog feed in loop(): a tick that stops advancing
// must reboot the coprocessor, not leave the motor frozen mid-plan.
static volatile uint32_t s_tickCount = 0;

static void __not_in_flash_func(stepperTick)() {
    // Tick lateness census. A tick that never ran is a sample that never
    // happened; the plan is a function of TIME, so the position after a late
    // tick is still correct and only the emitter had less runway.
    static uint32_t s_lastTickUs = 0;
    const uint32_t tnow = time_us_32();
    if (s_lastTickUs != 0 && (tnow - s_lastTickUs) > (kTickUs + kTickUs / 2u))
        ++s_lateTicks;
    s_lastTickUs = tnow;
    ++s_tickCount;

    // Frames are pumped from the CS rising-edge ISR (csRiseIsr), which sits
    // BELOW this timer's priority: frame processing is longer than a tick
    // slot and cost one late tick per frame when it lived here (2026-09-03,
    // late=100/s at the 10 ms poll; the gate wants 0).
    if (s_core.estopped()) return;   // hold: the reference stops advancing

    s_core.sampleCounts(tnow, s_pos, s_vel);
    // ONE emit call site, on purpose. Every other exit used to abandon whatever
    // (s_pos - s_emitted) the emitter still owed: unflagged, uncounted step
    // loss, once per stroke (sd-dxy.1.2).
    emitTowardPos();
    return;
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
static constexpr char kRpFwVersion[] = "0.2.6-rp";
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
    // the framework's own EEPROM commit uses exactly this sequence. The 20 kHz
    // tick stops for the duration, which is the BACKPRESSURE the master paces
    // on -- `want` cannot advance while a write is owed, so a stalled ack means
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
static uint32_t s_badCrc = 0;      // per-interval, folded into link_errs
static uint32_t s_torn = 0;        // per-interval
static uint32_t s_badCrcTotal = 0; // lifetime, for the serial census only
static uint32_t s_tornTotal = 0;
// kOpEventPull records the request; the reply is preloaded like any other.
static volatile bool s_eventReq = false;
static volatile uint8_t s_eventAck = 0;

// The version string overlays the telemetry tail for ONE reply, on request.
// Motion telemetry is meaningless while the S3 is asking what image this is,
// and the frame has no spare bytes to grow into. It stops at byte 27 so the
// reply can never forge a status variant.
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
    s_statusBuf[5] = s_core.lastSeq();
    rpflash::writeFlashStatus(s_statusBuf, s_flashRx.want(), s_flashRx.result(),
                              s_flashRx.detail());
    s_statusBuf[15] = s_core.lastSeq();
    stampVersion();
    crcStamp(s_statusBuf);
    digitalWrite(PIN_IRQ, LOW);
}

static void __not_in_flash_func(preloadStatus)() {
    if (s_flashMode) { preloadFlashStatus(); return; }
    if (s_eventReq) {
        s_eventReq = false;
        EventRecord rec;
        if (s_core.nextEvent(s_eventAck, rec)) {
            encodeEvent(std::span<uint8_t, kFrameBytes>(s_statusBuf, kFrameBytes),
                        rec);
            s_lastState = rec.state;
            digitalWrite(PIN_IRQ, rec.remaining != 0 ? HIGH : LOW);
            return;
        }
        // Nothing queued after the ack: an ordinary status is the honest reply.
    }
    StatusV2 st;
    s_core.fillStatus(st);
    // Renderer truth (sd-dxy.1.1). `pos` is COMMANDED; residue says what the
    // emitter actually managed. Read them as a pair or neither is evidence.
    st.residue = satResidue(s_pos - s_emitted);
    // PER-INTERVAL, reset here: v1 shipped saturating LIFETIME totals while the
    // master read deltas, so the only content the extra width carried was the
    // part that pins (MotionLinkProtocol.h, StatusV2).
    st.qdrops = sat16(s_qdrops);
    st.emit_overrun = sat16(s_emitOverrun);
    st.late_ticks = s_lateTicks > 0xFFu ? 0xFFu : uint8_t(s_lateTicks);
    const uint32_t errs = s_badCrc + s_torn;
    st.link_errs = errs > 0xFFu ? 0xFFu : uint8_t(errs);
    st.vel_clamped = s_velClamped > 0xFFu ? 0xFFu : uint8_t(s_velClamped);
    s_qdrops = s_emitOverrun = s_lateTicks = 0;
    s_badCrc = s_torn = s_velClamped = 0;
    s_lastState = st.state;
    encodeStatusV2(std::span<uint8_t, kFrameBytes>(s_statusBuf, kFrameBytes), st);
    if (s_flashVerReq) {
        s_flashVerReq = false;
        stampVersion();
        crcStamp(s_statusBuf);
    }
    // Look-at-me line: an unpulled event or a latched estop. There is no runway
    // to be low on any more -- the plan lives here.
    digitalWrite(PIN_IRQ, s_core.eventsPending() ? HIGH : LOW);
}

// ---- The tick's own alarm ---------------------------------------------------
// NOT the SDK alarm pool: its IRQ dispatch lives in flash, and with core 1
// running Ruckig the XIP cache belongs to core 1 (measured 2026-09-13: ~45
// late ticks/s whenever the machine moved, unchanged by moving stepperTick
// itself into RAM). Hardware alarm 1 of timer 0, handler in RAM, re-armed
// from the previous deadline so drift never accumulates. Priority 0x40: above
// the frame ISR (0xC0) and USB (0x80).
static constexpr uint kTickAlarm = 1;
static uint32_t s_tickDue = 0;
static uint32_t s_tickRearmed = 0;   // deadlines that had already passed
static void __not_in_flash_func(tickIsr)() {
    // INTR is write-1-to-clear: the atomic CLR alias writes ~mask and leaves
    // the bit set (the IRQ then re-enters forever). Plain write, SDK idiom.
    timer0_hw->intr = 1u << kTickAlarm;
    s_tickDue += kTickUs;
    // The alarm fires on EQUALITY; a deadline already behind the counter
    // would never fire again. Re-anchor and count it (it is a late tick).
    if (int32_t(s_tickDue - timer0_hw->timerawl) <= 2) {
        s_tickDue = timer0_hw->timerawl + kTickUs;
        ++s_tickRearmed;
    }
    timer0_hw->alarm[kTickAlarm] = s_tickDue;
    stepperTick();
}
static void tickBegin() {
    hardware_alarm_claim(kTickAlarm);
    irq_set_exclusive_handler(TIMER0_IRQ_1, tickIsr);
    irq_set_priority(TIMER0_IRQ_1, 0x40);
    hw_set_bits(&timer0_hw->inte, 1u << kTickAlarm);
    s_tickDue = timer0_hw->timerawl + kTickUs;
    timer0_hw->alarm[kTickAlarm] = s_tickDue;
    irq_set_enabled(TIMER0_IRQ_1, true);
}

// ---- Frame processor (SPI IRQ context: short, no allocation) ----------------
static void __not_in_flash_func(processFrame)(uint8_t* data, size_t len) {
    // Whole verified frames only: a torn or corrupted frame is DROPPED, never
    // partially parsed. The master re-sends what the status never acknowledged;
    // estop is repeated until the echoed state confirms it.
    const std::span<const uint8_t, kFrameBytes> f(data, kFrameBytes);
    if (len != kFrameBytes || !crcOk(f)) {
        ++s_badCrc;
        ++s_badCrcTotal;
        return;
    }
    const uint32_t now = time_us_32();
    // A firmware write owns the board. Every motion op is refused for its
    // duration, so a stale master frame cannot start a move into a half-
    // written slot; the estop latched by kOpFlashBegin holds the pins.
    if (s_flashMode && data[0] < kOpFlashBegin) return;

    // The motion vocabulary belongs to the core, which decodes and enqueues
    // without ever touching the engine.
    if (s_core.ingestFrame(f, now)) {
        if (data[0] == kOpSetPos) {
            // The emitter is glue, so the core cannot re-seed it: the homing
            // ritual's standstill write moves the reference AND what has been
            // pulsed, including the rollback snapshots, or a later FIFO-full
            // rollback would restore a pre-home reference.
            const float np = getF32(f, 2);
            s_pos = np;
            s_vel = 0.0f;
            s_emitted = np;
            s_qemitAtWord = np;
            s_qphaseAtWord = s_qphase;
        }
        return;
    }

    switch (data[0]) {
        case kOpEventPull:
            s_eventAck = data[2];
            s_eventReq = true;
            break;
        case kOpSetLimits: {
            const float cps = getF32(f, 2);
            // Reject nonsense rather than latching it: a bad ceiling here
            // silently throttles every move the machine will ever make.
            // Enforced at the EMITTER only, as a fault detector.
            if (cps > 0.0f && cps < 2.0e6f)
                s_maxCountsPerTick = cps * (float(kTickUs) * 1e-6f);
            break;
        }
        case kOpFlashBegin: {
            uint32_t sz = getU32(f, 2);
            // ONE gate, the same one estop uses: motion is held for the whole
            // transfer rather than a second flash-only interlock.
            s_core.estop(now);
            s_flashSlot.resolve();
            s_flashRx.begin(sz);
            s_flashEndReq = false;
            s_flashLastMs = millis();
            s_flashMode = true;
            break;
        }
        case kOpFlashData: {
            if (!s_flashMode) break;
            const uint32_t off = uint32_t(data[2]) |
                                 (uint32_t(data[3]) << 8) |
                                 (uint32_t(data[4]) << 16);
            // Staging only. The erase and the program happen in loop(): a
            // receive path that writes flash cannot also service the link.
            s_flashRx.data(off, data + kFlashChunkOffset, data[5]);
            break;
        }
        case kOpFlashEnd:
            if (!s_flashMode) break;
            s_flashEndCrc = getU32(f, 2);
            s_flashEndReq = true;   // loop() verifies: end() reads the slot
            break;
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

// The per-frame reset. spi_init() is tens of microseconds (baud-rate search,
// reset waits) and the master's next frame can start 200 us after CS rises;
// a frame clocked into a block mid-init is torn and answered with zeros
// (measured 2026-09-03: linkerr ~100/s whenever the S3 paired a poll with a
// retarget). This is the same RESETS pulse with the four registers restored
// by hand: about a microsecond, FIFOs and shift registers cleared.
static void __not_in_flash_func(spiBlockReset)() {
    spi_hw_t* hw = spi_get_hw(spi1);
    const uint32_t cr0 = hw->cr0, cpsr = hw->cpsr, cr1 = hw->cr1, dmacr = hw->dmacr;
    reset_block_num(RESET_SPI1);
    unreset_block_num_wait_blocking(RESET_SPI1);
    hw->cr0 = cr0;
    hw->cpsr = cpsr;
    hw->dmacr = dmacr;
    hw->cr1 = cr1;   // SSE last: the block comes up configured
}

static void __not_in_flash_func(armRx)() {
    dma_channel_abort(s_dmaRx);
    s_rxRead = 0;
    dma_channel_set_write_addr(s_dmaRx, s_rxRing, false);
    dma_channel_set_trans_count(s_dmaRx, 0x0FFFFFFFu, true);
}

static void __not_in_flash_func(armTx)() {
    dma_channel_abort(s_dmaTx);
    dma_channel_set_read_addr(s_dmaTx, s_statusBuf, false);
    dma_channel_set_trans_count(s_dmaTx, kFrameBytes, true);
}

static inline uint32_t __not_in_flash_func(rxWriteOff)() {
    return (uint32_t)dma_channel_hw_addr(s_dmaRx)->write_addr
         - (uint32_t)s_rxRing;
}

// Frame pump -- called ONLY from csRiseIsr (single consumer of s_rxRead).
static void __not_in_flash_func(pumpSpiFrames)() {
    uint32_t avail = (rxWriteOff() - s_rxRead) & 255u;
    if (avail % kFrameBytes != 0) {
        // CS delimits exchanges, so anything that is not whole frames is
        // garbage (a master reboot clocks stray bytes with CS floating).
        // Drop the lot; the master refreshes every intent it cares about.
        ++s_torn;
        ++s_tornTotal;
        avail = 0;
    }
    // Every whole frame, not one: an IRQ-off window (flash write) can leave
    // several queued behind one CS edge, and a one-per-edge pump would lag
    // the reply by that many frames for good.
    while (avail >= kFrameBytes) {
        for (uint32_t i = 0; i < kFrameBytes; i++)
            s_rxFrame[i] = s_rxRing[(s_rxRead + i) & 255u];
        s_rxRead = (s_rxRead + kFrameBytes) & 255u;
        processFrame(s_rxFrame, kFrameBytes);
        s_frames++;
        avail -= kFrameBytes;
    }
    // BLOCK RESET EVERY FRAME. A reply that the master clocked short, or a
    // stray clock while it rebooted, leaves bytes in the PL022 TX FIFO that
    // offset every later reply for good (measured 2026-09-03: status frames
    // byte-shifted, CRC-bad at 2750/s, the RX side perfectly aligned). The
    // reset is the only thing that empties that FIFO, it costs microseconds
    // inside the 200 us inter-frame gap, and the reply is re-armed anyway.
    dma_channel_abort(s_dmaTx);
    dma_channel_abort(s_dmaRx);
    spiBlockReset();
    preloadStatus();
    armRx();
    armTx();
}

// EVERY function on the frame path and the tick path lives in RAM
// (__not_in_flash_func). Measured on the scope 2026-09-13: with core 1 running
// Ruckig, a flash-resident pump armed the reply 237-241 us after CS rose, and
// the S3's second frame of a 225 us pair clocked 12-16 zero bytes before the
// status began. The XIP cache is shared; core 1's flash traffic during a
// commit starves core 0's instruction fetches.
// CS rising = frame end. The pump runs here, BELOW the tick's priority, so
// the reply is armed microseconds after every frame (the master's 200 us
// inter-frame floor is the budget) and the tick still preempts it. The last
// byte reaches the ring by DMA a hair after CS rises: wait for it, bounded,
// or a whole frame reads as torn.
static void __not_in_flash_func(csRiseIsr)() {
    // Exclusive IO_IRQ_BANK0 handler in RAM: the SDK's gpio dispatcher is
    // flash-resident (same XIP starvation as the tick, see tickBegin).
    if (!(gpio_get_irq_event_mask(PIN_SPI_CS) & GPIO_IRQ_EDGE_RISE)) return;
    gpio_acknowledge_irq(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE);
    const uint32_t t0 = time_us_32();
    while (((rxWriteOff() - s_rxRead) & 255u) % kFrameBytes != 0 &&
           time_us_32() - t0 < 10u) {}
    pumpSpiFrames();
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
    irq_set_exclusive_handler(IO_IRQ_BANK0, csRiseIsr);
    gpio_set_irq_enabled(PIN_SPI_CS, GPIO_IRQ_EDGE_RISE, true);
    irq_set_priority(IO_IRQ_BANK0, 0xC0);   // below the tick's 0x40
    irq_set_enabled(IO_IRQ_BANK0, true);
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

// ---- Core 1: the engine -----------------------------------------------------
// core1_separate_stack makes cores/rp2040/main.cpp launch core 1 through
// multicore_launch_core1_with_stack() with a HARDCODED 8 KB malloc. commit()
// nests KB-scale Ruckig temporaries and the first live homing measured 7268 B
// of that 8 KB used (2026-09-03), so the launch is wrapped (-Wl,--wrap in
// platformio.ini): the framework's buffer is ignored and core 1 runs on the
// 16 KB below. The framework's main1() loop and its FIFO protocol are kept,
// which is what the flash writes' park-the-other-core window relies on.
bool core1_separate_stack = true;

namespace core1 {

constexpr size_t kStackBytes = 16384;
static uint32_t s_stack[kStackBytes / 4] __attribute__((aligned(8)));
constexpr uint8_t kCanary = 0xA5;
constexpr uint32_t kServiceUs = 100;     // the contract's "at least every ms"
constexpr uint32_t kScanUs = 100000;     // the scan walks the untouched span

static uint32_t s_highWater = 0;
static volatile uint32_t s_serviceCount = 0;
// Core 1 publishes the render plan and Core::reset() is the only other writer
// of it, so core 1 stays parked until setup() has run that reset.
static volatile bool s_armed = false;

void paintStack() {
    // Leave a margin below the CURRENT sp so setup1()'s own live locals and
    // return address are not overwritten mid-function.
    const uint32_t sp = rp2040.getStackPointer();
    uint8_t* base = (uint8_t*)core1_separate_stack_address;
    uint8_t* top = (uint8_t*)(uintptr_t)(sp - 64u);
    if (top > base) memset(base, kCanary, (size_t)(top - base));
}

// rp2040.getFreeStack() reports CURRENT headroom only; it cannot see how deep a
// call that already returned went, which is exactly what commit() needs
// measured. Same technique as uxTaskGetStackHighWaterMark.
void scanHighWater() {
    const uint8_t* base = (const uint8_t*)core1_separate_stack_address;
    size_t untouched = 0;
    while (untouched < kStackBytes && base[untouched] == kCanary) untouched++;
    const uint32_t used = (uint32_t)(kStackBytes - untouched);
    if (used > s_highWater) s_highWater = used;
}

}  // namespace core1

extern "C" void __real_multicore_launch_core1_with_stack(void (*entry)(void),
                                                          uint32_t* stack_bottom,
                                                          size_t stack_size_bytes);
extern "C" void __wrap_multicore_launch_core1_with_stack(void (*entry)(void),
                                                          uint32_t*, size_t) {
    core1_separate_stack_address = core1::s_stack;   // paint/scan base
    __real_multicore_launch_core1_with_stack(entry, core1::s_stack,
                                             sizeof(core1::s_stack));
}

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
    // rather than through flash_safe_execute.
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

void setup1() {}
void loop1() {}
#else
void setup() {
    Serial.begin(115200);   // USB CDC status; printf is the Zero's only log
    pinMode(PIN_IRQ, OUTPUT);
    digitalWrite(PIN_IRQ, LOW);
    quadPioInit();   // owns PIN_STEP/PIN_DIR from here on (A/B via PIO)

    s_core.reset(time_us_32());

    s_px.begin();
    // Engine brightness stays 255; the adapter dims post-gamma in duty space.
    // Rainbow until the S3 speaks: an unwired or dead link never fakes ready.
    s_glow.requireReady(uint8_t(1u << uint8_t(slopglow::System::Link)));
    s_glowHb = s_glow.addHeartbeat(500);

    spiSlaveBegin();

    core1::s_armed = true;   // the engine may run now: the plan exists
    tickBegin();
    // Hardware watchdog, fed from loop() only while BOTH the tick and core 1
    // advance. A frozen core 1 would leave the tick rendering a stale plan
    // forever, which looks alive and is not. Every legitimate stall sits far
    // under 8 s: a 4 KB sector erase+program is ~50 ms.
    watchdog_enable(kWatchdogMs, true);
}

void setup1() { core1::paintStack(); }

// Core 1 owns the engine and nothing else. It is DELIBERATELY not the tick:
// commit() is milliseconds and the 20 kHz tick cannot wait for it, which is the
// whole reason the render plan is published rather than sampled in place.
void loop1() {
    if (!core1::s_armed) return;
    if (s_flashMode) return;   // a firmware write owns the board
    const uint32_t now = time_us_32();
    static uint32_t s_nextService = 0;
    static uint32_t s_nextScan = 0;
    if (int32_t(now - s_nextService) < 0) return;
    s_nextService = now + core1::kServiceUs;
    s_core.service(now);
    core1::s_serviceCount = core1::s_serviceCount + 1;
    if (int32_t(now - s_nextScan) >= 0) {
        s_nextScan = now + core1::kScanUs;
        core1::scanHighWater();
    }
}

void loop() {
    using namespace slopglow;
    {
        static uint32_t s_fedTick = 0;
        static uint32_t s_fedService = 0;
        const uint32_t t = s_tickCount;
        const uint32_t c = core1::s_serviceCount;
        if (t != s_fedTick && c != s_fedService) {
            s_fedTick = t;
            s_fedService = c;
            watchdog_update();
        }
    }
    flashBuyIfProven();
    if (s_flashMode) { flashServiceLoop(); return; }
    if (s_core.lastSeq() != 0) s_glow.markReady(System::Link);
    s_glow.set(System::Safety,
               s_core.estopped() ? Status::Urgent : Status::Nominal);
    s_glow.set(System::Motion,
               s_lastState == kStateRunning  ? Status::Working
               : s_lastState == kStateSettled ? Status::Degraded
                                              : Status::Nominal);
    // Frames, tears and the status snapshot are ALL the tick pump's business:
    // one consumer, IRQ context, 50 us cadence. loop() only observes.
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();
        Serial.printf("[mlink] lastSeq=%u badCrc=%lu torn=%lu qdrops=%lu "
                      "frames=%lu pos=%ld ticks=%lu rearm=%lu cfgfp=0x%04x "
                      "core1HW=%lu/%luB engine=%luB cs=%d\n",
                      unsigned(s_core.lastSeq()),
                      (unsigned long)s_badCrcTotal, (unsigned long)s_tornTotal,
                      (unsigned long)s_qdrops, (unsigned long)s_frames,
                      (long)s_pos, (unsigned long)s_tickCount,
                      (unsigned long)s_tickRearmed, unsigned(s_core.configFingerprint()),
                      (unsigned long)core1::s_highWater,
                      (unsigned long)core1::kStackBytes,
                      (unsigned long)rpmotion::Core::engineBytes(),
                      int(gpio_get(PIN_SPI_CS)));
    }
    s_glowHb->pulse();
    s_glow.update(millis());
    delay(2);
}
#endif  // MLINK_WIRE_PROBE
