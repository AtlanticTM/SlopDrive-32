// slopsync-core — runtime catalog-driven packed field codec, SPEC §8.2/§5.4.
//
// A catalog `layout` is an ordered list of fields, each wire = physical *
// scale (rounded to nearest for integer types; f32 fields pass through
// unrounded), packed little-endian back-to-back per channel/catalog.hpp's
// LayoutField/CatalogEntry (layoutWireSize() gives the total byte count).
//
// IMPORTANT — this file is NOT how production firmware channels encode.
// Production STATE/STREAM channels use compile-time C++ structs (a plain
// `struct { uint16_t pos_10um; ... }` written straight into the frame with
// memcpy/individual field stores) because that's the zero-cost hot path at
// 240-333 Hz the spec's whole packed-struct design exists for (§8.2,
// Appendix H "why hybrid CBOR + packed structs"). The codec below walks a
// runtime `CatalogEntry` field-by-field instead, at the cost of a branch and
// a float divide/multiply per field — appropriate for conformance tooling
// (byte-exact vector generation/checking against ANY catalog, not just the
// ones compiled in) and constrained clients that would rather carry one
// generic interpreter than N compile-time layouts. D-01 (test_main.cpp)
// exercises it against the frozen mini-catalog for exactly this reason.
#pragma once

#include <cmath>
#include <cstdint>
#include <span>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/core/result.hpp"
#include "slopsync/util/byte_io.hpp"

namespace slopsync {

// Packs one physical value into `out` per `field`'s type/scale. Returns
// field.wireSize() (== bytes written), or 0 if `out` is too small.
//
// bitfield8 is the one exception to "wire = physical*scale": SPEC §9.1
// describes bitfield8 as a raw flag byte, not a scaled quantity, so it
// passes `physical` straight through as an integer bit pattern (scale is
// ignored, matching the catalog convention of leaving bitfield8's scale at
// its 1.0 default).
inline size_t packField(const LayoutField& field, float physical, std::span<std::byte> out) {
    const size_t n = field.wireSize();
    if (out.size() < n) return 0;

    switch (field.type) {
        case PackedFieldType::bitfield8:
            return putU8(out, uint8_t(std::lround(physical)));
        case PackedFieldType::u8:
            return putU8(out, uint8_t(std::lround(double(physical) * double(field.scale))));
        case PackedFieldType::i8:
            return putU8(out, uint8_t(int8_t(std::lround(double(physical) * double(field.scale)))));
        case PackedFieldType::u16:
            return putU16(out, uint16_t(std::lround(double(physical) * double(field.scale))));
        case PackedFieldType::i16:
            return putU16(out, uint16_t(int16_t(std::lround(double(physical) * double(field.scale)))));
        case PackedFieldType::u32:
            return putU32(out, uint32_t(std::llround(double(physical) * double(field.scale))));
        case PackedFieldType::i32:
            return putU32(out, uint32_t(int32_t(std::llround(double(physical) * double(field.scale)))));
        case PackedFieldType::f32:
            return putF32(out, physical * field.scale);
    }
    return 0;
}

// Reads one physical value from `in` per `field`'s type/scale. Precondition
// (matching util/byte_io.hpp's convention): in.size() >= field.wireSize() —
// callers (decodeByLayout below) bound-check before slicing; this never
// sees short input.
inline float readField(const LayoutField& field, std::span<const std::byte> in) {
    switch (field.type) {
        case PackedFieldType::bitfield8:
            return float(getU8(in));
        case PackedFieldType::u8:
            return float(getU8(in)) / field.scale;
        case PackedFieldType::i8:
            return float(int8_t(getU8(in))) / field.scale;
        case PackedFieldType::u16:
            return float(getU16(in)) / field.scale;
        case PackedFieldType::i16:
            return float(int16_t(getU16(in))) / field.scale;
        case PackedFieldType::u32:
            return float(getU32(in)) / field.scale;
        case PackedFieldType::i32:
            return float(int32_t(getU32(in))) / field.scale;
        case PackedFieldType::f32:
            return getF32(in) / field.scale;
    }
    return 0.0f;
}

// Encodes every field of a layout-class (STATE/STREAM) `entry` in wire
// order. `physicalValues[i]` supplies field i's physical value. Returns the
// total bytes written (== entry.layoutWireSize()), or 0 on any failure:
// `entry` isn't a layout class, `physicalValues` is shorter than
// entry.fieldCount, or `out` is too small.
inline size_t encodeByLayout(const CatalogEntry& entry, std::span<const float> physicalValues,
                              std::span<std::byte> out) {
    if (!entry.usesLayout()) return 0;
    if (physicalValues.size() < entry.fieldCount) return 0;
    if (out.size() < entry.layoutWireSize()) return 0;

    size_t offset = 0;
    for (uint8_t i = 0; i < entry.fieldCount; ++i) {
        const LayoutField& f = entry.layout[i];
        const size_t n = packField(f, physicalValues[i], out.subspan(offset, f.wireSize()));
        if (n == 0) return 0;
        offset += n;
    }
    return offset;
}

// Decodes `entry`'s fields from `in` into `outPhysical`, in wire order.
// Returns bytes consumed (== entry.layoutWireSize() on success) or a
// DecodeError: CapacityExceeded if `outPhysical` can't hold entry.fieldCount
// values, Truncated if `in` runs out before `entry`'s known fields do.
//
// Append-only note (SPEC §5.4): `in` is allowed to be LONGER than
// entry.layoutWireSize() — this function only ever reads entry.fieldCount
// fields' worth of bytes and never looks past them, so calling it with an
// older/shorter `entry` against a payload a newer hub grew by appending
// fields IS the append-only prefix-parse rule; see
// channel/state_apply.hpp's appendOnlyRead() for the named, documented
// entry point callers should reach for when that's the intent.
inline Result<size_t, DecodeError> decodeByLayout(const CatalogEntry& entry, std::span<const std::byte> in,
                                                   std::span<float> outPhysical) {
    using Ret = Result<size_t, DecodeError>;
    if (!entry.usesLayout()) return Ret::err(DecodeError::Malformed);
    if (outPhysical.size() < entry.fieldCount) return Ret::err(DecodeError::CapacityExceeded);

    size_t offset = 0;
    for (uint8_t i = 0; i < entry.fieldCount; ++i) {
        const LayoutField& f = entry.layout[i];
        const size_t n = f.wireSize();
        if (offset + n > in.size()) return Ret::err(DecodeError::Truncated);
        outPhysical[i] = readField(f, in.subspan(offset, n));
        offset += n;
    }
    return Ret::ok(offset);
}

}  // namespace slopsync
