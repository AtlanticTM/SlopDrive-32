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
#include <SPISlave.h>

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
        digitalWrite(PIN_DIR, delta > 0 ? HIGH : LOW);
        digitalWrite(PIN_STEP, HIGH);
        busy_wait_us_32(2);   // drive input wants >1.2 us high time
        digitalWrite(PIN_STEP, LOW);
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
    SPISlave1.setData(s_statusBuf, sizeof(s_statusBuf));
    // Feed-me line: the producer paces on this, not on polling cadence.
    digitalWrite(PIN_IRQ, (rw < kRunwayLowMs && !s_estop) ? HIGH : LOW);
}

// ---- SPI callbacks (IRQ context: short, no allocation) ----------------------
static void onRecv(uint8_t* data, size_t len) {
    if (len < 2) return;
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

static void onSent() { preloadStatus(); }

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

void setup() {
    pinMode(PIN_IRQ, OUTPUT);
    digitalWrite(PIN_IRQ, LOW);
    pinMode(PIN_STEP, OUTPUT);
    pinMode(PIN_DIR, OUTPUT);

    s_px.begin();
    // Engine brightness stays 255; the adapter dims post-gamma in duty space.
    // Rainbow until the S3 speaks: an unwired or dead link never fakes ready.
    s_glow.requireReady(uint8_t(1u << uint8_t(slopglow::System::Link)));
    s_glowHb = s_glow.addHeartbeat(500);

    SPISlave1.setRX(PIN_SPI_RX);
    SPISlave1.setCS(PIN_SPI_CS);
    SPISlave1.setSCK(PIN_SPI_SCK);
    SPISlave1.setTX(PIN_SPI_TX);
    SPISlave1.onDataRecv(onRecv);
    SPISlave1.onDataSent(onSent);
    preloadStatus();
    SPISlave1.begin(SPISettings(kSpiHz, MSBFIRST, SPI_MODE0));

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
    preloadStatus();   // refresh runway/IRQ even between transactions
    s_glowHb->pulse();
    s_glow.update(millis());
    delay(2);
}
