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
// - Commands are sent THE MOMENT THEY ARRIVE, never on a tick
//   (operator-ratified 2026-09-02, architecture.md section 2): the link is a
//   bus, not a schedule. Only status polling may be periodic.
// See: dev board sd-dxy (wiring, buffer rulings), .claude/rules/transport.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    // ---- v2 intents (sd-4k1.2) ----------------------------------------------
    // The RP holds the plan, so v2 carries INTENTS with anchor times. The
    // codecs and the field vocabulary live at the tail of this file. No
    // clock op: every frame is a clock probe, see ClockFilter.
    kOpCommand   = 0x10,   // one slopmotion Command; see LinkCommand
    kOpConfig    = 0x11,   // field-tagged push; see ConfigTag
    kOpEventPull = 0x12,   // [ack_seq:u8]; the reply is an EventRecord
    // ---- RP2350 firmware update over the link (sd-4k1.3) --------------------
    // A/B slot write, fed from the C5 bridge's OTA surface. Motion is STOPPED
    // for the whole transfer by the S3's ONE shared OTA gate (governance
    // amendment 2026-08-06); these ops never arrive alongside motion, and the
    // slave refuses every motion op while a write is in flight.
    // The state machine both ends run is include/comms/RpFlashCore.h.
    kOpFlashBegin   = 0x40,   // [size:u32le] -- enter flash mode, want = 0
    kOpFlashData    = 0x41,   // [off:u24le][len:u8][bytes:kFlashChunkBytes]
    kOpFlashEnd     = 0x42,   // [crc32:u32le] -- verify the slot, then boot it
    kOpFlashAbort   = 0x43,   // [] leave flash mode, running image untouched
    kOpFlashStatus  = 0x44,   // [] poll only; the reply carries want/result
    kOpFlashVersion = 0x45,   // [] reply carries the RP fw string (C-8)
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

// ---- v2 vocabulary: anchored intents, config push, clock (sd-4k1.2) ---------
// The RP2350 HOLDS THE PLAN (architecture.md section 2, three-board split), so
// v2 carries INTENTS with anchor times, never rendered chunks. Every v1 op and
// the whole v1 status layout stay byte-for-byte: the boards flash
// independently, so a v1 slave answering a v2 master is a supported state and
// the status variant byte is how the master finds out.
// v2 fields are reached through the CODECS below, never by offset arithmetic.
// One encoder and one decoder that both ends call is the named-offset rule
// taken one step further: there is no second call site to transcribe into.

// Little-endian field access. Explicit shifts, not a struct overlay: the wire
// is LE whatever either compiler would have packed.
inline void putU16(std::span<uint8_t> f, size_t at, uint16_t v) {
    f[at] = uint8_t(v);
    f[at + 1] = uint8_t(v >> 8);
}
inline uint16_t getU16(std::span<const uint8_t> f, size_t at) {
    return uint16_t(uint16_t(f[at]) | uint16_t(uint16_t(f[at + 1]) << 8));
}
inline void putU32(std::span<uint8_t> f, size_t at, uint32_t v) {
    f[at] = uint8_t(v);
    f[at + 1] = uint8_t(v >> 8);
    f[at + 2] = uint8_t(v >> 16);
    f[at + 3] = uint8_t(v >> 24);
}
inline uint32_t getU32(std::span<const uint8_t> f, size_t at) {
    return uint32_t(f[at]) | (uint32_t(f[at + 1]) << 8) |
           (uint32_t(f[at + 2]) << 16) | (uint32_t(f[at + 3]) << 24);
}
inline uint32_t f32Bits(float v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, 4);
    return u;
}
inline float bitsF32(uint32_t u) {
    float v = 0.0f;
    std::memcpy(&v, &u, 4);
    return v;
}
inline void putF32(std::span<uint8_t> f, size_t at, float v) {
    putU32(f, at, f32Bits(v));
}
inline float getF32(std::span<const uint8_t> f, size_t at) {
    return bitsF32(getU32(f, at));
}

