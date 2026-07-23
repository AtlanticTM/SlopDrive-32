// slopsync-core — CborReader: the SPEC §5.3 deterministic CBOR profile,
// decode direction. Always strict: this reader is the one place the profile
// is *enforced*, not just produced. It rejects everything §5.3 excludes —
//   - indefinite lengths (maps/arrays/strings)
//   - non-shortest-form integer/length heads
//   - float16 and float64 (binary32 only)
//   - tags
//   - simple values other than false/true/null
//   - unsorted or duplicate map keys
//   - nesting deeper than 4
//   - truncated input
// with DecodeError::ProfileViolation, DepthExceeded, Malformed, or Truncated
// as appropriate (core/result.hpp). Unknown *keys* are NOT a profile
// violation — SPEC §4.3 requires them to be silently skipped; that is what
// skipValue() is for, and callers dispatching on readKey() are expected to
// call it for anything they don't recognize.
//
// Byte order note: like the writer, this file hand-decodes CBOR heads and
// float payloads BIG-endian (RFC 8949) — it never touches
// util/byte_io.hpp's little-endian helpers (that's the frame layer, §1.4).
//
// No heap, no exceptions, no RTTI. `readTstr`/`readBstr` return views into
// the caller's input span — valid only as long as that span is.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "slopsync/core/result.hpp"

namespace slopsync {

class CborReader {
public:
    // Mirrors CborWriter::kMaxDepth — the top-level map is depth 1.
    static constexpr uint32_t kMaxDepth = 4;

    enum class MajorType : uint8_t {
        UInt, NegInt, ByteString, TextString, Array, Map, Tag, Simple
    };

    explicit CborReader(std::span<const std::byte> in) : _in(in) {}

    // Inspects the next item's major type without consuming anything or
    // touching the container-frame bookkeeping. Used by skipValue() (and
    // available to callers) to decide how to dispatch. Major 6 (Tag) is
    // reported as its own type here even though every consuming read
    // rejects it outright (§5.3 forbids tags) — peeking is not consuming.
    Result<MajorType, DecodeError> peekType() const {
        if (_pos >= _in.size()) return Result<MajorType, DecodeError>::err(DecodeError::Truncated);
        switch (uint8_t(_in[_pos]) >> 5) {
            case 0: return Result<MajorType, DecodeError>::ok(MajorType::UInt);
            case 1: return Result<MajorType, DecodeError>::ok(MajorType::NegInt);
            case 2: return Result<MajorType, DecodeError>::ok(MajorType::ByteString);
            case 3: return Result<MajorType, DecodeError>::ok(MajorType::TextString);
            case 4: return Result<MajorType, DecodeError>::ok(MajorType::Array);
            case 5: return Result<MajorType, DecodeError>::ok(MajorType::Map);
            case 6: return Result<MajorType, DecodeError>::ok(MajorType::Tag);
            default: return Result<MajorType, DecodeError>::ok(MajorType::Simple);  // major 7
        }
    }

    // ---- Containers ---------------------------------------------------
    // Enters a definite-length map; returns its pair count. Pushes a frame
    // that (a) enforces the nesting-depth ceiling, (b) tracks the last key
    // seen at this level for readKey()'s sorted/duplicate check, and
    // (c) auto-pops — including cascading through parents — the instant its
    // slots are exhausted, exactly like CborWriter. No matching "endMap"
    // call exists or is needed; callers simply read the returned count of
    // key/value pairs and move on.
    Result<uint32_t, DecodeError> readMapHeader() { return enterContainer(5, /*isMap=*/true); }
    Result<uint32_t, DecodeError> readArrayHeader() { return enterContainer(4, /*isMap=*/false); }

    // Reads one map key (an unsigned int per the registry's CborKey space,
    // or a local/provisional sub-key — see wire/messages/welcome.hpp).
    // Enforces §5.3's "sorted ascending, no duplicates" at the CURRENT
    // nesting level: this call is only meaningful right after
    // readMapHeader() (or between a prior pair's fully-consumed value and
    // the next key), which is exactly how decodeHello/decodeWelcome use it.
    Result<uint64_t, DecodeError> readKey() {
        auto hr = parseHeadRaw();
        if (!hr) return Result<uint64_t, DecodeError>::err(hr.error());
        const Head h = hr.value();
        if (h.major != 0) return Result<uint64_t, DecodeError>::err(DecodeError::Malformed);
        if (_depth == 0 || !_stack[_depth - 1].isMap) {
            return Result<uint64_t, DecodeError>::err(DecodeError::Malformed);
        }
        Frame& f = _stack[_depth - 1];
        if (f.hasLastKey && h.arg <= f.lastKey) {
            return Result<uint64_t, DecodeError>::err(DecodeError::ProfileViolation);
        }
        _pos += h.totalBytes;
        f.hasLastKey = true;
        f.lastKey = h.arg;
        consumeOne();
        return Result<uint64_t, DecodeError>::ok(h.arg);
    }

