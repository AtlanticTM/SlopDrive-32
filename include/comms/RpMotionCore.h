// RpMotionCore -- the RP2350's motion half: engine, config, queue, render plan.
// Constraints:
// - HARDWARE-FREE and dependency-injected (architecture.md 1, module boundary
//   doctrine): no Arduino, no pico-sdk, no clock of its own. Microseconds come
//   in as a parameter, so the host suite drives the same code the RP runs.
// - NO HEAP after construction and NO MUTEX. Fixed arrays throughout; the
//   command queue and the event ring are SPSC with each side writing only its
//   own index, and the render plan crosses cores behind a sequence lock.
// - THREE CONTEXTS, and each has exactly one door. ingestFrame() runs in the
//   SPI IRQ and may only decode and enqueue -- it never touches the engine,
//   which is single-task by contract. service() runs on core 1 and owns the
//   engine. sampleCounts() runs in the 20 kHz tick on core 0 and reads only
//   the published render plan.
// - `float` per tick; `double` only inside the engine at plan time.
// - Units: the engine is normalized over the window, the wire's LinkCommand is
//   normalized, kCfgWindow* and the status are COUNTS. A normalized value
//   outside 0..1 is a real position outside the window and is never clamped on
//   the wire (docs/rp-motion-port.md, "Units").
// See: docs/rp-motion-port.md (the port contract), include/comms/
//      MotionLinkProtocol.h (the vocabulary), .claude/rules/motion-control.md.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "MotionLinkProtocol.h"
#include "slopmotion/slopmotion.hpp"

namespace rpmotion {

using motionlink::ConfigField;
using motionlink::ConfigImage;
using motionlink::EventRecord;
using motionlink::LinkCommand;
using motionlink::SelectedLimits;
using motionlink::StatusV2;

// ---- Sizing ----------------------------------------------------------------
// The queue absorbs the link's burst, not its rate: commands arrive the moment
// they are produced (architecture.md section 2) and core 1 drains free-running,
// so 16 is roughly a tenth of a second of the densest stream against a drain
// that runs tens of thousands of times a second.
inline constexpr size_t kQueueDepth = 16;
inline constexpr size_t kEventDepth = 32;

// THE RENDER TABLE. One horizon of the plan, cut into equal slices, each slice
// a quintic in its own normalized tau. Eight slices of one millisecond: the
// only reconstruction error lives on a slice that straddles a Ruckig jerk
// switch and scales as |dj| * h^3, which at h = 1 ms is orders under one count
// (see republish()).
inline constexpr size_t   kRenderSlices    = 8;
inline constexpr uint32_t kRenderSliceUs   = 1000;
inline constexpr uint32_t kRenderHorizonUs = kRenderSliceUs * kRenderSlices;

// A tick never spins on the writer: four attempts, then it renders the sample
// it rendered last. One frozen tick is 50 us of held position; an unbounded
// spin in an IRQ is the whole machine.
inline constexpr uint8_t kSeqlockRetries = 4;

// How many distinct unknown config tags are remembered so kEvtConfigTagUnknown
// fires once per tag rather than once per push. Beyond this the tag is still
// STORED (the fingerprint must agree across a firmware skew) and simply not
// re-reported.
inline constexpr size_t kUnknownTagMemory = 8;

// ---- The published render plan ---------------------------------------------
// POD by construction: the tick reads it with no knowledge of slopmotion or
// ruckig and no branch on plan kind. `lo`/`hi` are the engine's own window
// backstop for the plan in flight, carried across so the clamp cannot drift
// from the plan it guards.
struct RenderPlan {
    uint32_t t0_us = 0;
    uint32_t slice_us = kRenderSliceUs;
    float    lo = 0.0f;
    float    hi = 1.0f;
    std::array<std::array<float, 6>, kRenderSlices> c{};
};

// ---- Queue item ------------------------------------------------------------
// One decoded frame on its way from the SPI IRQ to core 1. Ops that need the
// engine ride this; ops the tick must act on within 50 us (estop) are applied
// at ingest AND queued, because the engine still has to be re-seeded.
struct QueueItem {
    uint8_t     op = 0;
    uint8_t     seq = 0;
    uint32_t    rx_us = 0;
    LinkCommand cmd{};
    std::array<ConfigField, motionlink::kConfigFieldsPerFrame> fields{};
    uint8_t     nfields = 0;
    float       counts = 0.0f;     // kOpSetPos target, kOpRetarget target
    float       rt_vmax = 0.0f;    // kOpRetarget, counts/s
    float       rt_accel = 0.0f;   // kOpRetarget, counts/s^2
};

// ---- Event ring record -----------------------------------------------------
struct Event {
    uint8_t  kind = 0;
    uint8_t  seq = 0;
    uint8_t  cmd_seq = 0;
    uint32_t t_us = 0;
    float    target = 0.0f;
    float    detail = 0.0f;
};

class Core {
  public:
    explicit Core(uint32_t now_us = 0) { reset(now_us); }

