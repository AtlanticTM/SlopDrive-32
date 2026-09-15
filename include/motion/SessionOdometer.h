// SessionOdometer -- distance, direction reversals and speed from a position stream
// Constraints: fed ONCE per fresh position sample by the owner that receives
// it (the link driver, on each RP status), never from a free-running loop
// reading a dead-reckoned getter: an extrapolated position zigzags at every
// status arrival and every zigzag reads as travel and a reversal. Speed and
// reversals evaluate once per kSpeedWindowUs. Hardware-free; the glue
// mirrors the totals into SystemState.
#pragma once

#include <cmath>
#include <cstdint>

class SessionOdometer {
public:
    void feed(float pos_mm, uint32_t now_us) {
        if (!_primed) {
            _primed = true;
            _last_pos = _spd_pos = pos_mm;
            _spd_us = now_us;
            return;
        }
        const float d = fabsf(pos_mm - _last_pos);
        if (d > kMinStepMm) _distance_mm += d;
        _last_pos = pos_mm;

        const uint32_t dt_us = now_us - _spd_us;
        if (dt_us < kSpeedWindowUs) return;
        if (dt_us < 1000000u) {   // a stall or wrap is not a speed sample
            const float dp   = pos_mm - _spd_pos;
            const float inst = fabsf(dp) / (float(dt_us) * 1e-6f);
            _live += kEmaGain * (inst - _live);
            if (_live > _peak) _peak = _live;
            // A stroke is a reversal with real travel behind it.
            const int8_t dir = (dp > kStrokeMm) ? int8_t(1) : (dp < -kStrokeMm) ? int8_t(-1) : _dir;
            if (dir != 0 && _dir != 0 && dir != _dir) ++_strokes;
            _dir = dir;
        }
        _spd_pos = pos_mm;
        _spd_us  = now_us;
    }

    void reset() {
        _distance_mm = 0.0f;
        _strokes = 0;
        _live = _peak = 0.0f;
        _dir = 0;
    }

    float    distanceMm() const { return _distance_mm; }
    uint32_t strokes()    const { return _strokes; }
    float    liveMmS()    const { return _live; }
    float    peakMmS()    const { return _peak; }

private:
    static constexpr float    kMinStepMm     = 0.002f;   // under: quantization
    static constexpr float    kStrokeMm      = 0.05f;    // travel that counts as a direction
    static constexpr float    kEmaGain       = 0.25f;    // ~70 ms at the 20 ms window
    static constexpr uint32_t kSpeedWindowUs = 20000;    // two link status samples, RP clock
    bool     _primed = false;
    float    _last_pos = 0.0f;
    float    _spd_pos = 0.0f;
    uint32_t _spd_us = 0;
    float    _distance_mm = 0.0f;
    uint32_t _strokes = 0;
    float    _live = 0.0f;
    float    _peak = 0.0f;
    int8_t   _dir = 0;
};
