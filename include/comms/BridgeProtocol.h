// BridgeProtocol -- S3<->C5 link control vocabulary, one definition, two ends.
// Constraints:
// - NEVER copy a constant out of this file. Both ends include it; a second
//   copy is the T20 drift class, and this channel carries firmware images.
// - NOT SlopSync. Slot 0xFF is link machinery with no protocol duty and is
//   invisible to clients and to conformance.
// - OTA rides here precisely BECAUSE it is not SlopSync: OTA rights are never
//   derivable from a SlopSync role, so it must never touch a session channel.
// - Ops are append-only. Both ends are reflashed independently, so a removed
//   or renumbered op means a bridge talking to a host that predates it.
// See: .claude/rules/transport.md, SPEC 13.5, RFC-056/RFC-057.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bridge {

// Session slots are 0..kSlots-1; kSlot sits outside that range so the
// receiving "slot >= kSlots" guard cannot mistake control for a session frame.
// The S3 static_asserts this against slopsync::kHubMaxSessions + 1.
inline constexpr uint8_t kSlots = 5;
inline constexpr uint8_t kSlot  = 0xFF;

// Envelope: [kSlot][op][args...]
enum Op : uint8_t {
    kOpSlotClosed = 0x01,   // C5->S3  [slot]                 socket actually closed
    kOpOtaBegin   = 0x02,   // C5->S3  [target][size:u32le]   size is REQUIRED, see below
    kOpOtaData    = 0x03,   // C5->S3  [seq:u16le][bytes...]  seq is contiguous from 0
    kOpOtaEnd     = 0x04,   // C5->S3  [crc32:u32le]          IEEE, over the plaintext image
    kOpOtaAbort   = 0x05,   // either  [reason]
    kOpOtaStatus  = 0x06,   // S3->C5  [state][detail][seq:u16le]
    // Diag archive pull (sd-0gy). PULL-based on purpose: the C5 requests ONE
    // bounded batch per round trip and does not ask again until its HTTP
    // client took the last one, so the link needs no flow control and a dead
    // client stalls nothing.
    kOpDiagReq    = 0x07,   // C5->S3  [from:u32le][tag utf8...]  tag empty = all
    kOpDiagData   = 0x08,   // S3->C5  [text bytes...]            whole lines
    kOpDiagEnd    = 0x09,   // S3->C5  [next:u32le][done]         next -> next req's from
    // /uitoken mint (sd-ykg.2). Same auth argument as OTA: reaching the C5's
    // LAN HTTP is the trust boundary, the wired link adds none. The S3's own
    // HTTP mint and this one are ONE implementation behind two doors.
    kOpTokenReq   = 0x0A,   // C5->S3  []
    kOpTokenResp  = 0x0B,   // S3->C5  [code][json...]  code: 0 ok, 1 disabled, 2 rate-limited
};

enum OtaTarget : uint8_t {
    kOtaTargetApp = 0x00,   // U_FLASH
    kOtaTargetFs  = 0x01,   // U_SPIFFS (LittleFS bundle)
};

enum OtaState : uint8_t {
    kOtaIdle    = 0x00,
    kOtaReady   = 0x01,     // begin accepted, erase bounded to the declared size
    kOtaWriting = 0x02,
    kOtaDone    = 0x03,     // written and verified; the S3 reboots after this
    kOtaFailed  = 0x04,
};

enum OtaAbortReason : uint8_t {
    kOtaAbortHost      = 0x00,   // C5 gave up (client vanished mid-upload)
    kOtaAbortSeqGap    = 0x01,   // a chunk was lost; the image would be corrupt
    kOtaAbortTooBig    = 0x02,   // declared size exceeds the partition
    kOtaAbortCrc       = 0x03,
    kOtaAbortBusy      = 0x04,   // machine not idle -- OTA is refused, never interrupts
    kOtaAbortWriteFail = 0x05,
};

// The COBS body is [slot][op][seq:2][data], so data carries a 4-byte prefix.
// 480 leaves headroom under the 512 the bridge's own kMaxFrame allows.
// 240, not 480: transport.md measured 64 B frames at 100% but 242/504 B at
// only 68-74% on this link. A 480 B payload encodes to ~486 and lost the first
// 13 chunks outright (2026-08-06).
inline constexpr size_t kOtaChunkBytes = 240;

// CREDIT WINDOW -- load-bearing, not tuning. ESP-IDF erases lazily INSIDE
// write(), stalling the receiver tens of ms while its RX ring holds ~82 ms.
// Unpaced, the driver drops bytes the receiver never counts (measured
// 2026-08-06: sent 1,738,631, counted 9). 16 chunks = 7.7 KB, half the ring.
// Ack EVERY chunk: the window buys throughput, not ack thinning, and a
// self-clocking ack removes a failure class -- a thinned ack that goes missing
// stalls the window until a retransmit timer notices. An 8-byte ack per 240
// bytes is 3% overhead on a link running at 3% of line rate.
inline constexpr uint16_t kOtaAckEvery = 1;
inline constexpr uint16_t kOtaWindow   = 16;

}  // namespace bridge
