// slopsync-core — INTENT (client -> hub), SPEC §9.3: the only way a client
// changes anything.
//
// CBOR map, keys ascending: channel_id(15), intent_id(18), value(20),
// optional precondition(30), optional takeover(32).
//
// `value` (20) is schema-dependent per the catalog (§9.3: "per the channel's
// schema") — this codec has no catalog in scope, so it represents `value` as
// a nested CBOR map of up to kIntentMaxValueFields (8) {uint8 sub-key ->
// IntentValue}, matching how SchemaField (channel/catalog.hpp) already
// numbers a schema's fields with a small per-channel integer key. ECHO's
// `applied` (19, echo.hpp) and EVENT's optional payload under `value` (20,
// event.hpp) reuse this exact IntentValueField/IntentValue shape — it lives
// here because INTENT is where it's first needed.
//
// IntentValue's `kind` is a WIRE-TYPE discriminator, not an app-level type
// tag: CBOR major 0 (uint) is indistinguishable byte-for-byte from "a
// non-negative value someone chose to call signed" (RFC 8949 draws no such
// distinction). Consequence, worth knowing before you rely on it: encoding
// Kind::I64 with a non-negative value round-trips through the wire as
// Kind::U64 on decode (major 0 either way); Kind::I64 only survives a
// round-trip for genuinely negative values (major 1). This is a property of
// CBOR itself, not a bug in this codec — callers needing a guaranteed-signed
// field on the decode side should either only ever send negative values
// through it, or treat U64/I64 as one "integer" case and read whichever of
// u64_val/i64_val is semantically right for that catalog field.
//
// Field-key ordering inside `value`/`applied`/payload maps: callers MUST
// supply entries already sorted ascending by their uint8 sub-key, same
// discipline as every other map in this profile (§5.3) — the encoder trusts
// the caller-given order and simply writes it; CborWriter::key() is what
// catches an ordering mistake (fails, encode returns 0).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

inline constexpr uint32_t kIntentMaxValueFields = 8;

// One value inside a `value`/`applied`/EVENT-payload map. Exactly one of the
// *_val members is meaningful, selected by `kind` (see file header for the
// U64/I64 wire-collapse caveat).
struct IntentValue {
    enum class Kind : uint8_t { U64, I64, F32, Bool, Tstr, Bstr };

    Kind kind = Kind::U64;
    uint64_t u64_val = 0;
    int64_t i64_val = 0;
    float f32_val = 0.0f;
    bool bool_val = false;
    std::string_view tstr_val;              // Kind::Tstr only; zero-copy view
    std::span<const std::byte> bstr_val;    // Kind::Bstr only; zero-copy view

    static IntentValue ofU64(uint64_t v) { IntentValue iv; iv.kind = Kind::U64; iv.u64_val = v; return iv; }
    static IntentValue ofI64(int64_t v) { IntentValue iv; iv.kind = Kind::I64; iv.i64_val = v; return iv; }
    static IntentValue ofF32(float v) { IntentValue iv; iv.kind = Kind::F32; iv.f32_val = v; return iv; }
    static IntentValue ofBool(bool v) { IntentValue iv; iv.kind = Kind::Bool; iv.bool_val = v; return iv; }
    static IntentValue ofTstr(std::string_view v) { IntentValue iv; iv.kind = Kind::Tstr; iv.tstr_val = v; return iv; }
    static IntentValue ofBstr(std::span<const std::byte> v) { IntentValue iv; iv.kind = Kind::Bstr; iv.bstr_val = v; return iv; }
};

struct IntentValueField {
    uint8_t key = 0;   // the schema's sub-map integer key (catalog SchemaField::key)
    IntentValue value;
};

using IntentValueArray = std::array<IntentValueField, kIntentMaxValueFields>;

// Convenience aggregate for passing a value map through role-layer APIs
// (HubDelegate::applyIntent, Client::sendIntent): count + fields, mirroring
// IntentMsg's own value_count/value pair.
struct IntentValueMap {
    uint32_t count = 0;
    IntentValueArray fields{};
};

// Shared by INTENT's `value`, ECHO's `applied`, and EVENT's optional
// payload: writes one already-decided scalar. Returns `w` for chaining.
inline CborWriter& encodeIntentValue(CborWriter& w, const IntentValue& v) {
    switch (v.kind) {
        case IntentValue::Kind::U64:  return w.uintVal(v.u64_val);
        case IntentValue::Kind::I64:  return w.intVal(v.i64_val);
        case IntentValue::Kind::F32:  return w.f32Val(v.f32_val);
        case IntentValue::Kind::Bool: return w.boolVal(v.bool_val);
        case IntentValue::Kind::Tstr: return w.tstrVal(v.tstr_val);
        case IntentValue::Kind::Bstr: return w.bstrVal(v.bstr_val);
    }
    return w;  // unreachable
}

