// rp2350_motion -- motion-coprocessor skeleton for the Waveshare RP2350-Zero.
// Constraints:
// - SPI SLAVE only; the S3 is the master and paces on the reported runway
//   (comms/MotionLinkProtocol.h is the one vocabulary, both ends -- T20).
// - The schedule is time-indexed: segments render at their own pace, never
//   faster (sd-dxy ruling). Underrun -> SETTLE at the last endpoint, never
//   extrapolation. ESTOP is handled in the SPI receive IRQ, ahead of the ring.
// - Pin choices here are solder-defined; change them WITH the loom, not before.
// - TODO(sd-dxy): the 20 kHz timer stepper below is a bring-up stub. The real
//   generator is a PIO program clocked from the interpolator; the stub exists
//   so the link, ring, credit, and settle logic are testable before PIO lands.
#include <Arduino.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"
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

static struct repeating_timer s_tick;
static constexpr uint32_t kTickUs = 50;   // 20 kHz stub cadence

static bool stepperTick(struct repeating_timer*) {
    // Never busy-wait in this ISR: a 20 us wait held off the SPI IRQ past the
    // PL022's 8-byte RX FIFO (8 us at 8 MHz) and tore frames.
    if (s_estop) { s_state = kStateEstop; return true; }   // hold: no motion

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
        const float jump = seg.p0 - s_emitted;
        if (jump > 32.0f || jump < -32.0f) {
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

    // Emit whole steps toward s_pos. Stub: one edge per tick maximum, which
    // caps the stub at 20 kstep/s -- fine for bring-up, PIO removes the cap.
    const float delta = s_pos - s_emitted;
    if (delta >= 1.0f || delta <= -1.0f) {
        // QUADRATURE A/B levels (drive saved in encoder-follow, 0x19=2):
        // one Gray transition per count, A leads B = forward. Step/dir no
        // longer drives the motor. ponytail: 20 k counts/s cap, PIO replaces.
        static uint8_t s_phase = 0;
        s_phase = uint8_t((s_phase + ((delta > 0) ? 1u : 3u)) & 3u);
        digitalWrite(PIN_STEP, (s_phase == 1 || s_phase == 2) ? HIGH : LOW);  // A
        digitalWrite(PIN_DIR,  (s_phase == 2 || s_phase == 3) ? HIGH : LOW);  // B
        s_emitted += (delta > 0) ? 1.0f : -1.0f;
    }

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
            s_head = s_tail = 0;
            break;
        case kOpClear:
            s_estop = false;
            s_head = s_tail = 0;
            s_flags = 0;
            s_state = kStateIdle;
            break;
        case kOpSegment: {
            if (len < 2 + kSegmentWireBytes) break;
            if (ringDepth() >= kSegmentDepth) { s_flags |= kFlagOverflow; break; }
            Segment seg;
            memcpy(&seg.duration_us, data + 2, 4);
            memcpy(&seg.p0, data + 6, 4);
            memcpy(&seg.v0, data + 10, 4);
            memcpy(&seg.p1, data + 14, 4);
            memcpy(&seg.v1, data + 18, 4);
            s_ring[s_head % kSegmentDepth] = seg;
            s_head = uint8_t(s_head + 1);
            s_flags &= uint8_t(~kFlagUnderran);
            break;
        }
        default:   // kOpPing and future ops: status answers regardless
            break;
    }
}

// ---- Raw PL022 slave (arduino-pico's SPISlave never fires on RP2350 --
// proven by the wire probe: cs/sck/mosi all arriving, zero callbacks) --------
// The IRQ drains RX into a frame buffer and keeps TX fed from the status
// snapshot; loop() resyncs byte counters whenever CS idles high (readable
// via SIO regardless of the pin's SPI funcsel).
static uint8_t s_rxFrame[kFrameBytes];
static volatile uint8_t s_rxCount = 0;
static volatile uint8_t s_txIdx = 0;

static void feedTx() {
    while (spi_is_writable(spi1) && s_txIdx < kFrameBytes)
        spi_get_hw(spi1)->dr = s_statusBuf[s_txIdx++];
}

// spi_init's RESETS-block reset is the ONLY thing that clears a PL022 TX
// FIFO -- an SSE cycle does not (measured 2026-08-07: +8 reply shift,
// self-sustaining). Never revert this to an SSE toggle.
static void spiConfigure() {
    spi_init(spi1, kSpiHz);
    spi_set_slave(spi1, true);
    static_assert(kSpiMode == 1, "PL022 slave needs CPHA=1 for held-low CS");
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);
    spi_get_hw(spi1)->imsc = SPI_SSPIMSC_RXIM_BITS | SPI_SSPIMSC_RTIM_BITS;
}