    // ---- Scalars --------------------------------------------------------
    Result<uint64_t, DecodeError> readUint() {
        auto hr = parseHeadRaw();
        if (!hr) return Result<uint64_t, DecodeError>::err(hr.error());
        const Head h = hr.value();
        if (h.major != 0) return Result<uint64_t, DecodeError>::err(DecodeError::Malformed);
        _pos += h.totalBytes;
        consumeOne();
        return Result<uint64_t, DecodeError>::ok(h.arg);
    }
    Result<int64_t, DecodeError> readInt() {
        auto hr = parseHeadRaw();
        if (!hr) return Result<int64_t, DecodeError>::err(hr.error());
        const Head h = hr.value();
        if (h.major == 0) {
            _pos += h.totalBytes;
            consumeOne();
            return Result<int64_t, DecodeError>::ok(int64_t(h.arg));
        }
        if (h.major == 1) {
            _pos += h.totalBytes;
            consumeOne();
            return Result<int64_t, DecodeError>::ok(-1 - int64_t(h.arg));
        }
        return Result<int64_t, DecodeError>::err(DecodeError::Malformed);
    }
    Result<bool, DecodeError> readBool() {
        auto sr = parseSimpleRaw();
        if (!sr) return Result<bool, DecodeError>::err(sr.error());
        const SimpleHead s = sr.value();
        if (s.kind != SimpleKind::False && s.kind != SimpleKind::True) {
            return Result<bool, DecodeError>::err(DecodeError::Malformed);
        }
        _pos += s.totalBytes;
        consumeOne();
        return Result<bool, DecodeError>::ok(s.kind == SimpleKind::True);
    }
    // Not in the strict brief list but symmetric with CborWriter::nullVal()
    // and free to provide: consumes a `null` and reports success.
    Result<bool, DecodeError> readNull() {
        auto sr = parseSimpleRaw();
        if (!sr) return Result<bool, DecodeError>::err(sr.error());
        if (sr.value().kind != SimpleKind::Null) return Result<bool, DecodeError>::err(DecodeError::Malformed);
        _pos += sr.value().totalBytes;
        consumeOne();
        return Result<bool, DecodeError>::ok(true);
    }
    Result<float, DecodeError> readF32() {
        auto sr = parseSimpleRaw();
        if (!sr) return Result<float, DecodeError>::err(sr.error());
        const SimpleHead s = sr.value();
        if (s.kind != SimpleKind::Float32) return Result<float, DecodeError>::err(DecodeError::Malformed);
        _pos += s.totalBytes;
        consumeOne();
        uint32_t bits = s.floatBits;
        float v;
        std::memcpy(&v, &bits, 4);
        return Result<float, DecodeError>::ok(v);
    }
    // Zero-copy: the returned view aliases `in` (the reader's input span).
    Result<std::string_view, DecodeError> readTstr() {
        auto hr = parseHeadRaw();
        if (!hr) return Result<std::string_view, DecodeError>::err(hr.error());
        const Head h = hr.value();
        if (h.major != 3) return Result<std::string_view, DecodeError>::err(DecodeError::Malformed);
        size_t start = _pos + h.totalBytes;
        size_t len = size_t(h.arg);
        if (start + len > _in.size()) return Result<std::string_view, DecodeError>::err(DecodeError::Truncated);
        _pos = start + len;
        consumeOne();
        return Result<std::string_view, DecodeError>::ok(
            std::string_view(reinterpret_cast<const char*>(_in.data() + start), len));
    }
    Result<std::span<const std::byte>, DecodeError> readBstr() {
        auto hr = parseHeadRaw();
        if (!hr) return Result<std::span<const std::byte>, DecodeError>::err(hr.error());
        const Head h = hr.value();
        if (h.major != 2) return Result<std::span<const std::byte>, DecodeError>::err(DecodeError::Malformed);
        size_t start = _pos + h.totalBytes;
        size_t len = size_t(h.arg);
        if (start + len > _in.size()) return Result<std::span<const std::byte>, DecodeError>::err(DecodeError::Truncated);
        _pos = start + len;
        consumeOne();
        return Result<std::span<const std::byte>, DecodeError>::ok(_in.subspan(start, len));
    }

