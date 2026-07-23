// slopsync-core — CborWriter: the SPEC §5.3 deterministic CBOR profile,
// encode direction. This is the ONLY CBOR this library ever writes:
//   - definite-length everything (no indefinite strings/arrays/maps)
//   - integers in shortest form; map keys sorted ascending by encoded bytes
//   - floats as binary32 only, never binary16/64
//   - no tags, no bignums, no simple values other than false/true/null
//   - maximum nesting depth 4
// Rationale for all of the above: §5.3. Exactly one valid encoding exists
// for any message — that's what makes the golden vectors byte-exact and
// canned templates (constrained clients, §8.5) legal.
//
// NOTE the byte-order split: CBOR heads/lengths/float payloads are
// BIG-endian per RFC 8949 — this file never touches util/byte_io.hpp (that's
// the *frame* layer's little-endian rule, SPEC §1.4). Every multi-byte value
// here is written by hand, most-significant byte first.
//
// Cursor style: no heap, no exceptions. `out` is sized once by the caller;
// every write checks remaining space and sets a sticky failure flag on
// overflow (size() then reports 0, matching the "encoders can't fail except
// by returning 0" idiom in core/result.hpp). Callers emit map keys in
// ascending order themselves (§5.3) — the writer does NOT sort; it only
// catches the mistake (last-key-per-open-map tracking) and fails loudly
// instead of silently producing non-deterministic bytes.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

class CborWriter {
public:
    // §5.3: "maximum nesting depth 4". The top-level map is depth 1; a 5th
    // nested container (map or array) is refused.
    static constexpr uint32_t kMaxDepth = 4;

    explicit CborWriter(std::span<std::byte> out) : _out(out) {}

    // Bytes written so far, or 0 if any write has failed (overflow or an
    // out-of-order key) — callers never have to check failed() separately
    // before trusting size() as the "how much did I write" answer.
    size_t size() const { return _failed ? 0 : _pos; }
    bool failed() const { return _failed; }

    // ---- Containers -------------------------------------------------------
    // Definite-length only: the count is written up front and there is no
    // closing marker. The writer tracks how many key/value (map) or element
    // (array) slots remain at each open depth and pops automatically the
    // moment the count is satisfied — including when a nested container
    // itself *is* the last slot of its parent. Callers never call an
    // "end" method; the counts already fully describe the structure.
    CborWriter& mapHeader(uint32_t nPairs) {
        openContainer(/*isMap=*/true, uint64_t(nPairs) * 2);
        writeHead(5, nPairs);
        return *this;
    }
    CborWriter& arrayHeader(uint32_t n) {
        openContainer(/*isMap=*/false, n);
        writeHead(4, n);
        return *this;
    }

    // ---- Map keys -----------------------------------------------------
    // A registry CborKey, or (for provisional/local sub-key spaces not yet
    // in registry.yaml — see wire/messages/welcome.hpp) a raw key number.
    // Both funnel through the same sorted-ascending / duplicate check.
    CborWriter& key(CborKey k) {
        return keyRaw(uint64_t(static_cast<std::underlying_type_t<CborKey>>(k)));
    }
    CborWriter& key(uint64_t rawKey) { return keyRaw(rawKey); }

    // ---- Scalars ------------------------------------------------------
    CborWriter& uintVal(uint64_t v) {
        writeHead(0, v);
        consumeOne();
        return *this;
    }
    CborWriter& intVal(int64_t v) {
        if (v >= 0) writeHead(0, uint64_t(v));
        else        writeHead(1, uint64_t(-1 - v));  // CBOR negint: -1-n
        consumeOne();
        return *this;
    }
    CborWriter& boolVal(bool v) {
        writeSimple(v ? 21 : 20);
        consumeOne();
        return *this;
    }
    CborWriter& nullVal() {
        writeSimple(22);
        consumeOne();
        return *this;
    }
    // major 7, additional info 26 (0xFA) + 4 BIG-ENDIAN payload bytes —
    // the float *bit pattern* is never shortest-form-checked (only lengths
    // and integer values are); any 32-bit pattern is a valid binary32.
    CborWriter& f32Val(float v) {
        if (!ensure(5)) return *this;
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        _out[_pos] = std::byte{0xFA};
        _out[_pos + 1] = std::byte(uint8_t(bits >> 24));
        _out[_pos + 2] = std::byte(uint8_t(bits >> 16));
        _out[_pos + 3] = std::byte(uint8_t(bits >> 8));
        _out[_pos + 4] = std::byte(uint8_t(bits));
        _pos += 5;
        consumeOne();
        return *this;
    }
    CborWriter& tstrVal(std::string_view s) {
        writeHead(3, s.size());
        if (_failed || !ensure(s.size())) { _failed = true; return *this; }
        std::memcpy(_out.data() + _pos, s.data(), s.size());
        _pos += s.size();
        consumeOne();
        return *this;
    }
    CborWriter& bstrVal(std::span<const std::byte> b) {
        writeHead(2, b.size());
        if (_failed || !ensure(b.size())) { _failed = true; return *this; }
        std::memcpy(_out.data() + _pos, b.data(), b.size());
        _pos += b.size();
        consumeOne();
        return *this;
    }

private:
    struct Frame {
        bool isMap;
        uint64_t remaining;   // slots left at this depth: 2*nPairs (map), n (array)
        bool hasLastKey = false;
        uint64_t lastKey = 0;
    };

