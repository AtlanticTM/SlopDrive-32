// TCode v0.4 transport-agnostic parser — SlopDrive-32
//
// D1 → "TCode v0.4", D2 → dynamic axis enumeration, multi-axis parsing,
// GetAssignedAxisValues, bidirectional actual-position queries (D4).

#include "TCodeParser.h"
#include "AppLog.h"
#include "config_api.h"

volatile bool TCodeParser::intifaceCompat = false;

// ============================================================================
// Axis registry
// ============================================================================

bool TCodeParser::registerAxis(TCodeAxisState* axis) {
    if (!axis || _axis_count >= MAX_AXES) return false;
    _axes[_axis_count++] = axis;
    return true;
}

TCodeAxisState* TCodeParser::findAxis(AxisType type, uint8_t channel) const {
    for (uint8_t i = 0; i < _axis_count; i++) {
        if (_axes[i]->type() == type && _axes[i]->channel() == channel)
            return _axes[i];
    }
    return nullptr;
}

TCodeAxisState* TCodeParser::axisByIndex(uint8_t idx) const {
    if (idx >= _axis_count) return nullptr;
    return _axes[idx];
}

float TCodeParser::getAxisEffectivePosition(AxisType type, uint8_t channel) const {
    if (_onActualPos) {
        return _onActualPos(type, channel);
    }
    // Fallback to parser's stored value
    TCodeAxisState* ax = findAxis(type, channel);
    return ax ? ax->getValue() : 0.0f;
}

// ============================================================================
// Magnitude decode — shared across all axis types
// ============================================================================

float TCodeParser::_decodeMagnitude(const char* token, int& out_digits) {
    const char* p = token + 2;  // skip axis type + channel digit
    uint32_t mag_value = 0;
    int mag_digits = 0;
    while (*p && isdigit((unsigned char)*p)) {
        if (mag_digits < TCODE_MAGNITUDE_MAX_DIGITS) {
            mag_value = mag_value * 10 + (uint32_t)(*p - '0');
            mag_digits++;
        }
        p++;
    }
    out_digits = mag_digits;

    if (mag_digits == 0) return -1.0f;  // invalid

    float position;
    if (intifaceCompat) {
        position = (float)mag_value / TCODE_MAGNITUDE_MAX;
    } else {
        float scale = 1.0f;
        for (int d = 0; d < mag_digits; d++) scale *= 10.0f;
        position = (float)mag_value / scale;
    }
    position = constrain(position, 0.0f, 1.0f);
    return position;
}

// ============================================================================
// Device command handling
// ============================================================================

void TCodeParser::_handleDeviceCmd(const char* token) {
    size_t tlen = strlen(token);

    if (strncasecmp(token, "DSTOP", 5) == 0) {
        applog("[TCode] DSTOP - stop motion");
        if (_onStop) _onStop();
        return;
    }

    if (strncasecmp(token, "D0", 2) == 0) {
        if (_onResponse) _onResponse("D0 SlopDrive-32 1.0\n");
        return;
    }

    if (strncasecmp(token, "D1", 2) == 0) {
        // v0.4: report TCode v0.4
        if (_onResponse) _onResponse("D1 TCode v0.4\n");
        return;
    }

    if (strncasecmp(token, "D2", 2) == 0) {
        // v0.4: dynamic axis enumeration from registry
        _handleGetAssignedAxisValues();
        return;
    }
}

// ============================================================================
// GetAssignedAxisValues (D2 / dynamic enumeration) — v0.4
// ============================================================================
//
// Format per TCode spec: one line per axis:
//   <type><chan> <min> <max> <name>\n
// e.g.: "L0 0 9999 Stroke\n"
// The legacy hardcoded response was "D2 L0 0 9999 Up\n"

void TCodeParser::_handleGetAssignedAxisValues() {
    char buf[128];
    for (uint8_t i = 0; i < _axis_count; i++) {
        TCodeAxisState* ax = _axes[i];
        if (!ax) continue;

        // Position: actual machine state for motion axes (D4 bidirectionality),
        // last-commanded value for non-motion axes.
        float pos;
        if (ax->type() == AxisType::Linear && ax->channel() == 0) {
            // L0: use actual position callback if set, else stored value
            pos = getAxisEffectivePosition(AxisType::Linear, 0);
        } else {
            pos = ax->getValue();
        }
        uint16_t pos_int = (uint16_t)(pos * 10000.0f);
        if (pos_int > 9999) pos_int = 9999;

        snprintf(buf, sizeof(buf), "%s %u 9999 %s\n",
                 ax->idStr(), (unsigned)pos_int, ax->name());
        if (_onResponse) _onResponse(buf);
    }
}

