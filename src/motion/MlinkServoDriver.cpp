// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - ONE OWNER TASK, captured on the first update() (motorTask, Core 1). Only
//   the owner runs xfer(); every other task posts and the owner ships. See
//   the header for the whole ownership rule.
// - Commands ship ON ARRIVAL from the owner; kTickMs paces the status poll
//   and nothing else (architecture.md section 2).
// - Frame trust = CRC both directions. Config heals through the fingerprint
//   compare, homing retargets through the idempotent refresh, estop/clear by
//   repetition until the echoed state confirms.
// - >=200 us between transactions: the slave block-resets its SPI per frame.
// See: include/motion/MlinkServoDriver.h, docs/rp-motion-port.md.

#include "motion/MlinkServoDriver.h"

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <cstring>

#include "system/config_api.h"
#include "system/MotionPassthrough.h"
#include "sloplog/sloplog.h"

using namespace motionlink;

// Drop-in pinout (docs/rp2350-wiring.md): the GPIO matrix remaps roles.
namespace {
constexpr int8_t kSck = 7, kMiso = 10, kMosi = 38, kCs = 48, kIrq = 4;
SPIClass s_spi(FSPI);
// Status poll only. Nothing else in this file is clocked.
constexpr uint32_t kTickMs = 10;
constexpr uint32_t kRefreshMs = 100;
// A config push is legitimately unacknowledged for one poll, so the
// fingerprint compare debounces before it resends anything.
constexpr uint32_t kConfigResyncMs = 250;
// Event frames pulled per tick. 8 x ~232 us is under 2 ms on the motion core;
// the rest ride the next tick, and `remaining` says they are there.
constexpr uint8_t kEventPullPerTick = 8;
}  // namespace

// ---- wire ------------------------------------------------------------------

static uint32_t s_lastEndUs = 0;   // busXfer's gap clock, both callers

void MlinkServoDriver::busXfer(uint8_t (&out)[kFrameBytes],
                               uint8_t (&in)[kFrameBytes]) {
    static_assert(kSpiMode == 1, "PL022 slave needs CPHA=1");
    const uint32_t sinceUs = micros() - s_lastEndUs;
    // 200 us gap: the slave's 20 kHz tick (50 us period) ALWAYS fires inside
    // any inter-frame gap, and at 60 us a heavy render tick plus the per-frame
    // SPI re-arm did not reliably fit before the next frame's clocks arrived
    // (measured ~1 torn frame per 4-8 s under motor load, 2026-08-08).
    if (sinceUs < 200) delayMicroseconds(200 - sinceUs);
    crcStamp(out);
    s_spi.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE1));
    // Scheduler lock for the ~40 us transaction: a same-core task otherwise
    // preempts mid-frame -- CS low, clock frozen -- and the slave's IRQ spin
    // bails at ~300 us of silence, tearing the frame. ISRs stay enabled.
    vTaskSuspendAll();
    digitalWrite(kCs, LOW);
    s_spi.transferBytes(out, in, kFrameBytes);
    digitalWrite(kCs, HIGH);
    xTaskResumeAll();
    s_spi.endTransaction();
    s_lastEndUs = micros();
}

void MlinkServoDriver::xfer(uint8_t (&out)[kFrameBytes],
                            uint8_t (&in)[kFrameBytes]) {
    const uint32_t t0 = micros();
    busXfer(out, in);
    const uint32_t t3 = s_lastEndUs;
    // Stamp EVERY outgoing frame: the reply that carries this frame's
    // clock_t1 arrives one transaction later and is paired by seq.
    Stamp& st = _stamp[out[1] & 3u];
    st.t0 = t0;
    st.t3 = t3;
    st.seq = out[1];
    st.ok = true;
}

bool MlinkServoDriver::isOwner() const {
    return _owner.load(std::memory_order_acquire) ==
           (void*)xTaskGetCurrentTaskHandle();
}

