// slopsync-core — HMAC-SHA256 (RFC 2104 over wire/sha256.hpp), used by the
// §12.2 pairing ceremony: pin_proof = HMAC(key = PIN ASCII, msg = WELCOME
// nonce), truncated to 16 bytes. Zero-dependency, heap-free.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/wire/sha256.hpp"

namespace slopsync {

inline std::array<std::byte, 32> hmacSha256(std::span<const std::byte> key,
                                            std::span<const std::byte> msg) {
    constexpr size_t B = 64;  // SHA-256 block size
    std::array<std::byte, B> k{};
    if (key.size() > B) {
        auto kh = Sha256::hash(key);
        std::memcpy(k.data(), kh.data(), kh.size());
    } else {
        std::memcpy(k.data(), key.data(), key.size());
    }

    std::array<std::byte, B> ipad{}, opad{};
    for (size_t i = 0; i < B; ++i) {
        ipad[i] = k[i] ^ std::byte{0x36};
        opad[i] = k[i] ^ std::byte{0x5C};
    }

    Sha256 inner;
    inner.update(ipad);
    inner.update(msg);
    auto innerHash = inner.final();

    Sha256 outer;
    outer.update(opad);
    outer.update(innerHash);
    return outer.final();
}

// The §12.2 proof: HMAC-SHA256(PIN-as-ASCII, nonce), first 16 bytes.
inline std::array<std::byte, 16> pairingPinProof(std::span<const char> pinAscii,
                                                 std::span<const std::byte> nonce) {
    auto mac = hmacSha256(std::as_bytes(pinAscii), nonce);
    std::array<std::byte, 16> out{};
    std::memcpy(out.data(), mac.data(), 16);
    return out;
}

}  // namespace slopsync
