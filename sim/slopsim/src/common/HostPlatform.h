#pragma once

// HostPlatform — desktop adapters for the SlopSync injected-dependency seams
// (IClock/IRandom).
// Constraints:
//   Mirrors include/comms/SlopSyncPlatform.h's EspClock/EspRandom; the ONE
//   place slopsim binds the hub's clock/rng to the host OS.
//   SPEC §7.2: hub time is u32 µs since boot, WRAPPING every ~71.6 min.
//   nowUs() truncates a 64-bit steady_clock reading to u32 exactly like
//   EspClock truncates esp_timer_get_time(). nowUs64() is the unwrapped
//   esp_timer_get_time() analog used for PacingRing due_us and
//   slopmotion::Engine time — only the WIRE timestamp wraps, like the
//   firmware's now64.
// See: include/comms/SlopSyncPlatform.h

#include <chrono>
#include <cstdint>
#include <random>
#include <span>
#include <thread>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace slopsim {

// ---- HostTimerResolution ----------------------------------------------------
// The single most important line in the sim: Windows' default scheduler
// tick is 15.625 ms, and sleep_for() rounds UP to
// it: `sleep_for(2ms)` measured 15.713 ms per iteration on this host. The sim's
// motion loop then ran at ~64 Hz while claiming a 1 kHz sampler, which made
// every stream commit land at the head of a ~16 ms substep burst — a staircase
// setpoint instead of a curve (see MachineSim::tickMachine).
//
// timeBeginPeriod(1) asks the multimedia timer for a 1 ms scheduler tick,
// process-wide, for as long as it is held (hence the RAII pair — leaving it
// raised costs the whole machine battery life). Nothing in this tree or its
// deps called it. On Linux the default 1 ms-ish nanosleep granularity already
// applies and this is a no-op.
class HostTimerResolution {
public:
    explicit HostTimerResolution(bool enable = true) {
#ifdef _WIN32
        if (enable && timeBeginPeriod(1) == TIMERR_NOERROR) _held = true;
#else
        (void)enable;
#endif
    }
    ~HostTimerResolution() {
#ifdef _WIN32
        if (_held) timeEndPeriod(1);
#endif
    }
    HostTimerResolution(const HostTimerResolution&) = delete;
    HostTimerResolution& operator=(const HostTimerResolution&) = delete;
    bool held() const { return _held; }

private:
    bool _held = false;
};

// Coarse sleep, best available. MEASURED on this host (Win 11, WinLibs GCC,
// 300-sample loops):
//                              default res        timeBeginPeriod(1)
//   std::this_thread::sleep_for(1ms)   15.75 ms          15.61 ms  (!!)
//   WinAPI Sleep(1)                    15.72 ms           1.88 ms
//   high-res waitable timer 1000us      1.54 ms           1.53 ms
//   yield-spin to a 1 ms deadline       1.000 ms          1.000 ms
// Two surprises worth keeping written down: (1) MinGW's sleep_for does NOT
// honor the multimedia timer period at all, so raising it fixes everything in
// the process EXCEPT the C++ sleep we were using; (2) the high-resolution
// waitable timer (Win10 1803+) is good without any period raising, but still
// has ~0.5 ms of its own overhead — which is half our loop period.
inline void coarseSleep(std::chrono::nanoseconds d) {
#ifdef _WIN32
    static thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(d.count() / 100);  // relative, 100 ns units
        if (due.QuadPart == 0) return;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(d);
}

// Sleep until `deadline`, spinning out the last millisecond. Nothing the OS
// offers can land a 1 ms deadline (see the table above), so the residual goes
// to a yield-spin — that is the ONLY way to get a genuine 1 ms cadence out of a
// general-purpose scheduler, and the sim's whole motion grid rests on it. Cost:
// the sim thread stays hot. Deliberate for a bench instrument; the coarse-sleep
// arm keeps longer waits (a lagging loop, any future slower cadence) cheap.
inline void preciseSleepUntil(std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
    const auto spin_margin = microseconds(1000);
    for (;;) {
        const auto now = steady_clock::now();
        if (now >= deadline) return;
        const auto left = deadline - now;
        if (left > spin_margin) {
            coarseSleep(left - spin_margin);
        } else {
            std::this_thread::yield();
        }
    }
}

class HostClock final : public slopsync::IClock {
public:
    HostClock() : _epoch(std::chrono::steady_clock::now()) {}

    uint32_t nowUs() const override { return uint32_t(nowUs64() & 0xFFFFFFFFull); }

    uint64_t nowUs64() const {
        auto d = std::chrono::steady_clock::now() - _epoch;
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(d).count());
    }

    uint32_t nowMs32() const { return uint32_t((nowUs64() / 1000ull) & 0xFFFFFFFFull); }

private:
    std::chrono::steady_clock::time_point _epoch;
};

// std::random_device is CSPRNG-backed on both target platforms (Windows UCRT:
// rand_s; Linux: /dev/urandom). Feeds session ids / boot id / nonces — never
// the library's deterministic test doubles.
class HostRandom final : public slopsync::IRandom {
public:
    uint32_t nextU32() override { return _rd(); }

    void fill(std::span<std::byte> out) override {
        for (auto& b : out) b = std::byte(_rd() & 0xFF);
    }

private:
    std::random_device _rd;
};

}  // namespace slopsim
