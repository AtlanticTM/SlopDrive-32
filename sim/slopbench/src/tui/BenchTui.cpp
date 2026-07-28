// BenchTui — ANSI console redraw implementation (see BenchTui.h for the
// keymap/threading contract).

#include "tui/BenchTui.h"

#include <cstdio>
#include <sstream>

#include "common/SessionLog.h"
#include "hub/BenchHub.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <conio.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace slopbench {

void BenchTui::enableAnsi() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return;
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    // POSIX terminals already speak ANSI; nothing to enable.
}

bool BenchTui::pollQuit() {
#ifdef _WIN32
    while (_kbhit()) {
        int c = _getch();
        if (c == 'q' || c == 'Q') return true;
    }
    return false;
#else
    // Non-blocking raw-mode read: one-time termios setup (kept in a static
    // guard so the terminal is restored via atexit, not scattered across
    // every call site), then a zero-timeout read of whatever is waiting.
    static bool initialized = [] {
        termios tio{};
        tcgetattr(STDIN_FILENO, &tio);
        termios raw = tio;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        static termios original = tio;
        std::atexit([] { tcsetattr(STDIN_FILENO, TCSANOW, &original); });
        return true;
    }();
    (void)initialized;
    char c = 0;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 'q' || c == 'Q') return true;
    }
    return false;
#endif
}

void BenchTui::draw(const BenchHub& hub, const SessionLog& log, uint32_t nowMs) {
    std::ostringstream o;
    o << "\x1b[2J\x1b[H";  // clear + home

    const HubConfig& cfg = hub.config();
    o << "SlopBench -- " << cfg.name << "  (" << cfg.product << " " << cfg.fwVersion << ")\n";
    o << "catalog: " << hub.catalogEncodedBytes() << " B encoded"
      << "   sessions: " << hub.sessionCount() << "   uptime: " << (nowMs / 1000) << "s"
      << "   [q] quit\n";
    o << std::string(78, '-') << "\n";

    o << "CHANNELS\n";
    for (const auto& ch : hub.snapshotStates()) {
        char idbuf[16];
        std::snprintf(idbuf, sizeof(idbuf), "0x%04X", unsigned(ch.id));
        o << "  " << idbuf << "  " << ch.name << "\n";
        for (const auto& f : ch.fields) {
            o << "      " << f.name << " = " << f.text << "\n";
        }
    }

    o << std::string(78, '-') << "\n";
    o << "SESSIONS\n";
    auto sessions = hub.sessionRows();
    if (sessions.empty()) {
        o << "  (none connected)\n";
    } else {
        for (const auto& s : sessions) {
            o << "  slot " << unsigned(s.slot) << ": " << s.peer << "\n";
        }
    }

    o << std::string(78, '-') << "\n";
    o << "RECENT WRITES\n";
    auto writes = hub.recentWrites(12);
    if (writes.empty()) {
        o << "  (none yet)\n";
    } else {
        for (const auto& w : writes) {
            o << "  [" << (w.atMs / 1000) << "s] " << w.channelName << ": " << w.summary << "\n";
        }
    }

    o << std::string(78, '-') << "\n";
    o << "LOG\n";
    for (const auto& line : log.tail(8)) {
        o << "  [" << line.level << "] " << line.text << "\n";
    }

    std::fputs(o.str().c_str(), stdout);
    std::fflush(stdout);
}

}  // namespace slopbench