// ---- Time: one domain on the wire, the master converts ----------------------
// ANCHORS ARE SLAVE MICROSECONDS, u32, wrapping every 71.6 minutes -- the same
// domain and width SlopSync uses for all protocol time (SPEC 7.2). The master
// owns the conversion because the master owns the offset estimate; the slave's
// hot path then does no clock math at all, it compares clockDelta against 0.
// WRAP HORIZON: an anchor is due when clockDelta(anchor, now) <= 0, which is
// unambiguous only while the gap is under 2^31 us = 35.8 minutes. Anchors run
// milliseconds ahead, so the rule is simply that nothing may schedule beyond
// the horizon.
// ERROR BUDGET: one 50 us slave tick end to end. The slave stamps at its
// 20 kHz pump, so a raw sample is biased LATE by 0 to 50 us plus half the
// ~40 us transaction; ClockFilter's windowed minimum removes that bias to a
// few microseconds. The master must NOT send anchored commands until
// converged() -- an unconverged offset is a rendered position error, and
// has_anchor false (plan at arrival) is the honest degradation.
inline int32_t clockDelta(uint32_t a, uint32_t b) {
    return int32_t(a - b);
}

// 16 samples at the 100 Hz status poll is a 160 ms window: quantization bias
// falls to about 50/(N+1) = 3 us, and a windowed minimum lags an UPWARD drift
// by at most one window, 16 us at 100 ppm. Both sit far inside the 50 us
// budget, which is why no rate estimator exists here. The step threshold
// separates "the crystals drifted" from "the slave rebooted".
inline constexpr size_t kClockWindow = 16;
inline constexpr uint32_t kClockStepUs = 1000;

// Master-side offset estimator: slave_us = master_us + offsetUs().
// PAIRING IS BY SEQ, NEVER BY POSITION. The slave preloads its reply after
// processing a frame, so a reply is always one transaction behind its request.
// status.seq_echo names the frame the slave stamped and the master pairs the
// sample with the (t0, t3) it recorded for that seq; a reply whose seq_echo it
// does not hold is DISCARDED, because a mispaired sample is biased EARLY and
// the minimum filter would latch onto it and never let go.
// NO CLOCK OP EXISTS: every frame is a probe. A dedicated exchange would add
// an op, a payload and a round trip to measure a link the master already
// drives 100 times a second.
// PROBE CADENCE MUST NOT DIVIDE THE PUMP PERIOD. A cadence that is an exact
// multiple of the slave's 50 us pump gives every sample the SAME quantization,
// and a minimum filter has nothing to select from: the bias pins at whatever
// the first sample carried, up to a full tick. The master's millis()-driven
// poll dithers on its own; a timer-driven poll must dither deliberately.
class ClockFilter {
  public:
    // t0/t3: master microseconds either side of the transaction that carried
    // the frame. t1: the slave's stamp for that same frame (status.clock_t1).
    void push(uint32_t t0, uint32_t t3, uint32_t t1) {
        const uint32_t mid = t0 + ((t3 - t0) >> 1);
        const uint32_t raw = t1 - mid;
        if (_n == 0) _base = raw;
        int32_t d = int32_t(raw - _base);
        if (d > int32_t(kClockStepUs) || d < -int32_t(kClockStepUs)) {
            _n = 0;
            _i = 0;
            _base = raw;
            d = 0;
        }
        _w[_i] = d;
        _i = (_i + 1u) % kClockWindow;
        if (_n < kClockWindow) ++_n;
        _rtt_us = t3 - t0;
        rebase();
    }
    void reset() {
        _n = 0;
        _i = 0;
        _base = 0;
        _rtt_us = 0;
    }
    bool converged() const { return _n >= kClockWindow; }
    size_t samples() const { return _n; }
    uint32_t rttUs() const { return _rtt_us; }
    uint32_t offsetUs() const { return _base; }
    uint32_t toSlave(uint32_t master_us) const { return master_us + _base; }

  private:
    // Every stored sample is a delta from _base, and rebase() folds the window
    // minimum into _base after each push. Deltas therefore stay small while
    // the true offset is an arbitrary 32-bit value, which is what keeps the
    // comparisons above from overflowing anywhere near the wrap.
    void rebase() {
        if (_n == 0) return;
        int32_t m = _w[0];
        for (size_t k = 1; k < _n; ++k)
            if (_w[k] < m) m = _w[k];
        for (size_t k = 0; k < _n; ++k) _w[k] = int32_t(_w[k] - m);
        _base = uint32_t(_base + uint32_t(m));
    }
    std::array<int32_t, kClockWindow> _w{};
    size_t _n = 0;
    size_t _i = 0;
    uint32_t _base = 0;
    uint32_t _rtt_us = 0;
};

