// slopsync-core — little-endian span primitives (SPEC §1.4: all multi-byte
// integers on the wire are little-endian). Every codec reads/writes through
// these; nothing else in the library touches raw byte order.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>

namespace slopsync {

// ---- Writers: return bytes written, or 0 if `out` is too small. ------------
inline size_t putU8(std::span<std::byte> out, uint8_t v) {
    if (out.size() < 1) return 0;
    out[0] = std::byte{v};
    return 1;
}
inline size_t putU16(std::span<std::byte> out, uint16_t v) {
    if (out.size() < 2) return 0;
    out[0] = std::byte(uint8_t(v));
    out[1] = std::byte(uint8_t(v >> 8));
    return 2;
}
inline size_t putU32(std::span<std::byte> out, uint32_t v) {
    if (out.size() < 4) return 0;
    for (int i = 0; i < 4; ++i) out[size_t(i)] = std::byte(uint8_t(v >> (8 * i)));
    return 4;
}
inline size_t putF32(std::span<std::byte> out, float v) {
    static_assert(sizeof(float) == 4);
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return putU32(out, bits);
}

// ---- Readers: precondition in.size() >= N (callers bound-check via the
// frame header's len field before slicing; these never see short input). ----
inline uint8_t getU8(std::span<const std::byte> in) { return uint8_t(in[0]); }
inline uint16_t getU16(std::span<const std::byte> in) {
    return uint16_t(uint8_t(in[0])) | uint16_t(uint8_t(in[1])) << 8;
}
inline uint32_t getU32(std::span<const std::byte> in) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(in[size_t(i)])) << (8 * i);
    return v;
}
inline float getF32(std::span<const std::byte> in) {
    uint32_t bits = getU32(in);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

}  // namespace slopsync
