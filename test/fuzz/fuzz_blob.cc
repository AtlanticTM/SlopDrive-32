// fuzz_blob — target: BLOB_CHUNK header + ChunkReassembler
// (wire/blob_chunks.hpp).
//
// Reassembly across chunks is where allocation bugs live, and this header is
// the one place `total_bytes` (RFC-028's know-the-size-before-you-allocate
// field) is trusted. The driver is STATEFUL on purpose: one input replays a
// whole transfer — begin from an attacker-chosen header, then a sequence of
// chunks with attacker-chosen index/count/ns/slot, interleaved with the
// timeout and missing-index queries a real client runs on its own timer.
//
// MaxChunks is the smallest realistic value (8) rather than the 64 default:
// a small backing array makes an off-by-one land inside ASan's redzone
// instead of in slack the default capacity happens to have.
#include "fuzz_common.hpp"

using namespace slopsync;
using namespace slopfuzz;

using SmallReassembler = ChunkReassembler<8>;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput fi(data, size);

    // Stateless header parse over the raw input first.
    {
        BlobChunkHeader h{};
        if (getBlobChunkHeader(asBytes(data, size), h)) {
            sink(h.total_bytes ^ h.chunk_index ^ h.chunk_count);
        }
    }

    static SmallReassembler ra;
    uint32_t nowMs = 0;

    // begin() from fuzzer-chosen identity/count/total — including counts and
    // totals far beyond this instance's capacity, which begin() is required
    // to REFUSE rather than clamp.
    BlobId id{};
    id.ns = fi.u8();
    id.store_id = fi.u8();
    id.slot = fi.u8();
    id.generation = fi.u16();
    ra.begin(id, fi.u16(), size_t(fi.u32()), nowMs);

    // Queries that a caller may legally make regardless of active() — an
    // implementation that only bounds itself inside insert() fails here.
    {
        std::array<uint16_t, 64> missing{};
        size_t m = ra.missingIndices(std::span<uint16_t>(missing));
        if (m > missing.size()) __builtin_trap();
        for (size_t i = 0; i < m; ++i) sink(missing[i]);
    }
    if (ra.complete()) sink(touch(ra.assembled()));

    while (fi.remaining() >= 3) {
        nowMs += fi.u16();
        size_t want = size_t(fi.u16()) % 300;  // header(14) + payload(192) = 206 nominal
        auto chunk = fi.take(want);
        ra.insert(chunk, nowMs);

        sink(uint32_t(ra.timedOut(nowMs)) | uint32_t(ra.gapElapsed(nowMs)) << 1);

        std::array<uint16_t, 64> missing{};
        size_t m = ra.missingIndices(std::span<uint16_t>(missing));
        if (m > missing.size()) __builtin_trap();

        if (ra.complete()) {
            auto asm_ = ra.assembled();
            // TOTALITY: the assembled view can never exceed the reassembler's
            // own backing capacity.
            if (asm_.size() > SmallReassembler::kMaxTotalBytes) __builtin_trap();
            sink(touch(asm_));
        }
    }

    // fillBlobChunk: the send side, with an attacker-influenced index.
    {
        FuzzInput f2(data, size);
        uint16_t idx = f2.u16();
        auto body = f2.rest();
        std::array<std::byte, 256> out{};
        size_t n = fillBlobChunk(id, body, idx, std::span<std::byte>(out));
        if (n > out.size()) __builtin_trap();
        sink(touch(std::span<const std::byte>(out.data(), n)));
    }
    return 0;
}