// ---- kOpCommand -------------------------------------------------------------
// A bare stream sample IS kCmdPoint. The engine's own cadence estimator
// decides dense against isolated, so a second kind for the same rendering is
// two homes for one fact (C-1). Mint a kind when the RENDERING differs, never
// when only the sender's provenance does.
enum CommandKind : uint8_t {
    kCmdPoint = 0,
    kCmdWaveform = 1,
};

// has_end_vel FALSE is the SENTINEL, "no handoff velocity was specified".
// has_end_vel TRUE with end_vel 0 is the explicit "arrive at rest". They are
// different commands and the engine plans them differently; collapsing them
// costs every arrival its stop.
enum CommandFlags : uint8_t {
    kCmdHasEndVel = 0x01,
    kCmdHasAnchor = 0x02,
    kCmdHasNextChord = 0x04,
};

// The ceiling set a command plans under, mirroring the arbiter's source-to-set
// selection (MotionArbiter::_planAndDispatch: MANUAL takes the USER set,
// everything else the INPUT set). It rides the COMMAND rather than config so a
// manual jog and a stream point can be in flight together without an ordering
// dependency on which config push landed last.
enum LimitSet : uint8_t {
    kLimitUser = 0,    // manual moves and UI controls; also every RP-initiated
                       // maneuver: settle, reversal recovery, window-entry and
                       // window glide, which are RECOVERY and never content
    kLimitInput = 1,   // stream, pattern, machine-driven
};

// Fields map one for one onto slopmotion::Command except axis, which this link
// carries from day one (sd-xvc; multi-axis is parked). Axis 0 is the only
// value a single-axis slave accepts; anything else is dropped and reported as
// kEvtCommandGated rather than rendered on the wrong carriage.
struct LinkCommand {
    uint8_t kind = kCmdPoint;
    uint8_t axis = 0;
    uint8_t flags = 0;
    uint8_t curve_family = 0;   // slopmotion Command::client_curve_family
    float target = 0.5f;        // normalized 0..1
    uint32_t duration_us = 0;   // kCmdWaveform only
    float end_vel = 0.0f;       // normalized units/s
    float next_chord = 0.0f;    // magnitude, normalized units/s
    uint32_t anchor_us = 0;     // SLAVE domain, see clockDelta
    uint8_t limit_set = kLimitUser;   // which ceiling set plans THIS command
};
inline constexpr size_t kCommandWireEnd = 27;

inline void encodeCommand(std::span<uint8_t, kFrameBytes> f, uint8_t seq,
                          const LinkCommand& c) {
    for (size_t i = 0; i < kFrameBytes; ++i) f[i] = 0;
    f[0] = kOpCommand;
    f[1] = seq;
    f[2] = c.kind;
    f[3] = c.axis;
    f[4] = c.flags;
    f[5] = c.curve_family;
    putF32(f, 6, c.target);
    putU32(f, 10, c.duration_us);
    putF32(f, 14, c.end_vel);
    putF32(f, 18, c.next_chord);
    putU32(f, 22, c.anchor_us);
    f[26] = c.limit_set;
    crcStamp(f);
}
inline LinkCommand decodeCommand(std::span<const uint8_t, kFrameBytes> f) {
    LinkCommand c;
    c.kind = f[2];
    c.axis = f[3];
    c.flags = f[4];
    c.curve_family = f[5];
    c.target = getF32(f, 6);
    c.duration_us = getU32(f, 10);
    c.end_vel = getF32(f, 14);
    c.next_chord = getF32(f, 18);
    c.anchor_us = getU32(f, 22);
    c.limit_set = f[26];
    return c;
}

