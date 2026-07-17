#pragma once

// ============================================================================
// TCodeAxisState — persistent per-axis state for TCode v0.4
// ============================================================================
//
// Each registered axis holds:
//   - Name (e.g., "Stroke", "Twist")
//   - AxisId {type, channel} — e.g., {Linear, 0}, {Rotation, 1}
//   - Last commanded value 0..1
//   - Extension type + value (I-ms or S-speed)
//   - Ramp-in/ramp-out data (from AxisRampData)
//   - Last command timestamp (for vibe timeout)
//   - Default position (0.0f = off, 0.5f = centered)
//
// NO internal interpolator/evaluate(t) — the axis object stores INTENT.
// The MotionArbiter plans it per D4 doctrine.
//
// RampShape struct shape derived from jcfain/TCodeESP32 v0.4 AxisRampData.
// MIT attribution: https://github.com/jcfain/TCodeESP32

#include <cstdint>

// ============================================================================
// Axis type + channel pairing (mirrors TCode::Axis::AxisId)
// ============================================================================
enum class AxisType : uint8_t {
    None      = 0,
    Linear    = 1,
    Rotation  = 2,
    Vibration = 3,
    Auxiliary = 4
};

struct AxisId {
    AxisType type    = AxisType::None;
    uint8_t  channel = 0;
};

// ============================================================================
// RampData — per-command acceleration multipliers
// ============================================================================
// MIT attribution: derived from jcfain/TCodeESP32 v0.4 AxisRampData
struct AxisRampData {
    float rampStartMultiplier = 1.0f;   // 1.0 = disabled (full derived accel)
    float rampEndMultiplier   = 1.0f;
    bool  rampStartEnable     = false;
    bool  rampEndEnable       = false;
    bool  rampEndVariable     = false;

    float effectiveMultiplier() const {
        // When disabled, return 1.0 (identity). When enabled, use the configured
        // multiplier, clamped to safe range.
        if (!rampStartEnable && !rampEndEnable) return 1.0f;
        float minVal = 1.0f;
        if (rampStartEnable && rampStartMultiplier > 0.01f) {
            if (rampStartMultiplier < minVal) minVal = rampStartMultiplier;
        }
        if (rampEndEnable && rampEndMultiplier > 0.01f) {
            if (rampEndMultiplier < minVal) minVal = rampEndMultiplier;
        }
        return minVal;
    }
};

// ============================================================================
// AxisExtention — the I (interval ms) or S (speed) modifier
// ============================================================================
enum class AxisExtentionType : uint8_t {
    None     = 0,
    Time     = 1,   // I — interval in milliseconds
    Speed    = 2,   // S — speed in units per 100ms
    Gradient = 3    // G — MFP end-slope in units per 100ms (v0.4 gradient path)
};

// ============================================================================
// TCodeAxisState — one registered axis
// ============================================================================
class TCodeAxisState {
public:
    TCodeAxisState(const char* name, AxisId id, float defaultVal = 0.0f)
        : _name(name), _id(id), _value(defaultVal), _default_val(defaultVal)
    {}

    // ---- Identity -----------------------------------------------------------
    const AxisId& id()        const { return _id; }
    const char*   name()      const { return _name; }
    AxisType      type()      const { return _id.type; }
    uint8_t       channel()   const { return _id.channel; }

    // ---- Value --------------------------------------------------------------
    float   getValue()        const { return _value; }
    void    setValue(float v)       { _value = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); }

    // ---- Extension (I/S) ---------------------------------------------------
    AxisExtentionType extType()          const { return _ext_type; }
    unsigned long     extValue()         const { return _ext_value; }
    void setExtension(AxisExtentionType t, unsigned long v) {
        _ext_type = t; _ext_value = v;
    }

    // ---- Ramp data ----------------------------------------------------------
    const AxisRampData& rampData() const { return _ramp; }
    void  setRampData(const AxisRampData& r) { _ramp = r; }

    // ---- Timestamp ----------------------------------------------------------
    unsigned long lastCommandTime() const { return _last_cmd_ms; }
    void          stamp()                 { _last_cmd_ms = millis(); }

    // ---- Default (initial value after registration) -------------------------
    float defaultVal() const { return _default_val; }

    // ---- Axis ID string (e.g., "L0", "R1", "V2") ---------------------------
    char _id_buf[4] = {0};
    const char* idStr() {
        if (_id_buf[0] == 0) {
            char typeCh = '?';
            switch (_id.type) {
                case AxisType::Linear:    typeCh = 'L'; break;
                case AxisType::Rotation:  typeCh = 'R'; break;
                case AxisType::Vibration: typeCh = 'V'; break;
                case AxisType::Auxiliary: typeCh = 'A'; break;
                default:                  typeCh = '?'; break;
            }
            _id_buf[0] = typeCh;
            _id_buf[1] = (char)('0' + _id.channel);
            _id_buf[2] = '\0';
        }
        return _id_buf;
    }

private:
    const char*    _name;
    AxisId         _id;
    float          _value = 0.0f;
    float          _default_val = 0.0f;
    AxisExtentionType _ext_type = AxisExtentionType::None;
    unsigned long  _ext_value = 0;
    AxisRampData   _ramp;
    unsigned long  _last_cmd_ms = 0;
};