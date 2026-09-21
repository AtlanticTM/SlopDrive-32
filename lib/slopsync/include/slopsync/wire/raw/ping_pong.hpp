// slopsync-core — PING/PONG liveness frames, SPEC §6.5.
//
// Both are raw frames (frame type carries the meaning; there is no
// structure to a PING/PONG payload beyond "whatever bytes are there").
// "Any received frame is proof of life" (§6.5) — PING is the fallback sent
// only once a side has otherwise been silent for its interval (200 ms while
// holding active control, 1 s otherwise). In practice a PING carries an
// EMPTY payload; the spec nonetheless requires PONG to echo back whatever
// payload bytes the PING carried, so a future PING variant that adds a
// nonce/counter round-trips correctly without a wire-grammar change — §4.3's
// "unknown means ignore" already covers a receiver that doesn't understand
// extra PING payload bytes.
//
// There is deliberately no decode step here beyond taking the frame's
// payload as-is: PING/PONG payloads are opaque to this layer by design.
#pragma once

#include <cstddef>
#include <cstring>
#include <span>

namespace slopsync {

// Copies `payload` into `out` verbatim. Returns payload.size() (which may
// be 0 — the normal case), or 0 if `out` is too small to hold it.
inline size_t encodePing(std::span<const std::byte> payload, std::span<std::byte> out) {
    if (out.size() < payload.size()) return 0;
    if (!payload.empty()) std::memcpy(out.data(), payload.data(), payload.size());
    return payload.size();
}

// PONG's whole content is "echo the PING's payload" (§6.5) — same operation
// as encodePing, given a distinct name so call sites read as intent, not as
// a coincidence that the two frames share an encoder.
inline size_t encodePong(std::span<const std::byte> pingPayload, std::span<std::byte> out) {
    return encodePing(pingPayload, out);
}

}  // namespace slopsync