// ============================================================================
// Per-axis token decode + dispatch
// ============================================================================
//
// Token format: <type><channel><magnitude>[I<ms>|S<rate>]
// Example: L0500  = linear ch0, magnitude 0.500
//          L0500I100 = linear ch0, 0.500 with 100ms ramp
//          R1750S200 = rotation ch1, 0.750 with speed extension

void TCodeParser::_decodeAndDispatch(const char* token, size_t tlen) {
    if (tlen < 2) return;

    char axisType = toupper(token[0]);
    uint8_t channel = (uint8_t)(token[1] - '0');
    if (channel > 9) return;

    // ---- Map to AxisType ----------------------------------------------------
    AxisType atype;
    switch (axisType) {
        case 'L': atype = AxisType::Linear;    break;
        case 'R': atype = AxisType::Rotation;  break;
        case 'V': atype = AxisType::Vibration; break;
        case 'A': atype = AxisType::Auxiliary; break;
        default:  return;  // not an axis token
    }

    // ---- Find the registered axis -------------------------------------------
    TCodeAxisState* ax = findAxis(atype, channel);
    if (!ax) {
        // Unregistered axis — silently consume (spec-tolerant). NOT routed
        // to onUnknownCmd — that's for non-TCode tokens only.
        return;
    }

    // ---- Decode magnitude ---------------------------------------------------
    int mag_digits = 0;
    float position = _decodeMagnitude(token, mag_digits);
    if (position < 0.0f) return;  // invalid magnitude

    // ---- Skip past magnitude digits to find extension(s) --------------------
    const char* p = token + 2;  // skip type + channel
    while (*p && isdigit((unsigned char)*p)) p++;  // skip magnitude digits

    // ---- Parse extensions (I/S/G, any order, multiple allowed) --------------
    // Faithful to TempestMAx's TCode reference parser (OSR2/SR6 v0.4,
    // TCode::processAxisCommand). Confirmed against MFP v0.4 wire capture
    // (2026-07-16): the interpolation stream emits  L<mag>I<ms>G<slope>  per
    // waypoint, e.g. L07999I467G-740.
    //   I<ms>    = duration of the segment ARRIVING at this waypoint (ms).
    //              Reference gate: accepted ONLY when val > 0. I0 (seen on the
    //              v3 path) therefore falls through to the bare-point/SHORT/live
    //              handler rather than becoming a zero-duration timed segment.
    //   S<rate>  = legacy v0.3 speed extension. Same val > 0 gate; converted to
    //              an equivalent duration.
    //   G<slope> = endpoint TANGENT (instantaneous velocity at this waypoint),
    //              SIGNED. Passed through RAW (the wire value, e.g. -740) exactly
    //              as the reference does — the MotionInterpolator applies the
    //              /1000 (units/second) and *durationSec (dp/dtau) scaling in
    //              setCubic(), matching Axis::setCubic. G0 is valid (slope 0 at
    //              stroke reversals); any G with digits present sets hasSlope.
    // Keeping G raw here means there is exactly ONE slope conversion end-to-end,
    // and our decode + motion math stay byte-for-byte aligned with TempestMAx.
    AxisExtentionType extType = AxisExtentionType::None;
    unsigned long extValue = 0;
    bool     hasDuration = false;
    bool     hasSlope    = false;
    uint32_t duration_ms = 0;
    float    slope_raw   = 0.0f;      // raw wire G; interpolator does the scaling

    while (*p) {
        char e = toupper((unsigned char)*p);
        if (e == 'I') {
            p++;
            long v = atol(p);
            if (v > 0) {                      // reference: I accepted only if > 0
                extType     = AxisExtentionType::Time;
                extValue    = (unsigned long)v;
                duration_ms = (uint32_t)v;
                hasDuration = true;
            }
        } else if (e == 'S') {
            p++;
            long v = atol(p);
            if (v > 0) {                      // reference: S accepted only if > 0
                extType     = AxisExtentionType::Speed;
                extValue    = (unsigned long)v;
                duration_ms = (uint32_t)(100000UL / (unsigned long)v);
                hasDuration = true;
            }
        } else if (e == 'G') {
            p++;
            long v = atol(p);                 // signed — G may be negative
            slope_raw = (float)v;             // RAW wire value, no scaling here
            hasSlope  = true;
        } else {
            p++;                              // stray char — stay robust
            continue;
        }
        // Advance past the numeric field we just consumed (sign + digits).
        if (*p == '+' || *p == '-') p++;
        while (*p && isdigit((unsigned char)*p)) p++;
    }

    // ---- Store on axis ------------------------------------------------------
    ax->setValue(position);
    ax->setExtension(extType, extValue);
    ax->stamp();

    // ---- Dispatch based on axis type ----------------------------------------
    // L0 is the primary motion axis → fires onLinearCmd (existing callback).
    // All other registered axes store state but don't fire motion callbacks
    // (they're polled by the application layer for aux outputs).
    if (atype == AxisType::Linear && channel == 0) {
        // Rate-limit logging to ~2/sec
        static uint32_t s_last_log_ms = 0;
        uint32_t now_log = millis();
        if (now_log - s_last_log_ms >= 500) {
            s_last_log_ms = now_log;
            applogf("[TCode] L0: pos=%.4f dur=%lums Graw=%.0f hasG=%d hasT=%d (digits=%d)",
                    position, (unsigned long)duration_ms, slope_raw,
                    (int)hasSlope, (int)hasDuration, mag_digits);
        }

        if (_onLinearCmd) {
            _onLinearCmd(position, duration_ms, slope_raw, hasSlope, hasDuration);
        }
    }
    // R/V/A channels: state stored, polled by application. No callback fire.
    // Application reads axis values in its update loop for servo/vibe outputs.
}

