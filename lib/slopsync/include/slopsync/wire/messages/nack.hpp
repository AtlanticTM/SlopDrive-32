// slopsync-core — NACK (hub -> client), SPEC §16.1: the refusal frame.
//
// CBOR map, keys ascending: [channel_id(15)], code(16), [detail(17)],
// [intent_id(18)], [retry_after_ms(31)]. `code` is the only required field;
// the rest identify WHAT was refused and, for BUSY, WHEN to retry. Per
// §4.3's tolerance rule, an unknown code value (one this decoder's NackCode
// enum doesn't name) is not a decode error — it just becomes a NackCode
// holding a raw numeric value the caller doesn't recognize; deciding to
// treat it as its range-generic code (§16.1: "high byte") is caller policy,
// not something this codec enforces.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

// Message-local wire cap for `detail` — SPEC only says "human-readable
// diagnostic", no length is mandated; 48 bytes is generous for a one-line
// reason string and keeps NACK tiny (it rides the never-shed set, §10.1).
inline constexpr size_t kNackMaxDetailBytes = 48;

struct NackMsg {
    NackCode code = NackCode::MALFORMED;

    bool has_channel_id = false;
    uint16_t channel_id = 0;

    bool has_intent_id = false;
    uint16_t intent_id = 0;

    bool has_detail = false;
    std::string_view detail;  // <= kNackMaxDetailBytes UTF-8 bytes

    bool has_retry_after_ms = false;
    uint32_t retry_after_ms = 0;
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeNack(const NackMsg& m, std::span<std::byte> out) {
    if (m.detail.size() > kNackMaxDetailBytes) return 0;

    uint32_t nKeys = 1;  // code
    if (m.has_channel_id) ++nKeys;
    if (m.has_detail) ++nKeys;
    if (m.has_intent_id) ++nKeys;
    if (m.has_retry_after_ms) ++nKeys;

    CborWriter w(out);
    w.mapHeader(nKeys);
    // Ascending: channel_id(15) < code(16) < detail(17) < intent_id(18) < retry_after_ms(31).
    if (m.has_channel_id) {
        w.key(CborKey::channel_id).uintVal(m.channel_id);
    }
    w.key(CborKey::code).uintVal(uint16_t(m.code));
    if (m.has_detail) {
        w.key(CborKey::detail).tstrVal(m.detail);
    }
    if (m.has_intent_id) {
        w.key(CborKey::intent_id).uintVal(m.intent_id);
    }
    if (m.has_retry_after_ms) {
        w.key(CborKey::retry_after_ms).uintVal(m.retry_after_ms);
    }
    return w.size();
}

// Decodes `in` into a NackMsg. Unknown keys are skipped per §4.3.
inline Result<NackMsg, DecodeError> decodeNack(std::span<const std::byte> in) {
    using Ret = Result<NackMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    NackMsg m{};
    bool gotCode = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::channel_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.channel_id = uint16_t(v.value());
                m.has_channel_id = true;
                break;
            }
            case uint64_t(CborKey::code): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                m.code = NackCode(uint16_t(v.value()));
                gotCode = true;
                break;
            }
            case uint64_t(CborKey::detail): {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() > kNackMaxDetailBytes) return Ret::err(DecodeError::CapacityExceeded);
                m.detail = v.value();
                m.has_detail = true;
                break;
            }
            case uint64_t(CborKey::intent_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.intent_id = uint16_t(v.value());
                m.has_intent_id = true;
                break;
            }
            case uint64_t(CborKey::retry_after_ms): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.retry_after_ms = uint32_t(v.value());
                m.has_retry_after_ms = true;
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

    if (!gotCode) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
