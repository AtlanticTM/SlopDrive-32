#pragma once

// RangeMapper -- maps a normalized stroke intensity (0..1) onto the physical
// stroke window, in mm from home.
// Constraints:
// - Window edits set the GOAL; the effective window GLIDES via tick(). A
//   snapped window is a violent one-sample reposition (sd-ey0).
// - No window ever leaves [0, max rail]. MIN_STROKE_WINDOW_MM widens a too-narrow
//   request from whichever end has room; it never pushes past the rail.
// - Each window travels as ONE value (WindowSlot), never as two floats: the goal
//   pair is published on Core 0 (settings apply) and the effective pair on
//   Core 1 (motorTask tick), and PatternEngine's own Core-1 task preempts
//   motorTask, so a field-at-a-time read can pair a min from one edit with a
//   max from another. Callers needing both halves take effectiveWindow().
// - tick() and every getter are lock-free and stay that way: tick() runs at
//   ~1 kHz on the motion core (.claude/rules/cpp-safety.md, Concurrency).
// See: .claude/rules/motion-control.md, docs/board-review-motion-pipeline.md D2

#include <atomic>
#include <cstdint>
#include <cstring>

#include "config_api.h"

// Narrowest window the machine accepts. This is a BEHAVIOR floor, not a
// numerical one: positionToIntensity() already guards its divide at range <= 0,
// but MotionArbiter::_isOutsideWindow's 0.5 mm entry epsilon becomes a large
// fraction of the span below this, so window-entry gentleness stops meaning
// anything. A narrower request is widened, never rejected.
inline constexpr float MIN_STROKE_WINDOW_MM = 5.0f;

// ---- WindowSlot -------------------------------------------------------------
// A (min,max) pair published as ONE 64-bit word so no reader can observe half
// of an edit. Not lock-free on Xtensa -- ESP-IDF implements 8-byte atomics with
// a brief interrupts-off section, tens of instructions, orders below the T27
// threshold -- and it is the whole reason the 1 kHz tick needs no mutex.
struct Window {
    float min_mm;
    float max_mm;
};
static_assert(sizeof(Window) == sizeof(uint64_t), "Window must pack into one atomic word");

class WindowSlot {
public:
    explicit WindowSlot(Window w) : _packed(pack(w)) {}

    Window load() const { return unpack(_packed.load(std::memory_order_acquire)); }
    void   store(Window w) { _packed.store(pack(w), std::memory_order_release); }

private:
    static uint64_t pack(Window w) {
        uint64_t v;
        std::memcpy(&v, &w, sizeof(v));
        return v;
    }
    static Window unpack(uint64_t v) {
        Window w;
        std::memcpy(&w, &v, sizeof(w));
        return w;
    }
    std::atomic<uint64_t> _packed;
};

class RangeMapper {
public:
    RangeMapper();

    // Runtime edits set the GOAL; the effective window GLIDES via tick().
    void setRange(float min_mm, float max_mm);
    void setRangeImmediate(float min_mm, float max_mm);   // boot/load only
    void tick(float dt_s, float rate_mm_s);   // ONE runtime caller: motorTask ~1 kHz

    // Effective (gliding) window: MOTION consumers read this. Take the PAIR
    // whenever both halves must agree with each other.
    Window effectiveWindow() const { return _range.load(); }
    // Goal window: echo/persist/broadcast read this, never the gliding pair.
    Window goalWindow() const { return _goal.load(); }

    float getMinMm() const { return _range.load().min_mm; }
    float getMaxMm() const { return _range.load().max_mm; }
    float getRangeSize() const {
        const Window w = _range.load();
        return w.max_mm - w.min_mm;
    }
    float getGoalMinMm() const { return _goal.load().min_mm; }
    float getGoalMaxMm() const { return _goal.load().max_mm; }

    // Map a normalized intensity (0.0-1.0) to a physical position in mm:
    // 0.0 -> window min, 1.0 -> window max.
    float intensityToPosition(float intensity) const;

    // Map a physical position back to an intensity value (inverse)
    float positionToIntensity(float pos_mm) const;

    // Get the center position of the configured range
    float getCenterPosition() const;

    // Set the max rail length (mm) -- the rail-length-agnostic physical ceiling
    // the window is clamped to. Pushed from ConfigStore/WebUI whenever the user
    // changes the max rail length. Re-fits the goal window immediately; the
    // effective window snaps to the new rail on the next tick (no glide ramp).
    void setMaxRailMm(float mm);
    float getMaxRailMm() const { return _max_rail_mm.load(std::memory_order_relaxed); }

    // Validate and clamp a position to physical limits (0 .. max rail length)
    float clampToPhysicalLimits(float pos_mm) const;

    // Validate and clamp an intensity value
    static float clampIntensity(float intensity);

    // Fit a requested window inside [0, rail] while honoring the minimum span.
    // Exposed for the native suite; the rail always wins over the minimum span.
    static Window fitToRail(Window w, float rail_mm);

private:
    WindowSlot         _range;        // effective (gliding); produced by tick(), Core 1
    WindowSlot         _goal;         // requested; produced on Core 0
    std::atomic<float> _max_rail_mm;  // physical ceiling (mm), user-set rail length
};
