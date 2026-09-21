// slopsync-core — CATALOG_READY (client -> hub), frame type 0x19, SPEC §8.4 /
// RFC-015: the client declaring which catalog it now operates against.
//
// Why it exists: the hub cannot decode-check its own pushes. It knows it SENT
// catalog chunks, not that they arrived (true on TCP, false on ESP-NOW), so
// "defer retained STATE until transfer completes" is not a property the hub
// can observe. The etag already makes the transfer self-verifying (SHA-256
// over the exact bytes), so the HASH IS THE ACKNOWLEDGMENT: the client
// verifies locally (zero round trips) and then sends its etag back. Until
// that arrives — or the HELLO carried a matching etag, which is the same proof
// one round trip earlier — the session's DATA PLANE is gated shut and its
// INTENTs are refused NOT_READY (§11.5(2): a client must adopt the retained
// safety latch before it can act).
//
// Raw plane: the payload is the 8 raw etag bytes, nothing else — no CBOR, no
// header beyond the frame's own. It is IDEMPOTENT by design (a client on a
// lossy binding re-sends it on the chunk-repair cadence until the first
// retained STATE arrives), so duplicates are harmless flag-sets.
//
// A payload of the wrong length is dropped, never NACKed — raw-plane frames
// follow PING/ESTOP's drop-on-bad-frame rule.
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include "slopsync/generated/registry_constants.hpp"

namespace slopsync {

inline constexpr size_t kCatalogReadyBytes = limits::etag_bytes;  // 8

// Writes the 8-byte etag into `out`. Returns bytes written, or 0 if `etag` is
// not exactly kCatalogReadyBytes long or `out` is too small.
inline size_t encodeCatalogReady(std::span<const std::byte> etag, std::span<std::byte> out) {
    if (etag.size() != kCatalogReadyBytes) return 0;
    if (out.size() < kCatalogReadyBytes) return 0;
    std::memcpy(out.data(), etag.data(), kCatalogReadyBytes);
    return kCatalogReadyBytes;
}

// Reads the etag a CATALOG_READY declares, or nullopt when the payload is not
// exactly 8 bytes (drop, per the raw-plane rule above).
inline std::optional<std::array<std::byte, kCatalogReadyBytes>> decodeCatalogReady(
    std::span<const std::byte> payload) {
    if (payload.size() != kCatalogReadyBytes) return std::nullopt;
    std::array<std::byte, kCatalogReadyBytes> etag{};
    std::memcpy(etag.data(), payload.data(), kCatalogReadyBytes);
    return etag;
}

}  // namespace slopsync
