#pragma once

// SessionLog — bounded in-memory log ring feeding the TUI's log pane.
// Constraints:
//   - The hub thread is the main writer; IXWebSocket connection threads may
//     log too — hence the one small mutex (mirrors sim/slopsim's own
//     one-lock-per-shared-structure doctrine).
// See: sim/slopsim/src/common/SessionLog.h (identical shape, copied rather
// than shared because slopsim/slopbench are separate CMake targets with no
// common host-only library between them yet).

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace slopbench {

class SessionLog {
public:
    struct Line {
        char level = 'I';  // T/D/I/W/E
        std::string text;
    };

    // Headless/scripted runs stream every line to stdout as it happens; the
    // TUI reads the ring instead.
    void setEcho(bool on) { _echo = on; }

    void logf(char level, const char* fmt, ...) {
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lk(_m);
        if (_echo) {
            std::printf("[%c] %s\n", level, buf);
            std::fflush(stdout);
        }
        _lines.push_back({level, std::string(buf)});
        if (_lines.size() > kMaxLines) _lines.pop_front();
        ++_revision;
    }

    // Monotonic change counter — lets the TUI redraw only when the log moved.
    size_t revision() const {
        std::lock_guard<std::mutex> lk(_m);
        return _revision;
    }

    std::vector<Line> tail(size_t n) const {
        std::lock_guard<std::mutex> lk(_m);
        size_t start = _lines.size() > n ? _lines.size() - n : 0;
        return {_lines.begin() + long(start), _lines.end()};
    }

private:
    static constexpr size_t kMaxLines = 500;
    mutable std::mutex _m;
    std::deque<Line> _lines;
    bool _echo = false;
    size_t _revision = 0;
};

}  // namespace slopbench
