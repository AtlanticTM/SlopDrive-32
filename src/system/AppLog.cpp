// AppLog — the SlopLog sink bridge for the S3 main controller. The web
// line-ring (/api/log) is a SlopLog sink; the four bridge functions register
// the sinks and pump/gate them. All logging now flows through SLOGx directly
// — see the header for the migration story.

#include "AppLog.h"

#include "freertos/FreeRTOS.h"

namespace {

// The /api/log web ring: formatted lines, oldest-first dump. Written only
// from the drain caller (httpTask), but dumped from HTTP handlers which can
// interleave on other priorities — a short spinlock keeps the copy honest.
class WebRingSink final : public sloplog::ISink {
public:
    void write(const sloplog::Record& r) override {
        char line[140];
        int n = snprintf(line, sizeof(line), "[%5lu.%03lu %c %s] %s",
                         (unsigned long)(r.ms / 1000u), (unsigned long)(r.ms % 1000u),
                         sloplog::levelChar(r.level), r.tag, r.msg);
        if (n < 0) return;
        if (n >= int(sizeof(line))) n = int(sizeof(line)) - 1;
        // Legacy call sites often carry their own trailing newline — the
        // ring is line-oriented, so strip it.
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (r.lost && size_t(n) < sizeof(line)) {
            snprintf(line + n, sizeof(line) - size_t(n), " (+%u lost)", r.lost);
        }
        portENTER_CRITICAL(&_mux);
        strncpy(_lines[_head], line, kLineBytes - 1);
        _lines[_head][kLineBytes - 1] = '\0';
        _head = (_head + 1) % kLines;
        if (_count < kLines) ++_count;
        portEXIT_CRITICAL(&_mux);
    }

    void dump(String& out) {
        // Copy under the lock, then build the String outside it (String
        // append can reallocate — never allocate in a critical section).
        static char snapshot[kLines][kLineBytes];  // static: too big for stack
        size_t count, head;
        portENTER_CRITICAL(&_mux);
        count = _count;
        head = _head;
        memcpy(snapshot, _lines, sizeof(_lines));
        portEXIT_CRITICAL(&_mux);
        size_t start = (head + kLines - count) % kLines;
        for (size_t i = 0; i < count; ++i) {
            out += snapshot[(start + i) % kLines];
            out += '\n';
        }
    }

private:
    static constexpr size_t kLines = 60;
    static constexpr size_t kLineBytes = 140;
    char _lines[kLines][kLineBytes] = {};
    size_t _head = 0;
    size_t _count = 0;
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

WebRingSink& webRing() {
    static WebRingSink s;
    return s;
}

}  // namespace

// Serial-sink gating is RUNTIME, not compile-time (the SERIAL_CONTROL_MODE
// #if that used to exclude the sink silenced ALL serial logging forever —
// a field incident). Two independent inputs pick the sink's floor:
//   - dedicated: serial TCode traffic is actively flowing (Intiface owns the
//     port) -> mute completely. Self-healing both directions, polled from
//     httpTask via applogSerialDedicated(serialTransport.isActive()).
//   - handshook: the WebUI served /api/log at least once -> Warn+ only.
static bool s_serialDedicated = false;
static bool s_serialHandshook = false;

static void applySerialFloor() {
    sloplog::Level floor = sloplog::Level::Trace;
    if (s_serialDedicated) floor = sloplog::Level::Off;
    else if (s_serialHandshook) floor = sloplog::Level::Warn;
    sloplog::logger().setSinkFloor(&sloplog::serialSink(), floor);
}

void applogBegin() {
    sloplog::logger().addSink(&webRing());
    // USB serial mirrors everything until told otherwise (see gating above).
    // Zero TX timeout = the CDC driver DROPS when full/unlistened instead of
    // blocking (~100 ms/line) — what makes unconditional writes safe on the
    // drain task.
    Serial.setTxTimeoutMs(0);
    sloplog::logger().addSink(&sloplog::serialSink());
    // Boot mode: drain synchronously after every line so the whole boot
    // narrates to serial in real time (single-task phase only — main.cpp
    // flips this off right before the FreeRTOS tasks spawn).
    sloplog::logger().setImmediateDrain(true);
}

void applogDrain() { sloplog::drainToSinks(); }

void applogSerialQuiet() {
    s_serialHandshook = true;
    applySerialFloor();
}

void applogSerialDedicated(bool dedicated) {
    if (dedicated == s_serialDedicated) return;
    s_serialDedicated = dedicated;
    applySerialFloor();
}

void applogDump(String& out) { webRing().dump(out); }

