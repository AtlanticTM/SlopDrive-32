// slopsync-core — the STREAM bundle wire layout, SPEC §5.4/§9.2/§7.2.
//
// Every STREAM channel (h2c or c2h) carries samples packed into a bundle:
//
//   offset  size      field
//   0       4         t_base     u32 hub-time µs of sample[0] (§7.2)
//   4       1         n          sample count, 1..32
//   5       1         reserved   zero
//   6       2×n       t_off[n]   u16 µs offset of sample[i] from t_base
//   6+2n    S×n       samples    n packed sample structs, catalog-declared
//                                size S (S is opaque here — the per-sample
//                                struct's own layout is out of scope for this
//                                file; see wire/packed/layout_codec.hpp for
//                                the runtime-catalog-driven per-field codec)
//
// Caps (SPEC §5.4, registry `limits`), enforced by BOTH BundleWriter and
// BundleView so a hand-rolled or corrupt bundle can never smuggle a
// value neither side would accept from the other:
//   - n <= limits::bundle_max_samples (32)
//   - span, i.e. t_off[n-1], <= limits::bundle_max_span_ms * 1000 µs
//     (note the unit crossing: the registry limit is in MILLISECONDS,
//     t_off is in MICROSECONDS — every comparison below multiplies by 1000)
//   - t_off is STRICTLY increasing and t_off[0] == 0. The spec's prose only
//     says "monotonic"; we take the stronger strict reading because t_off
//     is this sample's unique position in a time series — two samples
//     claiming the identical timestamp is a producer bug, not a valid
//     bundle, and catching it here is free.
//   - the bundle never fragments (§5.6): whatever doesn't fit `out`/`in` is
//     rejected outright, never partially written or partially parsed.
//
// BundleWriter keeps the destination buffer valid-so-far after every
// successful addSample() call (see the method's own comment for the
// in-place compaction trick this requires); finalize() simply reports the
// final size. BundleView validates the same caps at parse time and refuses
// to construct on any violation or truncation — SPEC §9.2's whole premise
// (STREAM tolerates loss) requires that bad/short data be *detected*, not
// crash the receiver.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/util/byte_io.hpp"

namespace slopsync {

inline constexpr size_t kStreamBundleHeaderBytes = 6;  // t_base(4) + n(1) + reserved(1)
inline constexpr size_t kStreamBundleTOffBytes = 2;

class BundleWriter {
public:
    // `out` is the caller-owned destination (sized to the binding's MTU or
    // better); `sampleSize` is the catalog-declared per-sample struct size S.
    // Writes t_base and the zero reserved byte immediately; `n` and the
    // t_off/sample regions are written incrementally by addSample(). If
    // `out` can't even hold the 6-byte header, the writer is permanently
    // invalid (every addSample() call returns false, finalize() returns 0)
    // — this is the constructor-time form of the "out-span too small" cap.
    BundleWriter(std::span<std::byte> out, uint32_t tBase, size_t sampleSize)
        : _out(out), _tBase(tBase), _sampleSize(sampleSize) {
        if (_out.size() >= kStreamBundleHeaderBytes) {
            putU32(_out.subspan(0, 4), _tBase);
            putU8(_out.subspan(5, 1), 0);  // reserved
            _valid = true;
        }
    }

    // Appends one sample at hub-µs offset `tOff` from t_base. Returns false
    // and leaves the buffer EXACTLY as it was before the call (no partial
    // writes) if any cap is violated:
    //   - the writer is invalid (see constructor)
    //   - sample.size() != the declared sampleSize
    //   - n is already at limits::bundle_max_samples
    //   - tOff isn't strictly greater than the previous sample's (or isn't
    //     0, for the first sample)
    //   - tOff exceeds limits::bundle_max_span_ms*1000 µs
    //   - the resulting total size wouldn't fit `out`
    //
    // Implementation note (why this isn't a trivial append): the wire
    // layout puts the WHOLE t_off array before ANY sample bytes, so adding
    // the (n+1)-th sample grows the t_off array by 2 bytes, which shifts
    // every already-written sample 2 bytes to the right. t_off entries
    // themselves never move (new ones only ever get APPENDED to the
    // array), so the fix is a single memmove of the sample block per call
    // — O(n * sampleSize), trivially cheap at n <= 32.
    bool addSample(uint16_t tOff, std::span<const std::byte> sample) {
        if (!_valid) return false;
        if (sample.size() != _sampleSize) return false;
        if (_n >= limits::bundle_max_samples) return false;
        if (_n == 0) {
            if (tOff != 0) return false;
        } else if (tOff <= _tOffs[_n - 1]) {
            return false;  // not strictly monotonic
        }
        if (uint32_t(tOff) > limits::bundle_max_span_ms * 1000u) return false;

        const size_t oldHeaderSize = kStreamBundleHeaderBytes + kStreamBundleTOffBytes * size_t(_n);
        const size_t newHeaderSize = oldHeaderSize + kStreamBundleTOffBytes;
        const size_t newTotal = newHeaderSize + size_t(_n + 1) * _sampleSize;
        if (newTotal > _out.size()) return false;

        if (_n > 0) {
            std::memmove(_out.data() + newHeaderSize, _out.data() + oldHeaderSize,
                         size_t(_n) * _sampleSize);
        }
        putU16(_out.subspan(oldHeaderSize, 2), tOff);
        std::memcpy(_out.data() + newHeaderSize + size_t(_n) * _sampleSize, sample.data(),
                   _sampleSize);

        _tOffs[_n] = tOff;
        ++_n;
        putU8(_out.subspan(4, 1), _n);  // header's `n` stays current after every add
        return true;
    }

