// slopsync-core — GOODBYE (either direction), SPEC §6.8: a courtesy
// teardown, not a requirement.
//
// CBOR map, keys ascending: code(16), [detail(17)]. `code` reuses the
// NackCode space rather than a separate enum — §6.8 names its codes of
// note (`SESSION_EVICTED`, `DUPLICATE_INSTANCE`) as values already defined
// in the registry's NackCode taxonomy (registry_constants.hpp), and GOODBYE
// has no reason to allocate a second, parallel numbering for the same kind
// of "why" question NACK already answers.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

// Message-local wire cap for `detail`, same rationale as NACK's (nack.hpp).
inline constexpr size_t kGoodbyeMaxDetailBytes = 48;

struct GoodbyeMsg {
    NackCode code = NackCode::MALFORMED;

    bool has_detail = false;
    std::string_view detail;  // <= kGoodbyeMaxDetailBytes UTF-8 bytes
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeGoodbye(const GoodbyeMsg& m, std::span<std::byte> out) {
    if (m.detail.size() > kGoodbyeMaxDetailBytes) return 0;

    uint32_t nKeys = 1;  // code
    if (m.has_detail) ++nKeys;

    CborWriter w(out);
    w.mapHeader(nKeys);
    w.key(CborKey::code).uintVal(uint16_t(m.code));
    if (m.has_detail) {
        w.key(CborKey::detail).tstrVal(m.detail);
    }
    return w.size();
}

// Decodes `in` into a GoodbyeMsg. Unknown keys are skipped per §4.3.
inline Result<GoodbyeMsg, DecodeError> decodeGoodbye(std::span<const std::byte> in) {
    using Ret = Result<GoodbyeMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    GoodbyeMsg m{};
    bool gotCode = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
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
                if (v.value().size() > kGoodbyeMaxDetailBytes) return Ret::err(DecodeError::CapacityExceeded);
                m.detail = v.value();
                m.has_detail = true;
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
