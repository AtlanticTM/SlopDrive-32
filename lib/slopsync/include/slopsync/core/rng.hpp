// slopsync-core — injected randomness (SPEC §17.2). Session ids, boot ids,
// nonces, tokens, and the in-process binding's fault schedule all draw from
// here so golden vectors and deterministic sim runs are reproducible.
#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

namespace slopsync {

class IRandom {
public:
    virtual ~IRandom() = default;
    virtual uint32_t nextU32() = 0;
    virtual void fill(std::span<std::byte> out) = 0;
};

// Test double: returns a fixed byte sequence verbatim (vector fixtures),
// wrapping around when exhausted — deterministic by construction.
class FixedSequenceRandom final : public IRandom {
public:
    explicit FixedSequenceRandom(std::span<const std::byte> seq)
        : _seq(seq), _pos(0) {}

    uint32_t nextU32() override {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(nextByte()) << (8 * i);  // LE
        return v;
    }
    void fill(std::span<std::byte> out) override {
        for (auto& b : out) b = std::byte{nextByte()};
    }

private:
    uint8_t nextByte() {
        if (_seq.empty()) return 0;
        uint8_t b = uint8_t(_seq[_pos]);
        _pos = (_pos + 1) % _seq.size();
        return b;
    }
    std::span<const std::byte> _seq;
    size_t _pos;
};

// A small deterministic PRNG (xorshift32) — used by the in-process binding's
// seeded fault-injection mode (§13.6); NOT cryptographic, never used for
// tokens in production (platform adapters supply hardware entropy there).
class XorShift32 final : public IRandom {
public:
    explicit XorShift32(uint32_t seed) : _s(seed ? seed : 0xB007CAFEu) {}
    uint32_t nextU32() override {
        uint32_t x = _s;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return _s = x;
    }
    void fill(std::span<std::byte> out) override {
        for (auto& b : out) b = std::byte(uint8_t(nextU32()));
    }
private:
    uint32_t _s;
};

}  // namespace slopsync