// ---- kOpConfig --------------------------------------------------------------
// Field-tagged, so ONE changed field costs ONE frame and any subset is
// re-sendable in any order. Tags are append-only. Value encoding is per tag:
// f32 bits, a plain integer, or an enum ordinal passed through to slopmotion.
// SCOPE RULE (operator ruling 2026-09-02, which overruled a resolved-ceilings
// -only draft): the slave holds BOTH ceiling sets and never exceeds whichever
// applies, because it must be SELF-SUFFICIENT for the maneuvers it starts on
// its own -- reversals, stops, settles, recovery glides -- and those cannot
// wait on a config push to tell them how gentle to be. The SET is chosen per
// command (LinkCommand::limit_set); the S3 still owns the CHOICE.
// Add a tag when the SLAVE must act on a value it cannot derive from one
// already here.
enum ConfigTag : uint8_t {
    // BOTH ceiling sets, normalized units per second^n (slopmotion::Limits);
    // the S3 derives normalized from its mm-domain sets and the window span.
    // The USER set is the gentle pair (speed, accel) and it has NO jerk of its
    // own, because the UI has none to offer: user-set plans take the INPUT
    // jerk ceiling, so jerk stays one fact with one home.
    kCfgInputVmax = 0x01,
    kCfgInputAmax = 0x02,
    kCfgInputJmax = 0x03,
    kCfgUserVmax = 0x04,
    kCfgUserAmax = 0x05,
    // Soft start stays S3 POLICY: the S3 runs the ramp and pushes its current
    // value, the slave applies it as a transient min() over the SELECTED set's
    // vmax. SPEED ONLY, never accel -- soft accel cannot even reach the
    // already-soft speed cap, which renders as freeze-then-jump (the rule and
    // its measurement live in MotionArbiter::_planAndDispatch). 0 = no cap.
    kCfgSoftStartCap = 0x06,
    kCfgSettleGraceUs = 0x07,      // u32 us
    kCfgSampleSynthesis = 0x08,    // u32 bool
    kCfgInfeasiblePolicy = 0x09,   // u32 InfeasiblePolicy ordinal
    kCfgInfeasibleBlend = 0x0A,
    kCfgCurvePolicy = 0x0B,        // u32 CurvePolicy ordinal
    kCfgHandoffChordFactor = 0x0C,
    kCfgOvershootGuard = 0x0D,
    kCfgOvershootChordSlack = 0x0E,
    // The rest of the engine tuning the S3 exposes (EngineConfigMap.h is
    // the S3 census of these; a tuning field without a tag is engine-
    // default-by-decision and named there). kCfgSampleSynthesis (0x08) is
    // RETIRED: synthesis left the engine 2026-09-03; a slave ignores it.
    kCfgBlendSteps = 0x0F,          // u32, infeasible_blend_steps
    kCfgSmoothBudget = 0x10,        // f32, infeasible_smooth_budget
    kCfgAmplitudeBudget = 0x11,     // f32, infeasible_amplitude_budget
    kCfgChaseFeedforward = 0x12,    // u32 bool
    kCfgChaseAccelFf = 0x13,        // u32 bool
    kCfgChaseFfGain = 0x14,         // f32
    kCfgChaseDenseUs = 0x15,        // u32 us
    kCfgChaseLookahead = 0x16,      // f32, intervals
    kCfgChaseAimExtrap = 0x17,      // u32 bool
    kCfgChaseStaleUs = 0x18,        // u32 us
    // Safety gates. ONE tag, because the slave enforces ONE predicate (homed
    // and not paused) and the bits exist so telemetry can name WHICH gate is
    // closed without a second field to disagree with it. Motion denied
    // mid-stroke BRAKES TO REST from live (p, v) and never freezes velocity
    // (sd-dxy.1.4). Commands arriving while denied are dropped and reported as
    // kEvtCommandGated. E-stop is NOT here: it keeps kOpEstop so it can punch
    // through in one 50 us tick.
    kCfgGates = 0x40,              // u32 bitfield, see GateBits
    // Window in the slave's native unit. Normalized 0..1 spans exactly
    // [min, max], so the slave derives its counts/s ceilings from this span
    // and whichever vmax the command selected. The EMITTER cap stays on
    // kOpSetLimits: it is a hardware fault detector, not engine config, and it
    // has one home already.
    kCfgWindowMinCounts = 0x60,
    kCfgWindowMaxCounts = 0x61,
};
enum GateBits : uint32_t {
    kGateHomed = 0x01,
    kGatePaused = 0x02,
    kGateSoftStart = 0x04,   // informational; the cap is kCfgSoftStartCap
};

struct ConfigField {
    uint8_t tag = 0;
    uint32_t raw = 0;
};
inline constexpr size_t kConfigFieldsPerFrame = 5;
inline constexpr size_t kConfigWireEnd = 3 + 5 * kConfigFieldsPerFrame;