void MlinkServoDriver::sendOp(uint8_t op) {
    uint8_t out[kFrameBytes] = {op, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
}

void MlinkServoDriver::pushCeiling(float mm_s) {
    _ceiling_mm_s = mm_s;
    const float cps = mm_s * AIM_STEPS_PER_MM;
    uint8_t out[kFrameBytes] = {kOpSetLimits, ++_seq};
    memcpy(&out[2], &cps, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    SLOGI("mlink", "render ceiling -> %.0f mm/s (%.0f counts/s)",
          (double)mm_s, (double)cps);
}

void MlinkServoDriver::setRenderCeiling(float mm_s) {
    if (!(mm_s > 0.0f)) return;
    // Reached from httpTask (WebUI settings) and from setup() before the owner
    // exists: only the owner may drive the bus, so everyone else posts.
    if (_begun && isOwner()) { pushCeiling(mm_s); return; }
    _ceiling_req.store(mm_s, std::memory_order_release);
}

void MlinkServoDriver::sendRetarget() {
    uint8_t out[kFrameBytes] = {kOpRetarget, ++_seq};
    memcpy(&out[2], &_rt_target, 4);
    memcpy(&out[6], &_rt_v, 4);
    memcpy(&out[10], &_rt_a, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    _rt_seq = _seq;
    _rt_unacked = true;
    _rt_dirty = false;
    _last_cmd_ms = millis();
}

// ---- command and config forwarding -----------------------------------------

void MlinkServoDriver::shipCommand(const LinkCommand& in_cmd) {
    LinkCommand c = in_cmd;
    // ONE conversion, HERE, because the master owns the offset estimate. An
    // unconverged filter is a rendered position error, so the anchor is
    // dropped rather than guessed: the slave then plans at arrival.
    if (c.flags & kCmdHasAnchor) {
        if (_clock.converged()) c.anchor_us = _clock.toSlave(c.anchor_us);
        else c.flags = uint8_t(c.flags & ~uint8_t(kCmdHasAnchor));
    }
    uint8_t out[kFrameBytes] = {};
    encodeCommand(std::span<uint8_t, kFrameBytes>(out, kFrameBytes), ++_seq, c);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    _last_cmd_ms = millis();
}

void MlinkServoDriver::shipConfig(uint8_t tag, uint32_t raw) {
    const ConfigField f{tag, raw};
    uint8_t out[kFrameBytes] = {};
    encodeConfig(std::span<uint8_t, kFrameBytes>(out, kFrameBytes), ++_seq,
                 std::span<const ConfigField>(&f, 1));
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
}

void MlinkServoDriver::sendCommand(const LinkCommand& c) {
    if (!_begun) return;
    if (isOwner()) { shipCommand(c); return; }
    // Non-owner (Core 0 has none today; the arbiter runs on motorTask). Post
    // and let the owner ship: NEWEST WINS is wrong for a queue of distinct
    // intents, so a full ring drops the arrival and says so.
    const uint8_t head = _cmd_head.load(std::memory_order_relaxed);
    const uint8_t next = uint8_t((head + 1u) % kPostDepth);
    if (next == _cmd_tail.load(std::memory_order_acquire)) {
        SLOGW_EVERY_MS(2000, "mlink", "command post queue full -- intent dropped");
        return;
    }
    _cmd_post[head] = c;
    _cmd_head.store(next, std::memory_order_release);
}

void MlinkServoDriver::pushConfig(uint8_t tag, uint32_t raw) {
    if (!_begun) return;
    // CHANGE DETECTION: one tag ships once per change. The image is the
    // master's record of what it BELIEVES the slave holds; the fingerprint
    // compare in update() is what catches a push that never landed.
    if (_cfg.has(tag) && _cfg.get(tag) == raw) return;
    _cfg.set(tag, raw);
    if (isOwner()) { shipConfig(tag, raw); return; }
    const uint8_t head = _cfgp_head.load(std::memory_order_relaxed);
    const uint8_t next = uint8_t((head + 1u) % kPostDepth);
    if (next == _cfgp_tail.load(std::memory_order_acquire)) {
        // Not lost: the image already holds the value, so the fingerprint
        // mismatch resync ships it within kConfigResyncMs.
        SLOGW_EVERY_MS(2000, "mlink", "config post queue full -- resync will carry it");
        return;
    }
    _cfg_post[head] = ConfigField{tag, raw};
    _cfgp_head.store(next, std::memory_order_release);
}

void MlinkServoDriver::drainPosts() {
    while (_cmd_tail.load(std::memory_order_relaxed) !=
           _cmd_head.load(std::memory_order_acquire)) {
        const uint8_t tail = _cmd_tail.load(std::memory_order_relaxed);
        shipCommand(_cmd_post[tail]);
        _cmd_tail.store(uint8_t((tail + 1u) % kPostDepth),
                        std::memory_order_release);
    }
    while (_cfgp_tail.load(std::memory_order_relaxed) !=
           _cfgp_head.load(std::memory_order_acquire)) {
        const uint8_t tail = _cfgp_tail.load(std::memory_order_relaxed);
        shipConfig(_cfg_post[tail].tag, _cfg_post[tail].raw);
        _cfgp_tail.store(uint8_t((tail + 1u) % kPostDepth),
                         std::memory_order_release);
    }
}

// ---- status, clock, events --------------------------------------------------

uint32_t MlinkServoDriver::statusAgeMs() const {
    return millis() - _status_ms;
}

bool MlinkServoDriver::popEvent(EventRecord& out) {
    const uint8_t tail = _evt_tail.load(std::memory_order_relaxed);
    if (tail == _evt_head.load(std::memory_order_acquire)) return false;
    out = _evt_ring[tail];
    _evt_tail.store(uint8_t((tail + 1u) % kEventDepth), std::memory_order_release);
    return true;
}

void MlinkServoDriver::pollStatus() {
    uint8_t out[kFrameBytes] = {kOpPing, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    consumeReply(std::span<const uint8_t, kFrameBytes>(in, kFrameBytes));
}

// EVERY reply is the slave's preload from the PREVIOUS frame, whatever this
// frame asked. A ping after an event pull receives the event; a pull after a
// ping receives the status. Parse on the variant byte, never on what we sent:
// pairing by op deadlocked the link on the first live boot (2026-09-03, an
// unacked event rode every ping reply and the pull path never saw it).
void MlinkServoDriver::requestVersion() {
    uint8_t out[kFrameBytes] = {kOpFlashVersion, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    consumeReply(std::span<const uint8_t, kFrameBytes>(in, kFrameBytes));
    _ver_pending = true;
}

void MlinkServoDriver::consumeReply(std::span<const uint8_t, kFrameBytes> reply) {
    if (_ver_pending) {
        // The version overlays the status tail and its last byte may cover the
        // variant slot, so this reply is a version reply, never a status.
        _ver_pending = false;
        char v[kFlashVersionBytes + 1] = {};
        bool printable = crcOk(reply);
        if (printable) {
            memcpy(v, &reply[kFlashStatusOffVersion], kFlashVersionBytes);
            for (char c : v) if (c != '\0' && (c < 0x20 || c > 0x7e)) printable = false;
        }
        // A reply that is not the version (the slave was mid-resync) reads as
        // binary; leave the string empty so the tick asks again, a few times.
        if (printable && v[0] != '\0') {
            memcpy(_rp_fw, v, sizeof(v));
            SLOGI("mlink", "RP fw '%s'", _rp_fw);
        } else if (++_ver_tries >= 5) {
            SLOGW("mlink", "RP fw version unreadable after %u tries", unsigned(_ver_tries));
            memcpy(_rp_fw, "?", 2);   // stop asking
        }
        return;
    }
    if (!crcOk(reply)) {
        ++_reply_crc_bad;
        SLOGW_EVERY_MS(5000, "mlink",
                       "reply CRC bad (%lu total) -- link out of sync: "
                       "%02X %02X %02X %02X %02X %02X %02X %02X .. [28]=%02X",
                       (unsigned long)_reply_crc_bad, reply[0], reply[1], reply[2],
                       reply[3], reply[4], reply[5], reply[6], reply[7],
                       reply[kStatusOffVariant]);
        return;
    }
    if (reply[0] == kStateFlash) return;       // the flash family owns the reply
    const uint8_t variant = reply[kStatusOffVariant];
    if (variant == kStatusV2) { applyStatus(decodeStatusV2(reply)); return; }
    if (variant == kStatusEvent) { applyEvent(decodeEvent(reply)); return; }
    SLOGW_EVERY_MS(10000, "mlink",
                   "slave answers status variant %u, not v2 -- flash the RP",
                   unsigned(variant));
}

void MlinkServoDriver::applyStatus(const StatusV2& s) {

    // Clock: every frame is a probe, paired BY SEQ (a mispaired sample is
    // biased early and the minimum filter would latch onto it forever).
    const Stamp& st = _stamp[s.seq_echo & 3u];
    if (st.ok && st.seq == s.seq_echo) _clock.push(st.t0, st.t3, s.clock_t1);

    // Rising-edge fault surfacing: UNDERRAN mid-stream is starvation, expected
    // at stream end.
    const uint8_t rising = uint8_t(s.flags & uint8_t(~_slave_flags));
    if (rising & kFlagOverflow)
        SLOGW("mlink", "RP command ring OVERFLOW: an intent was dropped");
    if (rising & kFlagUnderran)
        SLOGI("mlink", "RP underran -> SETTLE (expected at stream end)");
    _slave_flags = s.flags;
    _state = s.state;

    // Counters are PER-INTERVAL and reset on preload, so the lifetime total
    // lives here and nowhere else (MotionLinkProtocol.h, StatusV2).
    _tot_qdrops  += s.qdrops;
    _tot_overrun += s.emit_overrun;
    _tot_late    += s.late_ticks;
    _tot_clamped += s.vel_clamped;
    _tot_link_errs += s.link_errs;
    _rc_qd += s.qdrops;
    _rc_ov += s.emit_overrun;
    _rc_lt += s.late_ticks;
    _rc_vc += s.vel_clamped;
    _rc_le += s.link_errs;
    if (abs(int(s.residue)) > abs(int(_rc_res_max))) _rc_res_max = s.residue;

    _status = s;
    _status_ms = millis();
    _status_fresh = true;

    const uint32_t now = _status_ms;
    if (now - _rc_ms >= 1000u) {
        _rc_ms = now;
        if (_rc_qd || _rc_ov || _rc_lt || _rc_vc || _rc_le || _rc_res_max) {
            SLOGW("mlink", "RP renderer/s: qdrops=%lu overrun=%lu late=%lu "
                  "clamped=%lu linkerr=%lu residue_max=%d counts",
                  (unsigned long)_rc_qd, (unsigned long)_rc_ov,
                  (unsigned long)_rc_lt, (unsigned long)_rc_vc,
                  (unsigned long)_rc_le, int(_rc_res_max));
        }
        _rc_qd = _rc_ov = _rc_lt = _rc_vc = _rc_le = 0;
        _rc_res_max = 0;
    }
}

// Callers decide THAT there is something to pull (the IRQ line, or a status
// event_seq that moved); this only pulls.
void MlinkServoDriver::pumpEvents() {
    // Each pull's reply lands on the NEXT transaction (consumeReply), so the
    // loop keys on the slave's own line, which it refreshes per preload, and
    // never on the reply it just received.
    for (uint8_t n = 0; n < kEventPullPerTick; ++n) {
        uint8_t out[kFrameBytes] = {};
        encodeEventPull(std::span<uint8_t, kFrameBytes>(out, kFrameBytes),
                        ++_seq, _evt_acked);
        uint8_t in[kFrameBytes] = {};
        xfer(out, in);
        consumeReply(std::span<const uint8_t, kFrameBytes>(in, kFrameBytes));
        if (digitalRead(kIrq) == LOW) return;
    }
}

void MlinkServoDriver::applyEvent(const EventRecord& e) {
    _evt_acked = e.seq;
    _evt_acked = e.seq;

    if (e.kind == kEvtPlanAdopted) {
        // The plan strip's whole feed. Slave -> master is the filter's
        // inverse; elapsed is then measured in the clock the UI holds.
        PlanEvent p;
        p.due_master_us = e.t_us - _clock.offsetUs();
        p.late_us = int32_t(micros() - p.due_master_us);
        p.target = e.target;
        p.duration_us = uint32_t(e.detail * 1e6f);
        p.valid = true;
        _last_plan = p;
    } else if (e.kind == kEvtClockStep) {
        _clock.reset();
        for (auto& st : _stamp) st.ok = false;
        // A restarted slave counts from zero: any retarget aimed in the old
        // frame is a move in the new one. Drop it, never refresh it.
        _rt_valid = false;
        _rt_dirty = false;
        _rt_oneshot = false;
        // A restarted slave counts from zero: its position is no longer the
        // machine's, so homed is a lie until the ritual runs again.
        if (_homed) SLOGW("mlink", "RP time base restarted -- clock filter reset, HOMED DROPPED");
        else SLOGW("mlink", "RP time base restarted -- clock filter reset");
        _homed = false;
    } else if (e.kind == kEvtConfigTagUnknown) {
        SLOGW("mlink", "RP does not know config tag 0x%02X (firmware skew)",
              unsigned(uint32_t(e.detail)));
    }

    // Every event reaches the ring, including kEvtPlanAdopted: the
    // anomaly drain classifies by kind and the plan strip reads _last_plan.
    const uint8_t head = _evt_head.load(std::memory_order_relaxed);
    const uint8_t next = uint8_t((head + 1u) % kEventDepth);
    if (next != _evt_tail.load(std::memory_order_acquire)) {
        _evt_ring[head] = e;
        _evt_head.store(next, std::memory_order_release);
    }
}

// ---- lifecycle --------------------------------------------------------------

void MlinkServoDriver::init() {
    pinMode(kCs, OUTPUT);
    digitalWrite(kCs, HIGH);
    pinMode(kIrq, INPUT_PULLDOWN);
    s_spi.begin(kSck, kMiso, kMosi, -1);
    // The machine's steps/rev model must mirror the drive's saved gear
    // (32768/4 = 8192 counts/rev). TODO(sd-dnz): adopt from the drive.
    if (aimMotorStepsPerRev() != 8192) aimSetMotorStepsPerRev(8192, true);
    motionPassthroughEnable();
    _begun = true;
    // Seed the ceiling: an RP that has not been told a limit renders
    // unlimited, which is the state that cost 84 mm. init() is not the owner
    // task, so this posts and the first update() tick ships it -- still long
    // before the arbiter will dispatch anything (homing gates motion).
    setRenderCeiling(_max_speed_mm_s);
    SLOGI("mlink", "MlinkServoDriver up: RP2350 motion coprocessor, "
          "%.1f counts/mm, emitter cap %.0f counts/s",
          (double)AIM_STEPS_PER_MM, (double)kMaxCountsPerSec);
}

void MlinkServoDriver::update() {
    if (!_begun) return;
    // First tick claims the link. init() runs on the setup task, so the claim
    // cannot live there; every poster before this point ships on this tick.
    if (!_owner.load(std::memory_order_relaxed))
        _owner.store((void*)xTaskGetCurrentTaskHandle(),
                     std::memory_order_release);

    const uint32_t now = millis();
    const bool tick = (now - _last_tick_ms) >= kTickMs;
    if (tick) {
        _last_tick_ms = now;
        _status_fresh = false;
        pollStatus();
    }

    // Stop paths own the wire: nothing else ships while one is outstanding.
    if (_estop_pending.load(std::memory_order_acquire)) {
        if (tick) {
            if (!_status_fresh || _state != kStateEstop) sendOp(kOpEstop);
            else _estop_pending.store(false, std::memory_order_relaxed);
        }
        return;
    }
    if (_clear_pending.load(std::memory_order_acquire)) {
        if (tick) {
            if (_status_fresh && _state == kStateEstop) sendOp(kOpClear);
            else if (_status_fresh) _clear_pending.store(false, std::memory_order_relaxed);
        }
        return;
    }

    // Posted intents and config ship on ARRIVAL, not on the tick.
    drainPosts();

    // The RP holds IRQ high while an event is unpulled, so events ride the
    // owner's own pass (sub-millisecond) instead of the status cadence. Only
    // the poll below is clocked.
    const bool irq = digitalRead(kIrq) == HIGH;
    if (irq) pumpEvents();

    if (!tick) return;

    const float req = _ceiling_req.exchange(0.0f, std::memory_order_acquire);
    if (req > 0.0f) pushCeiling(req);
    if (_status_fresh && _rp_fw[0] == '\0' && !_ver_pending) requestVersion();

    if (_status_fresh) {
        // The IRQ pull above already covers the common case; this is the
        // backstop for a line that never rose (a slave that predates it, or a
        // pull that raced the last event out of the ring).
        if (!irq && _status.event_seq != _evt_acked) pumpEvents();

        // THE LOST-MIDDLE-FRAME DETECTOR. A counter cannot see a dropped
        // MIDDLE frame of a multi-frame set (the last frame still bumps it);
        // a checksum of the whole applied image can. fp 0 also means "the
        // slave has applied nothing", which is the restart detector.
        const uint16_t want = _cfg.fingerprint();
        if (_status.config_fp != want && _cfg.size() > 0) {
            if (_fp_mismatch_ms == 0) _fp_mismatch_ms = now;
            else if (now - _fp_mismatch_ms >= kConfigResyncMs) {
                _fp_mismatch_ms = now;
                if (_status.config_fp == 0 && _ceiling_mm_s > 0.0f) {
                    SLOGW("mlink", "RP restarted (config image empty); "
                          "re-pushing ceiling %.0f mm/s%s", (double)_ceiling_mm_s,
                          _homed ? " -- HOMED DROPPED" : "");
                    _homed = false;
                    _rt_valid = false;
                    _rt_dirty = false;
                    _rt_oneshot = false;
                    _rp_fw[0] = ' ';   // a restarted slave may run another image
                    _ver_tries = 0;
                    pushCeiling(_ceiling_mm_s);
                    _resync_at = 0;
                }
                // One frame of the image per resync interval, cycling: the
                // whole set is re-sent within a few intervals and the
                // fingerprint agreeing is what ends it.
                const auto fields = _cfg.fields();
                if (_resync_at >= fields.size()) _resync_at = 0;
                const size_t n = fields.size() - _resync_at < kConfigFieldsPerFrame
                                     ? fields.size() - _resync_at
                                     : kConfigFieldsPerFrame;
                uint8_t out[kFrameBytes] = {};
                encodeConfig(std::span<uint8_t, kFrameBytes>(out, kFrameBytes),
                             ++_seq, fields.subspan(_resync_at, n));
                uint8_t in[kFrameBytes] = {};
                xfer(out, in);
                _resync_at = uint8_t(_resync_at + n);
                SLOGW_EVERY_MS(5000, "mlink",
                               "config fingerprint mismatch (slave %u, image %u) "
                               "-- re-shipping %u of %u tags",
                               unsigned(_status.config_fp), unsigned(want),
                               unsigned(n), unsigned(fields.size()));
            }
        } else {
            _fp_mismatch_ms = 0;
        }
    }

    // Homing point moves only (see the header): idempotent, last wins, so a
    // missed seq echo just resends next tick.
    if (_rt_valid) {
        // seq_echo names the LAST FRAME the slave processed, any op (it is
        // also the clock tag), so "acked" is "the slave is at or past this
        // seq", never equality: the poll after a retarget already moves it
        // on. Equality made every tick a resend and paired a retarget with
        // each poll for good (2026-09-03: linkerr ~100/s, late ticks 50/s at
        // idle). A retarget torn in flight is what the refresh is for.
        if (_rt_unacked && _status_fresh &&
            int8_t(uint8_t(_status.seq_echo - _rt_seq)) >= 0)
            _rt_unacked = false;
        if (!_rt_unacked && !_rt_dirty && _rt_oneshot) {
            _rt_valid = false;
            _rt_oneshot = false;
        } else if (_rt_dirty || now - _last_cmd_ms >= kRefreshMs) {
            sendRetarget();
        }
    }
    if (tick) stallGuard(now);
}

void MlinkServoDriver::emergencyStop() {
    _rt_valid = false;
    // Flag first: a non-owner caller (OtaService::prepareForOta -> arbiter, on
    // httpTask) must leave the bus alone, and the next update() tick ships it.
    _estop_pending.store(true, std::memory_order_release);
    if (_begun && isOwner()) sendOp(kOpEstop);   // owner: immediate
}

void MlinkServoDriver::enable() {
    // Leaving an estop hold needs the explicit clear; harmless otherwise.
    if (_state == kStateEstop)
        _clear_pending.store(true, std::memory_order_release);
}

// ---- homing -----------------------------------------------------------------
// This motor free-runs at ~0.03 A (operator-measured 2026-08-08), so the AIM
// path's 3 A margin and 29.5 mm/s crawl are both far too timid here. Local to
// this backend on purpose: the AIM_* constants stay tuned for the FAS path.
namespace {
constexpr float kHomeFastMmS       = 60.0f;  // rough wall find
constexpr float kHomeSlowMmS       = 12.0f;  // accurate re-probe (rope settles)
constexpr float kHomeMarginA       = 0.4f;
constexpr float kHomePressedA      = 1.0f;   // free run is ~0.1 A; a wall is amps
constexpr float kHomeMarginMm      = 5.0f;   // usable window ends here, each wall
constexpr float kHomeReprobeBackMm = 30.0f;  // past rope slack AND fast-find overrun
}  // namespace

bool MlinkServoDriver::sendSetPos(float counts) {
    // Idempotent absolute set: repeat until the echoed position confirms.
    // Also re-seeds the RP's engine at the honest normalized position.
    for (int i = 0; i < 10; i++) {
        uint8_t out[kFrameBytes] = {kOpSetPos, ++_seq};
        memcpy(&out[2], &counts, 4);
        uint8_t in[kFrameBytes] = {};
        xfer(out, in);
        vTaskDelay(pdMS_TO_TICKS(15));
        update();
        if (fabsf(_status.pos - counts) < 4.0f) return true;
    }
    return false;
}

bool MlinkServoDriver::homingAbort(const char* what) {
    _rt_valid = false;
    _rt_dirty = false;
    _homing = false;
    SLOGW("mlink", "homing FAILED: %s", what);
    return false;
}

// One stall probe: sweep `dir` (+1 = toward the home wall) at `speed_mm_s`,
// bounded by `bound_mm` of travel. pos_out = where the carriage stopped.
// False = no stall inside the bound, or a stall riding the bound (a fault,
// not a wall -- the AIM_HOME_STALL_PLAUSIBLE_FRAC rule).
bool MlinkServoDriver::sweepToStall(float dir, float speed_mm_s, float bound_mm,
                                    float& pos_out, bool retry_once) {
    const float scale = AIM_STEPS_PER_MM;
    const float start = _status.pos;
    _rt_target = start + dir * bound_mm * scale;
    _rt_v = speed_mm_s * scale;
    _rt_a = 8.0f * _rt_v;
    _rt_valid = true;
    _rt_dirty = true;

    const uint32_t poll_ms = 1000u / AIM_HOME_POLL_HZ;
    // Spin-up, then a MOVING free-run baseline (start transients settle out).
    for (int i = 0; i < 40; i++) {
        update();
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    float base = 0.0f;
    for (int i = 0; i < AIM_HOME_BASELINE_SAMPLES; i++) {
        base += fabsf(_current.readCurrentA());
        update();
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    base /= float(AIM_HOME_BASELINE_SAMPLES);
    if (base > kHomePressedA) {
        // Already stalled: the carriage sits against a wall (a previous
        // sweep's demand overran it). A baseline taken pressed can never
        // show a rise, and sweeping on would run the demand the whole bound
        // past the wall (measured 2026-09-03: 437 mm, drive at 3.2 A). Back
        // off once and retry; still pressed means the drive is holding a
        // following error only its own reset clears.
        _rt_target = _status.pos;
        _rt_a = 40.0f * _rt_v;
        _rt_dirty = true;
        for (int i = 0; i < 60 && fabsf(_status.vel) > 50.0f; i++) {
            update();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!retry_once) {
            SLOGW("mlink", "sweep start pressed (%.2f A): backing off %.0f mm and retrying",
                  (double)base, (double)kHomeReprobeBackMm);
            glideTo(_status.pos - dir * kHomeReprobeBackMm * scale, kHomeFastMmS, 4000u);
            return sweepToStall(dir, speed_mm_s, bound_mm, pos_out, true);
        }
        SLOGW("mlink", "sweep start still pressed (%.2f A) after back-off -- "
              "drive holds a following error; power-cycle the drive", (double)base);
        pos_out = _status.pos;
        return false;
    }

    const uint32_t t0 = millis();
    const uint32_t timeout_ms =
        uint32_t(bound_mm / speed_mm_s * 1000.0f) + 4000u;
    int  consec = 0;
    bool wall   = false;
    while (millis() - t0 < timeout_ms) {
        update();
        // Homing forensics: the only per-second view of what the slave is
        // doing with the retarget (readable at /api/diag/mlink after a fail).
        SLOGI_EVERY_MS(1000, "mlink",
                       "sweep pos=%.0f vel=%.0f state=%u fp=0x%04x echo=%u/%u "
                       "I=%.2f base=%.2f tgt=%.0f",
                       (double)_status.pos, (double)_status.vel,
                       unsigned(_state), unsigned(_status.config_fp),
                       unsigned(_status.seq_echo), unsigned(_rt_seq),
                       (double)fabsf(_current.readCurrentA()), (double)base,
                       (double)_rt_target);
        if (fabsf(_current.readCurrentA()) > base + kHomeMarginA) {
            if (++consec >= AIM_HOME_STALL_CONSEC) { wall = true; break; }
        } else {
            consec = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
    }
    // THE STALL REACTION IS THE REVERSAL (operator ruling 2026-09-03): one
    // retarget straight to the back-off point at hard accel, planned from the
    // live velocity, so the demand turns around the instant the wall shows.
    // A brake-hold-then-glide left the drive pushing at 8 A for the hold and
    // tripped it. No wall found means brake where we are.
    pos_out = _status.pos;
    _rt_target = wall ? pos_out - dir * kHomeReprobeBackMm * scale : pos_out;
    // The back-off is a positioning move, not a probe: it runs at the fast
    // speed whatever the sweep ran at (a 12 mm/s slow probe backing off 30 mm
    // at 12 mm/s cost 2.5 s per wall, operator 2026-09-13).
    _rt_v = kHomeFastMmS * scale;
    _rt_a = 40.0f * _rt_v;
    _rt_dirty = true;
    if (wall)
        SLOGI("mlink", "stall at pos=%.0f (%.1f mm in, %lu ms) I=%.2f -> reversing %.0f mm",
              (double)pos_out, (double)(fabsf(pos_out - start) / scale),
              (unsigned long)(millis() - t0), (double)fabsf(_current.readCurrentA()),
              (double)kHomeReprobeBackMm);
    const uint32_t t1 = millis();
    while (millis() - t1 < 4000u) {
        update();
        if (fabsf(_status.pos - _rt_target) < 8.0f && fabsf(_status.vel) < 50.0f)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!wall) return false;
    const float swept_mm = fabsf(pos_out - start) / scale;
    return swept_mm < AIM_HOME_STALL_PLAUSIBLE_FRAC * bound_mm;
}

void MlinkServoDriver::glideTo(float counts, float speed_mm_s,
                               uint32_t max_ms) {
    _rt_target = counts;
    _rt_v = speed_mm_s * AIM_STEPS_PER_MM;
    _rt_a = 8.0f * _rt_v;
    _rt_valid = true;
    _rt_dirty = true;
    const uint32_t t0 = millis();
    while (millis() - t0 < max_ms) {
        update();
        if (fabsf(_status.pos - counts) < 8.0f && fabsf(_status.vel) < 50.0f)
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Both-ends current-stall homing (operator spec 2026-08-08): each wall gets a
// FAST rough find then a SLOW re-probe -- the rope slacks at the stops, so
// the fast contact position lies by that slack. Every stall ends backed off
// by kHomeReprobeBackMm (sweepToStall), so the next probe starts free. The
// usable window keeps kHomeMarginMm off each wall; the measured stroke rides
// the existing homed-edge NVS persist. BLOCKING on motorTask (sanctioned);
// update() inside every wait keeps the link serviced. No Modbus anywhere.
bool MlinkServoDriver::home(int32_t) {
    // _status_ms, not _status_fresh: the question is "has the link ever
    // answered", and _status_fresh reports only the LAST poll, so one bad CRC
    // frame would otherwise refuse homing.
    if (!_begun || _status_ms == 0) {
        SLOGW("mlink", "homing refused: mlink link not up");
        return false;
    }
    if (!_current.isReady()) _current.init();
    if (!_current.isReady()) {
        SLOGW("mlink", "homing refused: INA228 not answering -- no stall sense");
        return false;
    }
    // A latched slave estop holds the renderer through the whole sweep (no
    // motion, no spike, clean-looking timeout) -- clear it first, exactly as
    // forceHomeState() does for the bench path.
    if (_state == kStateEstop) {
        _clear_pending.store(true, std::memory_order_release);
        for (int i = 0; i < 50 && _state == kStateEstop; i++) {
            update();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (_state == kStateEstop) {
            SLOGW("mlink", "homing refused: slave estop will not clear");
            return false;
        }
    }
    _current.resetPeaks();
    _homing = true;
    _homed = false;

    const float scale = AIM_STEPS_PER_MM;
    const float rail  = getMaxRailMm();
    float wf = 0.0f;
    float wr = 0.0f;

    // FRONT wall first (- = away from home), REAR second: the ritual then
    // ENDS 5 mm from home, so the park is a glide instead of a full-rail
    // return trip (operator order: cut the wasted move).
    if (!sweepToStall(-1.0f, kHomeFastMmS, 1.2f * rail, wf))
        return homingAbort("front wall not found (fast sweep)");
    if (!sweepToStall(-1.0f, kHomeSlowMmS, kHomeReprobeBackMm + 10.0f, wf))
        return homingAbort("front wall not confirmed (slow probe)");

    // Rear wall (+ = toward home): rough across the rail, back off, accurate.
    if (!sweepToStall(+1.0f, kHomeFastMmS, 1.2f * rail, wr))
        return homingAbort("rear wall not found (fast sweep)");
    if (!sweepToStall(+1.0f, kHomeSlowMmS, kHomeReprobeBackMm + 10.0f, wr))
        return homingAbort("rear wall not confirmed (slow probe)");

    // Both walls measured in the same pre-zero frame; a margin comes off
    // each end of the usable window.
    const float span_mm = (wr - wf) / scale;
    const float usable  = span_mm - 2.0f * kHomeMarginMm;
    if (usable < 50.0f)
        return homingAbort("measured stroke implausibly short");

    // Zero: the carriage sits kHomeReprobeBackMm off the accurate rear wall
    // (every stall ends backed off), and the wall itself is kHomeMarginMm
    // behind home. The refresh dies first -- it would seek an old-frame
    // target after the set.
    _rt_valid = false;
    _rt_dirty = false;
    if (!sendSetPos((kHomeMarginMm - kHomeReprobeBackMm) * scale))
        return homingAbort("kOpSetPos never confirmed");
    setMeasuredStrokeMm(usable);

    // Home is a back-off plus a margin away, a positioning move at the fast
    // speed. The refresh dies with the ritual: the slave holds position on its
    // own, and a homing retarget outliving homing would fight the first real
    // command.
    glideTo(0.0f, kHomeFastMmS, 3000u);
    _rt_valid = false;
    _rt_dirty = false;
    _homing = false;
    _homed = true;
    SLOGI("mlink", "homed :3 wall-to-wall %.1f mm, usable %.1f mm "
          "(%.0f mm margin per wall, slow probe %.0f mm/s)",
          span_mm, usable, (float)kHomeMarginMm, (float)kHomeSlowMmS);
    return true;
}

// ---- stall guard (sd-4k1.29) ------------------------------------------------
// The drive follows quadrature into anything, at full current, forever. On
// 2026-09-13 a demand 1000 mm past a wall did exactly that for minutes and
// melted a print; nothing outside the homing ritual read the INA228. This
// does, at 10 Hz: bus current above kStallA for kStallMs with the RP reporting
// no motion is a standing push. The relief is to make the demand EQUAL the
// actual (kOpSetPos at the encoder's position, the drive then rests), then
// back off a little the way the push came from, and drop homed. Without an
// encoder reading there is no honest position: e-stop and drop homed.
namespace {
constexpr float    kStallA         = 2.5f;    // free run 0.05-0.10 A, a wall 3-8 A
constexpr uint32_t kStallMs        = 1500;
constexpr float    kStallStillCps  = 200.0f;  // ~1 mm/s: "the demand is not moving"
constexpr float    kStallBackoffMm = 10.0f;
}  // namespace

void MlinkServoDriver::stallGuard(uint32_t now) {
    if (++_guard_div < 10) return;   // 10 Hz on the 10 ms tick
    _guard_div = 0;
    if (_homing || !_status_fresh || _state == kStateEstop) { _stall_since_ms = 0; return; }
    if (!_current.isReady()) _current.init();
    if (!_current.isReady()) return;
    const float amps = fabsf(_current.readCurrentA());
    if (_stall_tripped) {
        if (amps < kStallA * 0.5f) _stall_tripped = false;   // re-arm once it rests
        return;
    }
    const bool still = fabsf(_status.vel) < kStallStillCps;
    if (!(amps > kStallA && still)) { _stall_since_ms = 0; return; }
    if (_stall_since_ms == 0) { _stall_since_ms = now; return; }
    if (now - _stall_since_ms < kStallMs) return;

    _stall_tripped = true;
    _stall_since_ms = 0;
    float actual_mm = 0.0f;
    const bool have_actual = _actual != nullptr && _actual->actualMm(actual_mm);
    SLOGE("mlink", "STALL: %.2f A for %u ms with the demand still (pos=%.0f) -- "
          "the drive is pushing into something; %s", (double)amps, unsigned(kStallMs),
          (double)_status.pos,
          have_actual ? "re-seeding to the encoder and backing off" : "no encoder reading, e-stop");
    _homed = false;
    if (!have_actual) {
        emergencyStop();
        return;
    }
    const float scale = AIM_STEPS_PER_MM;
    const float counts_act = -actual_mm * scale;   // the mm frame is the negated native
    const float push_dir = (_status.pos - counts_act) >= 0.0f ? 1.0f : -1.0f;
    _rt_valid = false;
    _rt_dirty = false;
    if (!sendSetPos(counts_act)) {
        SLOGE("mlink", "STALL: kOpSetPos to the encoder position never confirmed; e-stop");
        emergencyStop();
        return;
    }
    _rt_target = counts_act - push_dir * kStallBackoffMm * scale;
    _rt_v = kHomeFastMmS * scale;
    _rt_a = 40.0f * _rt_v;
    _rt_valid = true;
    _rt_dirty = true;
    _rt_oneshot = true;
    SLOGW("mlink", "STALL relief: demand re-seeded at %.1f mm (was %.1f mm past it), backing off %.0f mm",
          (double)actual_mm, (double)(fabsf(_status.pos - counts_act) / scale), (double)kStallBackoffMm);
}

// ---- stops ------------------------------------------------------------------
// Both are reachable from httpTask (WebUI -> arbiter). Neither touches the
// bus: they fill the homing retarget shadow and the owner ships it.

void MlinkServoDriver::stop() {
    // Full stop clears homed (interface contract). Land where we are.
    _homed = false;
    _rt_target = _status.pos;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _rt_dirty = true;
    _rt_oneshot = true;
}

void MlinkServoDriver::hardStop() {
    _rt_target = _status.pos;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _rt_dirty = true;
    _rt_oneshot = true;
}

// ---- readouts ---------------------------------------------------------------

float MlinkServoDriver::getPosition() const {
    // Native frame is NEGATED vs mm (endstop 0, front negative), same as
    // every driver: report nativeToMm(-native), or the arbiter plans every
    // move from a mirror-image p0 (the sd-ar3 phantom-distance bug).
    // Dead-reckon the one-tick-stale status by reported velocity: the raw
    // 100 Hz staircase beats vs the ~20 ms 0x0080 cadence (trail zigzag).
    // Capped so a dead link freezes; v=0 at rest keeps standstill raw.
    uint32_t age = millis() - _status_ms + kTickMs;
    if (age > 3 * kTickMs) age = 3 * kTickMs;
    const float ext = _status.pos + _status.vel * (float(age) * 1e-3f);
    return -ext / AIM_STEPS_PER_MM;
}

float MlinkServoDriver::getTargetPosition() const {
    // Homing owns the only target this side still holds; a streamed plan's
    // target lives on the RP and rides lastPlan() as a NORMALIZED value.
    const float t = _rt_valid ? _rt_target : _status.pos;
    return -t / AIM_STEPS_PER_MM;
}

int32_t MlinkServoDriver::mmToNative(float mm) const {
    return (int32_t)(mm * AIM_STEPS_PER_MM);
}

float MlinkServoDriver::nativeToMm(int32_t native) const {
    return (float)native / AIM_STEPS_PER_MM;
}
