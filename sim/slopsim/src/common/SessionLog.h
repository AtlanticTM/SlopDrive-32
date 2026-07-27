#pragma once

// Bounded in-memory log ring feeding the TUI's log pane. The sim thread is the
// main writer, but IXWebSocket connection threads may log too — hence the one
// small mutex (this and the WS port's slot mutexes are the only locks in
// slopsim, mirroring the firmware's one-task doctrine everywhere else).

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace slopsim {

class SessionLog {
public:
    struct Line {
        char level = 'I';  // T/D/I/W/E
        std::string text;
    };

    // Headless runs stream every line to stdout as it happens (a force-killed
    // process loses nothing); the TUI reads the ring instead.
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

}  // namespace slopsim
