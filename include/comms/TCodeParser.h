// TCode v0.4 transport-agnostic parser — SlopDrive-32
//
// Pure TCode v0.4 line parser with zero hardware/transport dependencies.
// Callbacks are registered by the application; transport-specific response
// delivery is injected via setResponseCallback() so the parser never touches
// Serial, WebSocket, or BLE directly.
//
// Usage:
//   1. Create a TCodeParser.
//   2. Register axes via registerAxis() — L0 ("Stroke") ALWAYS registered.
//   3. Register callbacks: onLinearCmd (L0 intents), onStop, onResponse.
//   4. Feed complete newline-terminated TCode lines via feedLine().
//   5. Read rxFrameCount for inbound rate diagnostics.
//
// TCode v0.4 format (wire-compatible with v0.3; see multiaxis/TCode-Specification):
//   L<ch><mag>          Linear, channel, magnitude 0.<mag> (variable digits!)
//   L<ch><mag>I<ms>     ...ramp to 0.<mag> over <ms> milliseconds
//   L<ch><mag>S<rate>   ...ramp to 0.<mag> at <rate> units per 100ms
//   R<ch><mag>...       Rotation axis (registered channels only)
//   V<ch><mag>...       Vibration axis (registered channels only)
//   A<ch><mag>...       Auxiliary axis (registered channels only)
//   DSTOP               Stop all axes
//   D0 / D1 / D2        Device info queries (replied via response callback)
//   D2 now reports DYNAMIC axis enumeration from the registry
//
// Multiple commands may be space-separated on one line.

#ifndef TCODE_PARSER_H
#define TCODE_PARSER_H

#include <Arduino.h>
#include "TCodeAxisState.h"

// ---- Callback type definitions ----

/// Called when a valid L0 (linear axis 0) command is received.
/// @param position     0.0 – 1.0 (normalised — the TCode magnitude decoded as an
///                     implicit decimal fraction, i.e. mag / 10^digits)
/// @param duration_ms  time to reach position in ms (0 = use configured speed /
///                     live cadence). Set from the I<ms> extension, or derived
///                     from S<rate>. hasDuration distinguishes "explicit I/S
///                     present" (timed segment) from "bare point" (v3 live).
/// @param slope        MFP end-slope from the v0.4 G<slope> extension, passed
///                     through as the RAW wire value (e.g. -740), exactly as
///                     TempestMAx's reference parser does. The MotionInterpolator
///                     applies the /1000 (→units/second) and *durationSec (→dp/dtau)
///                     scaling in setCubic(), so there is exactly ONE slope
///                     conversion end-to-end. Signed: negative = position
///                     decreasing. Only meaningful when hasSlope is true. Drives
///                     the interpolator's C1-continuous gradient (Hermite)
///                     endpoint tangent.
/// @param hasSlope     true when a G<slope> extension was present (v0.4 gradient).
/// @param hasDuration  true when an explicit I<ms> or S<rate> extension was
///                     present (a timed segment, not a bare live point).
typedef void (*LinearCmdCallback)(float position, uint32_t duration_ms,
                                  float slope, bool hasSlope, bool hasDuration);

/// Called when a DSTOP command is received.
typedef void (*StopCallback)();

/// Called when the parser needs to send a TCode response (D0/D1/D2 replies).
/// The transport that is currently active should register itself here.
/// @param msg  null-terminated response string (includes trailing \n)
typedef void (*ResponseCallback)(const char* msg);

/// Called when feedLine() encounters a token that is NOT a recognised TCode
/// command (not L/R/V/D-prefixed). This is the extensibility hook — any
/// custom sideband commands transmitted alongside TCode (e.g. "SPEED:50",
/// "PRESET:edge", future protocol extensions) land here instead of being
/// silently dropped. The token is null-terminated. :3
typedef void (*UnknownCmdCallback)(const char* token);

// ============================================================================
// TCodeParser — pure, transport-agnostic TCode v0.3 line parser
// ============================================================================

class TCodeParser {
public:
    TCodeParser() = default;

