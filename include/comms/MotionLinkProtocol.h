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

#include <cstdint>

namespace motionlink {

// Fixed-size transactions keep the SPI slave trivial: every master transfer
// is kFrameBytes out, kFrameBytes back (the slave's preloaded status).
inline constexpr uint32_t kSpiHz = 8000000;   // clean /10 of the S3's 80 MHz APB
inline constexpr size_t kFrameBytes = 32;

// Master -> slave: [op:u8][seq:u8][payload...]
enum Op : uint8_t {
    kOpPing    = 0x01,   // payload empty; slave answers with Status
    kOpSegment = 0x02,   // payload = Segment (packed LE, 20 B)
    kOpEstop   = 0x03,   // flush the schedule NOW, hold position
    kOpClear   = 0x04,   // leave the estop hold; schedule is empty after
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
};

// The credit contract: the S3 sends a segment only while runway_ms is below
// kRunwayTargetMs and depth is below kSegmentDepth. The IRQ line asserts when
// runway falls under kRunwayLowMs (feed me) or on estop/underrun (look at me).
inline constexpr uint16_t kRunwayTargetMs = 10;   // operator-ruled band
inline constexpr uint16_t kRunwayLowMs = 4;
inline constexpr size_t kSegmentDepth = 8;

}  // namespace motionlink
