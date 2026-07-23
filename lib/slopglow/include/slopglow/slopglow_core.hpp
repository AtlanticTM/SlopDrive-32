// SlopGlow — hardware-free status-LED core.
//
// Philosophy (the three rules everything here serves):
//   1. CALLERS SPEAK SEMANTICS. Modules raise/clear conditions
//      (glow.raise(GlowState::Fault)) and never pick colors or patterns —
//      the engine maps the highest-priority active state onto whatever
//      hardware the board actually has (8-pixel ring, one dumb PWM LED,
//      RGB nano LED). That mapping is the module's job, which is what makes
//      callers hardware-interchangeable.
//   2. THE LED IS A LIVENESS ORGAN. The engine has no task and no timer:
//      animation phase only advances inside update(), and only while every
//      registered heartbeat source (one per monitored core/task) has pulsed
//      recently. A frozen core freezes the animation mid-frame — a static
//      LED IS the failure indication. (NeoPixels latch their last color with
//      zero CPU, so this works all the way down.)
//   3. NOTHING HERE TOUCHES HARDWARE. Output is an injected IGlowOutput;
//      time is caller-supplied ms. The core runs identically on the S3, the
//      C5 nodes, and in the native doctest suite.
#pragma once

#include <cstddef>
#include <cstdint>

namespace slopglow {

// ---- Color ------------------------------------------------------------------

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;

    static constexpr Rgb black() { return {0, 0, 0}; }
    constexpr bool operator==(const Rgb&) const = default;

    // Integer lerp, t in [0,255]. Exact at both endpoints.
    static Rgb lerp(Rgb a, Rgb b8, uint8_t t) {
        auto mix = [t](uint8_t x, uint8_t y) {
            return uint8_t(x + ((int(y) - int(x)) * t) / 255);
        };
        return {mix(a.r, b8.r), mix(a.g, b8.g), mix(a.b, b8.b)};
    }

    // Perceptual luma (for mono outputs), 0..255.
    uint8_t luma() const {
        return uint8_t((uint16_t(r) * 54 + uint16_t(g) * 183 + uint16_t(b) * 19) >> 8);
    }
};

