// SlopGlow — ESP32/Arduino output drivers over the hardware-free core.
// Include this on firmware; native tests include slopglow_core.hpp directly.
//
// Drivers here are the generic, reusable ones (LEDC PWM color/mono). Board
// wiring — which pins, which states map to which SystemState fields, the
// heartbeat sources — lives in the firmware (src/system/), NOT here.
// A NeoPixel/RMT strip driver is deliberately absent until a board with an
// actual strip lands (C5 nodes) — no compiling unused driver objects.
#pragma once

#include "slopglow/slopglow_core.hpp"

#if defined(ARDUINO)
#include <Arduino.h>

namespace slopglow {

// Perceptual gamma (^2.2) lookup so PWM brightness ramps linearly to the eye.
inline uint8_t gamma8(uint8_t v) {
    static const uint8_t* table = [] {
        static uint8_t t[256];
        for (int i = 0; i < 256; ++i) {
            float f = powf(float(i) / 255.0f, 2.2f);
            t[i] = uint8_t(f * 255.0f + 0.5f);
        }
        return t;
    }();
    return table[v];
}

// One true-color pixel on three LEDC PWM pins (discrete RGB LED). Handles
// active-low (current-sinking) LEDs by inverting duty.
class LedcRgbOutput final : public IGlowOutput {
public:
    LedcRgbOutput(uint8_t pinR, uint8_t pinG, uint8_t pinB, bool activeLow,
                  uint32_t freqHz = 5000)
        : _pins{pinR, pinG, pinB}, _activeLow(activeLow), _freq(freqHz) {}

    void begin() {
        for (uint8_t p : _pins) {
            ledcAttach(p, _freq, 8);
            ledcWrite(p, _activeLow ? 255 : 0);
        }
    }

    size_t pixelCount() const override { return 1; }
    void set(size_t, Rgb c) override { _pending = c; }
    void show() override {
        uint8_t d[3] = {gamma8(_pending.r), gamma8(_pending.g), gamma8(_pending.b)};
        for (int i = 0; i < 3; ++i)
            ledcWrite(_pins[i], _activeLow ? uint32_t(255 - d[i]) : uint32_t(d[i]));
    }

private:
    uint8_t _pins[3];
    bool _activeLow;
    uint32_t _freq;
    Rgb _pending{};
};

// One mono LED on an LEDC PWM pin, driven by the frame color's perceptual
// luma — an intensity shadow of the status pixel. Because every stock spec
// animates, this LED pulses in sympathy with the state color and freezes
// with the engine: it IS the heartbeat lamp.
class LedcMonoOutput final : public IGlowOutput {
public:
    LedcMonoOutput(uint8_t pin, bool activeHigh, uint32_t freqHz = 5000)
        : _pin(pin), _activeHigh(activeHigh), _freq(freqHz) {}

    void begin() {
        ledcAttach(_pin, _freq, 8);
        ledcWrite(_pin, _activeHigh ? 0 : 255);
    }

    size_t pixelCount() const override { return 1; }
    void set(size_t, Rgb c) override { _pending = c; }
    void show() override {
        uint8_t d = gamma8(_pending.luma());
        ledcWrite(_pin, _activeHigh ? uint32_t(d) : uint32_t(255 - d));
    }

private:
    uint8_t _pin;
    bool _activeHigh;
    uint32_t _freq;
    Rgb _pending{};
};

// Fans one logical frame out to several physical outputs (e.g. the RGB
// status LED + the mono heartbeat lamp showing the same state).
template <size_t MaxChildren = 3>
class FanoutOutput final : public IGlowOutput {
public:
    bool add(IGlowOutput* o) {
        if (o == nullptr || _count >= MaxChildren) return false;
        _children[_count++] = o;
        return true;
    }
    size_t pixelCount() const override {
        size_t n = 1;
        for (size_t i = 0; i < _count; ++i)
            if (_children[i]->pixelCount() > n) n = _children[i]->pixelCount();
        return n;
    }
    void set(size_t i, Rgb c) override {
        for (size_t k = 0; k < _count; ++k)
            if (i < _children[k]->pixelCount()) _children[k]->set(i, c);
    }
    void show() override {
        for (size_t k = 0; k < _count; ++k) _children[k]->show();
    }

private:
    IGlowOutput* _children[MaxChildren] = {};
    size_t _count = 0;
};

}  // namespace slopglow

#endif  // ARDUINO
