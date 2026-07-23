// slopsync-core — SUBSCRIBE / UNSUBSCRIBE (client -> hub), SPEC §6.6.
//
// SUBSCRIBE: CBOR map { subscriptions(10): array of wish entries }, same
// wish shape HELLO uses for its own `subscriptions` key (§6.2) — reused here
// via SubscriptionWish from wire/messages/hello.hpp rather than redeclared.
// Wish-entry keys ascending: rate_hz(12) < priority(13) < channel_id(15),
// exactly HELLO's order.
//
// UNSUBSCRIBE: CBOR map { subscriptions(10): array of BARE channel_id uints
// }. This is a deliberate divergence from SUBSCRIBE's wish-maps, straight out
// of SPEC §6.6: "UNSUBSCRIBE: array of `channel_id`" — no rate, no priority,
// there is nothing left to negotiate when tearing a subscription down. Both
// frame types reuse CBOR key 10 (the registry's one `subscriptions` key) for
// their respective arrays; they never collide on the wire because SUBSCRIBE
// (0x06) and UNSUBSCRIBE (0x07) are distinct frame types with independently
// decoded payloads — key 10 simply means "the subscriptions array" in
// whichever shape its own frame type defines.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/hello.hpp"  // reuse SubscriptionWish

namespace slopsync {

// Message-local wire caps, same rationale as HELLO's (hello.hpp comment):
// bounds what fits in ONE SUBSCRIBE/UNSUBSCRIBE frame, distinct from the
// session-wide `max_subscriptions_per_session` (64, §6.6) ceiling.
inline constexpr uint32_t kSubscribeMaxWishes = 16;
inline constexpr uint32_t kUnsubscribeMaxChannels = 16;

struct SubscribeMsg {
    uint32_t subscriptions_count = 0;
    std::array<SubscriptionWish, kSubscribeMaxWishes> subscriptions{};
};

struct UnsubscribeMsg {
    uint32_t channel_count = 0;
    std::array<uint16_t, kUnsubscribeMaxChannels> channel_ids{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeSubscribe(const SubscribeMsg& m, std::span<std::byte> out) {
    if (m.subscriptions_count > kSubscribeMaxWishes) return 0;

    CborWriter w(out);
    w.mapHeader(1);
    w.key(CborKey::subscriptions).arrayHeader(m.subscriptions_count);
    for (uint32_t i = 0; i < m.subscriptions_count; ++i) {
        const SubscriptionWish& s = m.subscriptions[i];
        // Wish-entry keys ascending: rate_hz(12) < priority(13) < channel_id(15).
        w.mapHeader(3);
        w.key(CborKey::rate_hz).f32Val(s.rate_hz);
        w.key(CborKey::priority).uintVal(s.priority);
        w.key(CborKey::channel_id).uintVal(s.channel_id);
    }
    return w.size();
}

// Decodes `in` into a SubscribeMsg. Unknown keys are skipped per §4.3.
inline Result<SubscribeMsg, DecodeError> decodeSubscribe(std::span<const std::byte> in) {
    using Ret = Result<SubscribeMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    SubscribeMsg m{};
    bool gotSubscriptions = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::subscriptions): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kSubscribeMaxWishes) return Ret::err(DecodeError::CapacityExceeded);
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
                gotSubscriptions = true;
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

    if (!gotSubscriptions) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeUnsubscribe(const UnsubscribeMsg& m, std::span<std::byte> out) {
    if (m.channel_count > kUnsubscribeMaxChannels) return 0;

    CborWriter w(out);
    w.mapHeader(1);
    w.key(CborKey::subscriptions).arrayHeader(m.channel_count);
    for (uint32_t i = 0; i < m.channel_count; ++i) {
        w.uintVal(m.channel_ids[i]);
    }
    return w.size();
}

// Decodes `in` into an UnsubscribeMsg. Unknown keys are skipped per §4.3.
inline Result<UnsubscribeMsg, DecodeError> decodeUnsubscribe(std::span<const std::byte> in) {
    using Ret = Result<UnsubscribeMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    UnsubscribeMsg m{};
    bool gotChannels = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::subscriptions): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kUnsubscribeMaxChannels) return Ret::err(DecodeError::CapacityExceeded);
                for (uint32_t j = 0; j < cR.value(); ++j) {
                    auto vv = r.readUint();
                    if (!vv) return Ret::err(vv.error());
                    m.channel_ids[j] = uint16_t(vv.value());
                }
                m.channel_count = cR.value();
                gotChannels = true;
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

    if (!gotChannels) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