inline void encodeConfig(std::span<uint8_t, kFrameBytes> f, uint8_t seq,
                         std::span<const ConfigField> fields) {
    for (size_t i = 0; i < kFrameBytes; ++i) f[i] = 0;
    f[0] = kOpConfig;
    f[1] = seq;
    const size_t n = fields.size() < kConfigFieldsPerFrame
                         ? fields.size()
                         : kConfigFieldsPerFrame;
    f[2] = uint8_t(n);
    for (size_t k = 0; k < n; ++k) {
        const size_t at = 3 + k * 5;
        f[at] = fields[k].tag;
        putU32(f, at + 1, fields[k].raw);
    }
    crcStamp(f);
}
inline size_t decodeConfig(std::span<const uint8_t, kFrameBytes> f,
                           std::span<ConfigField> out) {
    size_t n = f[2];
    if (n > kConfigFieldsPerFrame) n = kConfigFieldsPerFrame;
    if (n > out.size()) n = out.size();
    for (size_t k = 0; k < n; ++k) {
        const size_t at = 3 + k * 5;
        out[k].tag = f[at];
        out[k].raw = getU32(f, at + 1);
    }
    return n;
}

// The applied-config image, held identically on both ends, and the reason
// status carries a FINGERPRINT rather than an epoch counter. A counter says
// "some push landed" and cannot see a lost MIDDLE frame of a multi-frame set,
// because the last frame still bumps it. A checksum of the image is evidence
// that the two ends hold the same bytes, which is C-4 applied to config
// (architecture.md section 4, a result carries a fingerprint of its inputs).
// The slave stores EVERY tag it receives, including ones it cannot apply, so
// the fingerprint still agrees across a firmware skew; an unknown tag is
// reported once as kEvtConfigTagUnknown and never applied.
inline constexpr size_t kConfigImageSlots = 32;
class ConfigImage {
  public:
    // Ascending-tag insertion keeps serialization canonical, so a fingerprint
    // depends on the SET and never on the order the pushes arrived in.
    bool set(uint8_t tag, uint32_t raw) {
        size_t k = 0;
        while (k < _n && _f[k].tag < tag) ++k;
        if (k < _n && _f[k].tag == tag) {
            _f[k].raw = raw;
            return true;
        }
        if (_n >= kConfigImageSlots) return false;
        for (size_t j = _n; j > k; --j) _f[j] = _f[j - 1];
        _f[k] = ConfigField{tag, raw};
        ++_n;
        return true;
    }
    bool setF(uint8_t tag, float v) { return set(tag, f32Bits(v)); }
    uint32_t get(uint8_t tag, uint32_t fallback = 0) const {
        for (size_t k = 0; k < _n; ++k)
            if (_f[k].tag == tag) return _f[k].raw;
        return fallback;
    }
    float getF(uint8_t tag, float fallback = 0.0f) const {
        return bitsF32(get(tag, f32Bits(fallback)));
    }
    bool has(uint8_t tag) const {
        for (size_t k = 0; k < _n; ++k)
            if (_f[k].tag == tag) return true;
        return false;
    }
    size_t size() const { return _n; }
    void clear() { _n = 0; }
    std::span<const ConfigField> fields() const {
        return std::span<const ConfigField>(_f.data(), _n);
    }
    // 0 means EMPTY, never a checksum, so a slave that has applied nothing
    // reads as a mismatch the master cannot miss. That is also the restart
    // detector, which is why the v1 counter-decrease heuristic does not need
    // to be carried into v2.
    uint16_t fingerprint() const {
        if (_n == 0) return 0;
        std::array<uint8_t, kConfigImageSlots * 5> buf{};
        for (size_t k = 0; k < _n; ++k) {
            buf[k * 5] = _f[k].tag;
            putU32(std::span<uint8_t>(buf), k * 5 + 1, _f[k].raw);
        }
        const uint16_t c = crc16(std::span<const uint8_t>(buf.data(), _n * 5));
        return c == 0 ? uint16_t(1) : c;
    }

  private:
    std::array<ConfigField, kConfigImageSlots> _f{};
    size_t _n = 0;
};

