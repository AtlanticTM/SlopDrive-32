// TCode v0.3 transport-agnostic parser — SlopDrive-32
//
// Pure TCode v0.3 line parser with zero hardware/transport dependencies.
// Callbacks are registered by the application; transport-specific response
// delivery is injected via setResponseCallback() so the parser never touches
// Serial, WebSocket, or BLE directly.
//
// Usage:
//   1. Create a TCodeParser.
//   2. Register linearCmd / stop / response callbacks.
//   3. Feed complete newline-terminated TCode lines via feedLine().
//   4. Read rxFrameCount for inbound rate diagnostics.
//
// TCode v0.3 linear format (see github.com/multiaxis/TCode-Specification):
//   L<ch><mag>          Linear, channel, magnitude 0.<mag> (variable digits!)
//   L<ch><mag>I<ms>     ...ramp to 0.<mag> over <ms> milliseconds
//   L<ch><mag>S<rate>   ...ramp to 0.<mag> at <rate> units per 100ms
//   DSTOP               Stop all axes
//   D0 / D1 / D2        Device info queries (replied via response callback)
//
// Multiple commands may be space-separated on one line.

#ifndef TCODE_PARSER_H
#define TCODE_PARSER_H

#include <Arduino.h>

// ---- Callback type definitions ----

/// Called when a valid L0 (linear axis 0) command is received.
/// @param position     0.0 – 1.0 (normalised — the TCode magnitude decoded as an
///                     implicit decimal fraction, i.e. mag / 10^digits)

/// @param duration_ms  time to reach position in ms (0 = use configured speed)
typedef void (*LinearCmdCallback)(float position, uint32_t duration_ms);

/// Called when a DSTOP command is received.
typedef void (*StopCallback)();

/// Called when the parser needs to send a TCode response (D0/D1/D2 replies).
/// The transport that is currently active should register itself here.
/// @param msg  null-terminated response string (includes trailing \n)
typedef void (*ResponseCallback)(const char* msg);

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

    // ---- Feed a complete TCode line ------------------------------------------
    /// Parse one or more whitespace-separated TCode commands.
    /// The line does NOT need to be null-terminated; a working copy is made.
    /// Increments rxFrameCount exactly once per call (the frame is the line).
    void feedLine(const char* str, size_t len);

    // ---- Programmatic stop (transport-initiated) -----------------------------
    /// Fire the registered stop callback directly — used by transports that
    /// detect a host disconnect and need to issue a synthetic DSTOP without
    /// going through feedLine(). Safe to call from any context. :3
    void triggerStop() { if (_onStop) _onStop(); }

    // ---- Raw frame counter (pre-parse, for rate diagnostics) -----------------
    volatile uint32_t rxFrameCount = 0;

    // ---- Intiface compatibility flag (set by the WebUI handler) ---------------
    // Static so the WebUI can flip it without threading a SystemState ref into
    // this transport-agnostic parser. When true, feedLine() decodes the L0
    // magnitude against the fixed TCODE_MAGNITUDE_MAX (/999) scale that
    // Intiface's buttplug bridge expects, instead of the spec-correct
    // mag/10^digits decode that MultiFunPlayer needs. One bool, two apps, no
    // more fighting over the same hole. :3
    static volatile bool intifaceCompat;

private:
    LinearCmdCallback _onLinearCmd = nullptr;
    StopCallback      _onStop      = nullptr;
    ResponseCallback  _onResponse  = nullptr;
};

#endif // TCODE_PARSER_H