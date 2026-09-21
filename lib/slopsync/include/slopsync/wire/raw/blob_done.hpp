// slopsync-core -- BLOB_DONE, frame type 0x20, SPEC §8.4 / RFC-050: the
// RECEIVER of a transfer reporting that reassembly concluded, and how.
//
// Why it exists: BLOB_CHUNK is fire-and-forget. Before this frame the sender
// "just stopped and hoped" -- it knew it had emitted every chunk, never that
// any of them landed, so a transfer that lost one chunk was indistinguishable
// from one that completed. CATALOG_READY already solved this for namespace 0
// (the etag IS the acknowledgment), but it is c2h-only and catalog-only, so a
// client-to-hub STORE import (§8.7) had no shape to report in. BLOB_DONE is
// that pattern generalized to every namespace and both directions.
//
// Direction is `any` BY THE RECEIVER'S ROLE, not by peer kind: for the common
// hub->client blob the CLIENT sends it; for a client->hub STORE import the HUB
// does. The sender treats a nonzero status per its own retry policy -- this
// frame reports an outcome, it never itself requests a retry (a sender that
// wants one re-issues a fresh BLOB_REQ).
//
// Raw plane, 7 bytes. The first 6 are BLOB_CHUNK's identity prefix VERBATIM
// (blob_chunks.hpp), so the two frames describe one vocabulary and a receiver
// can compare them field-for-field without a second parser:
//
//   offset  size  field         `blob_keys` name
//   0       1     ns            ns
//   1       1     store_id      store_id     (0 in the catalog namespace)
//   2       1     slot          slot         (0 in the catalog namespace)
//   3       1     reserved      --           (MUST be written 0, MUST be ignored)
//   4       2     generation    generation   (0 for the catalog)
//   6       1     status        --           (BlobDoneStatus)
//
// IDEMPOTENT, exactly like CATALOG_READY: re-sending on a duplicate delivery
// or a retried reassembly is harmless, so a lossy binding costs 15 framed
// bytes per repeat and no state machine.
//
// A payload of the wrong length is DROPPED, never NACKed -- raw-plane frames
// follow PING/ESTOP's drop-on-bad-frame rule.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "slopsync/util/byte_io.hpp"
#include "slopsync/wire/messages/blob_req.hpp"  // BlobId -- one identity vocabulary

namespace slopsync {

inline constexpr size_t kBlobDoneBytes = 7;

// Registry frame_types 0x20's note is the sole home for these numbers; there
// is no registry VOCABULARY table to generate from, so they are named here
// against that note rather than left as bare integers at the call sites.
enum class BlobDoneStatus : uint8_t {
    VerifiedComplete = 0,  // reassembled and the local hash check succeeded
    HashMismatch = 1,      // reassembled whole, but the bytes are not what was advertised
    Aborted = 2,           // gave up: §8.4 row 4, or the receiver's own frag_reassembly_timeout_ms
};

struct BlobDoneMsg {
    BlobId id{};
    BlobDoneStatus status = BlobDoneStatus::VerifiedComplete;
};

// Writes the 7-byte payload into `out`. Returns kBlobDoneBytes, or 0 if `out`
// is too small.
inline size_t encodeBlobDone(const BlobDoneMsg& m, std::span<std::byte> out) {
    if (out.size() < kBlobDoneBytes) return 0;
    putU8(out.subspan(0, 1), m.id.ns);
    putU8(out.subspan(1, 1), m.id.store_id);
    putU8(out.subspan(2, 1), m.id.slot);
    putU8(out.subspan(3, 1), 0);  // reserved
    putU16(out.subspan(4, 2), m.id.generation);
    putU8(out.subspan(6, 1), uint8_t(m.status));
    return kBlobDoneBytes;
}

// Reads a BLOB_DONE payload, or nullopt when it is not exactly 7 bytes (drop,
// per the raw-plane rule above). An UNKNOWN status value decodes as itself
// rather than being rejected: §4.3's forward-compatibility rule applies to
// enumerations too, and a future status a peer cannot name is still a peer
// saying "this transfer ended", which is more than silence.
inline std::optional<BlobDoneMsg> decodeBlobDone(std::span<const std::byte> payload) {
    if (payload.size() != kBlobDoneBytes) return std::nullopt;
    BlobDoneMsg m{};
    m.id.ns = getU8(payload.subspan(0, 1));
    m.id.store_id = getU8(payload.subspan(1, 1));
    m.id.slot = getU8(payload.subspan(2, 1));
    m.id.has_store_id = !m.id.isCatalog();
    m.id.has_slot = !m.id.isCatalog();
    m.id.generation = getU16(payload.subspan(4, 2));
    m.id.has_generation = m.id.generation != 0;
    m.status = BlobDoneStatus(getU8(payload.subspan(6, 1)));
    return m;
}

}  // namespace slopsync