// Flush by block reset, then arm from byte 0. A master clocking into the
// reset window tears that frame; CRC drops it, the credit loop re-sends.
// Master keeps >=60 us between transactions to make that rare.
// ponytail: per-frame reset is crude -- PIO/DMA rework (sd-dxy) owns it.
static void txFlushAndArm() {
    spiConfigure();
    s_txIdx = 0;
    feedTx();
}

static volatile uint32_t s_irqCount = 0;
static volatile uint32_t s_bytesDrained = 0;
static volatile uint8_t s_maxRx = 0;

static void spi1Irq() {
    s_irqCount = s_irqCount + 1;
    // Collect the WHOLE frame inside this one entry: bytes land 1 us apart at
    // 8 MHz, and the level-IRQ refire proved unreliable mid-burst (one
    // delivery per transaction, measured) -- so spin out the remaining ~30 us
    // here, interleaving TX refill so the full 32 B status answers. Bail when
    // CS rises with the frame short (torn transaction; resync handles it).
    uint32_t idle = 0;
    while (s_rxCount < kFrameBytes) {
        if (spi_is_readable(spi1)) {
            s_rxFrame[s_rxCount] = uint8_t(spi_get_hw(spi1)->dr);
            s_rxCount = uint8_t(s_rxCount + 1);
            s_bytesDrained = s_bytesDrained + 1;
            if (s_rxCount > s_maxRx) s_maxRx = s_rxCount;
            idle = 0;
        } else {
            if (gpio_get(PIN_SPI_CS) || ++idle > 4000) break;
        }
        feedTx();
    }
    if (s_rxCount >= kFrameBytes) {
        processFrame(s_rxFrame, kFrameBytes);
        preloadStatus();
        s_rxCount = 0;
        txFlushAndArm();   // aligned answer for the next transaction
    }
}

static void spiSlaveBegin() {
    gpio_set_function(PIN_SPI_RX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_TX, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_CS, GPIO_FUNC_SPI);
    preloadStatus();
    txFlushAndArm();   // full block config; byte 0 of the first answer is real
    irq_set_exclusive_handler(SPI1_IRQ, spi1Irq);
    irq_set_enabled(SPI1_IRQ, true);
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
            return uint8_t((uint16_t(slopglow::gamma8(v)) * 41u) >> 8);   // 40/255
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
    pinMode(PIN_STEP, OUTPUT);
    pinMode(PIN_DIR, OUTPUT);

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
    // CS idle-high resync, TORN RX ONLY (txIdx>0 with a loaded FIFO is the
    // normal between-transaction state; re-zeroing it here served duplicate
    // bytes). A tear flushes BOTH FIFOs: SSE cycle is the only TX flush.
    if (gpio_get(PIN_SPI_CS) && s_rxCount != 0) {
        ++s_torn;   // frames discarded HERE never reach the badCrc counter
        irq_set_enabled(SPI1_IRQ, false);
        while (spi_is_readable(spi1)) (void)spi_get_hw(spi1)->dr;
        s_rxCount = 0;
        preloadStatus();
        txFlushAndArm();
        irq_set_enabled(SPI1_IRQ, true);
    }
    // statusBuf has ONE writer, the SPI IRQ (preloadStatus after each frame).
    // A loop()-side refresh raced feedTx mid-transaction: half-updated
    // replies failed the master's CRC and starved the feed (2026-08-07).
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 1000) {
        lastPrint = millis();
        Serial.printf("[mlink] lastSeq=%u badCrc=%lu torn=%lu irqs=%lu drained=%lu maxRx=%u "
                      "imsc=0x%02lx ris=0x%02lx sspsr=0x%02lx cs=%d\n",
                      unsigned(s_lastSeq), (unsigned long)s_badCrc,
                      (unsigned long)s_torn, (unsigned long)s_irqCount,
                      (unsigned long)s_bytesDrained, unsigned(s_maxRx),
                      (unsigned long)spi_get_hw(spi1)->imsc,
                      (unsigned long)spi_get_hw(spi1)->ris,
                      (unsigned long)spi_get_hw(spi1)->sr,
                      int(gpio_get(PIN_SPI_CS)));
    }
    s_glowHb->pulse();
    s_glow.update(millis());
    delay(2);
}
#endif  // MLINK_WIRE_PROBE
