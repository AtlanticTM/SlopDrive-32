// slopsync-core — SHA-256 (FIPS 180-4), used ONLY to derive `catalog_etag`
// (SPEC §8.3: first 8 bytes of SHA-256 over the deterministic catalog
// encoding). Zero-dependency by design: no mbedtls, no platform crypto API —
// this file is IDENTICAL C++ on the host (native tests) and on the ESP32-S3,
// so the etag a desktop tool computes and the etag the firmware computes for
// the same catalog bytes are byte-for-byte the same value.
//
// Origin: adapted from Brad Conte's public-domain SHA-256 implementation
// (https://github.com/B-Con/crypto-algorithms, sha256.c/.h — "This code is
// released into the public domain free of any restrictions"), rewritten as a
// header-only C++ incremental hasher over std::span: no heap, no exceptions,
// no libc string.h dependency beyond std::memcpy/memset. The transform's
// round constants, message-schedule recurrence, and compression function are
// the standard FIPS 180-4 SHA-256 algorithm and are not this project's IP.
//
// Known-answer values (checked by the test suite):
//   SHA256("")    = e3b0c442 98fc1c14 9afbf4c8 996fb924 27ae41e4 649b934c a495991b 7852b855
//   SHA256("abc") = ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace slopsync {

class Sha256 {
public:
    static constexpr size_t kDigestBytes = 32;
    static constexpr size_t kBlockBytes = 64;

    Sha256() { reset(); }

    void reset() {
        _state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        _bitlen = 0;
        _bufLen = 0;
    }

    // Feeds any number of bytes, any number of times, in any chunking —
    // the incremental API a caller uses to hash data assembled piecewise.
    void update(std::span<const std::byte> data) {
        size_t offset = 0;
        // Top up a partial block first, if one is pending.
        if (_bufLen > 0) {
            size_t need = kBlockBytes - _bufLen;
            size_t take = data.size() < need ? data.size() : need;
            std::memcpy(_buf.data() + _bufLen, data.data(), take);
            _bufLen += take;
            offset += take;
            if (_bufLen == kBlockBytes) {
                transform(_buf.data());
                _bitlen += uint64_t(kBlockBytes) * 8;
                _bufLen = 0;
            }
        }
        // Full blocks straight out of the caller's span — no intermediate copy.
        while (data.size() - offset >= kBlockBytes) {
            transform(reinterpret_cast<const uint8_t*>(data.data()) + offset);
            _bitlen += uint64_t(kBlockBytes) * 8;
            offset += kBlockBytes;
        }
        // Whatever's left becomes the new partial block.
        size_t rem = data.size() - offset;
        if (rem > 0) {
            std::memcpy(_buf.data(), data.data() + offset, rem);
            _bufLen = rem;
        }
    }

    // Applies FIPS 180-4 padding (0x80, zero fill, 64-bit big-endian bit
    // length) and produces the 32-byte digest. The object is fully consumed
    // by this call (matches the reference implementation's ctx-clearing
    // final()); call reset() before reusing for a new message.
    std::array<std::byte, kDigestBytes> final() {
        size_t i = _bufLen;
        if (_bufLen < 56) {
            _buf[i++] = 0x80;
            while (i < 56) _buf[i++] = 0x00;
        } else {
            _buf[i++] = 0x80;
            while (i < kBlockBytes) _buf[i++] = 0x00;
            transform(_buf.data());
            std::memset(_buf.data(), 0, 56);
        }

        _bitlen += uint64_t(_bufLen) * 8;
        for (int b = 0; b < 8; ++b) {
            _buf[63 - size_t(b)] = uint8_t(_bitlen >> (8 * b));
        }
        transform(_buf.data());

        std::array<std::byte, kDigestBytes> out{};
        for (int w = 0; w < 8; ++w) {
            out[size_t(w) * 4 + 0] = std::byte(uint8_t(_state[size_t(w)] >> 24));
            out[size_t(w) * 4 + 1] = std::byte(uint8_t(_state[size_t(w)] >> 16));
            out[size_t(w) * 4 + 2] = std::byte(uint8_t(_state[size_t(w)] >> 8));
            out[size_t(w) * 4 + 3] = std::byte(uint8_t(_state[size_t(w)]));
        }
        return out;
    }

    // One-shot convenience: hash a single contiguous span.
    static std::array<std::byte, kDigestBytes> hash(std::span<const std::byte> data) {
        Sha256 sha;
        sha.update(data);
        return sha.final();
    }

private:
    std::array<uint32_t, 8> _state{};
    uint64_t _bitlen = 0;
    std::array<uint8_t, kBlockBytes> _buf{};
    size_t _bufLen = 0;

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    // FIPS 180-4 §4.2.2: the 64 round constants (first 32 bits of the
    // fractional parts of the cube roots of the first 64 primes).
    static constexpr std::array<uint32_t, 64> kK = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    // Processes exactly one 64-byte block, updating _state in place.
    void transform(const uint8_t block[kBlockBytes]) {
        uint32_t m[64];
        for (int i = 0; i < 16; ++i) {
            size_t j = size_t(i) * 4;
            m[i] = (uint32_t(block[j]) << 24) | (uint32_t(block[j + 1]) << 16) |
                   (uint32_t(block[j + 2]) << 8) | uint32_t(block[j + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
            uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
            m[i] = m[i - 16] + s0 + m[i - 7] + s1;
        }

        uint32_t a = _state[0], b = _state[1], c = _state[2], d = _state[3];
        uint32_t e = _state[4], f = _state[5], g = _state[6], h = _state[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t ep1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + ep1 + ch + kK[size_t(i)] + m[i];
            uint32_t ep0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = ep0 + maj;

            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
        _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
    }
};

}  // namespace slopsync
