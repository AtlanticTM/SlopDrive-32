// slopsync-core — WELCOME (hub -> client), SPEC §6.3.
//
// CBOR map, keys ascending: proto_ver(1), session_id(6), boot_id(7),
// catalog_etag(8), cfg_gen(9), limits(22), roles(23), deadman_ms(24),
// deadman_policy(25), nonce(29), grants(*). All fields are always present —
// unlike HELLO, nothing here is optional (§6.3: WELCOME is the moment
// grants become truth; a client that asked for nothing still gets an empty
// grants array, not an absent key).
//
// (Registry gap found during implementation, since fixed at the source of
// truth: grants = CborKey 35 and the welcome_limits sub-key space are now
// allocated in registry.yaml and flow in via the generated header.)
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

inline constexpr uint32_t kWelcomeMaxGrants = 16;  // SPEC §6.3: "array up to 16"

// SPEC §6.2: at most kHelloMaxPublishWishes (8) publish wishes ride one HELLO,
// so at most that many can be granted — mirror the cap here.
inline constexpr uint32_t kWelcomeMaxGrantedPublishes = 8;

struct Grant {
    uint16_t channel_id = 0;
    float granted_rate_hz = 0.0f;
    uint8_t priority = 0;
};

// A granted inbound-STREAM publish result (§6.2/§6.3, key 36). No priority —
// a c2h producer has no subscription priority; the pair is {rate, channel}.
struct GrantedPublish {
    uint16_t channel_id = 0;
    float granted_rate_hz = 0.0f;
};

// §6.3's `limits` (22) is itself a CBOR map with its OWN small integer key
// space local to that sub-map — registry section `welcome_limits_keys`,
// generated into namespace slopsync::welcome_limits.
namespace welcome_limits_subkeys = ::slopsync::welcome_limits;

struct WelcomeLimits {
    uint32_t max_frame = 0;
    uint32_t max_subscriptions = 0;
    uint32_t retained_pending = 0;
};

struct WelcomeMsg {
    uint8_t proto_ver = kProtocolVersion;
    uint32_t session_id = 0;
    uint32_t boot_id = 0;
    std::array<std::byte, limits::etag_bytes> catalog_etag{};
    uint16_t cfg_gen = 0;
    WelcomeLimits limits_info{};
    uint8_t roles = 0;
    uint32_t deadman_ms = limits::deadman_default_ms;
    uint8_t deadman_policy = 0;
    std::array<std::byte, 8> nonce{};  // SPEC §6.3: 8-byte pairing nonce (key 29)

    uint32_t grants_count = 0;
    std::array<Grant, kWelcomeMaxGrants> grants{};

    // Granted publishes (§6.2/§6.3, key 36). Emitted ONLY when non-empty — a
    // WELCOME with no granted publish is byte-identical to a pre-key-36 hub's
    // (the golden vectors and every non-streaming session stay unchanged).
    uint32_t granted_publishes_count = 0;
    std::array<GrantedPublish, kWelcomeMaxGrantedPublishes> granted_publishes{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeWelcome(const WelcomeMsg& m, std::span<std::byte> out) {
    if (m.grants_count > kWelcomeMaxGrants) return 0;
    if (m.granted_publishes_count > kWelcomeMaxGrantedPublishes) return 0;

    // granted_publishes (key 36) is the ONLY optional key — 11 fixed + it.
    const bool hasGrantedPublishes = m.granted_publishes_count > 0;

    CborWriter w(out);
    w.mapHeader(hasGrantedPublishes ? 12 : 11);
    w.key(CborKey::proto_ver).uintVal(m.proto_ver);
    w.key(CborKey::session_id).uintVal(m.session_id);
    w.key(CborKey::boot_id).uintVal(m.boot_id);
    w.key(CborKey::catalog_etag).bstrVal(std::span<const std::byte>(m.catalog_etag));
    w.key(CborKey::cfg_gen).uintVal(m.cfg_gen);

    w.key(CborKey::limits).mapHeader(3);
    w.key(uint64_t(welcome_limits_subkeys::max_frame)).uintVal(m.limits_info.max_frame);
    w.key(uint64_t(welcome_limits_subkeys::max_subscriptions)).uintVal(m.limits_info.max_subscriptions);
    w.key(uint64_t(welcome_limits_subkeys::retained_pending)).uintVal(m.limits_info.retained_pending);

    w.key(CborKey::roles).uintVal(m.roles);
    w.key(CborKey::deadman_ms).uintVal(m.deadman_ms);
    w.key(CborKey::deadman_policy).uintVal(m.deadman_policy);
    w.key(CborKey::nonce).bstrVal(std::span<const std::byte>(m.nonce));

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
            // Entry keys ascending: granted_rate_hz(14) < channel_id(15).
            w.mapHeader(2);
            w.key(CborKey::granted_rate_hz).f32Val(gp.granted_rate_hz);
            w.key(CborKey::channel_id).uintVal(gp.channel_id);
        }
    }
    return w.size();
}

