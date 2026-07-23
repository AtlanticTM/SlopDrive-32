// slopsync-core — Result<T, E>: error handling without exceptions.
// Arduino targets build with -fno-exceptions; this is the library-wide error
// idiom (SPEC-adjacent decision: uniform on every platform, no std::expected).
#pragma once

#include <cstdint>
#include <utility>

namespace slopsync {

template <typename T, typename E>
class Result {
public:
    static Result ok(T v) { return Result(std::move(v)); }
    static Result err(E e) { return Result(Err{e}); }

    bool isOk() const { return _ok; }
    explicit operator bool() const { return _ok; }

    // Precondition: isOk() (callers branch first; no exceptions to throw).
    T&       value() { return _value; }
    const T& value() const { return _value; }
    E        error() const { return _error; }

    T valueOr(T fallback) const { return _ok ? _value : std::move(fallback); }

private:
    struct Err { E e; };
    explicit Result(T v) : _ok(true), _value(std::move(v)), _error{} {}
    explicit Result(Err e) : _ok(false), _value{}, _error(e.e) {}

    bool _ok;
    T    _value;
    E    _error;
};

// Errors shared by every decoder in wire/ (encoders can't fail: output-span
// too small is the only encode failure and is reported by a 0 return).
enum class DecodeError : uint8_t {
    Truncated,          // input ends before the structure does
    Malformed,          // structurally invalid (bad type byte, bad lengths)
    ProfileViolation,   // valid CBOR, but outside the deterministic profile (§5.3)
    DepthExceeded,      // nesting > 4 (§5.3)
    BadCrc,             // ESTOP candidate failed CRC-32 (§5.5)
    UnknownVersion,     // structure version this decoder cannot interpret
    CapacityExceeded,   // decoded content exceeds a fixed capacity of the caller
};

}  // namespace slopsync