// The ceilings a command actually plans under: the selected set, then the
// soft-start cap folded in. ONE place resolves it, so the slave cannot derive
// a different answer for a reversal than for the command that provoked it.
// A zero vmax means the set was never pushed; the slave treats that as
// unconfigured and gates the command rather than planning at zero.
struct SelectedLimits {
    float vmax = 0.0f;
    float amax = 0.0f;
    float jmax = 0.0f;
};
inline SelectedLimits selectedLimits(const ConfigImage& cfg,
                                     uint8_t limit_set) {
    const bool input = limit_set == kLimitInput;
    SelectedLimits l;
    l.vmax = input ? cfg.getF(kCfgInputVmax) : cfg.getF(kCfgUserVmax);
    l.amax = input ? cfg.getF(kCfgInputAmax) : cfg.getF(kCfgUserAmax);
    l.jmax = cfg.getF(kCfgInputJmax);
    const float cap = cfg.getF(kCfgSoftStartCap);
    if (cap > 0.0f && cap < l.vmax) l.vmax = cap;
    return l;
}

// ---- Status v2 and the event record -----------------------------------------
// The slave chooses the reply layout; the master parses on the VARIANT BYTE
// and never on what it sent. Offset 28 is the discriminator because the v1
// path never writes bytes 28 or 29, so a v1 slave reports variant 0 for free.
// Never write byte 28 in the v1 path.
// ONE PRECEDENCE RULE: state (byte 0) is read FIRST and is never overloaded.
// kStateFlash claims the whole reply for the flash family, whose version
// string legitimately covers byte 28 (see kFlashStatusOffVersion above); the
// variant byte means nothing while that state is reported.
inline constexpr size_t kStatusOffVariant = 28;
static_assert(kStatusOffVelClamped + 2 <= kStatusOffVariant,
              "v1 telemetry would collide with the variant discriminator");
enum StatusVariant : uint8_t {
    kStatusV1 = 0x00,
    kStatusV2 = 0x02,
    kStatusEvent = 0x03,
};

// COUNTERS ARE PER-INTERVAL AND RESET ON PRELOAD. That is the v2 fix for the
// blind counter: v1 shipped saturating LIFETIME totals while the master read
// DELTAS, so the only content the extra width carried was the part that pins
// (overrun 61191 in one second, then 0xFFFF forever and every later delta 0).
// Widening postpones that; per-interval removes it, because a full u16 inside
// one 10 ms poll needs 6.5 million events per second and the next interval
// starts clean regardless. The LIFETIME total moves to the master, which
// already accumulates the deltas and can hold u32 there.
// The cost, stated: a torn or bad-CRC reply loses one interval of census.
// These are rate diagnostics rather than accounting, and link_errs counts the
// losses, so the gap is visible instead of silent.
struct StatusV2 {
    uint8_t state = 0;        // State
    uint8_t flags = 0;        // Flags
    uint8_t seq_echo = 0;     // last frame processed: command ack AND clock tag
    uint8_t event_seq = 0;    // rolling; differs from last drained -> pull
    float pos = 0.0f;         // counts, evaluated from the plan
    float vel = 0.0f;         // counts/s
    int16_t residue = 0;      // counts, pos minus emitted, saturating
    uint8_t mode = 0;         // slopmotion::Mode ordinal, pass-through
    uint8_t plan_kind = 0;    // slopmotion::PlanKind ordinal, pass-through
    uint32_t clock_t1 = 0;    // slave us at receipt of frame seq_echo
    uint16_t config_fp = 0;   // ConfigImage fingerprint, 0 = nothing applied
    uint16_t qdrops = 0;
    uint16_t emit_overrun = 0;
    uint8_t late_ticks = 0;
    uint8_t link_errs = 0;    // bad CRC plus torn frames this interval
    uint8_t vel_clamped = 0;
};
inline constexpr size_t kStatusV2WireEnd = 30;