    // Consumes exactly one value of whatever type follows, discarding it —
    // for unknown map keys (§4.3) and unknown array elements. Recurses into
    // nested containers (bounded by the same depth ceiling as everything
    // else) so a skipped value may itself be an arbitrarily-shaped (but
    // still profile-conformant) map or array.
    Result<bool, DecodeError> skipValue() {
        auto t = peekType();
        if (!t) return Result<bool, DecodeError>::err(t.error());
        switch (t.value()) {
            case MajorType::UInt: {
                auto r = readUint();
                if (!r) return Result<bool, DecodeError>::err(r.error());
                return Result<bool, DecodeError>::ok(true);
            }
            case MajorType::NegInt: {
                auto r = readInt();
                if (!r) return Result<bool, DecodeError>::err(r.error());
                return Result<bool, DecodeError>::ok(true);
            }
            case MajorType::ByteString: {
                auto r = readBstr();
                if (!r) return Result<bool, DecodeError>::err(r.error());
                return Result<bool, DecodeError>::ok(true);
            }
            case MajorType::TextString: {
                auto r = readTstr();
                if (!r) return Result<bool, DecodeError>::err(r.error());
                return Result<bool, DecodeError>::ok(true);
            }
            case MajorType::Tag:
                return Result<bool, DecodeError>::err(DecodeError::ProfileViolation);  // §5.3: no tags
            case MajorType::Array: {
                auto n = readArrayHeader();
                if (!n) return Result<bool, DecodeError>::err(n.error());
                for (uint32_t i = 0; i < n.value(); ++i) {
                    auto s = skipValue();
                    if (!s) return s;
                }
                return Result<bool, DecodeError>::ok(true);
            }
            case MajorType::Map: {
                auto n = readMapHeader();
                if (!n) return Result<bool, DecodeError>::err(n.error());
                for (uint32_t i = 0; i < n.value(); ++i) {
                    auto k = readKey();
                    if (!k) return Result<bool, DecodeError>::err(k.error());
                    auto v = skipValue();
                    if (!v) return v;
                }
                return Result<bool, DecodeError>::ok(true);
            }
            case MajorType::Simple: {
                auto sr = parseSimpleRaw();
                if (!sr) return Result<bool, DecodeError>::err(sr.error());
                _pos += sr.value().totalBytes;
                consumeOne();
                return Result<bool, DecodeError>::ok(true);
            }
        }
        return Result<bool, DecodeError>::err(DecodeError::Malformed);  // unreachable
    }

private:
    struct Frame {
        bool isMap;
        uint64_t remaining;
        bool hasLastKey = false;
        uint64_t lastKey = 0;
    };
    struct Head { uint8_t major; uint64_t arg; size_t totalBytes; };
    enum class SimpleKind : uint8_t { False, True, Null, Float32 };
    struct SimpleHead { SimpleKind kind; uint32_t floatBits; size_t totalBytes; };

    std::span<const std::byte> _in;
    size_t _pos = 0;
    Frame _stack[kMaxDepth]{};
    uint32_t _depth = 0;

    // Shortest-form head parser for majors 0–5 (uint, negint, bstr/tstr
    // length, array/map count). Pure: does not advance _pos — every caller
    // validates its expected major first, then advances by totalBytes
    // itself. Major 6 (tag) and major 7 (simple/float) are handled by their
    // own dedicated logic below/above since their "argument" byte doesn't
    // mean the same thing (major 7's additional-info selects a *kind*, not
    // a length or value to shortest-form-check).
    Result<Head, DecodeError> parseHeadRaw() const {
        if (_pos >= _in.size()) return Result<Head, DecodeError>::err(DecodeError::Truncated);
        uint8_t ib = uint8_t(_in[_pos]);
        uint8_t major = ib >> 5;
        uint8_t ai = ib & 0x1F;
        if (major == 6) return Result<Head, DecodeError>::err(DecodeError::ProfileViolation);  // no tags
        if (major == 7) return Result<Head, DecodeError>::err(DecodeError::Malformed);  // use parseSimpleRaw()
        if (ai <= 23) return Result<Head, DecodeError>::ok(Head{major, ai, 1});
        size_t nbytes = (ai == 24) ? 1 : (ai == 25) ? 2 : (ai == 26) ? 4 : (ai == 27) ? 8 : 0;
        if (nbytes == 0) {
            // ai == 31 is the indefinite-length marker; 28-30 are reserved
            // and never valid CBOR at all.
            return Result<Head, DecodeError>::err(ai == 31 ? DecodeError::ProfileViolation : DecodeError::Malformed);
        }
        if (_pos + 1 + nbytes > _in.size()) return Result<Head, DecodeError>::err(DecodeError::Truncated);
        uint64_t v = 0;
        for (size_t i = 0; i < nbytes; ++i) v = (v << 8) | uint64_t(uint8_t(_in[_pos + 1 + i]));
        uint64_t minForSize = (nbytes == 1) ? 24ull : (nbytes == 2) ? 256ull : (nbytes == 4) ? 65536ull : 4294967296ull;
        if (v < minForSize) return Result<Head, DecodeError>::err(DecodeError::ProfileViolation);  // non-shortest
        return Result<Head, DecodeError>::ok(Head{major, v, 1 + nbytes});
    }

