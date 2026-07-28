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
#include <string_view>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/trust_submap.hpp"

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
// `burst` (key 42, RFC-013) is the APPLIED token-bucket capacity, echoed back
// post-clamp exactly like every other granted value (ground-truth doctrine).
// It is emitted only when the wish ASKED for a burst: a wish that didn't ask
// gets capacity == granted rate (the documented default), and its grant stays
// byte-identical to a pre-RFC-013 hub's.
struct GrantedPublish {
    uint16_t channel_id = 0;
    float granted_rate_hz = 0.0f;
    bool has_burst = false;
    float burst = 0.0f;
    // RFC-030: the EFFECTIVE curve family (key 45) — the wish AFTER the hub's
    // own curve_policy override, so a client can tell "honored" from
    // "downgraded". Emitted only when the wish declared a family, mirroring
    // burst's byte-identical rule for everyone else.
    bool has_curve_family = false;
    uint8_t curve_family = 0;
    // RFC-049b: the client's ORIGINAL `curve_family` wish (key 48), echoed
    // verbatim — unmodified by `curve_policy`, unlike `curve_family` above.
    // Present iff the wish declared a family, exactly mirroring that field's
    // own presence rule. Comparing this against `curve_family` is how a
    // client tells "honored" from "downgraded" without remembering what it
    // sent.
    bool has_requested_curve_family = false;
    uint8_t requested_curve_family = 0;
};

// §6.3's `limits` (22) is itself a CBOR map with its OWN small integer key
// space local to that sub-map — registry section `welcome_limits_keys`,
// generated into namespace slopsync::welcome_limits.
namespace welcome_limits_subkeys = ::slopsync::welcome_limits;
namespace identity_subkeys = ::slopsync::identity;

struct WelcomeLimits {
    uint32_t max_frame = 0;
    uint32_t max_subscriptions = 0;
    uint32_t retained_pending = 0;
    // RFC-033.3: most wishes one SUBSCRIBE/HELLO frame may carry. 0 = not
    // advertised (sub-map key 4 omitted — pre-RFC-033 hubs and the frozen
    // golden vectors stay byte-identical); a hub that decodes a bounded wish
    // array MUST advertise the bound so clients stop finding it by
    // binary-searching a live machine.
    uint32_t max_subscriptions_per_frame = 0;
};

// RFC-016(a): the WELCOME `identity` (37) sub-map — registered and specified
// at v1.0, codec landed with the RFC-030..040 batch. Strings are views into
// hub-owned storage (encode) or the frame buffer (decode). All three are
// optional; the sub-map is emitted only when at least one is non-empty, so a
// hub that sets nothing stays byte-identical to a pre-identity WELCOME.
// (`identity_keys::info` — device-defined extras — is deliberately not
// implemented yet; decoders skip it per §4.3.)
inline constexpr size_t kIdentityProductMaxBytes = 32;
inline constexpr size_t kIdentityFwVersionMaxBytes = 24;
inline constexpr size_t kIdentityHubNameMaxBytes = 32;

struct IdentityInfo {
    std::string_view product;     // <= kIdentityProductMaxBytes
    std::string_view fw_version;  // <= kIdentityFwVersionMaxBytes
    std::string_view hub_name;    // <= kIdentityHubNameMaxBytes
    // RFC-048: durable cross-boot identity (identity_keys 5, u64). An explicit
    // presence flag rather than a 0-sentinel — unlike ws_port/ipv4 below, 0 is
    // a value esp_random() could plausibly produce, however unlikely, and
    // "the hub has no durable identity yet" is a real, distinct state.
    bool has_hub_instance_id = false;
    uint64_t hub_instance_id = 0;
    bool any() const {
        return !product.empty() || !fw_version.empty() || !hub_name.empty() || has_hub_instance_id;
    }
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

    // RFC-016(a): hub identity (key 37). Emitted only when any field is
    // non-empty. Key 37 sorts between granted_publishes(36) and trust(39),
    // which is exactly the room the original encoder ordering left for it.
    bool has_identity = false;
    IdentityInfo identity{};

