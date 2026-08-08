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
// - ESTOP punches through on its own op, handled in the receive IRQ, never
//   queued behind buffered motion.
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
};
inline constexpr size_t kSegmentWireBytes = 20;

// Slave -> master, preloaded before every transaction:
// [state:u8][flags:u8][runway_ms:u16le][depth:u8][seq_echo:u8][pos:f32le][vel:f32le]
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
