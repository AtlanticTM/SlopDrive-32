// slopsync-core — catalog_etag, SPEC §8.3:
//   catalog_etag = first 8 bytes of SHA-256 over the catalog encoded in the
//   §5.3 deterministic CBOR profile, entries sorted ascending by id.
// Deterministic encoding -> reproducible hash from catalog CONTENT alone:
// any implementation, any language, same bytes in, same etag out. The etag
// covers ids/names/classes/access/rates/layouts/schemas (everything catalog
// encoding carries); it does NOT cover retained channel *values* — that is
// `cfg_gen`'s and per-channel `seq`'s job (§4.2), not this file's.
#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "slopsync/channel/catalog.hpp"
#include "slopsync/wire/catalog_codec.hpp"
#include "slopsync/wire/sha256.hpp"

namespace slopsync {

// Computes the etag using `scratch` as the intermediate encode buffer —
// required so this stays heap-free: encoding the catalog to bytes is a
// necessary step before it can be hashed, and this library never allocates,
// so the caller supplies the scratch span (sized for its own catalog; see
// encodeCatalog's size requirements — layoutWireSize()-style reasoning
// applies to the CBOR encoding, not the packed wire size, so callers should
// simply size generously, e.g. a few hundred bytes per entry).
//
// If encoding fails (scratch too small for `cat`, or `cat`'s entries are not
// ascending by id — see encodeCatalog), this returns the SHA-256-of-zero-
// bytes etag rather than asserting or throwing: an etag is cache-invalidation
// metadata, never a safety-critical path, and a caller that needs to detect
// the failure explicitly already has encodeCatalog's own 0-return for that.
template <size_t N>
std::array<std::byte, 8> catalogEtag(const BasicCatalog<N>& cat, std::span<std::byte> scratch) {
    size_t n = encodeCatalog(cat, scratch);
    auto digest = Sha256::hash(scratch.first(n));  // n==0 -> hash of empty span

    std::array<std::byte, 8> etag{};
    for (size_t i = 0; i < etag.size(); ++i) etag[i] = digest[i];
    return etag;
}

}  // namespace slopsync