    // ---- Configure callbacks -------------------------------------------------
    void onLinearCmd(LinearCmdCallback cb) { _onLinearCmd = cb; }
    void onLinearRampTo(LinearCmdCallback cb) { _onLinearCmd = cb; }  // alias

    void onStop(StopCallback cb) { _onStop = cb; }
    void onLinearStop(StopCallback cb) { _onStop = cb; }  // alias

    /// Set the response hook.  The active transport calls this so D0/D1/D2
    /// replies go to the right place.  May be nullptr (responses are silently
    /// dropped).
    void onResponse(ResponseCallback cb) { _onResponse = cb; }

/// Set the unknown-command hook. Any token that doesn't match a recognised
/// TCode prefix (L/R/V/A/D) is passed here verbatim. May be nullptr (unknown
/// tokens are silently dropped). Unregistered R/V/A channels are silently
/// consumed (spec-tolerant), NOT routed to this hook.
    void onUnknownCmd(UnknownCmdCallback cb) { _onUnknownCmd = cb; }

    // ---- Axis registry (v0.4 multi-axis model) --------------------------------
    /// Register an axis. The parser accepts commands for registered axes and
    /// silently ignores unregistered ones (spec-tolerant). L0 is always
    /// registered by main.cpp. Returns true if registered, false if slot full.
    bool registerAxis(TCodeAxisState* axis);

    /// Look up an axis by type+channel. Returns nullptr if not registered.
    TCodeAxisState* findAxis(AxisType type, uint8_t channel) const;

    /// Number of registered axes — drives dynamic D2 enumeration.
    uint8_t axisCount() const { return _axis_count; }

    /// Get a registered axis by index (0..axisCount()-1).
    TCodeAxisState* axisByIndex(uint8_t idx) const;

    // ---- Bidirectional queries (v0.4 D4: report actual machine state) -------
    // Called from the motion layer when the host polls position. The parser
    // stores the last known position value for each axis; for motion-mapped
    // axes (L0 mapped to mm), the APPLICATION hooks an external position
    // source (motor.getPosition() mapped to 0..1 range). The v0.3 parser only
    // kept last-commanded value; v0.4 adds this hook for actual machine state.
    //
    // D4: "Position/state queries must report ACTUAL machine state, not the
    // parser's last-commanded value." Set this callback to provide actual
    // position for motion axes. If not set, falls back to _last_value.
    typedef float (*ActualPositionCallback)(AxisType type, uint8_t channel);
    void setActualPositionCallback(ActualPositionCallback cb) {
        _onActualPos = cb;
    }

    // Get the effective position for an axis (actual if callback set, else last)
    float getAxisEffectivePosition(AxisType type, uint8_t channel) const;

// ---- Feed a complete TCode line ------------------------------------------
    void feedLine(const char* str, size_t len);

    // ---- Programmatic stop (transport-initiated) -----------------------------
    void triggerStop() { if (_onStop) _onStop(); }

    // ---- Raw frame counter (pre-parse, for rate diagnostics) -----------------
    volatile uint32_t rxFrameCount = 0;

    // ---- Intiface compatibility flag -----------------------------------------
    static volatile bool intifaceCompat;

private:
    LinearCmdCallback  _onLinearCmd    = nullptr;
    StopCallback       _onStop         = nullptr;
    ResponseCallback   _onResponse     = nullptr;
    UnknownCmdCallback _onUnknownCmd   = nullptr;
    ActualPositionCallback _onActualPos = nullptr;

    // ---- Axis registry (up to 12 slots — L0–L9 + R/V/A overflow) ------------
    static constexpr uint8_t MAX_AXES = 12;
    TCodeAxisState* _axes[MAX_AXES] = {};
    uint8_t         _axis_count = 0;

    // ---- Internal helpers ----------------------------------------------------
    void _decodeAndDispatch(const char* token, size_t tlen);
    float _decodeMagnitude(const char* token, int& out_digits);
    void _handleDeviceCmd(const char* token);
    void _handleGetAssignedAxisValues();  // new v0.4 device command
};

#endif // TCODE_PARSER_H