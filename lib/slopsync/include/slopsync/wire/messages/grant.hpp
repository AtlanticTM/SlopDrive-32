// slopsync-core — GRANT (hub -> client), SPEC §10.2, answered per-entry to
// SUBSCRIBE (§6.6) or pushed unsolicited whenever the hub re-splits capacity.
//
// CBOR map: `grants` (35) = array of grant results, the SAME entry shape
// WELCOME batches at session start (§6.3) — reused here via the Grant struct
// from wire/messages/welcome.hpp rather than redeclared. Grant-entry keys
// ascending: priority(13) < granted_rate_hz(14) < channel_id(15), identical
// to WELCOME's order.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/welcome.hpp"  // reuse Grant

namespace slopsync {

// Message-local wire cap, mirroring WELCOME's kWelcomeMaxGrants (§6.3:
// "array up to 16") — GRANT batches the same shape at the same scale,
// whether it's re-stating one SUBSCRIBE's result or a full re-split.
inline constexpr uint32_t kGrantMsgMaxGrants = 16;

struct GrantMsg {
    uint32_t grants_count = 0;
    std::array<Grant, kGrantMsgMaxGrants> grants{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeGrant(const GrantMsg& m, std::span<std::byte> out) {
    if (m.grants_count > kGrantMsgMaxGrants) return 0;

    CborWriter w(out);
    w.mapHeader(1);
    w.key(CborKey::grants).arrayHeader(m.grants_count);
    for (uint32_t i = 0; i < m.grants_count; ++i) {
        const Grant& g = m.grants[i];
        // Grant-entry keys ascending: priority(13) < granted_rate_hz(14) < channel_id(15).
        w.mapHeader(3);
        w.key(CborKey::priority).uintVal(g.priority);
        w.key(CborKey::granted_rate_hz).f32Val(g.granted_rate_hz);
        w.key(CborKey::channel_id).uintVal(g.channel_id);
    }
    return w.size();
}

// Decodes `in` into a GrantMsg. Unknown keys are skipped per §4.3.
inline Result<GrantMsg, DecodeError> decodeGrant(std::span<const std::byte> in) {
    using Ret = Result<GrantMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    GrantMsg m{};
    bool gotGrants = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::grants): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kGrantMsgMaxGrants) return Ret::err(DecodeError::CapacityExceeded);
                for (uint32_t j = 0; j < cR.value(); ++j) {
                    auto pR = r.readMapHeader();
                    if (!pR) return Ret::err(pR.error());
                    Grant g{};
                    for (uint32_t f = 0; f < pR.value(); ++f) {
                        auto fk = r.readKey();
                        if (!fk) return Ret::err(fk.error());
                        switch (fk.value()) {
                            case uint64_t(CborKey::channel_id): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                g.channel_id = uint16_t(vv.value());
                                break;
                            }
                            case uint64_t(CborKey::granted_rate_hz): {
                                auto vv = r.readF32();
                                if (!vv) return Ret::err(vv.error());
                                g.granted_rate_hz = vv.value();
                                break;
                            }
                            case uint64_t(CborKey::priority): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                g.priority = uint8_t(vv.value());
                                break;
                            }
                            default: {
                                auto sv = r.skipValue();
                                if (!sv) return Ret::err(sv.error());
                                break;
                            }
                        }
                    }
                    m.grants[j] = g;
                }
                m.grants_count = cR.value();
                gotGrants = true;
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

    if (!gotGrants) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
