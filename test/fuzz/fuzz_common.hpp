// SlopSync fuzz harness — shared skeleton (RFC-028 parser-totality gate).
//
// Every target in this directory is a libFuzzer `LLVMFuzzerTestOneInput` over
// exactly one DECODE surface of lib/slopsync. The obligation being proven is
// RFC-028 §1: any byte string maps to accept-or-reject — never an OOB read,
// never unbounded allocation/recursion, never UB. The sanitizers (ASan +
// UBSan with -fno-sanitize-recover) are the oracle; the harness itself only
// has to reach the code and never itself be the thing that crashes.
//
// Rules for adding a target (keep them, they are what makes this cheap):
//   * include this header, define LLVMFuzzerTestOneInput, done — no build
//     wiring beyond adding the .cc name to build.sh's TARGETS list.
//   * NEVER assert on decoder *semantics* here. A decoder returning
//     Malformed for something a human thinks is valid is a conformance
//     question for the golden vectors, not a fuzz finding. The only
//     assertions worth making are TOTALITY invariants — e.g. "a successful
//     decode's reported length never exceeds the input" — because those are
//     memory-safety statements in disguise.
//   * Consume the fuzzer's bytes through FuzzInput so that a target that
//     needs a mode selector or a size parameter still degrades gracefully on
//     a 0-byte input.
//   * Anything derived from fuzzer bytes that indexes memory must be bounded
//     by the harness, not by the library — otherwise a harness bug is
//     indistinguishable from a library finding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/slopsync.h"

namespace slopfuzz {

// A tiny front-consuming byte reader. Everything it returns is well-defined
// on an exhausted buffer (zeros / empty spans), so no target ever needs a
// length precondition of its own.
class FuzzInput {
public:
    FuzzInput(const uint8_t* data, size_t size) : _p(data), _n(size) {}

    uint8_t u8(uint8_t dflt = 0) {
        if (_i >= _n) return dflt;
        return _p[_i++];
    }
    uint16_t u16(uint16_t dflt = 0) {
        if (_i + 2 > _n) return dflt;
        uint16_t v = uint16_t(_p[_i]) | uint16_t(uint16_t(_p[_i + 1]) << 8);
        _i += 2;
        return v;
    }
    uint32_t u32(uint32_t dflt = 0) {
        if (_i + 4 > _n) return dflt;
        uint32_t v = 0;
        std::memcpy(&v, _p + _i, 4);
        _i += 4;
        return v;
    }
    // Everything not yet consumed, as the library's byte type.
    std::span<const std::byte> rest() const {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(_p + _i), _n - _i);
    }
    // A bounded slice off the front (clamped to what is left).
    std::span<const std::byte> take(size_t want) {
        size_t have = _n - _i;
        size_t n = want < have ? want : have;
        auto s = std::span<const std::byte>(reinterpret_cast<const std::byte*>(_p + _i), n);
        _i += n;
        return s;
    }
    size_t remaining() const { return _n - _i; }
    bool empty() const { return _i >= _n; }

private:
    const uint8_t* _p;
    size_t _n;
    size_t _i = 0;
};

// The whole input as library bytes — the common case (a target that fuzzes
// one decoder with no mode selector).
inline std::span<const std::byte> asBytes(const uint8_t* data, size_t size) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
}

// Touch every byte of a decoded view so ASan actually witnesses an
// out-of-bounds view rather than letting an unread bad span slide. Returns a
// checksum purely so the compiler cannot optimize the reads away.
inline uint32_t touch(std::span<const std::byte> s) {
    uint32_t acc = 0;
    for (std::byte b : s) acc = acc * 31u + uint32_t(b);
    return acc;
}
inline uint32_t touch(std::string_view s) {
    uint32_t acc = 0;
    for (char c : s) acc = acc * 31u + uint32_t(uint8_t(c));
    return acc;
}

// Sink for the checksums above: keeps the reads live without any output.
inline volatile uint32_t g_sink = 0;
inline void sink(uint32_t v) { g_sink = g_sink ^ v; }

// Floats decoded from fuzzer bytes are routinely NaN/inf/1e34 — a static_cast
// to an integer type is UB for those, so the SINK is the one that must be
// total, not the library. Bit-cast instead of converting.
inline void sinkF(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    g_sink = g_sink ^ bits;
}

}  // namespace slopfuzz