// Decodes `in` into a WelcomeMsg. Unknown keys are skipped per §4.3.
inline Result<WelcomeMsg, DecodeError> decodeWelcome(std::span<const std::byte> in) {
    using Ret = Result<WelcomeMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    WelcomeMsg m{};
    bool gotProtoVer = false, gotSession = false, gotBoot = false, gotEtag = false;
    bool gotCfgGen = false, gotLimits = false, gotRoles = false, gotDeadmanMs = false;
    bool gotDeadmanPolicy = false, gotNonce = false, gotGrants = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::proto_ver): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                m.proto_ver = uint8_t(v.value());
                gotProtoVer = true;
                break;
            }
            case uint64_t(CborKey::session_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.session_id = uint32_t(v.value());
                gotSession = true;
                break;
            }
            case uint64_t(CborKey::boot_id): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.boot_id = uint32_t(v.value());
                gotBoot = true;
                break;
            }
            case uint64_t(CborKey::catalog_etag): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.catalog_etag.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.catalog_etag.data(), v.value().data(), m.catalog_etag.size());
                gotEtag = true;
                break;
            }
            case uint64_t(CborKey::cfg_gen): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.cfg_gen = uint16_t(v.value());
                gotCfgGen = true;
                break;
            }
            case uint64_t(CborKey::limits): {
                auto lR = r.readMapHeader();
                if (!lR) return Ret::err(lR.error());
                for (uint32_t f = 0; f < lR.value(); ++f) {
                    auto fk = r.readKey();
                    if (!fk) return Ret::err(fk.error());
                    switch (fk.value()) {
                        case welcome_limits_subkeys::max_frame: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.limits_info.max_frame = uint32_t(vv.value());
                            break;
                        }
                        case welcome_limits_subkeys::max_subscriptions: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.limits_info.max_subscriptions = uint32_t(vv.value());
                            break;
                        }
                        case welcome_limits_subkeys::retained_pending: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.limits_info.retained_pending = uint32_t(vv.value());
                            break;
                        }
                        default: {
                            auto sv = r.skipValue();
                            if (!sv) return Ret::err(sv.error());
                            break;
                        }
                    }
                }
                gotLimits = true;
                break;
            }
            case uint64_t(CborKey::roles): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.roles = uint8_t(v.value());
                gotRoles = true;
                break;
            }
            case uint64_t(CborKey::deadman_ms): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.deadman_ms = uint32_t(v.value());
                gotDeadmanMs = true;
                break;
            }
            case uint64_t(CborKey::deadman_policy): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.deadman_policy = uint8_t(v.value());
                gotDeadmanPolicy = true;
                break;
            }
            case uint64_t(CborKey::nonce): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.nonce.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.nonce.data(), v.value().data(), m.nonce.size());
                gotNonce = true;
                break;
            }
            case uint64_t(CborKey::grants): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kWelcomeMaxGrants) return Ret::err(DecodeError::CapacityExceeded);
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
                // NOT added to the required-keys set below: granted_publishes is
                // optional (absent from a WELCOME with no granted publish, and
                // from any pre-key-36 hub — §4.3 tolerance).
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

    if (!(gotProtoVer && gotSession && gotBoot && gotEtag && gotCfgGen && gotLimits &&
          gotRoles && gotDeadmanMs && gotDeadmanPolicy && gotNonce && gotGrants)) {
        return Ret::err(DecodeError::Malformed);
    }
    return Ret::ok(m);
}

}  // namespace slopsync