inline void encodeStatusV2(std::span<uint8_t, kFrameBytes> f,
                           const StatusV2& s) {
    for (size_t i = 0; i < kFrameBytes; ++i) f[i] = 0;
    f[0] = s.state;
    f[1] = s.flags;
    f[2] = s.seq_echo;
    f[3] = s.event_seq;
    putF32(f, 4, s.pos);
    putF32(f, 8, s.vel);
    putU16(f, 12, uint16_t(s.residue));
    f[14] = s.mode;
    f[15] = s.plan_kind;
    putU32(f, 16, s.clock_t1);
    putU16(f, 20, s.config_fp);
    putU16(f, 22, s.qdrops);
    putU16(f, 24, s.emit_overrun);
    f[26] = s.late_ticks;
    f[27] = s.link_errs;
    f[kStatusOffVariant] = kStatusV2;
    f[29] = s.vel_clamped;
    crcStamp(f);
}
inline StatusV2 decodeStatusV2(std::span<const uint8_t, kFrameBytes> f) {
    StatusV2 s;
    s.state = f[0];
    s.flags = f[1];
    s.seq_echo = f[2];
    s.event_seq = f[3];
    s.pos = getF32(f, 4);
    s.vel = getF32(f, 8);
    s.residue = int16_t(getU16(f, 12));
    s.mode = f[14];
    s.plan_kind = f[15];
    s.clock_t1 = getU32(f, 16);
    s.config_fp = getU16(f, 20);
    s.qdrops = getU16(f, 22);
    s.emit_overrun = getU16(f, 24);
    s.late_ticks = f[26];
    s.link_errs = f[27];
    s.vel_clamped = f[29];
    return s;
}
// Residue is the ONE number worth its bytes out of the (pos, emitted) pair the
// v1 status shipped whole: their difference is the diagnostic, and shipping it
// directly cannot be read as evidence by itself.
inline int16_t satResidue(float counts) {
    if (counts > 32767.0f) return 32767;
    if (counts < -32768.0f) return -32768;
    return int16_t(counts);
}

// Anomalies ride a PULL rather than the status frame. They are rare, they are
// wider than the bytes status has left, and paying one transaction when status
// says there is an event beats paying ten bytes of every poll forever.
// kOpEventPull carries the last event_seq the master received; the slave pops
// through it and preloads the next record, so a lost reply costs a retry
// instead of an event.
// kind below kEvtLinkBase is a slopmotion::AnomalyType ordinal passed through
// verbatim; this header does not restate that table, slopmotion.hpp is its
// home. At or above kEvtLinkBase the event belongs to the link itself.
inline constexpr uint8_t kEvtLinkBase = 0x80;
enum LinkEvent : uint8_t {
    kEvtCommandGated = 0x80,       // detail = the gate bits that denied it
    kEvtConfigTagUnknown = 0x81,   // detail = the tag
    kEvtClockStep = 0x82,          // the slave time base restarted
    // A plan was adopted (commit, scheduled promotion, settle, hold): the
    // S3 rebuilds its plan strip from these. t_us = plan start (slave us),
    // target = the plan's end position (normalized), detail = duration in
    // seconds (0 = a hold), cmd_seq = the frame that caused it (0 = the
    // slave's own settle or hold). Start position is the status pos at
    // t_us; velocity rides status. Rate is the commit rate, never a tick.
    kEvtPlanAdopted = 0x83,
};
struct EventRecord {
    uint8_t state = 0;      // State, so byte 0 means one thing in every reply
    uint8_t kind = 0;
    uint8_t seq = 0;        // this record's rolling id
    uint8_t remaining = 0;  // records still queued after this one
    uint8_t axis = 0;
    uint8_t cmd_seq = 0;    // frame seq that produced it, 0 = none
    uint32_t t_us = 0;      // slave us at record
    float target = 0.0f;
    float detail = 0.0f;
};
inline constexpr size_t kEventWireEnd = 18;

inline void encodeEventPull(std::span<uint8_t, kFrameBytes> f, uint8_t seq,
                            uint8_t ack_seq) {
    for (size_t i = 0; i < kFrameBytes; ++i) f[i] = 0;
    f[0] = kOpEventPull;
    f[1] = seq;
    f[2] = ack_seq;
    crcStamp(f);
}
inline void encodeEvent(std::span<uint8_t, kFrameBytes> f,
                        const EventRecord& e) {
    for (size_t i = 0; i < kFrameBytes; ++i) f[i] = 0;
    f[0] = e.state;
    f[1] = e.kind;
    f[2] = e.seq;
    f[3] = e.remaining;
    f[4] = e.axis;
    f[5] = e.cmd_seq;
    putU32(f, 6, e.t_us);
    putF32(f, 10, e.target);
    putF32(f, 14, e.detail);
    f[kStatusOffVariant] = kStatusEvent;
    crcStamp(f);
}
inline EventRecord decodeEvent(std::span<const uint8_t, kFrameBytes> f) {
    EventRecord e;
    e.state = f[0];
    e.kind = f[1];
    e.seq = f[2];
    e.remaining = f[3];
    e.axis = f[4];
    e.cmd_seq = f[5];
    e.t_us = getU32(f, 6);
    e.target = getF32(f, 10);
    e.detail = getF32(f, 14);
    return e;
}

