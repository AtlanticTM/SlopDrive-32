// slopsync-core — a bounded, heap-free, always-valid short string.
//
// EXISTS BECAUSE A LEDGER ENTRY OUTLIVES ITS FRAME. Every decoded message hands
// out std::string_view into the frame buffer — zero-copy, correct, and valid
// only for the duration of the dispatch. Anything the hub REMEMBERS (a paired
// device's name, a pending knock's version) must own its bytes, and it must do
// so without a heap, because this library never allocates in steady state.
//
// TRUNCATES, NEVER REJECTS. A roster entry that vanishes because a client chose
// a long name is a worse failure than a shortened label. The one place that
// rule is inverted is the DECODER: RFC-028.2 says a receiver rejects an
// over-cap string in a structural payload rather than truncating it, so wire
// decoding enforces the cap before anything reaches here. This type is the
// storage side of that split, not the parsing side.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace slopsync {

template <size_t N>
struct BoundedString {
    std::array<char, N> data{};
    uint8_t len = 0;

    void assign(std::string_view s) {
        len = uint8_t(s.size() < N ? s.size() : N);
        if (len) std::memcpy(data.data(), s.data(), len);
        for (size_t i = len; i < N; ++i) data[i] = char(0);
    }
    std::string_view view() const { return std::string_view(data.data(), len); }
    bool sameAs(std::string_view s) const {
        // Compare against the TRUNCATED form of `s`, so a client whose name is
        // longer than the cap does not appear to change on every connect —
        // which on the RFC-029 version tripwire would be a false alarm every
        // single time.
        const size_t n = s.size() < N ? s.size() : N;
        return n == len && std::memcmp(data.data(), s.data(), len) == 0;
    }
    bool empty() const { return len == 0; }
};

}  // namespace slopsync