// Shared decode counterpart: dispatches on the wire's own major type since
// no catalog/schema is in scope here (see file header). Rejects nested
// containers (Array/Map/Tag) as Malformed — those are not legal INTENT
// scalar values in this profile.
inline Result<IntentValue, DecodeError> decodeIntentValue(CborReader& r) {
    using Ret = Result<IntentValue, DecodeError>;

    auto t = r.peekType();
    if (!t) return Ret::err(t.error());

    IntentValue v{};
    switch (t.value()) {
        case CborReader::MajorType::UInt: {
            auto vv = r.readUint();
            if (!vv) return Ret::err(vv.error());
            v.kind = IntentValue::Kind::U64;
            v.u64_val = vv.value();
            return Ret::ok(v);
        }
        case CborReader::MajorType::NegInt: {
            auto vv = r.readInt();
            if (!vv) return Ret::err(vv.error());
            v.kind = IntentValue::Kind::I64;
            v.i64_val = vv.value();
            return Ret::ok(v);
        }
        case CborReader::MajorType::TextString: {
            auto vv = r.readTstr();
            if (!vv) return Ret::err(vv.error());
            v.kind = IntentValue::Kind::Tstr;
            v.tstr_val = vv.value();
            return Ret::ok(v);
        }
        case CborReader::MajorType::ByteString: {
            auto vv = r.readBstr();
            if (!vv) return Ret::err(vv.error());
            v.kind = IntentValue::Kind::Bstr;
            v.bstr_val = vv.value();
            return Ret::ok(v);
        }
        case CborReader::MajorType::Simple: {
            // Bool and F32 are both major 7. readBool() is a pure peek on a
            // mismatch (parseSimpleRaw doesn't advance the cursor unless the
            // kind matches what's asked), so trying it first is safe: it
            // either consumes a real bool, or leaves the cursor untouched
            // for readF32() to consume the float.
            auto b = r.readBool();
            if (b) {
                v.kind = IntentValue::Kind::Bool;
                v.bool_val = b.value();
                return Ret::ok(v);
            }
            auto f = r.readF32();
            if (!f) return Ret::err(f.error());
            v.kind = IntentValue::Kind::F32;
            v.f32_val = f.value();
            return Ret::ok(v);
        }
        case CborReader::MajorType::Array:
        case CborReader::MajorType::Map:
        case CborReader::MajorType::Tag:
            return Ret::err(DecodeError::Malformed);
    }
    return Ret::err(DecodeError::Malformed);  // unreachable
}

// Reads a definite-length map of up to `Capacity` IntentValueFields, entering
// the map header itself (the caller has only just read the KEY that
// introduced this map, e.g. `value`/`applied` — the header read happens in
// here). Shared by INTENT/ECHO/EVENT decoders.
template <size_t Capacity>
inline Result<uint32_t, DecodeError> decodeIntentValueMap(
    CborReader& r, std::array<IntentValueField, Capacity>& out) {
    using Ret = Result<uint32_t, DecodeError>;

    auto cR = r.readMapHeader();
    if (!cR) return Ret::err(cR.error());
    if (cR.value() > Capacity) return Ret::err(DecodeError::CapacityExceeded);

    for (uint32_t i = 0; i < cR.value(); ++i) {
        auto fk = r.readKey();
        if (!fk) return Ret::err(fk.error());
        if (fk.value() > 0xFF) return Ret::err(DecodeError::Malformed);

        auto vR = decodeIntentValue(r);
        if (!vR) return Ret::err(vR.error());

        out[i] = IntentValueField{uint8_t(fk.value()), vR.value()};
    }
    return Ret::ok(cR.value());
}

struct IntentMsg {
    uint16_t channel_id = 0;
    uint16_t intent_id = 0;

    uint32_t value_count = 0;
    IntentValueArray value{};

    bool has_precondition = false;
    uint16_t precondition = 0;   // expected cfg_gen (CAS guard, §9.3)

    bool has_takeover = false;
    bool takeover = false;
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeIntent(const IntentMsg& m, std::span<std::byte> out) {
    if (m.value_count > kIntentMaxValueFields) return 0;

    uint32_t nKeys = 3;  // channel_id, intent_id, value
    if (m.has_precondition) ++nKeys;
    if (m.has_takeover) ++nKeys;

    CborWriter w(out);
    w.mapHeader(nKeys);
    w.key(CborKey::channel_id).uintVal(m.channel_id);
    w.key(CborKey::intent_id).uintVal(m.intent_id);
    w.key(CborKey::value).mapHeader(m.value_count);
    for (uint32_t i = 0; i < m.value_count; ++i) {
        const IntentValueField& f = m.value[i];
        w.key(uint64_t(f.key));
        encodeIntentValue(w, f.value);
    }
    if (m.has_precondition) {
        w.key(CborKey::precondition).uintVal(m.precondition);
    }
    if (m.has_takeover) {
        w.key(CborKey::takeover).boolVal(m.takeover);
    }
    return w.size();
}

// Decodes `in` into an IntentMsg. Unknown keys are skipped per §4.3.
inline Result<IntentMsg, DecodeError> decodeIntent(std::span<const std::byte> in) {
    using Ret = Result<IntentMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    IntentMsg m{};
    bool gotChannel = false, gotIntentId = false, gotValue = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::channel_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.channel_id = uint16_t(v.value());
                gotChannel = true;
                break;
            }
            case uint64_t(CborKey::intent_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.intent_id = uint16_t(v.value());
                gotIntentId = true;
                break;
            }
            case uint64_t(CborKey::value): {
                auto cR = decodeIntentValueMap(r, m.value);
                if (!cR) return Ret::err(cR.error());
                m.value_count = cR.value();
                gotValue = true;
                break;
            }
            case uint64_t(CborKey::precondition): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.precondition = uint16_t(v.value());
                m.has_precondition = true;
                break;
            }
            case uint64_t(CborKey::takeover): {
                auto v = r.readBool();
                if (!v) return Ret::err(v.error());
                m.takeover = v.value();
                m.has_takeover = true;
                break;
            }
            default: {
                // §4.3: unknown map key -> ignore the pair.
                auto sv = r.skipValue();
                if (!sv) return Ret::err(sv.error());
                break;
            }
        }
    }

    if (!(gotChannel && gotIntentId && gotValue)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
