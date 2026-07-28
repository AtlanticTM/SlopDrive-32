#pragma once

// SlopSyncDiscoveryWire — pure, hardware-free byte encode/decode for the
// SlopSync UDP discovery probe/reply and the BLE-advertising flags byte
//
// Constraints:
//   Covers the UDP discovery probe/reply (SPEC §13.8; registry
//   `udp_discovery`; frame_types 0x1E DISCOVER_PROBE / 0x1F DISCOVER_REPLY)
//   and the BLE-advertising flags byte (SPEC §13.4/§13.6; registry
//   `ble_adv_flags`) — SHARED with the UDP reply's own `flags` field, which
//   the registry's 0x1F note says carries "the same philosophy... plus the
//   endpoint" as the BLE/BEACON flag.
//
//   registry.yaml sections are cited directly rather than via
//   generated/registry_constants.hpp: the SlopSync repo's tools/gen_registry_header.py does not
//   emit `udp_discovery`/`ble_adv_flags` as C++ constants as of the RFC-046
//   landing (they are the identity/port numbers a SOCKET binds to and an
//   ADVERTISING PAYLOAD builds from, not CBOR wire numbers the codegen's
//   schema covers) — a documented fallback (LEDGER.md Phase E entry), not
//   a spec gap. The numbers below are transcribed
//   from registry.yaml verbatim and must be kept in sync by hand if that
//   file ever changes them.
//
//   No Arduino/NimBLE/socket code lives here: this is the part of the radio
//   work that is a pure function of bytes in, bytes out, and therefore
//   host-testable (test/native/test_slopsync_discovery).
//
// See:
//   SlopSyncUdpDiscovery.{h,cpp} — AsyncUDP glue built on these functions
//   SlopSyncBleTransport.{h,cpp} — NimBLE advertising glue built on these functions

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace slopdrive::discovery {

// ---- registry.yaml `udp_discovery` (RFC-046 §5) -----------------------------
inline constexpr uint16_t kPort = 21328;  // 0x5350 — ASCII "SP" ("SlopSync Probe")
inline constexpr uint32_t kReplyRateLimitPerSourceS = 1;  // one reply per source IP per second
inline constexpr std::array<uint8_t, 4> kMagic = {0x53, 0x4C, 0x4F, 0x50};  // ASCII "SLOP"

// ---- registry.yaml `ble_adv_flags` (RFC-046 §2) — shared byte ---------------
// Bits 2-7 reserved, MUST be zero (enforced structurally: buildFlags() has no
// way to set them).
inline constexpr uint8_t kFlagPairingWindowOpen = 0x01;  // bit0
inline constexpr uint8_t kFlagWsAvailable = 0x02;        // bit1

constexpr uint8_t buildFlags(bool pairingWindowOpen, bool wsAvailable) {
    uint8_t f = 0;
    if (pairingWindowOpen) f |= kFlagPairingWindowOpen;
    if (wsAvailable) f |= kFlagWsAvailable;
    return f;
}

// ---- DISCOVER_PROBE (0x1E, c2h, raw) ----------------------------------------
// magic(4) + proto_ver:u8 + nonce:u32
inline constexpr size_t kProbeBytes = 4 + 1 + 4;

struct Probe {
    uint8_t proto_ver = 0;
    uint32_t nonce = 0;  // client entropy, echoed verbatim in the reply
};

// §13.8: "reject bad magic silently" — a non-SlopSync packet landing on this
// port (or a stray retransmit/garbage) is not this protocol's problem to
// report; nullopt covers both a bad magic and a wrong-length datagram.
inline std::optional<Probe> parseProbe(std::span<const std::byte> in) {
    if (in.size() != kProbeBytes) return std::nullopt;
    for (size_t i = 0; i < kMagic.size(); ++i) {
        if (uint8_t(in[i]) != kMagic[i]) return std::nullopt;
    }
    Probe p;
    p.proto_ver = uint8_t(in[4]);
    p.nonce = uint32_t(uint8_t(in[5])) | (uint32_t(uint8_t(in[6])) << 8) |
              (uint32_t(uint8_t(in[7])) << 16) | (uint32_t(uint8_t(in[8])) << 24);
    return p;
}

// ---- DISCOVER_REPLY (0x1F, h2c, raw) — RFC-048-corrected layout, 76 bytes -
// magic(4) + nonce:u32(4) + hub_name:str32(32) + hub_instance_id:u64(8) +
// proto_ver:u8(1) + ws_port:u16(2) + fw_version:str16(16) +
// catalog_etag(8) + flags:u8(1)
//
// str16/str32 are the fixed-width, zero-padded, byte-truncated field types of
// §5.4/RFC-026 — same semantics as slopsync-core's packStringField()
// (lib/slopsync/include/slopsync/wire/packed/layout_codec.hpp), reimplemented
// here rather than shared because this header intentionally has zero
// dependency on lib/slopsync (this is firmware-side discovery-socket glue,
// not protocol-core).
inline constexpr size_t kHubNameMaxBytes = 32;
inline constexpr size_t kFwVersionMaxBytes = 16;
inline constexpr size_t kEtagBytes = 8;
inline constexpr size_t kReplyBytes =
    4 /*magic*/ + 4 /*nonce*/ + kHubNameMaxBytes + 8 /*hub_instance_id*/ + 1 /*proto_ver*/ +
    2 /*ws_port*/ + kFwVersionMaxBytes + kEtagBytes + 1 /*flags*/;
static_assert(kReplyBytes == 76, "RFC-048 pins DISCOVER_REPLY at 76 bytes — see registry.yaml frame_types 0x1F");

struct ReplyFields {
    uint32_t nonce = 0;  // echo of the probe's nonce
    std::string_view hub_name;
    uint64_t hub_instance_id = 0;
    uint8_t proto_ver = 0;
    uint16_t ws_port = 0;
    std::string_view fw_version;
    std::array<std::byte, kEtagBytes> catalog_etag{};
    uint8_t flags = 0;  // buildFlags() output
};

namespace detail {
// Truncates to `width` bytes, zero-pads the remainder — identical semantics
// to packStringField()'s doc comment (no guaranteed NUL terminator when the
// text exactly fills the field).
inline void putFixedString(std::span<std::byte> out, std::string_view text, size_t width) {
    const size_t copied = text.size() < width ? text.size() : width;
    if (copied > 0) std::memcpy(out.data(), text.data(), copied);
    if (copied < width) std::memset(out.data() + copied, 0, width - copied);
}
}  // namespace detail

// Encodes into `out` (must be >= kReplyBytes). Returns bytes written
// (== kReplyBytes), or 0 on failure (out too small) — the same "0 on failure"
// convention slopsync-core's own encoders use.
inline size_t buildReply(const ReplyFields& f, std::span<std::byte> out) {
    if (out.size() < kReplyBytes) return 0;
    size_t off = 0;
    for (uint8_t b : kMagic) out[off++] = std::byte(b);
    out[off++] = std::byte(uint8_t(f.nonce));
    out[off++] = std::byte(uint8_t(f.nonce >> 8));
    out[off++] = std::byte(uint8_t(f.nonce >> 16));
    out[off++] = std::byte(uint8_t(f.nonce >> 24));
    detail::putFixedString(out.subspan(off, kHubNameMaxBytes), f.hub_name, kHubNameMaxBytes);
    off += kHubNameMaxBytes;
    for (int i = 0; i < 8; ++i) out[off++] = std::byte(uint8_t(f.hub_instance_id >> (8 * i)));
    out[off++] = std::byte(f.proto_ver);
    out[off++] = std::byte(uint8_t(f.ws_port));
    out[off++] = std::byte(uint8_t(f.ws_port >> 8));
    detail::putFixedString(out.subspan(off, kFwVersionMaxBytes), f.fw_version, kFwVersionMaxBytes);
    off += kFwVersionMaxBytes;
    for (std::byte b : f.catalog_etag) out[off++] = b;
    out[off++] = std::byte(f.flags);
    return off;  // == kReplyBytes
}

}  // namespace slopdrive::discovery