// ============================================================================
// feedLine — parse one or more whitespace-separated TCode commands
// ============================================================================

void TCodeParser::feedLine(const char* str, size_t len) {
    rxFrameCount++;

    // Null-terminated working copy
    char buf[256];
    size_t copy_len = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
    memcpy(buf, str, copy_len);
    buf[copy_len] = '\0';

    // ---- WIFI sideband intercept (must run BEFORE tokenizing) ---------------
    // `WIFI <ssid> <password>` sets the secondary NVS credentials over a text
    // transport (USB serial). SSIDs and passwords can contain spaces, so we
    // must NOT let strtok chew the line into whitespace-separated tokens — pass
    // the whole tail after the "WIFI " prefix to the app callback verbatim.
    // Strip a trailing CR/LF the transport may have left on the copy. :3\n
    if (strncasecmp(buf, "WIFI ", 5) == 0) {
        char* args = buf + 5;
        while (*args == ' ' || *args == '\t') args++;   // skip leading spaces
        // Trim trailing whitespace/newline
        size_t alen = strlen(args);
        while (alen > 0 && (args[alen - 1] == '\r' || args[alen - 1] == '\n' ||
                            args[alen - 1] == ' '  || args[alen - 1] == '\t')) {
            args[--alen] = '\0';
        }
        if (_onWifiCmd) _onWifiCmd(args);
        return;
    }

    char* token = strtok(buf, " \t\r\n");
    while (token != nullptr) {
        size_t tlen = strlen(token);
        if (tlen == 0) {
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        char axis = toupper(token[0]);

        // ---- Device commands (D...) -----------------------------------------
        if (axis == 'D') {
            _handleDeviceCmd(token);
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        // ---- Unknown / sideband commands — extensibility hook ---------------
        if (axis != 'D' && axis != 'L' && axis != 'R' && axis != 'V' && axis != 'A') {
            if (_onUnknownCmd) _onUnknownCmd(token);
            token = strtok(nullptr, " \t\r\n");
            continue;
        }

        // ---- Axis command (L/R/V/A) — decode + dispatch ---------------------
        _decodeAndDispatch(token, tlen);

        token = strtok(nullptr, " \t\r\n");
    }
}