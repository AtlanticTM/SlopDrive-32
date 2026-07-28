// slopsync-core — GRANT (hub -> client), SPEC §10.2, answered per-entry to
// SUBSCRIBE (§6.6) or pushed unsolicited whenever the hub re-splits capacity.
//
// CBOR map: `grants` (35) = array of grant results, the SAME entry shape
// WELCOME batches at session start (§6.3) — reused here via the Grant struct
// from wire/messages/welcome.hpp rather than redeclared. Grant-entry keys
// ascending: priority(13) < granted_rate_hz(14) < channel_id(15), identical
// to WELCOME's order.
//
// RFC-013 addition: `granted_publishes` (36) rides the SAME frame when GRANT
// is answering a mid-session PUBLISH (0x18) — same key, same entry shape, same
// meaning as WELCOME's, so a client has exactly one publish-grant decoder for
// both the session-start batch and every renegotiation. Both arrays are
// optional here (a GRANT answering SUBSCRIBE carries only `grants`; one
// answering PUBLISH carries only `granted_publishes`), and a GRANT with
// neither is not emitted at all.
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
    // RFC-029 item 6: the answer to a successful AUTH (0x1C). `roles` (23) is
    // the SAME key WELCOME uses for the same thing, in the frame that already
    // means "your access to things was re-evaluated" — which is why AUTH did
    // not need an answer frame of its own. Optional and omitted by every
    // pre-RFC-029 GRANT, so a hub that never authenticates emits byte-identical
    // frames and an old client skips the key per §4.3.
    //
    // A role change does NOT retroactively re-grant subscriptions: the wishes a
    // client sent in HELLO were evaluated against the role it had THEN, and the
    // hub does not keep a rejected wish around waiting for permission to arrive.
    // A client whose role went up re-SUBSCRIBEs, which is exactly what SUBSCRIBE
    // is for and costs the hub no per-session memory to remember.
    bool has_roles = false;
    uint8_t roles = 0;

    uint32_t grants_count = 0;
    std::array<Grant, kGrantMsgMaxGrants> grants{};

    // RFC-013 publish-grant results (key 36). Emitted ONLY when non-empty, so
    // a SUBSCRIBE answer is byte-identical to a pre-RFC-013 hub's GRANT.
    uint32_t granted_publishes_count = 0;
    std::array<GrantedPublish, kWelcomeMaxGrantedPublishes> granted_publishes{};
};

// ---- Encode -----------------------------------------------------------------
// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeGrant(const GrantMsg& m, std::span<std::byte> out) {
    if (m.grants_count > kGrantMsgMaxGrants) return 0;
    if (m.granted_publishes_count > kWelcomeMaxGrantedPublishes) return 0;

    const bool hasGrantedPublishes = m.granted_publishes_count > 0;

    CborWriter w(out);
    w.mapHeader(1 + uint32_t(hasGrantedPublishes) + uint32_t(m.has_roles));
    // roles(23) < grants(35) < granted_publishes(36): map order stays ascending.
    if (m.has_roles) w.key(CborKey::roles).uintVal(m.roles);
    // `grants` is ALWAYS present (possibly an empty array) — same posture as
    // WELCOME's, so every existing decoder's required-key check still passes
    // on a GRANT that is purely a publish answer.
    w.key(CborKey::grants).arrayHeader(m.grants_count);
    for (uint32_t i = 0; i < m.grants_count; ++i) {
        const Grant& g = m.grants[i];
        // Grant-entry keys ascending: priority(13) < granted_rate_hz(14) < channel_id(15).
        w.mapHeader(3);
        w.key(CborKey::priority).uintVal(g.priority);
        w.key(CborKey::granted_rate_hz).f32Val(g.granted_rate_hz);
        w.key(CborKey::channel_id).uintVal(g.channel_id);
    }
    if (hasGrantedPublishes) {
        // granted_publishes(36) > grants(35): map order stays ascending.
        w.key(CborKey::granted_publishes).arrayHeader(m.granted_publishes_count);
        for (uint32_t i = 0; i < m.granted_publishes_count; ++i) {
            const GrantedPublish& gp = m.granted_publishes[i];
            // Entry keys ascending: granted_rate_hz(14) < channel_id(15) < burst(42)
            // < curve_family(45) < requested_curve_family(48).
            w.mapHeader(2 + uint32_t(gp.has_burst) + uint32_t(gp.has_curve_family) +
                        uint32_t(gp.has_requested_curve_family));
            w.key(CborKey::granted_rate_hz).f32Val(gp.granted_rate_hz);
            w.key(CborKey::channel_id).uintVal(gp.channel_id);
            if (gp.has_burst) w.key(CborKey::burst).f32Val(gp.burst);
            if (gp.has_curve_family) w.key(CborKey::curve_family).uintVal(gp.curve_family);
            if (gp.has_requested_curve_family)
                w.key(CborKey::requested_curve_family).uintVal(gp.requested_curve_family);
        }
    }
    return w.size();
}

// ---- Decode -----------------------------------------------------------------
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
            case uint64_t(CborKey::roles): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > uint64_t(AccessLevel::configure)) return Ret::err(DecodeError::Malformed);
                m.roles = uint8_t(v.value());
                m.has_roles = true;
                break;
            }
            case uint64_t(CborKey::granted_publishes): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kWelcomeMaxGrantedPublishes) return Ret::err(DecodeError::CapacityExceeded);
                for (uint32_t j = 0; j < cR.value(); ++j) {
                    auto pR = r.readMapHeader();
                    if (!pR) return Ret::err(pR.error());
                    GrantedPublish gp{};
                    for (uint32_t f = 0; f < pR.value(); ++f) {
                        auto fk = r.readKey();
                        if (!fk) return Ret::err(fk.error());
                        switch (fk.value()) {
                            case uint64_t(CborKey::granted_rate_hz): {
                                auto vv = r.readF32();
                                if (!vv) return Ret::err(vv.error());
                                gp.granted_rate_hz = vv.value();
                                break;
                            }
                            case uint64_t(CborKey::channel_id): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                gp.channel_id = uint16_t(vv.value());
                                break;
                            }
                            case uint64_t(CborKey::burst): {
                                auto vv = r.readF32();
                                if (!vv) return Ret::err(vv.error());
                                gp.burst = vv.value();
                                gp.has_burst = true;
                                break;
                            }
                            case uint64_t(CborKey::curve_family): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                if (vv.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                                gp.curve_family = uint8_t(vv.value());
                                gp.has_curve_family = true;
                                break;
                            }
                            case uint64_t(CborKey::requested_curve_family): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                if (vv.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                                gp.requested_curve_family = uint8_t(vv.value());
                                gp.has_requested_curve_family = true;
                                break;
                            }
                            default: {
                                auto sv = r.skipValue();
                                if (!sv) return Ret::err(sv.error());
                                break;
                            }
                        }
                    }
                    m.granted_publishes[j] = gp;
                }
                m.granted_publishes_count = cR.value();
                // Optional key — deliberately NOT part of the required set.
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