    std::span<std::byte> _out;
    size_t _pos = 0;
    bool _failed = false;
    Frame _stack[kMaxDepth]{};
    uint32_t _depth = 0;  // 0 == nothing open yet

    bool ensure(size_t n) {
        if (_failed) return false;
        if (_pos + n > _out.size()) { _failed = true; return false; }
        return true;
    }

    // Shortest-form unsigned head/argument, majors 0–5 (§5.3: "integers in
    // shortest form"). Also used for map/array counts and bstr/tstr lengths —
    // CBOR encodes all four the same way, just with a different major.
    void writeHead(uint8_t major, uint64_t v) {
        uint8_t ib0 = uint8_t(major << 5);
        if (v <= 23) {
            if (!ensure(1)) return;
            _out[_pos] = std::byte(ib0 | uint8_t(v));
            _pos += 1;
        } else if (v <= 0xFFull) {
            if (!ensure(2)) return;
            _out[_pos] = std::byte(ib0 | 24);
            _out[_pos + 1] = std::byte(uint8_t(v));
            _pos += 2;
        } else if (v <= 0xFFFFull) {
            if (!ensure(3)) return;
            _out[_pos] = std::byte(ib0 | 25);
            _out[_pos + 1] = std::byte(uint8_t(v >> 8));
            _out[_pos + 2] = std::byte(uint8_t(v));
            _pos += 3;
        } else if (v <= 0xFFFFFFFFull) {
            if (!ensure(5)) return;
            _out[_pos] = std::byte(ib0 | 26);
            for (int i = 0; i < 4; ++i)
                _out[_pos + 1 + size_t(i)] = std::byte(uint8_t(v >> (24 - 8 * i)));
            _pos += 5;
        } else {
            if (!ensure(9)) return;
            _out[_pos] = std::byte(ib0 | 27);
            for (int i = 0; i < 8; ++i)
                _out[_pos + 1 + size_t(i)] = std::byte(uint8_t(v >> (56 - 8 * i)));
            _pos += 9;
        }
    }
    void writeSimple(uint8_t additionalInfo) {
        if (!ensure(1)) return;
        _out[_pos] = std::byte(uint8_t(7 << 5) | additionalInfo);
        _pos += 1;
    }

    CborWriter& keyRaw(uint64_t kv) {
        if (_failed) return *this;
        if (_depth == 0 || !_stack[_depth - 1].isMap) { _failed = true; return *this; }
        Frame& f = _stack[_depth - 1];
        if (f.hasLastKey && kv <= f.lastKey) { _failed = true; return *this; }  // §5.3 sorted ascending
        f.hasLastKey = true;
        f.lastKey = kv;
        writeHead(0, kv);
        consumeOne();
        return *this;
    }

    // Opens a nested container. Deliberately does NOT consume a slot of the
    // enclosing frame here — that happens later, automatically, the moment
    // this new frame's own remaining count reaches 0 and it pops (see
    // afterConsume). Doing it at open time instead would mis-attribute the
    // "this container is one value of its parent" bookkeeping to the wrong
    // point in time whenever the container is its parent's *last* slot,
    // silently collapsing a level of depth tracking.
    void openContainer(bool isMap, uint64_t remaining) {
        if (_failed) return;
        if (_depth + 1 > kMaxDepth) { _failed = true; return; }
        _stack[_depth] = Frame{isMap, remaining, false, 0};
        _depth += 1;
        afterConsume();  // handles the zero-length (empty map/array) case immediately
    }

    // One slot of the current top-of-stack frame was just fully written.
    // Pops the frame if that was its last slot, cascading upward: finishing
    // a nested container is itself one slot of ITS parent.
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
