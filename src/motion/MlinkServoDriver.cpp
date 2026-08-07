// MlinkServoDriver -- MotorDriver over the RP2350 SPI motion link.
// Constraints:
// - motorTask (Core 1) only. update() is the single send point; commands set
//   flags that the next tick ships. The 10 ms tick is the command latency.
// - Frame trust = CRC both directions; a dropped retarget heals via the
//   100 ms idempotent refresh, estop/clear by repetition until the echoed
//   state confirms.
// - >=60 us between transactions: the slave block-resets its SPI per frame.
// See: include/motion/MlinkServoDriver.h, dev board sd-dxy.

#include "motion/MlinkServoDriver.h"

#include <Arduino.h>
#include <SPI.h>
#include <cstring>

#include "system/config_api.h"
#include "system/MotionPassthrough.h"
#include "sloplog/sloplog.h"

using namespace motionlink;

// Drop-in pinout (docs/rp2350-wiring.md): the GPIO matrix remaps roles.
namespace {
constexpr int8_t kSck = 7, kMiso = 10, kMosi = 38, kCs = 48, kIrq = 4;
SPIClass s_spi(FSPI);
constexpr uint32_t kTickMs = 10;
constexpr uint32_t kRefreshMs = 100;
}  // namespace

void MlinkServoDriver::xfer(uint8_t (&out)[kFrameBytes],
                            uint8_t (&in)[kFrameBytes]) {
    static_assert(kSpiMode == 1, "PL022 slave needs CPHA=1");
    static uint32_t s_lastEndUs = 0;
    const uint32_t sinceUs = micros() - s_lastEndUs;
    if (sinceUs < 60) delayMicroseconds(60 - sinceUs);
    crcStamp(out);
    s_spi.beginTransaction(SPISettings(kSpiHz, MSBFIRST, SPI_MODE1));
    digitalWrite(kCs, LOW);
    s_spi.transferBytes(out, in, kFrameBytes);
    digitalWrite(kCs, HIGH);
    s_spi.endTransaction();
    s_lastEndUs = micros();
}

void MlinkServoDriver::sendOp(uint8_t op) {
    uint8_t out[kFrameBytes] = {op, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
}

void MlinkServoDriver::sendRetarget() {
    uint8_t out[kFrameBytes] = {kOpRetarget, ++_seq};
    memcpy(&out[2], &_rt_target, 4);
    memcpy(&out[6], &_rt_v, 4);
    memcpy(&out[10], &_rt_a, 4);
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    _rt_dirty = false;
    _last_cmd_ms = millis();
}

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
    SLOGI("mlink", "MlinkServoDriver up: RP2350 quadrature backend, "
          "%.1f counts/mm, speed cap %.0f counts/s",
          (double)AIM_STEPS_PER_MM, (double)kMaxCountsPerSec);
}

void MlinkServoDriver::update() {
    if (!_begun) return;
    const uint32_t now = millis();
    if (now - _last_tick_ms < kTickMs) return;
    _last_tick_ms = now;

    // Status poll; every reply is CRC-gated before anything trusts it.
    uint8_t out[kFrameBytes] = {kOpPing, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    const bool sane = crcOk(in);
    if (sane) {
        _state = in[0];
        _slave_flags = in[1];
        memcpy(&_pos_counts, &in[6], 4);
        memcpy(&_vel_counts, &in[10], 4);
        _status_fresh = true;
    }

    if (_estop_pending) {
        if (!sane || _state != kStateEstop) sendOp(kOpEstop);
        else _estop_pending = false;
        return;                       // nothing else while stopping
    }
    if (_clear_pending) {
        if (sane && _state == kStateEstop) sendOp(kOpClear);
        else if (sane) _clear_pending = false;
        return;
    }
    if (_rt_valid && (_rt_dirty || now - _last_cmd_ms >= kRefreshMs))
        sendRetarget();
}

void MlinkServoDriver::emergencyStop() {
    _estop_pending = true;
    _rt_valid = false;
    if (_begun) sendOp(kOpEstop);     // motorTask context: safe, immediate
}

void MlinkServoDriver::enable() {
    // Leaving an estop hold needs the explicit clear; harmless otherwise.
    if (_state == kStateEstop) _clear_pending = true;
}

bool MlinkServoDriver::home(int32_t) {
    SLOGW("mlink", "homing not implemented on the mlink backend yet -- "
          "use HOME_OVERRIDE (sd-dxy owns real homing)");
    return false;
}

void MlinkServoDriver::streamToSteps(int32_t target_steps,
                                     uint32_t speed_steps_s,
                                     uint32_t accel_steps_s2) {
    if (!_homed) return;
    float v = float(speed_steps_s);
    if (v > kMaxCountsPerSec) v = kMaxCountsPerSec;
    if (v < 1.0f) v = 1.0f;
    float a = float(accel_steps_s2);
    if (a < 1.0f) a = 1.0f;
    _rt_target = float(target_steps);
    _rt_v = v;
    _rt_a = a;
    _last_accel_native = accel_steps_s2;
    _rt_valid = true;
    _rt_dirty = true;
}

void MlinkServoDriver::stop() {
    // Full stop clears homed (interface contract). Land where we are.
    _homed = false;
    _rt_target = _pos_counts;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _rt_dirty = true;
}

void MlinkServoDriver::hardStop() {
    _rt_target = _pos_counts;
    _rt_v = kMaxCountsPerSec;
    if (_rt_a < 1.0f) _rt_a = 100000.0f;
    _rt_valid = true;
    _rt_dirty = true;
}

float MlinkServoDriver::getPosition() const {
    return _pos_counts / AIM_STEPS_PER_MM;
}

float MlinkServoDriver::getTargetPosition() const {
    return (_rt_valid ? _rt_target : _pos_counts) / AIM_STEPS_PER_MM;
}

int32_t MlinkServoDriver::mmToNative(float mm) const {
    return (int32_t)(mm * AIM_STEPS_PER_MM);
}

float MlinkServoDriver::nativeToMm(int32_t native) const {
    return (float)native / AIM_STEPS_PER_MM;
}
