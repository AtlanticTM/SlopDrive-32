// slopsync-core — CRC-32/ISO-HDLC ("CRC-32 IEEE 802.3"), used by the ESTOP
// frame's integrity check (SPEC §5.5): poly 0xEDB88320 (reflected), init
// 0xFFFFFFFF, final XOR 0xFFFFFFFF, no reflection needed on in/out since the
// reflected-poly table already computes the bit-reversed form directly.
// Known-answer check (verified by the test suite): crc32("123456789") ==
// 0xCBF43926 — the canonical value every CRC-32/ISO-HDLC implementation is
// checked against.
//
// The table is built at COMPILE TIME (constexpr-evaluated array
// initializer) so there is no runtime table-build step and no static-init
// ordering concern; it costs 1 KiB of flash/rodata, same as any other
// table-driven CRC-32.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace slopsync {

namespace detail {
inline constexpr std::array<uint32_t, 256> makeCrc32Table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}
}  // namespace detail

inline constexpr std::array<uint32_t, 256> kCrc32Table = detail::makeCrc32Table();

// ---- Incremental API: init -> update (any number of times) -> final. -------
// Lets a caller CRC a header-shaped-but-scattered buffer (e.g. compute over
// bytes assembled piecewise) without a temporary contiguous copy.
inline constexpr uint32_t crc32Init() { return 0xFFFFFFFFu; }

inline constexpr uint32_t crc32Update(uint32_t state, std::span<const std::byte> data) {
    for (std::byte b : data) {
        state = kCrc32Table[(state ^ uint32_t(uint8_t(b))) & 0xFFu] ^ (state >> 8);
    }
    return state;
}

inline constexpr uint32_t crc32Final(uint32_t state) { return state ^ 0xFFFFFFFFu; }

// One-shot convenience over a single contiguous span.
inline constexpr uint32_t crc32(std::span<const std::byte> data) {
    return crc32Final(crc32Update(crc32Init(), data));
}

}  // namespace slopsync
