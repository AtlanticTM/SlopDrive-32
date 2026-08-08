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

#include "comms/MotionLinkProtocol.h"
#include "slopglow/slopglow_core.hpp"

#include <Adafruit_NeoPixel.h>

using namespace motionlink;

// ---- Pins (RP2350-Zero) -----------------------------------------------------
// SPI1 corner cluster (operator loom preference). RP2350 pins carry FIXED
// SPI roles by position (mod 4: RX, CSn, SCK, TX), so within GP26..29+15 the
// legal set is exactly this; SCK cannot land on 29 nor CS on 15.
static constexpr uint8_t PIN_SPI_RX  = 28;  // S3 MOSI (GPIO38) -> here
static constexpr uint8_t PIN_SPI_CS  = 29;  // S3 CS   (GPIO4)
static constexpr uint8_t PIN_SPI_SCK = 26;  // S3 SCK  (GPIO48)
static constexpr uint8_t PIN_SPI_TX  = 27;  // -> S3 MISO (GPIO10)
static constexpr uint8_t PIN_IRQ     = 15;  // -> S3 IRQ (GPIO7), active HIGH
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
// Renders the active segment's cubic Hermite, emits step edges from a float
// accumulator. Position unit = steps at the drive input.
static float s_pos = 0.0f;        // rendered position (steps)
static float s_vel = 0.0f;
static float s_emitted = 0.0f;    // steps actually pulsed out
static uint32_t s_segElapsedUs = 0;
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
    if (n > kStatesPerTick) n = kStatesPerTick;
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

static bool stepperTick(struct repeating_timer*) {
    // Frame pump FIRST, and unconditionally: an ESTOP frame must act this
    // tick, and a latched estop must still process its kOpClear.
    pumpSpiFrames();
    if (s_estop) { s_state = kStateEstop; return true; }   // hold: no motion

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
        emitTowardPos();
        return true;
    }

    if (ringDepth() == 0) {
        if (s_state == kStateRunning) {
            // Underrun: SETTLE. Hold the last endpoint; never extrapolate.
            s_state = kStateSettled;
            s_flags |= kFlagUnderran;
            s_vel = 0.0f;
        }
        return true;
    }

    const Segment& seg = s_ring[s_tail % kSegmentDepth];
    // First tick of a segment: a p0 far from the emitted position would slew
    // the whole gap at the 20 kstep/s cap (motor shoots). Teleport the
    // reference instead -- zero pulses -- and report it. Small offsets slew
    // legitimately.
    if (s_segElapsedUs == 0) {
        // Threshold sits above kStatesPerTick so a legit full-rate tick's
        // in-flight delta can never read as a discontinuity.
        const float jump = seg.p0 - s_emitted;
        if (jump > 64.0f || jump < -64.0f) {
            s_emitted += jump;
            s_flags |= kFlagJumped;
        }
    }
    s_state = kStateRunning;
    s_segElapsedUs += kTickUs;
    const float T = float(seg.duration_us);
    float t = float(s_segElapsedUs);
    if (t >= T) t = T;

    // Cubic Hermite in normalized time.
    const float u = (T > 0.0f) ? (t / T) : 1.0f;
    const float u2 = u * u, u3 = u2 * u;
    const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u;
    const float h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    const float Ts = T * 1e-6f;
    s_pos = h00 * seg.p0 + h10 * Ts * seg.v0 + h01 * seg.p1 + h11 * Ts * seg.v1;
    s_vel = seg.v0 + (seg.v1 - seg.v0) * u;   // display-grade estimate only

    emitTowardPos();

    if (s_segElapsedUs >= seg.duration_us) {
        s_tail = uint8_t(s_tail + 1);
        s_segElapsedUs = 0;
    }
    return true;
}

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

static void preloadStatus() {
    const uint16_t rw = runwayMs();
    s_statusBuf[0] = s_state;
    s_statusBuf[1] = s_flags;
    s_statusBuf[2] = uint8_t(rw);
    s_statusBuf[3] = uint8_t(rw >> 8);
    s_statusBuf[4] = ringDepth();
    s_statusBuf[5] = s_lastSeq;
    memcpy(&s_statusBuf[6], (const void*)&s_pos, 4);
    memcpy(&s_statusBuf[10], (const void*)&s_vel, 4);
    s_statusBuf[14] = 0xA5;          // alignment signature (bench)
    s_statusBuf[15] = s_lastSeq;     // seq duplicate for offset hunting
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
    switch (data[0]) {
        case kOpEstop:   // ahead of the ring, by design
            s_estop = true;
            s_rtActive = false;
            s_head = s_tail = 0;
            s_lastSegSeq = 0xFFFF;
            break;
        case kOpClear:
            s_estop = false;
            s_rtActive = false;   // never resume a pre-estop target
            s_head = s_tail = 0;
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
            s_rtActive = true;
            break;
        }
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
}

void loop() {
    using namespace slopglow;
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
