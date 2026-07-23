// slopsync-core — HELLO (client -> hub), SPEC §6.2.
//
// CBOR map, keys ascending: proto_ver(1), client_kind(2), client_name(3),
// instance_id(4), [token(5)], [catalog_etag(8)], [subscriptions(10)],
// [publishes(11)]. The four bracketed fields are optional/possibly-empty
// and, per this codec's brief, OMITTED from the map entirely when absent —
// never encoded as null (§5.3 forbids meaningless simple values anyway; §4.3
// is what makes omission safe on the decode side: an unknown-to-a-future-
// decoder key is just skipped, and an absent-here key is simply never read).
// Map pair count therefore varies message to message.
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

namespace slopsync {

// Message-local wire caps bounding how many wishes fit in the ONE-ROUND-TRIP
// HELLO map (SPEC §6.2: "fixed array up to 16" / "up to 8"). Distinct from
// `limits::max_subscriptions_per_session` (64, §6.6): that one bounds the
// session's whole standing grant set across HELLO *and* later SUBSCRIBE
// frames; these bound only what a single HELLO can carry.
inline constexpr uint32_t kHelloMaxSubscriptionWishes = 16;
inline constexpr uint32_t kHelloMaxPublishWishes = 8;
inline constexpr size_t kHelloMaxClientKindBytes = 16;
inline constexpr size_t kHelloMaxClientNameBytes = 32;

struct SubscriptionWish {
    uint16_t channel_id = 0;
    float rate_hz = 0.0f;
    uint8_t priority = 0;
};

struct PublishWish {
    uint16_t channel_id = 0;
    float rate_hz = 0.0f;
};

struct HelloMsg {
    uint8_t proto_ver = kProtocolVersion;
    std::string_view client_kind;   // <= kHelloMaxClientKindBytes UTF-8 bytes
    std::string_view client_name;   // <= kHelloMaxClientNameBytes UTF-8 bytes
    std::array<std::byte, limits::instance_id_bytes> instance_id{};

    bool has_token = false;
    std::array<std::byte, limits::token_bytes> token{};

    bool has_catalog_etag = false;
    std::array<std::byte, limits::etag_bytes> catalog_etag{};

    uint32_t subscriptions_count = 0;
    std::array<SubscriptionWish, kHelloMaxSubscriptionWishes> subscriptions{};