    // ---- IRQ context -------------------------------------------------------
    // Decode one CRC-checked frame. Returns true when this core owns the op;
    // false leaves it to the caller (the flash family, the emitter cap, ping).
    // NEVER calls the engine: commit() is milliseconds and belongs to core 1.
    bool ingestFrame(std::span<const uint8_t, motionlink::kFrameBytes> f,
                     uint32_t now_us) {
        // Every frame is a clock probe (MotionLinkProtocol.h, "Time"): stamp
        // the receipt instant against the seq the master will pair it with.
        _last_seq.store(f[1], std::memory_order_relaxed);
        _clock_t1.store(now_us, std::memory_order_relaxed);
        switch (f[0]) {
            case motionlink::kOpCommand: {
                QueueItem it;
                it.op = motionlink::kOpCommand;
                it.seq = f[1];
                it.rx_us = now_us;
                it.cmd = motionlink::decodeCommand(f);
                push(it);
                return true;
            }
            case motionlink::kOpConfig: {
                QueueItem it;
                it.op = motionlink::kOpConfig;
                it.seq = f[1];
                it.rx_us = now_us;
                it.nfields = uint8_t(motionlink::decodeConfig(
                    f, std::span<ConfigField>(it.fields)));
                push(it);
                return true;
            }
            case motionlink::kOpRetarget: {
                QueueItem it;
                it.op = motionlink::kOpRetarget;
                it.seq = f[1];
                it.rx_us = now_us;
                it.counts = motionlink::getF32(f, 2);
                it.rt_vmax = motionlink::getF32(f, 6);
                it.rt_accel = motionlink::getF32(f, 10);
                push(it);
                return true;
            }
            case motionlink::kOpEstop:
                estop(now_us);
                return true;
            case motionlink::kOpClear: {
                QueueItem it;
                it.op = motionlink::kOpClear;
                it.seq = f[1];
                it.rx_us = now_us;
                push(it);
                _estop.store(false, std::memory_order_release);
                _flags.store(0, std::memory_order_relaxed);
                return true;
            }
            case motionlink::kOpSetPos:
                setPositionCounts(motionlink::getF32(f, 2), f[1], now_us);
                return true;
            default:
                return false;
        }
    }

    // E-stop acts in the IRQ that carried it: the tick reads `estopped()` and
    // stops rendering this tick, and the queued item re-seeds the engine at the
    // held position whenever core 1 gets there.
    void estop(uint32_t now_us) {
        _estop.store(true, std::memory_order_release);
        QueueItem it;
        it.op = motionlink::kOpEstop;
        it.rx_us = now_us;
        push(it);
    }

    // The homing ritual's "the wall is HERE" write, at standstill. The tick is
    // PINNED to the declared position at once, so no further sample of the plan
    // this write invalidated is rendered; core 1 re-seeds the engine at the
    // honest normalized position and releases the pin. The pin exists because
    // the render plan has exactly ONE writer, core 1.
    void setPositionCounts(float counts, uint8_t seq, uint32_t now_us) {
        _pin_counts = counts;
        _pinned.store(true, std::memory_order_release);
        QueueItem it;
        it.op = motionlink::kOpSetPos;
        it.seq = seq;
        it.rx_us = now_us;
        it.counts = counts;
        push(it);
    }

