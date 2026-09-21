// slopsync-core — PUBLISH (client -> hub), frame type 0x18, SPEC §6.6 /
// RFC-013: the c2h counterpart to SUBSCRIBE, for PUBLISH wishes.
//
// Before this frame existed there was no way to add or change a publish grant
// mid-session: a producer that wanted a new c2h STREAM channel had to tear the
// whole session down and reconnect just to put a different `publishes` array
// in its HELLO. PUBLISH carries exactly that array — same key (11), same entry
// shape — and is answered with a GRANT frame carrying `granted_publishes`
// (36), the same structure WELCOME already uses.
//
// CBOR map, one key: publishes(11) = array of {rate_hz(12), channel_id(15),
// [burst(42)]}. Semantics mirror SUBSCRIBE (§6.6): the named channels are
// ADDED or REPLACED, everything not named is untouched, and a wish failing
// §6.2 validation (unknown channel / not STREAM / not c2h / access above the
// session's role / non-positive rate) is silently OMITTED from the grants,
// exactly as in HELLO — absence, not error.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"
#include "slopsync/wire/messages/hello.hpp"  // reuse PublishWish + its wire cap

namespace slopsync {

// Same cap as HELLO's publishes array (8): a renegotiation cannot express more
// publish wishes than the handshake could, and the hub's per-session grant
// table (HubSession::kMaxPublishGrants) is sized to match.
inline constexpr uint32_t kPublishMaxWishes = kHelloMaxPublishWishes;

struct PublishMsg {
    uint32_t publishes_count = 0;
    std::array<PublishWish, kPublishMaxWishes> publishes{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodePublish(const PublishMsg& m, std::span<std::byte> out) {
    if (m.publishes_count > kPublishMaxWishes) return 0;

    CborWriter w(out);
    w.mapHeader(1);
    w.key(CborKey::publishes).arrayHeader(m.publishes_count);
    for (uint32_t i = 0; i < m.publishes_count; ++i) {
        const PublishWish& p = m.publishes[i];
        // Wish-entry keys ascending: rate_hz(12) < channel_id(15) < burst(42) < curve_family(45).
        w.mapHeader(2 + uint32_t(p.has_burst) + uint32_t(p.has_curve_family));
        w.key(CborKey::rate_hz).f32Val(p.rate_hz);
        w.key(CborKey::channel_id).uintVal(p.channel_id);
        if (p.has_burst) w.key(CborKey::burst).f32Val(p.burst);
        if (p.has_curve_family) w.key(CborKey::curve_family).uintVal(p.curve_family);
    }
    return w.size();
}

// Decodes `in` into a PublishMsg. Unknown keys are skipped per §4.3.
inline Result<PublishMsg, DecodeError> decodePublish(std::span<const std::byte> in) {
    using Ret = Result<PublishMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    PublishMsg m{};
    bool gotPublishes = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::publishes): {
                auto cR = r.readArrayHeader();
                if (!cR) return Ret::err(cR.error());
                if (cR.value() > kPublishMaxWishes) return Ret::err(DecodeError::CapacityExceeded);
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
                            case uint64_t(CborKey::burst): {
                                auto vv = r.readF32();
                                if (!vv) return Ret::err(vv.error());
                                wish.burst = vv.value();
                                wish.has_burst = true;
                                break;
                            }
                            case uint64_t(CborKey::curve_family): {
                                auto vv = r.readUint();
                                if (!vv) return Ret::err(vv.error());
                                if (vv.value() > 0xFF) return Ret::err(DecodeError::Malformed);
                                wish.curve_family = uint8_t(vv.value());
                                wish.has_curve_family = true;
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
                gotPublishes = true;
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

    if (!gotPublishes) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
