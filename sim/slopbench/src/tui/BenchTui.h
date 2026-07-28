#pragma once

// BenchTui — the plain-ANSI console front end (task item 4: "simple tui").
// Constraints:
//   - No curses dependency: a redraw is one big string of ANSI escapes
//     ("\x1b[2J\x1b[H" + plain text), which Windows Terminal and any real
//     terminal render fine. Never add a TUI library here — if ANSI redraw
//     stops being enough, that is a Canon Flag conversation, not a
//     silent dependency add.
//   - Keyboard: `q` quits. That is the whole keymap, on purpose.
//   - Single-threaded: pollQuit()/draw() run on the same thread as
//     BenchHub::tick() (see main.cpp's loop) — no locking here.
// See: sim/slopbench/README.md ("TUI keys").

#include <cstdint>

namespace slopbench {

class BenchHub;
class SessionLog;

class BenchTui {
public:
    // Best-effort: enables ANSI escape processing on a legacy Windows
    // console. A no-op (and harmless) everywhere ANSI already works.
    static void enableAnsi();

    // Non-blocking: true the instant `q`/`Q` has been typed. Never blocks —
    // a hub with nobody at the keyboard (a CI run) must still tick.
    static bool pollQuit();

    // One full-screen redraw. `hub` and `log` must outlive the call (they
    // do: main.cpp keeps both on the stack for the process lifetime).
    static void draw(const BenchHub& hub, const SessionLog& log, uint32_t nowMs);
};

}  // namespace slopbench
