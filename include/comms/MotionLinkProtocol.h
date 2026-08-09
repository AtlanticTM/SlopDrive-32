// MotionLinkProtocol -- S3 <-> RP2350 motion-coprocessor SPI vocabulary.
// Constraints:
// - ONE definition, BOTH ends include it (T20). Never copy a constant out.
// - Link machinery between two boards of one product, like BridgeProtocol.h:
//   NOT SlopSync, no conformance duty, invisible to clients.
// - Ops are append-only: the two boards flash independently.
// - The schedule is TIME-INDEXED (operator ruling, sd-dxy): the RP2350 renders
//   segments at their own pace, NEVER drains faster to catch up. Overflow is
//   the producer's fault; the S3 paces on the reported runway. Underrun gets
//   SETTLE (hold at the last endpoint), never extrapolation.
// - ESTOP punches through on its own op, pumped by the slave's 20 kHz tick
//   (<=50 us to act), never queued behind buffered motion.
// See: dev board sd-dxy (wiring, buffer rulings), .claude/rules/transport.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace motionlink {

// Fixed-size transactions keep the SPI slave trivial: every master transfer
// is kFrameBytes out, kFrameBytes back (the slave's preloaded status).
inline constexpr uint32_t kSpiHz = 8000000;   // clean /10 of the S3's 80 MHz APB
// MODE 1 (CPHA=1) IS LOAD-BEARING: the RP2350's PL022 slave in mode 0
// requires CS to pulse between EVERY word, so a continuous-CS 32-byte burst
// delivers exactly one byte (measured 2026-08-06: maxRx=1 per transaction).
// CPHA=1 latches on the trailing edge and survives held-low CS. Both ends
// read this constant; changing it on one side kills the link.
inline constexpr uint8_t kSpiMode = 1;
inline constexpr size_t kFrameBytes = 32;

// Master -> slave: [op:u8][seq:u8][payload...]
enum Op : uint8_t {
    kOpPing     = 0x01,   // payload empty; slave answers with Status
    kOpSegment  = 0x02,   // payload = Segment (packed LE, 20 B)
    kOpEstop    = 0x03,   // flush the schedule NOW, hold position
    kOpClear    = 0x04,   // leave the estop hold; schedule is empty after
    // Trapezoid retarget: [target:f32][vmax:f32][accel:f32] counts, counts/s,
    // counts/s^2. The slave seeks target from its LIVE (p, v) -- idempotent,
    // re-sendable, last one wins; switches the renderer out of segment mode.
    // This is streamToSteps() on the wire; segments remain the native-plan path.
    kOpRetarget = 0x05,
    // Standstill position set: [pos:f32] counts. Flushes schedule + retarget,
    // zeroes velocity -- the homing ritual's "the wall is HERE" write.
    kOpSetPos   = 0x06,
    // C2 segment: [dur:u32][p0 v0 a0 p1 v1 a1 :f32] -- quintic Hermite with
    // ACCELERATION knots, so chained segments are torque-continuous (the C1
    // cubic stepped accel at every knot: a 100 Hz notch the servo renders as
    // texture). 28 payload bytes: fills the frame to the CRC exactly.
    kOpSegment2 = 0x07,
    // Renderer speed ceiling: [max_counts_per_s:f32]. The RP is open loop --
    // it emits exactly what the trajectory asks for and the drive either
    // follows or silently loses steps. Measured: emit_overrun climbing while
    // residue stayed under one count, i.e. the renderer faithfully commanded
    // >400,000 counts/s against a 900 mm/s operator ceiling, and the motor lost
    // 84 mm. A ceiling HERE converts silent physical loss into visible,
    // counted lag. Enforced by the slave as an EMITTER slew cap only, never
    // on the reference. Sent at init, whenever either limit set changes, and
    // again when the master detects a slave restart (RAM-held, boots to 0 =
    // unlimited).
    kOpSetLimits = 0x08,
};

// One C1 motion segment: cubic Hermite from (p0, v0) to (p1, v1) over
// duration_us. Positions in STEPS at the drive's input, velocities in
// steps/s. 10 ms of runway is one to three of these; the RP2350 interpolates
// (the whole reason it exists -- pre-rendered pulses are not sent).
struct Segment {
    uint32_t duration_us;
    float p0;
    float v0;
    float p1;
    float v1;
    // Acceleration knots (kOpSegment2); kOpSegment leaves them 0 and renders
    // as the zero-curvature quintic.
    float a0;
    float a1;
};
inline constexpr size_t kSegmentWireBytes  = 20;
inline constexpr size_t kSegment2WireBytes = 28;