    // RFC-046: the hub's own WS endpoint (keys 46/47). 0 = absent — same
    // sentinel the registry's own note documents ("0 = none") — and the key
    // is OMITTED from the wire at 0, not encoded as a literal zero, so a hub
    // that never calls Hub::setEndpoint() stays byte-identical to a
    // pre-RFC-046 WELCOME. Same additive-safe pattern as every other optional
    // key here (max_subscriptions_per_frame, identity, trust).
    uint16_t ws_port = 0;
    uint32_t ipv4 = 0;

    // The scoped `trust` (39) sub-map. M4b puts `pairing_modes` here — the
    // BITMASK of association ceremonies this hub is offering RIGHT NOW, which
    // is why it is re-evaluated per session rather than fixed at boot: a
    // push-to-pair window is advertised exactly while it is open, and that is
    // half of how RFC-027(c) keeps window state observable in-band. Emitted
    // only when non-empty, so a hub offering nothing (and every pre-M4b
    // WELCOME) stays byte-identical.
    bool has_trust = false;
    TrustMap trust_map{};
};

// ---- Encode -----------------------------------------------------------------
// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeWelcome(const WelcomeMsg& m, std::span<std::byte> out) {
    if (m.grants_count > kWelcomeMaxGrants) return 0;
    if (m.granted_publishes_count > kWelcomeMaxGrantedPublishes) return 0;

    // 11 fixed keys; granted_publishes (36), identity (37) and trust (39) are
    // the optionals.
    const bool hasGrantedPublishes = m.granted_publishes_count > 0;
    const bool hasIdentity = m.has_identity && m.identity.any();
    const bool hasTrust = m.has_trust && m.trust_map.any();
    const bool hasWsPort = m.ws_port != 0;
    const bool hasIpv4 = m.ipv4 != 0;
    if (hasIdentity) {
        if (m.identity.product.size() > kIdentityProductMaxBytes) return 0;
        if (m.identity.fw_version.size() > kIdentityFwVersionMaxBytes) return 0;
        if (m.identity.hub_name.size() > kIdentityHubNameMaxBytes) return 0;
    }

    CborWriter w(out);
    w.mapHeader(11 + uint32_t(hasGrantedPublishes) + uint32_t(hasIdentity) + uint32_t(hasTrust) +
                uint32_t(hasWsPort) + uint32_t(hasIpv4));
    w.key(CborKey::proto_ver).uintVal(m.proto_ver);
    w.key(CborKey::session_id).uintVal(m.session_id);
    w.key(CborKey::boot_id).uintVal(m.boot_id);
    w.key(CborKey::catalog_etag).bstrVal(std::span<const std::byte>(m.catalog_etag));
    w.key(CborKey::cfg_gen).uintVal(m.cfg_gen);

    // limits sub-map: key 4 (RFC-033) rides only when advertised, so a hub
    // that leaves it 0 — and every frozen golden vector — stays byte-identical.
    const bool hasPerFrame = m.limits_info.max_subscriptions_per_frame > 0;
    w.key(CborKey::limits).mapHeader(3 + uint32_t(hasPerFrame));
    w.key(uint64_t(welcome_limits_subkeys::max_frame)).uintVal(m.limits_info.max_frame);
    w.key(uint64_t(welcome_limits_subkeys::max_subscriptions)).uintVal(m.limits_info.max_subscriptions);
    w.key(uint64_t(welcome_limits_subkeys::retained_pending)).uintVal(m.limits_info.retained_pending);
    if (hasPerFrame) {
        w.key(uint64_t(welcome_limits_subkeys::max_subscriptions_per_frame))
            .uintVal(m.limits_info.max_subscriptions_per_frame);
    }

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
    if (hasIdentity) {
        // identity(37) between granted_publishes(36) and trust(39). Sub-map
        // keys ascending: product(1) < fw_version(2) < hub_name(3); empty
        // strings are omitted, never encoded as "".
        uint32_t idKeys = 0;
        if (!m.identity.product.empty()) ++idKeys;
        if (!m.identity.fw_version.empty()) ++idKeys;
        if (!m.identity.hub_name.empty()) ++idKeys;
        if (m.identity.has_hub_instance_id) ++idKeys;
        w.key(CborKey::identity).mapHeader(idKeys);
        if (!m.identity.product.empty())
            w.key(uint64_t(identity_subkeys::product)).tstrVal(m.identity.product);
        if (!m.identity.fw_version.empty())
            w.key(uint64_t(identity_subkeys::fw_version)).tstrVal(m.identity.fw_version);
        if (!m.identity.hub_name.empty())
            w.key(uint64_t(identity_subkeys::hub_name)).tstrVal(m.identity.hub_name);
        // hub_instance_id(5) sorts after info(4, unimplemented) — ascending order intact.
        if (m.identity.has_hub_instance_id)
            w.key(uint64_t(identity_subkeys::hub_instance_id)).uintVal(m.identity.hub_instance_id);
    }
    if (hasTrust) encodeTrustMap(w, m.trust_map);  // key 39
    // ws_port(46) / ipv4(47) sort after trust(39): §5.3 ascending order intact.
    if (hasWsPort) w.key(CborKey::ws_port).uintVal(m.ws_port);
    if (hasIpv4) w.key(CborKey::ipv4).uintVal(m.ipv4);
    return w.size();
}

