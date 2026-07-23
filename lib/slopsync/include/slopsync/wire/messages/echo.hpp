// slopsync-core — ECHO (hub -> client), SPEC §9.3: the mandatory, truthful
// reply to every INTENT the hub actually applies (a rejected one gets NACK
// instead, nack.hpp).
//
// CBOR map, keys ascending: cfg_gen(9), intent_id(18), applied(19). `applied`
// is the post-clamp values actually in effect — same IntentValueField/
// IntentValue nested-map shape INTENT's `value` uses (intent.hpp), reused
// here rather than redeclared, per §9.3: "applied MAY differ from
// requested".
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/intent.hpp"  // reuse IntentValueField/IntentValue

namespace slopsync {

struct EchoMsg {
    uint16_t intent_id = 0;
    uint16_t cfg_gen = 0;

    uint32_t applied_count = 0;
    IntentValueArray applied{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeEcho(const EchoMsg& m, std::span<std::byte> out) {
    if (m.applied_count > kIntentMaxValueFields) return 0;

    CborWriter w(out);
    w.mapHeader(3);
    w.key(CborKey::cfg_gen).uintVal(m.cfg_gen);
    w.key(CborKey::intent_id).uintVal(m.intent_id);
    w.key(CborKey::applied).mapHeader(m.applied_count);
    for (uint32_t i = 0; i < m.applied_count; ++i) {
        const IntentValueField& f = m.applied[i];
        w.key(uint64_t(f.key));
        encodeIntentValue(w, f.value);
    }
    return w.size();
}

// Decodes `in` into an EchoMsg. Unknown keys are skipped per §4.3.
inline Result<EchoMsg, DecodeError> decodeEcho(std::span<const std::byte> in) {
    using Ret = Result<EchoMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    EchoMsg m{};
    bool gotIntentId = false, gotCfgGen = false, gotApplied = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::cfg_gen): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.cfg_gen = uint16_t(v.value());
                gotCfgGen = true;
                break;
            }
            case uint64_t(CborKey::intent_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.intent_id = uint16_t(v.value());
                gotIntentId = true;
                break;
            }
            case uint64_t(CborKey::applied): {
                auto cR = decodeIntentValueMap(r, m.applied);
                if (!cR) return Ret::err(cR.error());
                m.applied_count = cR.value();
                gotApplied = true;
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

    if (!(gotIntentId && gotCfgGen && gotApplied)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