    // ---- Core 1 ------------------------------------------------------------
    // Drain the queue into the engine, advance it (promotion, settle, coast),
    // drain its anomalies, and republish the render table when the plan changed
    // or the tick has eaten half the horizon.
    void service(uint32_t now_us) {
        advanceClock(now_us);
        drainQueue();

        const slopmotion::Snapshot snap = _engine.snapshot(_now64);
        _mode.store(snap.mode, std::memory_order_relaxed);
        _plan_kind.store(snap.plan_kind, std::memory_order_relaxed);
        _busy.store(_engine.isBusy(_now64), std::memory_order_relaxed);
        drainAnomalies();

        const uint64_t start = _engine.lastPlanUs();
        if (start != _adopt_start || snap.plan_kind != _adopt_kind ||
            snap.mode != _adopt_mode || snap.duration_s != _adopt_dur) {
            _adopt_start = start;
            _adopt_kind = snap.plan_kind;
            _adopt_mode = snap.mode;
            _adopt_dur = snap.duration_s;
            // t_us is the plan START in slave time, target its END, detail its
            // duration in seconds: the S3 rebuilds its plan strip from exactly
            // these (MotionLinkProtocol.h, kEvtPlanAdopted).
            emit(motionlink::kEvtPlanAdopted, snap.target, snap.duration_s,
                 uint32_t(start), _adopt_seq);
            _adopt_seq = 0;
            republish();
        } else if (renderStale(now_us)) {
            republish();
        }
        _serviced.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- 20 kHz tick, core 0 -----------------------------------------------
    // Evaluate the published table and convert to counts. Never blocks, never
    // touches the engine.
    void sampleCounts(uint32_t now_us, float& pos, float& vel) {
        float p = _last_norm;
        float v = 0.0f;
        if (_pinned.load(std::memory_order_acquire)) {
            pos = _pin_counts;
            vel = 0.0f;
            _last_norm = countsToNorm(pos);
            _pos_counts = pos;
            _vel_counts = 0.0f;
            return;
        }
        for (uint8_t attempt = 0; attempt < kSeqlockRetries; ++attempt) {
            const uint32_t s1 = _rp_seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            const uint32_t t0 = _rp.t0_us;
            const uint32_t slice = _rp.slice_us;
            const float lo = _rp.lo, hi = _rp.hi;
            uint32_t dt = now_us - t0;
            size_t k = slice != 0 ? dt / slice : 0;
            float tau;
            if (k >= kRenderSlices) {
                k = kRenderSlices - 1;
                tau = 1.0f;
            } else {
                tau = float(dt - uint32_t(k) * slice) / float(slice);
            }
            const float* c = _rp.c[k].data();
            const float pp =
                ((((c[5] * tau + c[4]) * tau + c[3]) * tau + c[2]) * tau +
                 c[1]) * tau + c[0];
            const float vv =
                ((((5.0f * c[5] * tau + 4.0f * c[4]) * tau + 3.0f * c[3]) *
                  tau + 2.0f * c[2]) * tau + c[1]) /
                (float(slice) * 1e-6f);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (_rp_seq.load(std::memory_order_relaxed) != s1) continue;
            p = pp;
            v = vv;
            // The window backstop, and the reason velocity dies with it: a
            // clamped position reporting outward velocity tells the emitter to
            // keep driving into a rail (slopmotion sampleClampedNoSettle).
            if (p < lo) {
                p = lo;
                if (v < 0.0f) v = 0.0f;
            } else if (p > hi) {
                p = hi;
                if (v > 0.0f) v = 0.0f;
            }
            break;
        }
        _last_norm = p;
        const float span = _span.load(std::memory_order_relaxed);
        const float base = _win_lo.load(std::memory_order_relaxed);
        pos = base + p * span;
        vel = v * span;
        _pos_counts = pos;
        _vel_counts = vel;
    }

    // ---- Status and events -------------------------------------------------
    // Fills what this core owns. The emitter's own census (residue, qdrops,
    // emit_overrun, late_ticks, link_errs, vel_clamped) belongs to the glue and
    // is filled there; those counters are per-interval and reset on preload.
    void fillStatus(StatusV2& s) const {
        s.state = state();
        s.flags = _flags.load(std::memory_order_relaxed);
        s.seq_echo = _last_seq.load(std::memory_order_relaxed);
        s.event_seq = _event_newest.load(std::memory_order_relaxed);
        s.pos = _pos_counts;
        s.vel = _vel_counts;
        s.mode = _mode.load(std::memory_order_relaxed);
        s.plan_kind = _plan_kind.load(std::memory_order_relaxed);
        s.clock_t1 = _clock_t1.load(std::memory_order_relaxed);
        s.config_fp = _config_fp.load(std::memory_order_relaxed);
    }

    // Pop through everything the master has acknowledged, then preload the next
    // record. `remaining` counts what still sits behind it, which is how the
    // master knows to pull again without waiting for another status.
    bool nextEvent(uint8_t ack_seq, EventRecord& out) {
        while (_ev_read != _ev_write) {
            const Event& head = _ev[_ev_read % kEventDepth];
            if (ack_seq == 0 || int8_t(uint8_t(head.seq - ack_seq)) > 0) break;
            _ev_read = uint8_t(_ev_read + 1);
        }
        if (_ev_read == _ev_write) return false;
        const Event& head = _ev[_ev_read % kEventDepth];
        out = EventRecord{};
        out.state = state();
        out.kind = head.kind;
        out.seq = head.seq;
        out.remaining = uint8_t(uint8_t(_ev_write - _ev_read) - 1u);
        out.axis = 0;
        out.cmd_seq = head.cmd_seq;
        out.t_us = head.t_us;
        out.target = head.target;
        out.detail = head.detail;
        return true;
    }

    // ---- Observers the glue needs ------------------------------------------
    bool estopped() const { return _estop.load(std::memory_order_acquire); }
    uint8_t lastSeq() const { return _last_seq.load(std::memory_order_relaxed); }
    // The look-at-me line: an event the master has not pulled yet. There is no
    // runway to be low on any more, so this is what the IRQ pin carries.
    bool eventsPending() const { return _ev_read != _ev_write; }
    uint32_t serviced() const {
        return _serviced.load(std::memory_order_relaxed);
    }
    uint16_t configFingerprint() const {
        return _config_fp.load(std::memory_order_relaxed);
    }
    // The sticky link flags (MotionLinkProtocol.h, Flags); the master reports
    // each bit's rising edge, so a bit must stay set until the condition ends.
    void setFlags(uint8_t bits) {
        _flags.fetch_or(bits, std::memory_order_relaxed);
    }
    void clearFlags(uint8_t bits) {
        _flags.fetch_and(uint8_t(~bits), std::memory_order_relaxed);
    }
    float countsToNorm(float counts) const {
        const float span = _span.load(std::memory_order_relaxed);
        return (counts - _win_lo.load(std::memory_order_relaxed)) / span;
    }
    float normToCounts(float norm) const {
        return _win_lo.load(std::memory_order_relaxed) +
               norm * _span.load(std::memory_order_relaxed);
    }

    // Full re-arm: the clock base, the engine seed, the plan. Emits
    // kEvtClockStep, which is what tells the master its offset estimate is
    // built on a time base that no longer exists.
    // CONSTRAINT: core 1 must not be servicing yet. It publishes the render
    // plan, and this is the one other place that does.
    void reset(uint32_t now_us) {
        _hi64 = 0;
        _last32 = now_us;
        _now64 = now_us;
        _engine.resetAt(0.0f, _now64);
        _seed = 0.0f;
        _adopt_start = _engine.lastPlanUs();
        _adopt_kind = uint8_t(slopmotion::PlanKind::None);
        _adopt_mode = uint8_t(slopmotion::Mode::Idle);
        _adopt_dur = 0.0f;
        _adopt_seq = 0;
        publishHold(0.0f, now_us);
        emit(motionlink::kEvtClockStep, 0.0f, 0.0f, now_us, 0);
    }

    // Sizing evidence for the glue's once-a-second report. ONE engine: the
    // render table is read off the live plan (see republish()).
    static constexpr size_t engineBytes() { return sizeof(slopmotion::Engine); }

  private:
    // ---- Clock -------------------------------------------------------------
    // The wire is 32-bit slave microseconds and the engine wants a monotonic
    // 64-bit clock; core 1 is the only widener, so the wrap is counted in one
    // place. Anchors widen against it with clockDelta, which is unambiguous
    // inside the 35.8 minute horizon the vocabulary already declares.
    void advanceClock(uint32_t now_us) {
        if (now_us < _last32) _hi64 += 0x100000000ull;
        _last32 = now_us;
        _now64 = _hi64 + now_us;
    }
    uint64_t widen(uint32_t t) const {
        return uint64_t(int64_t(_now64) +
                        motionlink::clockDelta(t, uint32_t(_now64)));
    }

    // ---- Command queue (SPSC: SPI IRQ produces, core 1 consumes) -----------
    void push(const QueueItem& it) {
        const uint8_t head = _q_head.load(std::memory_order_relaxed);
        if (uint8_t(head - _q_tail.load(std::memory_order_acquire)) >=
            kQueueDepth) {
            // A full queue is a producer bug: the link sends on arrival and
            // core 1 drains far faster than the wire delivers.
            _flags.fetch_or(motionlink::kFlagOverflow,
                            std::memory_order_relaxed);
            return;
        }
        _q[head % kQueueDepth] = it;
        _q_head.store(uint8_t(head + 1), std::memory_order_release);
    }

    void drainQueue() {
        for (;;) {
            const uint8_t tail = _q_tail.load(std::memory_order_relaxed);
            if (tail == _q_head.load(std::memory_order_acquire)) return;
            apply(_q[tail % kQueueDepth]);
            _q_tail.store(uint8_t(tail + 1), std::memory_order_release);
        }
    }

    void apply(const QueueItem& it) {
        switch (it.op) {
            case motionlink::kOpCommand: applyCommand(it); break;
            case motionlink::kOpConfig:  applyConfig(it);  break;
            case motionlink::kOpRetarget: applyRetarget(it); break;
            case motionlink::kOpSetPos:
                seedAt(countsToNorm(it.counts));
                _pinned.store(false, std::memory_order_release);
                break;
            case motionlink::kOpEstop:
            case motionlink::kOpClear:
                // Both land on a hard hold at the position actually rendered:
                // an estop holds it, a clear resumes from it. Neither may
                // inherit a pre-estop plan.
                seedAt(_last_norm);
                break;
            default: break;
        }
    }

    // ---- Gates -------------------------------------------------------------
    // ONE predicate, homed and not paused; the bits exist so telemetry can name
    // which gate is closed (MotionLinkProtocol.h, kCfgGates). A denied command
    // is DROPPED and reported, never queued for later.
    uint32_t denyingGates() const {
        const uint32_t g = _cfg_img.get(motionlink::kCfgGates);
        uint32_t deny = 0;
        if ((g & motionlink::kGateHomed) == 0) deny |= motionlink::kGateHomed;
        if (g & motionlink::kGatePaused) deny |= motionlink::kGatePaused;
        return deny;
    }

    void applyCommand(const QueueItem& it) {
        const LinkCommand& lc = it.cmd;
        const SelectedLimits lim =
            motionlink::selectedLimits(_cfg_img, lc.limit_set);
        uint32_t deny = denyingGates();
        // Axis 0 is the only carriage this slave has; anything else is dropped
        // rather than rendered on the wrong one (sd-xvc).
        if (lc.axis != 0) deny |= motionlink::kGateAxis;
        // A zero vmax means the set was never pushed: gate it rather than plan
        // at zero, and NAME it, because "gated with detail 0" is unreadable.
        if (!(lim.vmax > 0.0f)) deny |= motionlink::kGateUnconfigured;
        if (deny != 0) {
            emit(motionlink::kEvtCommandGated, lc.target, float(deny),
                 uint32_t(_now64), it.seq);
            return;
        }
        _engine.setConfig(configFor(lc.limit_set));
        slopmotion::Command c;
        c.target = lc.target;
        c.has_duration =
            lc.kind == motionlink::kCmdWaveform && lc.duration_us != 0;
        c.duration_us = lc.duration_us;
        c.has_end_vel = (lc.flags & motionlink::kCmdHasEndVel) != 0;
        c.end_vel = lc.end_vel;
        c.has_next_chord = (lc.flags & motionlink::kCmdHasNextChord) != 0;
        c.next_chord = lc.next_chord;
        c.has_anchor = (lc.flags & motionlink::kCmdHasAnchor) != 0;
        c.anchor_us = c.has_anchor ? widen(lc.anchor_us) : 0;
        c.client_curve_family = lc.curve_family;
        if (_engine.commit(c, _now64)) {
            _adopt_seq = it.seq;
            clearFlags(motionlink::kFlagUnderran);
        }
    }

    // kOpRetarget is the homing ritual's point move and it BYPASSES the homed
    // gate on purpose: homing is what makes the machine homed. It still obeys
    // pause, plans at the USER set, and takes its own commanded ceilings as a
    // further min() so a slow glide stays a slow glide.
    void applyRetarget(const QueueItem& it) {
        if (!(it.rt_vmax > 0.0f) || !(it.rt_accel > 0.0f)) {
            emit(motionlink::kEvtCommandGated, countsToNorm(it.counts),
                 float(motionlink::kGateUnconfigured), uint32_t(_now64),
                 it.seq);
            return;
        }
        if (_cfg_img.get(motionlink::kCfgGates) & motionlink::kGatePaused) {
            emit(motionlink::kEvtCommandGated, countsToNorm(it.counts),
                 float(motionlink::kGatePaused), uint32_t(_now64), it.seq);
            return;
        }
        slopmotion::Config c = configFor(motionlink::kLimitUser);
        // Ceilings are magnitudes. The span carries the frame's sign (native
        // counts run negative for positive mm), so dividing by it signed
        // handed Ruckig a negative vmax: ErrorInvalidInput, -100, the first
        // live homing on the port (2026-09-03).
        const float span = fabsf(_span.load(std::memory_order_relaxed));
        const float vn = it.rt_vmax / span;
        const float an = it.rt_accel / span;
        if (!(c.limits.vmax > 0.0f) || vn < c.limits.vmax) c.limits.vmax = vn;
        if (!(c.limits.amax > 0.0f) || an < c.limits.amax) c.limits.amax = an;
        // Homing runs before any policy push may have landed, and a retarget
        // carries speed and accel but no jerk. Ruckig refuses a zero limit
        // (ErrorInvalidInput, -100: the first live boot, 2026-09-03), so an
        // unpushed jerk ceiling falls back to reaching the commanded accel in
        // 20 ms, which is a positioning glide's shape, never content.
        if (!(c.limits.jmax > 0.0f)) c.limits.jmax = c.limits.amax * 50.0f;
        // A homing glide is a positioning move by definition, so the cold-start
        // governor has nothing left to soften and would only fight the ceilings
        // this command carried.
        c.recovery_vmax = 0.0f;
        _engine.setConfig(c);
        slopmotion::Command cmd;
        cmd.target = countsToNorm(it.counts);
        if (_engine.commit(cmd, _now64)) _adopt_seq = it.seq;
    }

    // ---- Config ------------------------------------------------------------
    void applyConfig(const QueueItem& it) {
        const uint32_t gates_before = _cfg_img.get(motionlink::kCfgGates);
        const float lo_before = _cfg_img.getF(motionlink::kCfgWindowMinCounts);
        const float hi_before = _cfg_img.getF(motionlink::kCfgWindowMaxCounts);
        for (uint8_t k = 0; k < it.nfields; ++k) {
            const ConfigField& fd = it.fields[k];
            if (!known(fd.tag)) noteUnknown(fd.tag, it.seq);
            // STORED whether or not it can be applied: the fingerprint is
            // evidence that both ends hold the same bytes, and it has to agree
            // across a firmware skew (MotionLinkProtocol.h, ConfigImage).
            _cfg_img.set(fd.tag, fd.raw);
        }
        _cfg_fresh = false;
        _config_fp.store(_cfg_img.fingerprint(), std::memory_order_relaxed);

        const float lo = _cfg_img.getF(motionlink::kCfgWindowMinCounts);
        const float hi = _cfg_img.getF(motionlink::kCfgWindowMaxCounts);
        if (lo != lo_before || hi != hi_before) adoptWindow(lo, hi);

        // Motion denied mid-stroke BRAKES TO REST from live state and never
        // freezes velocity (sd-dxy.1.4). The engine's own settle is a private
        // boundary event, so the public expression of it is a point move to
        // where the machine is now, at the user set: Ruckig arrives at rest.
        const uint32_t gates = _cfg_img.get(motionlink::kCfgGates);
        const bool denied_now = denyingGates() != 0;
        const bool denied_before =
            (gates_before & motionlink::kGateHomed) == 0 ||
            (gates_before & motionlink::kGatePaused) != 0;
        (void)gates;
        if (denied_now && !denied_before && _engine.isBusy(_now64)) {
            slopmotion::Config c = configFor(motionlink::kLimitUser);
            c.recovery_vmax = 0.0f;
            if (c.limits.vmax > 0.0f) {
                _engine.setConfig(c);
                slopmotion::Command stop;
                stop.target = _engine.positionAt(_now64);
                _engine.commit(stop, _now64);
            }
        }
    }

    // The window is the unit conversion, so a window edit moves every
    // normalized position under the machine. Re-seed at the COUNTS actually
    // rendered: the carriage does not move, the engine's belief follows it, and
    // a hard hold is the honest answer to an edit made mid-stroke.
    void adoptWindow(float lo, float hi) {
        const float counts = _pos_counts;
        const float span = (hi - lo) != 0.0f ? (hi - lo) : 1.0f;
        _win_lo.store(lo, std::memory_order_relaxed);
        _span.store(span, std::memory_order_relaxed);
        seedAt((counts - lo) / span);
    }

    void seedAt(float norm) {
        if (!std::isfinite(norm)) norm = 0.0f;
        _engine.resetAt(norm, _now64);
        _seed = norm;
        _last_norm = norm;
        republish();
    }

    // Every ConfigTag the engine can act on, mapped ONCE. A tag missing from
    // the push keeps the ENGINE default rather than a zero: a default-
    // constructed Config pushed as if it were configuration is sd-6b2.4.
    slopmotion::Config configFor(uint8_t limit_set) {
        const bool user = limit_set != motionlink::kLimitInput;
        if (_cfg_fresh) return user ? _cfg_user : _cfg_input;
        buildConfig(_cfg_user, motionlink::kLimitUser);
        buildConfig(_cfg_input, motionlink::kLimitInput);
        _cfg_fresh = true;
        return user ? _cfg_user : _cfg_input;
    }

    void buildConfig(slopmotion::Config& c, uint8_t limit_set) const {
        c = slopmotion::Config{};
        const SelectedLimits l = motionlink::selectedLimits(_cfg_img, limit_set);
        c.limits.vmax = l.vmax;
        c.limits.amax = l.amax;
        c.limits.jmax = l.jmax;
        // The cold-start governor IS the user ceiling: an opening plan out of
        // rest is a positioning move, and the S3 derives the same number the
        // same way (EngineConfigMap.h). No tag of its own, by that identity.
        c.recovery_vmax = _cfg_img.getF(motionlink::kCfgUserVmax);
        c.settle_grace_us =
            _cfg_img.get(motionlink::kCfgSettleGraceUs, c.settle_grace_us);
        // Ordinals are SPARSE (Stretch 0, Blend 5) and pinned by NVS; anything
        // that is not Stretch resolves to Blend, exactly as the host maps a
        // stored value (slopmotion.hpp, InfeasiblePolicy).
        c.infeasible_policy = _cfg_img.get(motionlink::kCfgInfeasiblePolicy,
                                           uint32_t(c.infeasible_policy)) == 0
                                  ? slopmotion::InfeasiblePolicy::Stretch
                                  : slopmotion::InfeasiblePolicy::Blend;
        c.infeasible_blend =
            _cfg_img.getF(motionlink::kCfgInfeasibleBlend, c.infeasible_blend);
        const uint32_t cp = _cfg_img.get(motionlink::kCfgCurvePolicy,
                                         uint32_t(c.curve_policy));
        c.curve_policy = slopmotion::CurvePolicy(cp > 2 ? 0 : cp);
        c.handoff_chord_factor = _cfg_img.getF(
            motionlink::kCfgHandoffChordFactor, c.handoff_chord_factor);
        c.overshoot_guard =
            _cfg_img.getF(motionlink::kCfgOvershootGuard, c.overshoot_guard);
        c.overshoot_chord_slack = _cfg_img.getF(
            motionlink::kCfgOvershootChordSlack, c.overshoot_chord_slack);
        c.infeasible_blend_steps =
            uint8_t(_cfg_img.get(motionlink::kCfgBlendSteps,
                                 c.infeasible_blend_steps));
        c.infeasible_smooth_budget = _cfg_img.getF(
            motionlink::kCfgSmoothBudget, c.infeasible_smooth_budget);
        c.infeasible_amplitude_budget = _cfg_img.getF(
            motionlink::kCfgAmplitudeBudget, c.infeasible_amplitude_budget);
        c.chase_feedforward =
            _cfg_img.get(motionlink::kCfgChaseFeedforward,
                         c.chase_feedforward ? 1u : 0u) != 0;
        c.chase_accel_ff = _cfg_img.get(motionlink::kCfgChaseAccelFf,
                                        c.chase_accel_ff ? 1u : 0u) != 0;
        c.chase_ff_gain =
            _cfg_img.getF(motionlink::kCfgChaseFfGain, c.chase_ff_gain);
        c.chase_dense_us =
            _cfg_img.get(motionlink::kCfgChaseDenseUs, c.chase_dense_us);
        c.chase_lookahead =
            _cfg_img.getF(motionlink::kCfgChaseLookahead, c.chase_lookahead);
        c.chase_aim_accel_extrap =
            _cfg_img.get(motionlink::kCfgChaseAimExtrap,
                         c.chase_aim_accel_extrap ? 1u : 0u) != 0;
        c.chase_stale_us =
            _cfg_img.get(motionlink::kCfgChaseStaleUs, c.chase_stale_us);
    }

    // 0x08 is RETIRED (synthesis left the engine): stored so the fingerprint
    // agrees, never applied, never reported unknown.
    static bool known(uint8_t tag) {
        switch (tag) {
            case motionlink::kCfgInputVmax:
            case motionlink::kCfgInputAmax:
            case motionlink::kCfgInputJmax:
            case motionlink::kCfgUserVmax:
            case motionlink::kCfgUserAmax:
            case motionlink::kCfgSoftStartCap:
            case motionlink::kCfgSettleGraceUs:
            case motionlink::kCfgSampleSynthesis:
            case motionlink::kCfgInfeasiblePolicy:
            case motionlink::kCfgInfeasibleBlend:
            case motionlink::kCfgCurvePolicy:
            case motionlink::kCfgHandoffChordFactor:
            case motionlink::kCfgOvershootGuard:
            case motionlink::kCfgOvershootChordSlack:
            case motionlink::kCfgBlendSteps:
            case motionlink::kCfgSmoothBudget:
            case motionlink::kCfgAmplitudeBudget:
            case motionlink::kCfgChaseFeedforward:
            case motionlink::kCfgChaseAccelFf:
            case motionlink::kCfgChaseFfGain:
            case motionlink::kCfgChaseDenseUs:
            case motionlink::kCfgChaseLookahead:
            case motionlink::kCfgChaseAimExtrap:
            case motionlink::kCfgChaseStaleUs:
            case motionlink::kCfgGates:
            case motionlink::kCfgWindowMinCounts:
            case motionlink::kCfgWindowMaxCounts:
                return true;
            default:
                return false;
        }
    }

    void noteUnknown(uint8_t tag, uint8_t seq) {
        for (size_t k = 0; k < _unknown_n; ++k)
            if (_unknown[k] == tag) return;
        if (_unknown_n < kUnknownTagMemory) _unknown[_unknown_n++] = tag;
        emit(motionlink::kEvtConfigTagUnknown, 0.0f, float(tag),
             uint32_t(_now64), seq);
    }

    // ---- Events ------------------------------------------------------------
    // Produced on core 1 only, consumed by the SPI IRQ: each side writes one
    // index. Seq 0 is reserved for "no event", so the master's ack of 0 acks
    // nothing.
    void emit(uint8_t kind, float target, float detail, uint32_t t_us,
              uint8_t cmd_seq) {
        if (uint8_t(_ev_write - _ev_read) >= kEventDepth) {
            _ev_read = uint8_t(_ev_read + 1);   // oldest loses, never the newest
        }
        _ev_seq = uint8_t(_ev_seq + 1);
        if (_ev_seq == 0) _ev_seq = 1;
        Event& slot = _ev[_ev_write % kEventDepth];
        slot.kind = kind;
        slot.seq = _ev_seq;
        slot.cmd_seq = cmd_seq;
        slot.t_us = t_us;
        slot.target = target;
        slot.detail = detail;
        _ev_write = uint8_t(_ev_write + 1);
        _event_newest.store(_ev_seq, std::memory_order_release);
    }

    // Engine anomaly ordinals pass through VERBATIM; the table's one home is
    // slopmotion.hpp and this file does not restate it (T20).
    void drainAnomalies() {
        slopmotion::Anomaly a;
        while (_engine.popAnomaly(a)) {
            if (a.kind == uint8_t(slopmotion::AnomalyType::SettleEngaged))
                setFlags(motionlink::kFlagUnderran);
            emit(a.kind, a.target, a.detail, uint32_t(a.t_us), _adopt_seq);
        }
    }

    // ---- The render hand-off -----------------------------------------------
    // The plan comes off the engine AS DATA (slopmotion::PlanView) and is
    // sampled with no side effects: the accessor does not settle, promote or
    // coast, so building the table cannot disturb the engine core 1 owns.
    // The scheduled successor is in the view too, and it takes over at its own
    // anchor exactly where the engine promotes it, which is what keeps a
    // promotion inside the horizon continuous.
    //
    // A quintic through (p, v, a) at both ends of a slice reproduces a Hermite
    // plan exactly and a single Ruckig jerk phase exactly, so error exists only
    // on a slice that straddles a phase switch and scales as |dj| * h^3; at
    // h = 1 ms it is orders below one count.
    //
    // What the view CANNOT show is a settle that has not been planned yet: a
    // brake is a Ruckig solve and belongs to core 1. Until core 1 reaches it
    // the table renders the engine's own bounded coast, and core 1 republishes
    // on the plan change, which it detects within one service pass.
    void republish() {
        const slopmotion::PlanView pv = _engine.planView();
        RenderPlan next;
        next.t0_us = uint32_t(_now64);
        next.slice_us = kRenderSliceUs;
        // The engine's own backstop for the plan in flight, carried across so
        // the tick's clamp cannot drift from the plan it guards.
        next.lo = float(pv.lo);
        next.hi = float(pv.hi);
        const double T = double(kRenderSliceUs) * 1e-6;
        double p0, v0, a0, p1, v1, a1, c[6];
        evalPlanAt(pv, _now64, p0, v0, a0);
        for (size_t k = 0; k < kRenderSlices; ++k) {
            evalPlanAt(pv, _now64 + uint64_t(k + 1) * kRenderSliceUs, p1, v1,
                       a1);
            slopmotion::Engine::senderCurve(false, p0, v0, a0, p1, v1, a1, T, c);
            for (size_t i = 0; i < 6; ++i) next.c[k][i] = float(c[i]);
            p0 = p1;
            v0 = v1;
            a0 = a1;
        }
        commitPlan(next);
    }

    // Promotion, on the render side: past the successor's anchor the successor
    // is the plan. One branch, and it is the same test promoteDue makes.
    // The view carries ONE successor however many are queued, which covers the
    // kRenderHorizonUs table as long as two anchors are not closer than that;
    // a closer pair renders from the republish that follows the promotion.
    static void evalPlanAt(const slopmotion::PlanView& pv, uint64_t t_us,
                           double& p, double& v, double& a) {
        const slopmotion::PlanPiece& pc =
            (pv.next_ok && t_us >= pv.next.start_us) ? pv.next : pv.active;
        slopmotion::Engine::evalPiece(pc, t_us, p, v, a);
    }

    // A constant plan: the machine is held here, so every slice is the same
    // degenerate quintic and the tick needs no special case.
    void publishHold(float norm, uint32_t now_us) {
        if (!std::isfinite(norm)) norm = 0.0f;
        RenderPlan next;
        next.t0_us = now_us;
        next.slice_us = kRenderSliceUs;
        next.lo = norm < 0.0f ? norm : 0.0f;
        next.hi = norm > 1.0f ? norm : 1.0f;
        for (size_t k = 0; k < kRenderSlices; ++k) next.c[k][0] = norm;
        commitPlan(next);
    }

    // Sequence lock: odd while the writer is inside, so a reader that sees an
    // odd count or a changed count retries. The writer is core 1 and the reader
    // is a 20 kHz IRQ on core 0, which is why the reader's retries are bounded.
    void commitPlan(const RenderPlan& p) {
        _rp_seq.fetch_add(1, std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_release);
        _rp = p;
        std::atomic_thread_fence(std::memory_order_release);
        _rp_seq.fetch_add(1, std::memory_order_release);
    }

    // Rebuild once the tick has eaten half the horizon, so the table is always
    // fresher than the time it covers.
    bool renderStale(uint32_t now_us) const {
        return (now_us - _rp.t0_us) > (kRenderHorizonUs / 2);
    }

    uint8_t state() const {
        if (_estop.load(std::memory_order_relaxed)) return motionlink::kStateEstop;
        if (_mode.load(std::memory_order_relaxed) ==
            uint8_t(slopmotion::Mode::Settle))
            return motionlink::kStateSettled;
        return _busy.load(std::memory_order_relaxed) ? motionlink::kStateRunning
                                                     : motionlink::kStateIdle;
    }

    // ---- State -------------------------------------------------------------
    slopmotion::Engine _engine{};
    ConfigImage _cfg_img{};
    slopmotion::Config _cfg_user{};
    slopmotion::Config _cfg_input{};
    bool _cfg_fresh = false;

    uint64_t _now64 = 0;
    uint64_t _hi64 = 0;
    uint32_t _last32 = 0;
    float _seed = 0.0f;

    std::array<QueueItem, kQueueDepth> _q{};
    std::atomic<uint8_t> _q_head{0};   // SPI IRQ
    std::atomic<uint8_t> _q_tail{0};   // core 1

    std::array<Event, kEventDepth> _ev{};
    uint8_t _ev_write = 0;   // core 1
    uint8_t _ev_read = 0;    // SPI IRQ
    uint8_t _ev_seq = 0;
    std::atomic<uint8_t> _event_newest{0};

    RenderPlan _rp{};
    std::atomic<uint32_t> _rp_seq{0};

    uint64_t _adopt_start = 0;
    uint8_t _adopt_kind = 0;
    uint8_t _adopt_mode = 0;
    float _adopt_dur = 0.0f;
    uint8_t _adopt_seq = 0;

    std::array<uint8_t, kUnknownTagMemory> _unknown{};
    size_t _unknown_n = 0;

    // Written by the tick, read by the status preload: same core, single-word.
    float _last_norm = 0.0f;
    float _pos_counts = 0.0f;
    float _vel_counts = 0.0f;
    float _pin_counts = 0.0f;
    std::atomic<bool> _pinned{false};

    std::atomic<float> _win_lo{0.0f};
    std::atomic<float> _span{1.0f};
    std::atomic<bool> _estop{false};
    std::atomic<uint8_t> _flags{0};
    std::atomic<uint8_t> _last_seq{0};
    std::atomic<uint8_t> _mode{0};
    std::atomic<uint8_t> _plan_kind{0};
    std::atomic<bool> _busy{false};
    std::atomic<uint32_t> _clock_t1{0};
    std::atomic<uint16_t> _config_fp{0};
    std::atomic<uint32_t> _serviced{0};
};

}  // namespace rpmotion
