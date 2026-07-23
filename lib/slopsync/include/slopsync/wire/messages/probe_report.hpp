// slopsync-core — PROBE_REPORT (client -> hub), SPEC §6.4: the client's
// measurement of the hub's post-WELCOME bandwidth probe, used to justify a
// possible upward GRANT.
//
// CBOR map: `probe_result` (26) = nested map { bytes_received, span_ms,
// loss_pct_x100, rtt_ms }.
//
// REGISTRY GAP (flagged for a follow-up registry.yaml PR, same pattern as
// welcome.hpp's `welcome_limits` sub-key space): SPEC §6.4 names the
// probe_result fields in prose ("bytes, span_ms, loss_pct, rtt_ms") but
// registry.yaml has no `probe_result_keys` section yet, unlike `limits` (22)
// which got `welcome_limits`. The sub-key numbers below are therefore
// LOCALLY allocated by this header, not generated — small, scoped to this
// one sub-map, and safe to allocate here under the same reasoning
// welcome.hpp used before its own gap was closed at the source of truth.
// `loss_pct` is carried as `loss_pct_x100` (percentage * 100, integer) to
// stay an integer field per §5.3's "integral values ... encoded as
// integers, not floats" rather than a float32 percentage.
#pragma once

#include <cstdint>
#include <span>

#include "slopsync/core/result.hpp"
#include "slopsync/generated/registry_constants.hpp"
#include "slopsync/wire/cbor/cbor_reader.hpp"
#include "slopsync/wire/cbor/cbor_writer.hpp"

namespace slopsync {

// `probe_result` (26) sub-key space — registry section `probe_result_keys`,
// generated into namespace slopsync::probe_result (gap found during
// implementation, since fixed at the source of truth).
namespace probe_result_subkeys = ::slopsync::probe_result;

struct ProbeResult {
    uint32_t bytes_received = 0;
    uint32_t span_ms = 0;
    uint32_t loss_pct_x100 = 0;
    uint32_t rtt_ms = 0;
};

struct ProbeReportMsg {
    ProbeResult probe_result{};
};

// Encodes into `out`; returns bytes written, or 0 on any failure.
inline size_t encodeProbeReport(const ProbeReportMsg& m, std::span<std::byte> out) {
    CborWriter w(out);
    w.mapHeader(1);
    w.key(CborKey::probe_result).mapHeader(4);
    w.key(uint64_t(probe_result_subkeys::bytes_received)).uintVal(m.probe_result.bytes_received);
    w.key(uint64_t(probe_result_subkeys::span_ms)).uintVal(m.probe_result.span_ms);
    w.key(uint64_t(probe_result_subkeys::loss_pct_x100)).uintVal(m.probe_result.loss_pct_x100);
    w.key(uint64_t(probe_result_subkeys::rtt_ms)).uintVal(m.probe_result.rtt_ms);
    return w.size();
}

// Decodes `in` into a ProbeReportMsg. Unknown keys are skipped per §4.3.
inline Result<ProbeReportMsg, DecodeError> decodeProbeReport(std::span<const std::byte> in) {
    using Ret = Result<ProbeReportMsg, DecodeError>;

    CborReader r(in);
    auto nR = r.readMapHeader();
    if (!nR) return Ret::err(nR.error());

    ProbeReportMsg m{};
    bool gotProbeResult = false;

    for (uint32_t i = 0; i < nR.value(); ++i) {
        auto kR = r.readKey();
        if (!kR) return Ret::err(kR.error());
        switch (kR.value()) {
            case uint64_t(CborKey::probe_result): {
                auto pR = r.readMapHeader();
                if (!pR) return Ret::err(pR.error());
                for (uint32_t f = 0; f < pR.value(); ++f) {
                    auto fk = r.readKey();
                    if (!fk) return Ret::err(fk.error());
                    switch (fk.value()) {
                        case probe_result_subkeys::bytes_received: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.probe_result.bytes_received = uint32_t(vv.value());
                            break;
                        }
                        case probe_result_subkeys::span_ms: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.probe_result.span_ms = uint32_t(vv.value());
                            break;
                        }
                        case probe_result_subkeys::loss_pct_x100: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.probe_result.loss_pct_x100 = uint32_t(vv.value());
                            break;
                        }
                        case probe_result_subkeys::rtt_ms: {
                            auto vv = r.readUint();
                            if (!vv) return Ret::err(vv.error());
                            m.probe_result.rtt_ms = uint32_t(vv.value());
                            break;
                        }
                        default: {
                            auto sv = r.skipValue();
                            if (!sv) return Ret::err(sv.error());
                            break;
                        }
                    }
                }
                gotProbeResult = true;
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

    if (!gotProbeResult) return Ret::err(DecodeError::Malformed);
    return Ret::ok(m);
}

}  // namespace slopsync
