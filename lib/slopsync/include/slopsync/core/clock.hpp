// slopsync-core — injected clock (SPEC §17.2: conformance requires an
// injectable clock so golden vectors and behavioral tests are deterministic).
// All protocol time is hub time: µs since hub boot, u32, wrapping (§7.2).
#pragma once

#include <cstdint>

namespace slopsync {

class IClock {
public:
    virtual ~IClock() = default;
    virtual uint32_t nowUs() const = 0;  // hub-µs, wraps ~71.6 min (§7.2)
    uint32_t nowMs() const { return nowUs() / 1000u; }
};

// Test double: time moves only when the test says so.
class ManualClock final : public IClock {
public:
    explicit ManualClock(uint32_t startUs = 0) : _us(startUs) {}
    uint32_t nowUs() const override { return _us; }
    void advanceUs(uint32_t d) { _us += d; }  // wrapping is intentional
    void setUs(uint32_t t) { _us = t; }

private:
    uint32_t _us;
};

// Platform adapters (steady/monotonic sources) live OUTSIDE the library:
// firmware wraps esp_timer_get_time(), the sim wraps std::chrono::steady_clock.
// The library itself never names a platform clock — that is the whole point.

}  // namespace slopsync