// Payload census: every v2 frame ends before the CRC, and status fills to it
// exactly, so a field added to status must displace one already there.
static_assert(kCommandWireEnd <= kCrcOffset, "kOpCommand overruns the CRC");
static_assert(kConfigWireEnd <= kCrcOffset, "kOpConfig overruns the CRC");
static_assert(kEventWireEnd <= kCrcOffset, "event record overruns the CRC");
static_assert(kStatusV2WireEnd == kCrcOffset, "status v2 must fill to the CRC");

// ---- RP2350 firmware update over the link (sd-4k1.3) ------------------------
// Payload budget: [op][seq][off:u24][len] is 6 bytes of header, leaving
// kCrcOffset - 6 bytes of image per frame -- 75 percent of a 32-byte frame.
// The flash mode deliberately does NOT renegotiate the frame size: at the
// proven 200 us inter-frame floor the wire ceiling is ~103 kB/s while the
// RP2350's own erase+program of the same bytes costs more than that, so a
// wider frame would buy time the transfer does not spend. Arithmetic and the
// measured bound live in .claude/rules/build-test-deploy.md.
inline constexpr size_t kFlashChunkOffset = 6;
inline constexpr size_t kFlashChunkBytes  = kCrcOffset - kFlashChunkOffset;
// One RP2350 flash sector. The receiver stages a whole sector in RAM and
// erase-plus-programs it as a unit, so this is also the backpressure quantum:
// `want` stops advancing while a sector write is owed.
inline constexpr uint32_t kFlashSectorBytes = 4096;

// Status overlay reported while state == kStateFlash. Motion is stopped, so
// `pos` and `vel` carry no meaning and the flash reply reuses their bytes
// rather than growing the frame. BOTH ends read these; never transcribe a
// number (T20).
inline constexpr size_t kFlashStatusOffWant   = 6;    // u32le, next byte wanted
inline constexpr size_t kFlashStatusOffResult = 10;   // u8, FlashResult
inline constexpr size_t kFlashStatusOffDetail = 11;   // u8, FlashDetail
// kOpFlashVersion reply only: NUL-padded ASCII in the telemetry tail, which is
// what makes C-8 version verification possible without a motion session.
inline constexpr size_t kFlashStatusOffVersion = 16;
// 12, not 14: kOpFlashVersion is answerable while the renderer is running, and
// that reply is an ORDINARY status frame whose byte 28 is the status-variant
// discriminator. Stopping at 27 keeps the version request from ever being able
// to forge a variant.
inline constexpr size_t kFlashVersionBytes     = 12;
static_assert(kFlashStatusOffVersion + kFlashVersionBytes <= kCrcOffset,
              "version string overruns the CRC field");
static_assert(kFlashStatusOffVersion + kFlashVersionBytes <= kStatusOffVariant,
              "version string would forge a status variant");

// State reported while a firmware write is in flight. Declared here rather
// than inside enum State because this file's tail is the only anchor an
// append may use; the value continues State's numbering.
inline constexpr uint8_t kStateFlash = 0x04;

enum FlashResult : uint8_t {
    kFlashIdle    = 0x00,
    kFlashReady   = 0x01,   // begin accepted, want == 0
    kFlashWriting = 0x02,
    kFlashDone    = 0x03,   // image verified; the slave reboots into it
    kFlashFailed  = 0x04,
};

// Why the failure, so a bench operator reads a cause and not a number.
enum FlashDetail : uint8_t {
    kFlashDetailNone      = 0x00,
    kFlashDetailNoSlot    = 0x01,   // no A/B partition table written yet
    kFlashDetailTooBig    = 0x02,   // declared size exceeds the target slot
    kFlashDetailWriteFail = 0x03,
    kFlashDetailCrc       = 0x04,   // read-back mismatch: the slot is not bought
    kFlashDetailShort     = 0x05,   // end arrived before the last byte
    kFlashDetailBuyFail   = 0x06,   // bootrom refused the flash-update reboot
};

}  // namespace motionlink