    // Total encoded size so far (kStreamBundleHeaderBytes + 2*n + n*S), or 0
    // if the writer never became valid. The buffer is already fully correct
    // after every successful addSample() — finalize() is where callers read
    // the final size to slice `out` down to (and the BundleView-symmetric
    // name for that read).
    size_t finalize() const {
        if (!_valid) return 0;
        return kStreamBundleHeaderBytes + kStreamBundleTOffBytes * size_t(_n) + size_t(_n) * _sampleSize;
    }

    uint8_t sampleCount() const { return _n; }

private:
    std::span<std::byte> _out;
    uint32_t _tBase;
    size_t _sampleSize;
    bool _valid = false;
    uint8_t _n = 0;
    std::array<uint16_t, limits::bundle_max_samples> _tOffs{};
};

// Read-only, validating view over a decoded STREAM bundle payload.
class BundleView {
public:
    BundleView() = default;  // default-constructible: required by Result<BundleView, E>'s error arm

    // Validates and parses `in` as a bundle whose samples are `sampleSize`
    // bytes each. Rejects (never crashes) on: a header that doesn't fit, an
    // n over the cap, a span over the cap, or a total size that doesn't fit
    // `in` — the last case covers a bundle deliberately or accidentally
    // truncated in flight (§9.2: STREAM data is loss-tolerant BECAUSE bad
    // bundles are detected and dropped, not acted on half-parsed).
    static Result<BundleView, DecodeError> parse(std::span<const std::byte> in, size_t sampleSize) {
        using Ret = Result<BundleView, DecodeError>;
        if (in.size() < kStreamBundleHeaderBytes) return Ret::err(DecodeError::Truncated);

        const uint32_t tBase = getU32(in.subspan(0, 4));
        const uint8_t n = getU8(in.subspan(4, 1));
        // byte 5 (reserved) is intentionally never read: §4.3 tolerance — a
        // future nonzero use of it must not break this decoder.

        if (n > limits::bundle_max_samples) return Ret::err(DecodeError::Malformed);

        const size_t tOffEnd = kStreamBundleHeaderBytes + kStreamBundleTOffBytes * size_t(n);
        if (in.size() < tOffEnd) return Ret::err(DecodeError::Truncated);

        uint16_t lastTOff = 0;
        for (uint8_t i = 0; i < n; ++i) {
            lastTOff = getU16(in.subspan(kStreamBundleHeaderBytes + kStreamBundleTOffBytes * size_t(i), 2));
        }
        if (n > 0 && uint32_t(lastTOff) > limits::bundle_max_span_ms * 1000u) {
            return Ret::err(DecodeError::Malformed);
        }

        const size_t total = tOffEnd + size_t(n) * sampleSize;
        if (in.size() < total) return Ret::err(DecodeError::Truncated);

        return Ret::ok(BundleView(in, tBase, n, sampleSize));
    }

    uint32_t tBase() const { return _tBase; }
    uint8_t sampleCount() const { return _n; }

    // t_base + t_off[i], wrapping (§7.2). Plain unsigned addition IS the
    // wrap here — deliberately NOT routed through util/serial_arithmetic.hpp:
    // that module compares two already-resolved timestamps; this produces
    // one by adding a small, non-wrapping offset to a base. There is no
    // "which is newer" question to ask.
    uint32_t sampleTimeUs(size_t i) const {
        const uint16_t tOff = getU16(_in.subspan(kStreamBundleHeaderBytes + kStreamBundleTOffBytes * i, 2));
        return _tBase + uint32_t(tOff);
    }

    std::span<const std::byte> sample(size_t i) const {
        const size_t samplesStart = kStreamBundleHeaderBytes + kStreamBundleTOffBytes * size_t(_n);
        return _in.subspan(samplesStart + i * _sampleSize, _sampleSize);
    }

private:
    BundleView(std::span<const std::byte> in, uint32_t tBase, uint8_t n, size_t sampleSize)
        : _in(in), _tBase(tBase), _n(n), _sampleSize(sampleSize) {}

    std::span<const std::byte> _in{};
    uint32_t _tBase = 0;
    uint8_t _n = 0;
    size_t _sampleSize = 0;
};

}  // namespace slopsync