// ---- Decode -----------------------------------------------------------------
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
                        case welcome_limits_subkeys::max_subscriptions_per_frame: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.limits_info.max_subscriptions_per_frame = uint32_t(vv.value());
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
                // NOT added to the required-keys set below: granted_publishes is
                // optional (absent from a WELCOME with no granted publish, and
                // from any pre-key-36 hub — §4.3 tolerance).
                break;
            }
            case uint64_t(CborKey::identity): {
                // RFC-016(a). Views into `in` — same zero-copy lifetime rule
                // as HELLO's strings. Unknown sub-keys (incl. the deliberately
                // unimplemented `info` map) are skipped per §4.3.
                auto iR = r.readMapHeader();
                if (!iR) return Ret::err(iR.error());
                for (uint32_t f = 0; f < iR.value(); ++f) {
                    auto fk = r.readKey();
                    if (!fk) return Ret::err(fk.error());
                    switch (fk.value()) {
                        case identity_subkeys::product: {
                            auto vv = r.readTstr();
                            if (!vv) return Ret::err(vv.error());
                            if (vv.value().size() > kIdentityProductMaxBytes) return Ret::err(DecodeError::CapacityExceeded);
                            m.identity.product = vv.value();
                            break;
                        }
                        case identity_subkeys::fw_version: {
                            auto vv = r.readTstr();
                            if (!vv) return Ret::err(vv.error());
                            if (vv.value().size() > kIdentityFwVersionMaxBytes) return Ret::err(DecodeError::CapacityExceeded);
                            m.identity.fw_version = vv.value();
                            break;
                        }
                        case identity_subkeys::hub_name: {
                            auto vv = r.readTstr();
                            if (!vv) return Ret::err(vv.error());
                            if (vv.value().size() > kIdentityHubNameMaxBytes) return Ret::err(DecodeError::CapacityExceeded);
                            m.identity.hub_name = vv.value();
                            break;
                        }
                        case identity_subkeys::hub_instance_id: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.identity.hub_instance_id = vv.value();
                            m.identity.has_hub_instance_id = true;
                            break;
                        }
                        default: {
                            auto sv = r.skipValue();
                            if (!sv) return Ret::err(sv.error());
                            break;
                        }
                    }
                }
                m.has_identity = true;
                break;
            }
            case uint64_t(CborKey::trust): {
                auto tR = decodeTrustMap(r, m.trust_map);
                if (!tR) return Ret::err(tR.error());
                m.has_trust = true;
                break;
            }
            case uint64_t(CborKey::ws_port): {
                // RFC-046. Optional (§4.3 tolerance) — NOT added to the
                // required-keys set below, mirroring granted_publishes: absent
                // from a pre-RFC-046 hub and from any WELCOME the sender chose
                // not to populate (0 = absent is the wire convention, §6.3).
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFF) return Ret::err(DecodeError::Malformed);
                m.ws_port = uint16_t(v.value());
                break;
            }
            case uint64_t(CborKey::ipv4): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                if (v.value() > 0xFFFFFFFFull) return Ret::err(DecodeError::Malformed);
                m.ipv4 = uint32_t(v.value());
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