// HSV→RGB for the rainbow/cycle mode. h in [0,255] wraps; s,v in [0,255].
inline Rgb hsv(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return {v, v, v};
    uint8_t region = h / 43;
    uint8_t rem = uint8_t((h - region * 43) * 6);
    uint8_t p = uint8_t((uint16_t(v) * (255 - s)) >> 8);
    uint8_t q = uint8_t((uint16_t(v) * (255 - ((uint16_t(s) * rem) >> 8))) >> 8);
    uint8_t t = uint8_t((uint16_t(v) * (255 - ((uint16_t(s) * (255 - rem)) >> 8))) >> 8);
    switch (region) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

// ---- Output driver contract -------------------------------------------------

// A strip of N logical pixels. A dumb mono LED is a 1-pixel output whose
// driver maps Rgb -> luma() -> PWM duty; an RGB LED is 1 pixel of true
// color; a NeoPixel ring is N. show() latches the frame to hardware.
class IGlowOutput {
public:
    virtual ~IGlowOutput() = default;
    virtual size_t pixelCount() const = 0;
    virtual void set(size_t i, Rgb c) = 0;
    virtual void show() = 0;
};

// ---- Semantic vocabulary ----------------------------------------------------

// Ordered by ascending priority: the highest ACTIVE state owns the LEDs.
// (Estop outranks everything, always. Boot is the implicit floor state.)
enum class GlowState : uint8_t {
    Boot = 0,      // power-up until the system says otherwise
    LinkDown,      // no network/transport
    Ready,         // homed, idle, all good
    Active,        // motion in progress (pattern/stream/manual)
    Paused,
    Calibrating,   // homing / self-measurement — exclusive, hands off
    Pairing,       // pairing window open — visible invitation
    Warning,       // degraded but running
    Ota,           // flashing — do not power off
    Fault,         // needs attention, motion refused
    Estop,         // latched emergency stop
    kCount_,
};
inline constexpr size_t kGlowStateCount = size_t(GlowState::kCount_);

enum class GlowMode : uint8_t {
    Solid = 0,
    Breathe,    // colorA <-> colorB sine-ish lerp over period
    Blink,      // colorA / colorB hard 50% duty over period
    Chase,      // one colorA pixel orbits a colorB background (multi-pixel;
                // degrades to Breathe on a single pixel)
    Rainbow,    // hue cycle over period (colorA ignored)
};

// How a state looks. Boards/apps may override any entry; the defaults are
// chosen to read unambiguously on a single RGB LED.
struct GlowSpec {
    Rgb colorA{};
    Rgb colorB{};
    GlowMode mode = GlowMode::Solid;
    uint16_t period_ms = 1000;
};

inline GlowSpec defaultSpec(GlowState s) {
    switch (s) {
        case GlowState::Boot:     return {{255, 120, 0}, {10, 4, 0}, GlowMode::Breathe, 900};
        case GlowState::LinkDown: return {{0, 60, 255}, {0, 2, 12}, GlowMode::Breathe, 2200};
        case GlowState::Ready:    return {{0, 255, 60}, {0, 10, 2}, GlowMode::Breathe, 3000};
        case GlowState::Active:   return {{0, 200, 255}, {0, 8, 12}, GlowMode::Breathe, 1400};
        case GlowState::Paused:   return {{255, 200, 0}, {12, 9, 0}, GlowMode::Breathe, 2600};
        case GlowState::Calibrating: return {{0, 80, 255}, {0, 0, 10}, GlowMode::Blink, 500};
        case GlowState::Pairing:  return {{180, 0, 255}, {4, 0, 8}, GlowMode::Blink, 700};
        case GlowState::Warning:  return {{255, 140, 0}, {12, 6, 0}, GlowMode::Blink, 1000};
        case GlowState::Ota:      return {{255, 255, 255}, {8, 8, 8}, GlowMode::Blink, 300};
        case GlowState::Fault:    return {{255, 0, 0}, {12, 0, 0}, GlowMode::Breathe, 1200};
        case GlowState::Estop:    return {{255, 0, 0}, {40, 0, 0}, GlowMode::Blink, 400};
        default:                  return {};
    }
}

// ---- Heartbeat gate ---------------------------------------------------------

// One counter per monitored loop. The owner of that loop bumps pulse() every
// iteration (an atomic-free relaxed increment is fine: any observed change
// proves liveness). The engine freezes animation when ANY registered source
// hasn't changed within its staleness window.
struct HeartbeatSource {
    volatile uint32_t counter = 0;
    void pulse() { counter = counter + 1; }
};

// ---- The engine -------------------------------------------------------------

inline constexpr size_t kMaxPixels = 16;
inline constexpr size_t kMaxHeartbeats = 4;
inline constexpr uint16_t kCrossfadeMs = 350;

class GlowEngine {
public:
    explicit GlowEngine(IGlowOutput& out) : _out(out) {
        for (size_t i = 0; i < kGlowStateCount; ++i) _specs[i] = defaultSpec(GlowState(i));
        _active[size_t(GlowState::Boot)] = true;  // implicit floor
    }

    // ---- Semantic surface (callable from any task: single writer per state
    // is the intended pattern; a bool store is atomic on every target).
    void raise(GlowState s) { _active[size_t(s)] = true; }
    void clear(GlowState s) {
        if (s != GlowState::Boot) _active[size_t(s)] = false;  // floor never clears
    }
    void set(GlowState s, bool on) { on ? raise(s) : clear(s); }
    bool isRaised(GlowState s) const { return _active[size_t(s)]; }

    // Highest-priority active state — what the LEDs are showing.
    GlowState current() const {
        for (size_t i = kGlowStateCount; i-- > 0;)
            if (_active[i]) return GlowState(i);
        return GlowState::Boot;
    }

    // Override how a state renders on this board.
    void setSpec(GlowState s, const GlowSpec& spec) { _specs[size_t(s)] = spec; }
    const GlowSpec& spec(GlowState s) const { return _specs[size_t(s)]; }

    // Global brightness ceiling, 0..255 (applied after everything else).
    void setBrightness(uint8_t b) { _brightness = b; }

    // ---- Heartbeat registration (call once per monitored loop at init).
    // Returns nullptr when full. staleMs: how long silence means "frozen".
    HeartbeatSource* addHeartbeat(uint16_t staleMs = 150) {
        if (_hbCount >= kMaxHeartbeats) return nullptr;
        _hb[_hbCount].staleMs = staleMs;
        return &_hb[_hbCount++].src;
    }

    bool frozen() const { return _frozen; }

    // ---- Pump. Call from ONE task (Core 0). Animation time advances only
    // while every heartbeat is alive; a stalled source freezes the frame
    // exactly where it is (rule 2 in the header).
    void update(uint32_t nowMs) {
        uint32_t dt = _hasTime ? (nowMs - _lastMs) : 0;
        _lastMs = nowMs;
        _hasTime = true;

        _frozen = anyHeartbeatStale(nowMs);
        if (_frozen) return;  // no phase advance, no show(): frame latches as-is

        _animMs += dt;

        GlowState target = current();
        if (target != _shownState) {
            // Crossfade start: capture the outgoing frame as the fade origin.
            for (size_t i = 0; i < framePixels(); ++i) _fadeFrom[i] = _frame[i];
            _shownState = target;
            _fadeRemainingMs = kCrossfadeMs;
            _animMs = 0;  // new state starts its pattern at phase 0
        }

        renderState(_specs[size_t(_shownState)], _animMs);

        if (_fadeRemainingMs > 0) {
            uint16_t step = uint16_t(dt > _fadeRemainingMs ? _fadeRemainingMs : dt);
            _fadeRemainingMs = uint16_t(_fadeRemainingMs - step);
            uint8_t t = uint8_t(255 - (uint32_t(_fadeRemainingMs) * 255) / kCrossfadeMs);
            for (size_t i = 0; i < framePixels(); ++i)
                _frame[i] = Rgb::lerp(_fadeFrom[i], _frame[i], t);
        }

        for (size_t i = 0; i < framePixels(); ++i) _out.set(i, scale(_frame[i]));
        _out.show();
    }

private:
    struct Heartbeat {
        HeartbeatSource src;
        uint16_t staleMs = 150;
        uint32_t lastSeen = 0;
        uint32_t lastChangeMs = 0;
        bool seeded = false;
    };

    size_t framePixels() const {
        size_t n = _out.pixelCount();
        return n < kMaxPixels ? n : kMaxPixels;
    }

    bool anyHeartbeatStale(uint32_t nowMs) {
        bool stale = false;
        for (size_t i = 0; i < _hbCount; ++i) {
            Heartbeat& h = _hb[i];
            uint32_t c = h.src.counter;
            if (!h.seeded || c != h.lastSeen) {
                h.seeded = true;
                h.lastSeen = c;
                h.lastChangeMs = nowMs;
            } else if (nowMs - h.lastChangeMs > h.staleMs) {
                stale = true;  // keep scanning: every source's bookkeeping stays fresh
            }
        }
        return stale;
    }

    // Triangle wave 0..255..0 over period, from _animMs.
    static uint8_t trianglePhase(uint32_t animMs, uint16_t period) {
        if (period == 0) return 255;
        uint32_t ph = (animMs % period) * 512u / period;  // 0..511
        return uint8_t(ph < 256 ? ph : 511 - ph);
    }

    void renderState(const GlowSpec& s, uint32_t animMs) {
        size_t n = framePixels();
        switch (s.mode) {
            case GlowMode::Solid:
                for (size_t i = 0; i < n; ++i) _frame[i] = s.colorA;
                break;
            case GlowMode::Breathe: {
                Rgb c = Rgb::lerp(s.colorB, s.colorA, trianglePhase(animMs, s.period_ms));
                for (size_t i = 0; i < n; ++i) _frame[i] = c;
                break;
            }
            case GlowMode::Blink: {
                bool on = s.period_ms == 0 || (animMs % s.period_ms) < (s.period_ms / 2u);
                Rgb c = on ? s.colorA : s.colorB;
                for (size_t i = 0; i < n; ++i) _frame[i] = c;
                break;
            }
            case GlowMode::Chase: {
                if (n < 2) {  // degrades gracefully on a lone LED
                    Rgb c = Rgb::lerp(s.colorB, s.colorA, trianglePhase(animMs, s.period_ms));
                    _frame[0] = c;
                    break;
                }
                size_t head = s.period_ms ? (animMs % s.period_ms) * n / s.period_ms : 0;
                for (size_t i = 0; i < n; ++i) _frame[i] = (i == head) ? s.colorA : s.colorB;
                break;
            }
            case GlowMode::Rainbow: {
                uint8_t h0 = s.period_ms ? uint8_t((animMs % s.period_ms) * 255u / s.period_ms) : 0;
                for (size_t i = 0; i < n; ++i)
                    _frame[i] = hsv(uint8_t(h0 + (n > 1 ? i * 255 / n : 0)), 255, 255);
                break;
            }
        }
    }

    Rgb scale(Rgb c) const {
        auto s = [this](uint8_t v) { return uint8_t((uint16_t(v) * (_brightness + 1)) >> 8); };
        return {s(c.r), s(c.g), s(c.b)};
    }

    IGlowOutput& _out;
    GlowSpec _specs[kGlowStateCount];
    volatile bool _active[kGlowStateCount] = {};
    Rgb _frame[kMaxPixels] = {};
    Rgb _fadeFrom[kMaxPixels] = {};
    GlowState _shownState = GlowState::Boot;
    Heartbeat _hb[kMaxHeartbeats];
    size_t _hbCount = 0;
    uint32_t _animMs = 0;
    uint32_t _lastMs = 0;
    uint16_t _fadeRemainingMs = 0;
    uint8_t _brightness = 255;
    bool _hasTime = false;
    bool _frozen = false;
};

}  // namespace slopglow
