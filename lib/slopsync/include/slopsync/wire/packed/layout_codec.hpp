// slopsync-core — runtime catalog-driven packed field codec, SPEC §8.2/§5.4.
//
// A catalog `layout` is an ordered list of fields, each wire = physical *
// scale (rounded to nearest for integer types; f32 fields pass through
// unrounded), packed little-endian back-to-back per channel/catalog.hpp's
// LayoutField (layoutWireSize() gives the total byte count). Fields live in
// the owning catalog's layout pool, so the primitives here take a
// std::span<const LayoutField> — get one from cat.layoutFields(entry), or
// use the (catalog, entry) overloads at the bottom of this file.
//
// IMPORTANT — this file is NOT how production firmware channels encode.
// Production STATE/STREAM channels use compile-time C++ structs (a plain
// `struct { uint16_t pos_10um; ... }` written straight into the frame with
// memcpy/individual field stores) because that's the zero-cost hot path at
// 240-333 Hz the spec's whole packed-struct design exists for (§8.2,
// Appendix H "why hybrid CBOR + packed structs"). The codec below walks a
// runtime field list field-by-field instead, at the cost of a branch and
// a float divide/multiply per field — appropriate for conformance tooling
// (byte-exact vector generation/checking against ANY catalog, not just the
// ones compiled in) and constrained clients that would rather carry one
// generic interpreter than N compile-time layouts. D-01 (test_main.cpp)
// exercises it against the frozen mini-catalog for exactly this reason.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

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
//
// The str16/str32/str64 types (RFC-026) carry TEXT, which a float cannot
// describe: this numeric entry point writes the field's full width as NULs
// (the encoding of the empty string) and reports the correct width, so a
// numeric caller can never desynchronise the offsets of the fields AFTER a
// string field. Use packStringField() to actually put text in one.
//
// The switch is EXHAUSTIVE with no default arm — a new PackedFieldType is a
// -Wswitch compile error here, never a silent fallthrough.
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
        case PackedFieldType::str16:
        case PackedFieldType::str32:
        case PackedFieldType::str64:
            std::memset(out.data(), 0, n);  // no numeric text: the empty string
            return n;
    }
    return 0;
}

// Writes `text` into a fixed-width NUL-padded UTF-8 string field (RFC-026).
// Text longer than the field truncates to its width (no NUL terminator is
// required when the string exactly fills the field); shorter text is
// zero-padded to the full width so the following field's offset is fixed.
// Returns bytes written (== field.wireSize()), or 0 if `field` is not a
// string type or `out` is too small.
//
// Truncation is BYTE-wise: a caller splitting a multi-byte UTF-8 sequence is
// authoring a name too long for the width it chose (the registry's own note
// on session_roster says as much) — this codec does not silently reflow it.
inline size_t packStringField(const LayoutField& field, std::string_view text,
                              std::span<std::byte> out) {
    if (!field.isString()) return 0;
    const size_t n = field.wireSize();
    if (out.size() < n) return 0;
    const size_t copied = text.size() < n ? text.size() : n;
    if (copied > 0) std::memcpy(out.data(), text.data(), copied);
    if (copied < n) std::memset(out.data() + copied, 0, n - copied);
    return n;
}

// Reads one physical value from `in` per `field`'s type/scale. Precondition
// (matching util/byte_io.hpp's convention): in.size() >= field.wireSize() —
// callers (decodeByLayout below) bound-check before slicing; this never
// sees short input. String fields have no numeric value and read as 0.0f;
// use readStringField() for their text.
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
        case PackedFieldType::str16:
        case PackedFieldType::str32:
        case PackedFieldType::str64:
            return 0.0f;
    }
    return 0.0f;
}

// Reads a fixed-width NUL-padded string field: the bytes up to the first NUL,
// or the field's full width if there is none. The returned view aliases `in`
// — it is valid exactly as long as the buffer is. Empty view if `field` is
// not a string type or `in` is shorter than the field.
inline std::string_view readStringField(const LayoutField& field, std::span<const std::byte> in) {
    if (!field.isString()) return {};
    const size_t n = field.wireSize();
    if (in.size() < n) return {};
    const char* p = reinterpret_cast<const char*>(in.data());
    size_t len = 0;
    while (len < n && p[len] != '\0') ++len;
    return std::string_view(p, len);
}

// Encodes every field of a layout in wire order. `physicalValues[i]` supplies
// field i's physical value. Returns the total bytes written (==
// layoutWireSize(fields)), or 0 on any failure: `physicalValues` shorter than
// `fields`, or `out` too small.
inline size_t encodeByLayout(std::span<const LayoutField> fields,
                             std::span<const float> physicalValues, std::span<std::byte> out) {
    if (physicalValues.size() < fields.size()) return 0;
    if (out.size() < layoutWireSize(fields)) return 0;

    size_t offset = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        const LayoutField& f = fields[i];
        const size_t n = packField(f, physicalValues[i], out.subspan(offset, f.wireSize()));
        if (n == 0) return 0;
        offset += n;
    }
    return offset;
}

// Decodes `fields` from `in` into `outPhysical`, in wire order. Returns bytes
// consumed (== layoutWireSize(fields) on success) or a DecodeError:
// CapacityExceeded if `outPhysical` can't hold one value per field, Truncated
// if `in` runs out before the known fields do.
//
// Append-only note (SPEC §5.4): `in` is allowed to be LONGER than
// layoutWireSize(fields) — this function only ever reads fields.size()
// fields' worth of bytes and never looks past them, so calling it with an
// older/shorter field list against a payload a newer hub grew by appending
// fields IS the append-only prefix-parse rule; see channel/state_apply.hpp's
// appendOnlyRead() for the named, documented entry point callers should
// reach for when that's the intent.
inline Result<size_t, DecodeError> decodeByLayout(std::span<const LayoutField> fields,
                                                  std::span<const std::byte> in,
                                                  std::span<float> outPhysical) {
    using Ret = Result<size_t, DecodeError>;
    if (outPhysical.size() < fields.size()) return Ret::err(DecodeError::CapacityExceeded);

    size_t offset = 0;
    for (size_t i = 0; i < fields.size(); ++i) {
        const LayoutField& f = fields[i];
        const size_t n = f.wireSize();
        if (offset + n > in.size()) return Ret::err(DecodeError::Truncated);
        outPhysical[i] = readField(f, in.subspan(offset, n));
        offset += n;
    }
    return Ret::ok(offset);
}

// (catalog, entry) conveniences — resolve `entry`'s fields through its owning
// catalog and apply the layout-class check the entry-shaped API used to do
// internally. 0 / Malformed if `entry` isn't a STATE/STREAM entry.
template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline size_t encodeByLayout(const BasicCatalog<E, L, S, B, T>& cat, const CatalogEntry& entry,
                             std::span<const float> physicalValues, std::span<std::byte> out) {
    if (!entry.usesLayout()) return 0;
    return encodeByLayout(cat.layoutFields(entry), physicalValues, out);
}

template <size_t E, size_t L, size_t S, size_t B, size_t T>
inline Result<size_t, DecodeError> decodeByLayout(const BasicCatalog<E, L, S, B, T>& cat,
                                                  const CatalogEntry& entry,
                                                  std::span<const std::byte> in,
                                                  std::span<float> outPhysical) {
    if (!entry.usesLayout()) return Result<size_t, DecodeError>::err(DecodeError::Malformed);
    return decodeByLayout(cat.layoutFields(entry), in, outPhysical);
}

}  // namespace slopsync