// Slave -> master, preloaded before every transaction:
// [state:u8][flags:u8][runway_ms:u16le][depth:u8][seq_echo:u8][pos:f32le][vel:f32le]
//   ...bench signature at 14, seq duplicate at 15...
// [emitted:f32le][qdrops:u16le][emit_overrun:u16le][late_ticks:u16le]
// [vel_clamped:u16le] (28..29 spare)
//
// WHY `emitted` IS ON THE WIRE (sd-dxy.1.1). `pos` is the COMMANDED position,
// recomputed from the segment polynomial every tick -- the number that cannot
// be wrong. `emitted` is what was actually PULSED. Their difference is the
// renderer's residue, and shipping only the first made lost motion invisible
// to the master by construction. Read them as a pair or neither is evidence.
//   qdrops       PIO TX FIFO was full: a whole 16-state word was dropped and
//                phase+count rolled back. Production == consumption off one
//                crystal, so ANY nonzero value is a fault, not a design state.
//   emit_overrun the tick owed more steps than the emitter can pass in one
//                tick. With plans clamped below kMaxCountsPerSec this is
//                unreachable, so nonzero means an illegal plan, a late tick,
//                or a discontinuous reference -- all defects.
//   late_ticks   trajectory ticks that arrived >1.5x kTickUs after the last.
//                Plan time advances by tick COUNT, so a late tick stretches
//                the timeline while still landing on the endpoint.
// Offsets are named here and read by BOTH ends. Never transcribe a number.
// Only 14 bytes exist between the seq duplicate and the CRC, so the three
// counters are u16 and SATURATE at 0xFFFF rather than wrap: a pinned counter
// honestly reads "lots", a wrapped one reads "healthy" and lies.
inline constexpr size_t kStatusOffEmitted     = 16;
inline constexpr size_t kStatusOffQDrops      = 20;
inline constexpr size_t kStatusOffEmitOverrun = 22;
inline constexpr size_t kStatusOffLateTicks   = 24;
inline constexpr size_t kStatusOffVelClamped = 26;   // ticks the ceiling bit
inline constexpr uint16_t sat16(uint32_t v) {
    return v > 0xFFFFu ? uint16_t(0xFFFFu) : uint16_t(v);
}
enum State : uint8_t {
    kStateIdle    = 0x00,   // schedule empty, settled
    kStateRunning = 0x01,
    kStateSettled = 0x02,   // underran and braked to rest at the last endpoint
    kStateEstop   = 0x03,   // holding; only kOpClear leaves this
};
enum Flags : uint8_t {
    kFlagOverflow = 0x01,   // a segment arrived with the ring full (producer bug)
    kFlagUnderran = 0x02,   // sticky until the next segment lands
    // A segment started far from the emitted position; the renderer TELEPORTED
    // its reference instead of slewing the gap at max rate (producer
    // discontinuity, e.g. a leg fed twice after a dropped frame). Sticky.
    kFlagJumped   = 0x04,
};

// The credit contract: the S3 sends a segment only while runway_ms is below
// kRunwayTargetMs and depth is below kSegmentDepth. The IRQ line asserts when
// runway falls under kRunwayLowMs (feed me) or on estop/underrun (look at me).
inline constexpr uint16_t kRunwayTargetMs = 10;   // operator-ruled band
inline constexpr uint16_t kRunwayLowMs = 4;
inline constexpr size_t kSegmentDepth = 8;

// Renderer emit ceiling. The PIO stepgen streams pin states at 400 kHz (one
// transition per state max); plans clamp BELOW it so the emitter never falls
// behind the trajectory (falling behind trips the teleport guard = lost
// motion). The drive's own input limit is 500 kHz -- the hard roof.
inline constexpr float kMaxCountsPerSec = 300000.0f;

// Every frame, BOTH directions, carries CRC-16/CCITT-FALSE over bytes
// [0, kCrcOffset) stored LE at [kCrcOffset]. A frame that fails the check is
// DROPPED whole: no partial parse, no estop from garbage. Recovery is the
// credit contract itself -- the master re-sends what the status never
// acknowledged. Ops that must not be lost (kOpEstop) are repeated by the
// master until the echoed state confirms them.
inline constexpr size_t kCrcOffset = 30;
static_assert(kStatusOffVelClamped + 2 <= kCrcOffset,
              "status telemetry overruns the CRC field");
inline constexpr uint16_t crc16(std::span<const uint8_t> d) {
    uint16_t c = 0xFFFF;
    for (uint8_t byte : d) {
        c ^= uint16_t(uint16_t(byte) << 8);
        for (int b = 0; b < 8; ++b)
            c = (c & 0x8000u) ? uint16_t(uint16_t(c << 1) ^ 0x1021u)
                              : uint16_t(c << 1);
    }
    return c;
}
inline void crcStamp(std::span<uint8_t, kFrameBytes> frame) {
    const uint16_t c = crc16(frame.first(kCrcOffset));
    frame[kCrcOffset] = uint8_t(c);
    frame[kCrcOffset + 1] = uint8_t(c >> 8);
}
inline bool crcOk(std::span<const uint8_t, kFrameBytes> frame) {
    const uint16_t c = crc16(frame.first(kCrcOffset));
    return frame[kCrcOffset] == uint8_t(c) &&
           frame[kCrcOffset + 1] == uint8_t(c >> 8);
}

}  // namespace motionlink
