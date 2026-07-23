// SlopLog — the systemwide logging front door.
//
//   SLOGI("wifi", "connected to %s ch%d", ssid, ch);
//   SLOGW_EVERY_MS(1000, "arbiter", "intent rejected: %s", why);   // per-call-site throttle
//
// Levels: SLOGT / SLOGD / SLOGI / SLOGW / SLOGE / SLOGF.
// Compile-time floor: define SLOPLOG_COMPILE_LEVEL (0=Trace..5=Fatal) in
// build flags; anything below it compiles to NOTHING (zero code, zero
// strings in flash). Runtime floor on top: sloplog::logger().setFloor().
//
// Threading contract: SLOG* is callable from any FreeRTOS task on either
// core (bounded format + spinlock'd slot copy — never blocks, never
// allocates). NOT ISR-safe. Exactly one task pumps sloplog::drainToSinks()
// (Core 0 on this firmware); sinks run on that task only.
//
// The hardware-free core (ring/sinks/records) lives in sloplog_core.hpp and
// is what the native test suite exercises; this header is the ESP32/Arduino
// glue + macro surface.
#pragma once

#include "sloplog/sloplog_core.hpp"

#if defined(ARDUINO)
#include <Arduino.h>
#include "freertos/FreeRTOS.h"

namespace sloplog {

class Esp32Port final : public IPort {
public:
    uint32_t nowMs() override { return millis(); }
    uint8_t coreId() override { return uint8_t(xPortGetCoreID()); }
    void lock() override { portENTER_CRITICAL(&_mux); }
    void unlock() override { portEXIT_CRITICAL(&_mux); }

private:
    portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
};

using FirmwareLog = LogCore<64, 4>;

// Meyers singletons: construction is race-free and needs no init() ordering
// games — the first SLOG anywhere (even during static init) just works.
inline Esp32Port& port() {
    static Esp32Port p;
    return p;
}
inline FirmwareLog& logger() {
    static FirmwareLog l(port(), Level::Trace);
    return l;
}

// A serial sink you can register at boot (after Serial.begin):
//   sloplog::logger().addSink(&sloplog::serialSink());
class SerialSink final : public ISink {
public:
    void write(const Record& r) override {
        // [ 12.345 W1 arbiter ] message   (seconds, level+core, tag)
        Serial.printf("[%7lu.%03lu %c%u %-10s] %s",
                      (unsigned long)(r.ms / 1000u), (unsigned long)(r.ms % 1000u),
                      levelChar(r.level), r.core, r.tag, r.msg);
        if (r.lost) Serial.printf("  (+%u lost)", r.lost);
        Serial.println();
    }
};

inline SerialSink& serialSink() {
    static SerialSink s;
    return s;
}

// Pump from exactly one Core-0 task (httpTask on this firmware).
inline void drainToSinks() { logger().drain(); }

}  // namespace sloplog

#define SLOPLOG_EMIT(lvl, tag, ...) ::sloplog::logger().logf(lvl, tag, __VA_ARGS__)

#else  // native/host builds: no Arduino glue; tests drive LogCore directly.
#define SLOPLOG_EMIT(lvl, tag, ...) ((void)0)
#endif  // ARDUINO

// ---- Compile-time floor -----------------------------------------------------
// 0 Trace, 1 Debug, 2 Info, 3 Warn, 4 Error, 5 Fatal. Default keeps Debug+.
#ifndef SLOPLOG_COMPILE_LEVEL
#define SLOPLOG_COMPILE_LEVEL 1
#endif

#if SLOPLOG_COMPILE_LEVEL <= 0
#define SLOGT(tag, ...) SLOPLOG_EMIT(::sloplog::Level::Trace, tag, __VA_ARGS__)
#else
#define SLOGT(tag, ...) ((void)0)
#endif
#if SLOPLOG_COMPILE_LEVEL <= 1
#define SLOGD(tag, ...) SLOPLOG_EMIT(::sloplog::Level::Debug, tag, __VA_ARGS__)
#else
#define SLOGD(tag, ...) ((void)0)
#endif
#if SLOPLOG_COMPILE_LEVEL <= 2
#define SLOGI(tag, ...) SLOPLOG_EMIT(::sloplog::Level::Info, tag, __VA_ARGS__)
#else
#define SLOGI(tag, ...) ((void)0)
#endif
#if SLOPLOG_COMPILE_LEVEL <= 3
#define SLOGW(tag, ...) SLOPLOG_EMIT(::sloplog::Level::Warn, tag, __VA_ARGS__)
#else
#define SLOGW(tag, ...) ((void)0)
#endif
#if SLOPLOG_COMPILE_LEVEL <= 4
#define SLOGE(tag, ...) SLOPLOG_EMIT(::sloplog::Level::Error, tag, __VA_ARGS__)
#else
#define SLOGE(tag, ...) ((void)0)
#endif
#define SLOGF(tag, ...) SLOPLOG_EMIT(::sloplog::Level::Fatal, tag, __VA_ARGS__)

// ---- Per-call-site rate limiting -------------------------------------------
// Each macro expansion owns its own static throttle state (that's the point:
// the rate limit is per SITE, not per tag). Suppressed emissions are counted
// and reported on the next one that passes: "... (suppressed 42)".
// The unsigned now-last compare is wrap-safe for intervals < 2^31 ms.
#if defined(ARDUINO)
#define SLOPLOG_EVERY_MS(lvl_macro, interval_ms, tag, fmt, ...)                        \
    do {                                                                               \
        static uint32_t _slog_last = 0;                                                \
        static uint32_t _slog_skips = 0;                                               \
        static bool _slog_ever = false;                                                \
        uint32_t _slog_now = millis();                                                 \
        if (!_slog_ever || (_slog_now - _slog_last) >= (uint32_t)(interval_ms)) {      \
            _slog_ever = true;                                                         \
            _slog_last = _slog_now;                                                    \
            if (_slog_skips) {                                                         \
                lvl_macro(tag, fmt " (suppressed %lu)", ##__VA_ARGS__,                 \
                          (unsigned long)_slog_skips);                                 \
                _slog_skips = 0;                                                       \
            } else {                                                                   \
                lvl_macro(tag, fmt, ##__VA_ARGS__);                                    \
            }                                                                          \
        } else {                                                                       \
            ++_slog_skips;                                                             \
        }                                                                              \
    } while (0)
#else
#define SLOPLOG_EVERY_MS(lvl_macro, interval_ms, tag, fmt, ...) ((void)0)
#endif

#define SLOGD_EVERY_MS(interval_ms, tag, fmt, ...) SLOPLOG_EVERY_MS(SLOGD, interval_ms, tag, fmt, ##__VA_ARGS__)
#define SLOGI_EVERY_MS(interval_ms, tag, fmt, ...) SLOPLOG_EVERY_MS(SLOGI, interval_ms, tag, fmt, ##__VA_ARGS__)
#define SLOGW_EVERY_MS(interval_ms, tag, fmt, ...) SLOPLOG_EVERY_MS(SLOGW, interval_ms, tag, fmt, ##__VA_ARGS__)
#define SLOGE_EVERY_MS(interval_ms, tag, fmt, ...) SLOPLOG_EVERY_MS(SLOGE, interval_ms, tag, fmt, ##__VA_ARGS__)
