// slopsync-core — EVENT (hub -> client), SPEC §9.4: edges, not levels — a
// discrete occurrence, best-effort, never replayed on reconnect (see §9.4's
// event/state duality rule: anything safety-critical needs a latched STATE
// twin, not just this).
//
// CBOR map, keys ascending: channel_id(15), [payload under value(20)],
// timestamp(21), event_kind(33), [seq_of_state(34)]. The optional payload
// reuses INTENT's IntentValueField/IntentValue nested-map shape
// (intent.hpp) for the same reason ECHO's `applied` does: kind-specific
// fields per schema, no catalog in scope at this codec layer.
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

struct EventMsg {
    uint16_t channel_id = 0;
    uint32_t timestamp = 0;   // hub-ms (§7.2)
    uint16_t event_kind = 0;  // kind discriminator per catalog entry (§9.4)

    bool has_seq_of_state = false;
    uint16_t seq_of_state = 0;  // seq of the STATE twin frame (§9.4), u16 per §7.3

    bool has_payload = false;
    uint32_t payload_count = 0;
    IntentValueArray payload{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeEvent(const EventMsg& m, std::span<std::byte> out) {
    if (m.payload_count > kIntentMaxValueFields) return 0;

    uint32_t nKeys = 3;  // channel_id, timestamp, event_kind
    if (m.has_payload) ++nKeys;
    if (m.has_seq_of_state) ++nKeys;

    CborWriter w(out);
    w.mapHeader(nKeys);
    w.key(CborKey::channel_id).uintVal(m.channel_id);
    if (m.has_payload) {
        w.key(CborKey::value).mapHeader(m.payload_count);
        for (uint32_t i = 0; i < m.payload_count; ++i) {
            const IntentValueField& f = m.payload[i];
            w.key(uint64_t(f.key));
            encodeIntentValue(w, f.value);
        }
    }
    w.key(CborKey::timestamp).uintVal(m.timestamp);
    w.key(CborKey::event_kind).uintVal(m.event_kind);
    if (m.has_seq_of_state) {
        w.key(CborKey::seq_of_state).uintVal(m.seq_of_state);
    }
    return w.size();
}

// Decodes `in` into an EventMsg. Unknown keys are skipped per §4.3.
inline Result<EventMsg, DecodeError> decodeEvent(std::span<const std::byte> in) {
    using Ret = Result<EventMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    EventMsg m{};
    bool gotChannel = false, gotTimestamp = false, gotKind = false;

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
            case uint64_t(CborKey::value): {
                auto cR = decodeIntentValueMap(r, m.payload);
                if (!cR) return Ret::err(cR.error());
                m.payload_count = cR.value();
                m.has_payload = true;
                break;
            }
            case uint64_t(CborKey::timestamp): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.timestamp = uint32_t(v.value());
                gotTimestamp = true;
                break;
            }
            case uint64_t(CborKey::event_kind): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.event_kind = uint16_t(v.value());
                gotKind = true;
                break;
            }
            case uint64_t(CborKey::seq_of_state): {
                auto v = r.readUint();
                if (!v) return Ret::err(v.error());
                m.seq_of_state = uint16_t(v.value());
                m.has_seq_of_state = true;
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

    if (!(gotChannel && gotTimestamp && gotKind)) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