    // major 7 only: false(20)/true(21)/null(22)/float32(26) are the entire
    // legal set under the profile; everything else (undefined, extended
    // simple, float16, float64, reserved, the indefinite "break" byte) is
    // rejected here, at the single place that ever interprets major 7.
    Result<SimpleHead, DecodeError> parseSimpleRaw() const {
        if (_pos >= _in.size()) return Result<SimpleHead, DecodeError>::err(DecodeError::Truncated);
        uint8_t ib = uint8_t(_in[_pos]);
        if ((ib >> 5) != 7) return Result<SimpleHead, DecodeError>::err(DecodeError::Malformed);
        uint8_t ai = ib & 0x1F;
        switch (ai) {
            case 20: return Result<SimpleHead, DecodeError>::ok(SimpleHead{SimpleKind::False, 0, 1});
            case 21: return Result<SimpleHead, DecodeError>::ok(SimpleHead{SimpleKind::True, 0, 1});
            case 22: return Result<SimpleHead, DecodeError>::ok(SimpleHead{SimpleKind::Null, 0, 1});
            case 26: {
                if (_pos + 5 > _in.size()) return Result<SimpleHead, DecodeError>::err(DecodeError::Truncated);
                uint32_t bits = 0;
                for (int i = 0; i < 4; ++i) bits = (bits << 8) | uint32_t(uint8_t(_in[_pos + 1 + size_t(i)]));
                return Result<SimpleHead, DecodeError>::ok(SimpleHead{SimpleKind::Float32, bits, 5});
            }
            case 28: case 29: case 30:
                return Result<SimpleHead, DecodeError>::err(DecodeError::Malformed);  // reserved, never valid CBOR
            default:
                // 23 (undefined), 24 (1-byte simple ext), 25 (float16),
                // 27 (float64), 31 (break) — valid general CBOR, all
                // forbidden by §5.3.
                return Result<SimpleHead, DecodeError>::err(DecodeError::ProfileViolation);
        }
    }

    Result<uint32_t, DecodeError> enterContainer(uint8_t expectedMajor, bool isMap) {
        auto hr = parseHeadRaw();
        if (!hr) return Result<uint32_t, DecodeError>::err(hr.error());
        const Head h = hr.value();
        if (h.major != expectedMajor) return Result<uint32_t, DecodeError>::err(DecodeError::Malformed);
        if (_depth + 1 > kMaxDepth) return Result<uint32_t, DecodeError>::err(DecodeError::DepthExceeded);
        if (h.arg > 0xFFFFFFFFull) return Result<uint32_t, DecodeError>::err(DecodeError::Malformed);
        _pos += h.totalBytes;
        _stack[_depth] = Frame{isMap, isMap ? uint64_t(h.arg) * 2 : uint64_t(h.arg), false, 0};
        _depth += 1;
        afterConsume();  // immediately pops a zero-length (empty) container
        return Result<uint32_t, DecodeError>::ok(uint32_t(h.arg));
    }

    // Mirrors CborWriter's identical bookkeeping — see its comments.
    void consumeOne() {
        if (_depth == 0) return;
        if (_stack[_depth - 1].remaining > 0) _stack[_depth - 1].remaining -= 1;
        afterConsume();
    }
    void afterConsume() {
        while (_depth > 0 && _stack[_depth - 1].remaining == 0) {
            _depth -= 1;
            if (_depth > 0 && _stack[_depth - 1].remaining > 0) {
                _stack[_depth - 1].remaining -= 1;
            }
        }
    }
};

}  // namespace slopsync