    uint32_t publishes_count = 0;
    std::array<PublishWish, kHelloMaxPublishWishes> publishes{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure (bad sizes,
// or the writer running out of room / catching an ordering mistake).
inline size_t encodeHello(const HelloMsg& m, std::span<std::byte> out) {
    if (m.client_kind.size() > kHelloMaxClientKindBytes) return 0;
    if (m.client_name.size() > kHelloMaxClientNameBytes) return 0;
    if (m.subscriptions_count > kHelloMaxSubscriptionWishes) return 0;
    if (m.publishes_count > kHelloMaxPublishWishes) return 0;

    uint32_t nKeys = 4;  // proto_ver, client_kind, client_name, instance_id
    if (m.has_token) ++nKeys;
    if (m.has_catalog_etag) ++nKeys;
    if (m.subscriptions_count > 0) ++nKeys;
    if (m.publishes_count > 0) ++nKeys;

    CborWriter w(out);
    w.mapHeader(nKeys);
    w.key(CborKey::proto_ver).uintVal(m.proto_ver);
    w.key(CborKey::client_kind).tstrVal(m.client_kind);
    w.key(CborKey::client_name).tstrVal(m.client_name);
    w.key(CborKey::instance_id).bstrVal(std::span<const std::byte>(m.instance_id));
    if (m.has_token) {
        w.key(CborKey::token).bstrVal(std::span<const std::byte>(m.token));
    }
    if (m.has_catalog_etag) {
        w.key(CborKey::catalog_etag).bstrVal(std::span<const std::byte>(m.catalog_etag));
    }
    if (m.subscriptions_count > 0) {
        w.key(CborKey::subscriptions).arrayHeader(m.subscriptions_count);
        for (uint32_t i = 0; i < m.subscriptions_count; ++i) {
            const SubscriptionWish& s = m.subscriptions[i];
            // Wish-entry keys ascending: rate_hz(12) < priority(13) < channel_id(15).
            w.mapHeader(3);
            w.key(CborKey::rate_hz).f32Val(s.rate_hz);
            w.key(CborKey::priority).uintVal(s.priority);
            w.key(CborKey::channel_id).uintVal(s.channel_id);
        }
    }
    if (m.publishes_count > 0) {
        w.key(CborKey::publishes).arrayHeader(m.publishes_count);
        for (uint32_t i = 0; i < m.publishes_count; ++i) {
            const PublishWish& p = m.publishes[i];
            // Wish-entry keys ascending: rate_hz(12) < channel_id(15).
            w.mapHeader(2);
            w.key(CborKey::rate_hz).f32Val(p.rate_hz);
            w.key(CborKey::channel_id).uintVal(p.channel_id);
        }
    }
    return w.size();
}

// Decodes `in` into a HelloMsg. Unknown keys are skipped per §4.3, never an
// error. `client_kind`/`client_name` are zero-copy views into `in`.
inline Result<HelloMsg, DecodeError> decodeHello(std::span<const std::byte> in) {
    using Ret = Result<HelloMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    HelloMsg m{};
    bool gotProtoVer = false, gotKind = false, gotName = false, gotInstance = false;

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
            case uint64_t(CborKey::client_kind): {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() > kHelloMaxClientKindBytes) return Ret::err(DecodeError::CapacityExceeded);
                m.client_kind = v.value();
                gotKind = true;
                break;
            }
            case uint64_t(CborKey::client_name): {
                auto v = r.readTstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() > kHelloMaxClientNameBytes) return Ret::err(DecodeError::CapacityExceeded);
                m.client_name = v.value();
                gotName = true;
                break;
            }
            case uint64_t(CborKey::instance_id): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.instance_id.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.instance_id.data(), v.value().data(), m.instance_id.size());
                gotInstance = true;
                break;
            }
            case uint64_t(CborKey::token): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.token.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.token.data(), v.value().data(), m.token.size());
                m.has_token = true;
                break;
            }
            case uint64_t(CborKey::catalog_etag): {
                auto v = r.readBstr();
                if (!v) return Ret::err(v.error());
                if (v.value().size() != m.catalog_etag.size()) return Ret::err(DecodeError::Malformed);
                std::memcpy(m.catalog_etag.data(), v.value().data(), m.catalog_etag.size());
                m.has_catalog_etag = true;
                break;
            }
            case uint64_t(CborKey::subscriptions): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kHelloMaxSubscriptionWishes) return Ret::err(DecodeError::CapacityExceeded);
                for (uint32_t j = 0; j < cR.value(); ++j) {
                    auto pR = r.readMapHeader();
                    if (!pR) return Ret::err(pR.error());
                    SubscriptionWish wish{};
                    for (uint32_t f = 0; f < pR.value(); ++f) {
                        auto fk = r.readKey();
                        if (!fk) return Ret::err(fk.error());
                        switch (fk.value()) {
                            case uint64_t(CborKey::channel_id): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                wish.channel_id = uint16_t(vv.value());
                                break;
                            }
                            case uint64_t(CborKey::rate_hz): {
                                auto vv = r.readF32();
                                if (!vv) return Ret::err(vv.error());
                                wish.rate_hz = vv.value();
                                break;
                            }
                            case uint64_t(CborKey::priority): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                wish.priority = uint8_t(vv.value());
                                break;
                            }
                            default: {
                                auto sv = r.skipValue();
                                if (!sv) return Ret::err(sv.error());
                                break;
                            }
                        }
                    }
                    m.subscriptions[j] = wish;
                }
                m.subscriptions_count = cR.value();
                break;
            }
            case uint64_t(CborKey::publishes): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kHelloMaxPublishWishes) return Ret::err(DecodeError::CapacityExceeded);
                for (uint32_t j = 0; j < cR.value(); ++j) {
                    auto pR = r.readMapHeader();
                    if (!pR) return Ret::err(pR.error());
                    PublishWish wish{};
                    for (uint32_t f = 0; f < pR.value(); ++f) {
                        auto fk = r.readKey();
                        if (!fk) return Ret::err(fk.error());
                        switch (fk.value()) {
                            case uint64_t(CborKey::channel_id): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                wish.channel_id = uint16_t(vv.value());
                                break;
                            }
                            case uint64_t(CborKey::rate_hz): {
                                auto vv = r.readF32();
                                if (!vv) return Ret::err(vv.error());
                                wish.rate_hz = vv.value();
                                break;
                            }
                            default: {
                                auto sv = r.skipValue();
                                if (!sv) return Ret::err(sv.error());
                                break;
                            }
                        }
                    }
                    m.publishes[j] = wish;
                }
                m.publishes_count = cR.value();
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

    if (!(gotProtoVer && gotKind && gotName && gotInstance)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
