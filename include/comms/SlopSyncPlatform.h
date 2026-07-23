#pragma once

// ============================================================================
// SlopSyncPlatform — ESP32-S3 adapters for the SlopSync injected-dependency
// seams (IClock / IRandom). The library itself never names a platform source
// (SPEC §17.2: determinism is a conformance requirement, so ALL time + entropy
// enters through these injected interfaces). This header is the ONE place the
// hub's clock/rng are bound to real hardware — nothing wire-visible lives here.
//
// Header-only, tiny. Included by SlopSyncHubService (the composition root).
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <span>

#include <esp_random.h>
#include <esp_timer.h>

#include "slopsync/core/clock.hpp"
#include "slopsync/core/rng.hpp"

namespace slopdrive {

// ---- IClock over esp_timer -------------------------------------------------
// SPEC §7.2: hub time is u32 µs since boot, WRAPPING every ~71.6 min. The wrap
// is BY SPEC, not an accident — every ms deadline in the hub goes through
// MonotonicMs/timeReached() which own the wrap window. So we deliberately
// truncate esp_timer_get_time()'s int64 µs to u32 and let it wrap; do NOT
// widen it or the hub's wrap-safe compares stop matching the wire clock.
class EspClock final : public slopsync::IClock {
public:
    uint32_t nowUs() const override {
        return static_cast<uint32_t>(esp_timer_get_time() & 0xFFFFFFFFull);
    }
};

// ---- IRandom over the hardware RNG -----------------------------------------
// esp_random() is the SoC hardware entropy source (valid once RF/Wi-Fi is up,
// which it always is on the main controller by the time the hub spins up). This
// feeds session ids, boot id, WELCOME nonces, and pairing tokens (§6.1, §12.2)
// — production entropy, never the library's deterministic test doubles.
class EspRandom final : public slopsync::IRandom {
public:
    uint32_t nextU32() override { return esp_random(); }

    void fill(std::span<std::byte> out) override {
        if (!out.empty()) esp_fill_random(out.data(), out.size());
    }
};

}  // namespace slopdrive
