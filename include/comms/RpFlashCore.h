// RpFlashCore -- S3-to-RP2350 firmware-update state machine, both halves.
// Constraints:
// - HARDWARE-FREE and dependency-injected (architecture.md 1, module boundary
//   doctrine): the flash back end is an interface, so the host suite exercises
//   the same code the RP2350 runs. Nothing here includes Arduino or pico-sdk.
// - GO-BACK-N at both ends (T33 rule 1). The receiver accepts ONLY the chunk
//   sitting at `want` and drops every other one; the sender's next frame is
//   always the offset the last CRC-valid reply asked for. A `want` that stops
//   advancing is BACKPRESSURE, not loss -- the receiver refuses data while a
//   sector write is owed -- so the sender paces on progress, never on a timer
//   (T32 rule 2).
// - The receiver NEVER writes flash from the frame path. data() stages into a
//   RAM sector buffer and service() does the erase-plus-program from task
//   context, because a receive path that writes flash cannot also be servicing
//   the link (T33 rule 2, same class).
// - Offsets are IMAGE-relative. Where the slot lives in flash is the sink's
//   business; nothing here knows a partition address.
// See: include/comms/MotionLinkProtocol.h, .claude/rules/build-test-deploy.md,
//      dev board sd-4k1.3.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "MotionLinkProtocol.h"

namespace rpflash {

using motionlink::kFlashChunkBytes;
using motionlink::kFlashChunkOffset;
using motionlink::kFlashSectorBytes;
using motionlink::kFrameBytes;

// ---- CRC-32/ISO-HDLC --------------------------------------------------------
// The same algorithm slopsync::crc32 uses (poly 0xEDB88320 reflected, init and
// final XOR 0xFFFFFFFF), restated rather than included: the RP2350 build has no
// SlopSync include path and must not grow one for a checksum. Bitwise, not
// table-driven -- 1 KiB of table buys nothing on a transfer bounded by flash
// erase time. Known answer crc32 of "123456789" is 0xCBF43926, which the host
// suite asserts against the same vector the SlopSync suite uses.
inline constexpr uint32_t crc32Init() { return 0xFFFFFFFFu; }
inline constexpr uint32_t crc32Update(uint32_t s, const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        s ^= d[i];
        for (int b = 0; b < 8; ++b)
            s = (s & 1u) ? ((s >> 1) ^ 0xEDB88320u) : (s >> 1);
    }
    return s;
}
inline constexpr uint32_t crc32Final(uint32_t s) { return s ^ 0xFFFFFFFFu; }

// ---- Receiver, the RP2350 side ----------------------------------------------

// The injected flash back end. The RP2350 implementation talks to
// hardware/flash.h; the host suite records the calls.
struct IFlashSink {
    virtual ~IFlashSink() = default;
    // Target slot size in bytes. 0 means no A/B slot is available, which is
    // the honest report when no partition table has been written yet.
    virtual uint32_t capacity() const = 0;
    // Erase and program `n` bytes at slot-relative `offset`. `offset` is
    // always a multiple of kFlashSectorBytes and `n` is at most one sector;
    // the implementation pads a short tail.
    virtual bool writeSector(uint32_t offset, const uint8_t* data, uint32_t n) = 0;
    // Read back what writeSector landed, for the whole-image verify.
    virtual bool read(uint32_t offset, uint8_t* out, uint32_t n) = 0;
};

class Receiver {
public:
    explicit Receiver(IFlashSink& sink) : _sink(sink) {}

    // Enter flash mode for an image of `size` bytes. A size the slot cannot
    // hold is refused BEFORE anything is erased, so a too-big image never
    // leaves the running one half-destroyed.
    void begin(uint32_t size) {
        _size = size;
        _want = 0;
        _sectorBase = 0;
        _fill = 0;
        _pending = false;
        _detail = motionlink::kFlashDetailNone;
        const uint32_t cap = _sink.capacity();
        if (cap == 0) { fail(motionlink::kFlashDetailNoSlot); return; }
        if (size == 0 || size > cap) { fail(motionlink::kFlashDetailTooBig); return; }
        _result = motionlink::kFlashReady;
    }

    // Stage one chunk. Returns true only when `want` advanced; every other
    // return is a drop the sender heals by resending from `want`.
    bool data(uint32_t offset, const uint8_t* bytes, uint8_t len) {
        if (!active()) return false;
        if (_pending) return false;          // a sector write is owed first
        if (offset != _want) return false;   // go-back-N: only the next chunk
        if (len == 0 || len > kFlashChunkBytes) return false;
        uint32_t n = len;
        if (_want + n > _size) n = _size - _want;   // never past the image
        if (n == 0) return false;
        // A chunk may straddle a sector boundary. Stage the head only and let
        // `want` stop there: the sender resends the remainder from the new
        // `want` once service() has cleared the write.
        const uint32_t room = kFlashSectorBytes - (_want - _sectorBase);
        if (n > room) n = room;
        memcpy(_buf + (_want - _sectorBase), bytes, n);
        _want += n;
        _result = motionlink::kFlashWriting;
        if (_want - _sectorBase == kFlashSectorBytes || _want == _size) {
            _fill = _want - _sectorBase;
            _pending = true;
        }
        return true;
    }

    // Task-context half: perform the owed sector write. Returns false only on
    // a write failure, which ends the transfer.
    bool service() {
        if (!_pending) return true;
        if (!_sink.writeSector(_sectorBase, _buf, _fill)) {
            fail(motionlink::kFlashDetailWriteFail);
            return false;
        }
        _sectorBase += _fill;
        _fill = 0;
        _pending = false;
        return true;
    }

    // Whole-image read-back verify against the sender's CRC. Reads the slot
    // back rather than trusting a streaming CRC on purpose: the question is
    // whether the SLOT holds the image, not whether the wire delivered it.
    bool end(uint32_t crc) {
        if (!active()) return false;
        if (_pending) return false;              // service() owes a write
        if (_want != _size) { fail(motionlink::kFlashDetailShort); return false; }
        uint32_t c = crc32Init();
        uint8_t tmp[64];
        for (uint32_t off = 0; off < _size;) {
            uint32_t n = _size - off;
            if (n > sizeof(tmp)) n = uint32_t(sizeof(tmp));
            if (!_sink.read(off, tmp, n)) {
                fail(motionlink::kFlashDetailWriteFail);
                return false;
            }
            c = crc32Update(c, tmp, n);
            off += n;
        }
        if (crc32Final(c) != crc) { fail(motionlink::kFlashDetailCrc); return false; }
        _result = motionlink::kFlashDone;
        return true;
    }

    void abort() {
        _result = motionlink::kFlashIdle;
        _detail = motionlink::kFlashDetailNone;
        _pending = false;
        _want = 0;
        _size = 0;
    }

    // A failure the caller discovered outside this class, such as a bootrom
    // that refused the flash-update reboot.
    void fail(uint8_t detail) {
        _result = motionlink::kFlashFailed;
        _detail = detail;
        _pending = false;
    }

    bool active() const {
        return _result == motionlink::kFlashReady ||
               _result == motionlink::kFlashWriting;
    }
    bool     pending() const { return _pending; }
    uint32_t want()    const { return _want; }
    uint32_t size()    const { return _size; }
    uint8_t  result()  const { return _result; }
    uint8_t  detail()  const { return _detail; }

private:
    IFlashSink& _sink;
    uint32_t _size = 0;
    uint32_t _want = 0;         // next image byte the sender should send
    uint32_t _sectorBase = 0;   // image offset of _buf[0]
    uint32_t _fill = 0;         // bytes of _buf owed to flash
    bool     _pending = false;
    uint8_t  _result = motionlink::kFlashIdle;
    uint8_t  _detail = motionlink::kFlashDetailNone;
    uint8_t  _buf[kFlashSectorBytes] = {};
};

// ---- Sender, the S3 side ----------------------------------------------------
// The byte source is the CURRENT bridge chunk only: the HTTP body upstream is
// forward-only, so a `want` that rewinds behind the staged chunk is a protocol
// failure and not a retry. Holds no pointer to the caller's bytes -- they are a
// parameter, never a member (cpp-safety.md).

enum class Step : uint8_t {
    kSend,        // `out` holds the frame to transact
    kPoll,        // nothing left to send; transact a status frame for the ack
    kChunkDone,   // this bridge chunk is fully acknowledged; feed the next
    kOutOfRange,  // the ack is behind the staged chunk: unrecoverable
};

// SEND OPTIMISTICALLY, REWIND ON EVIDENCE. The link is full duplex, so the
// reply captured during transaction i was preloaded before frame i existed and
// can only report frame i-1. Waiting for each frame's own ack would therefore
// cost TWO transactions per chunk and post one resend per chunk -- the exact
// every-first-send-is-lost signature sd-6kz.1 turned out to be, arrived at by
// construction instead of by a bug. So the sender advances on send and rewinds
// only when an ack proves the previous frame did not land.
class Sender {
public:
    void begin(uint32_t size) {
        _size = size;
        _next = 0;
        _endLast = kNoFrame;
        _endPrev = kNoFrame;
        _stalls = 0;
        _resends = 0;
    }

    // `ack` is the receiver's `want` from the last CRC-valid reply. `bytes` is
    // the staged bridge chunk [base, base + len). The CRC is NOT stamped here:
    // the transport owns it.
    //
    // THE ACK IS TWO FRAMES BEHIND THE ONE BEING BUILT. The reply read during
    // transaction k was preloaded before frame k existed, so it reports frame
    // k-1; building frame k+1 from it therefore judges frame k-1. Comparing it
    // against the frame just sent would score every healthy send as a loss.
    Step step(uint32_t ack, uint32_t base, const uint8_t* bytes, uint32_t len,
              uint8_t seq, uint8_t* out) {
        if (_endPrev != kNoFrame) {
            if (ack < _endPrev) {
                _next = ack;      // that frame was dropped or refused
                ++_resends;
                ++_stalls;
            } else {
                _stalls = 0;
            }
        }
        _endPrev = _endLast;
        _endLast = kNoFrame;
        if (_next < ack) _next = ack;   // never behind what the receiver took
        // Chunk completion is decided by the ACK, never by the optimistic
        // pointer: moving on while the last frame is unproven would strand the
        // receiver behind a base the forward-only body can no longer serve.
        if (ack >= _size || ack >= base + len) {
            _endPrev = kNoFrame;
            return Step::kChunkDone;
        }
        if (ack < base) return Step::kOutOfRange;
        if (_next >= _size || _next >= base + len) return Step::kPoll;
        uint32_t n = base + len - _next;
        if (n > kFlashChunkBytes) n = uint32_t(kFlashChunkBytes);
        out[0] = motionlink::kOpFlashData;
        out[1] = seq;
        out[2] = uint8_t(_next);
        out[3] = uint8_t(_next >> 8);
        out[4] = uint8_t(_next >> 16);
        out[5] = uint8_t(n);
        memcpy(out + kFlashChunkOffset, bytes + (_next - base), n);
        // Zero the rest of the frame: a short chunk must not carry the
        // previous frame's bytes under the CRC.
        memset(out + kFlashChunkOffset + n, 0,
               motionlink::kCrcOffset - kFlashChunkOffset - n);
        _endLast = _next + n;
        _next += n;
        return Step::kSend;
    }

    // CONSECUTIVE rewinds: nonzero means the receiver is not taking bytes
    // right now, so the caller backs off instead of flooding its RX ring.
    uint32_t stalls() const { return _stalls; }
    // TOTAL rewinds. Sector backpressure contributes roughly two per 4 KB by
    // design, so the floor is 2 * sectors; only the excess is wire loss. Named
    // for what it measures rather than "resends", which would read as loss.
    uint32_t rewinds() const { return _resends; }

private:
    static constexpr uint32_t kNoFrame = 0xFFFFFFFFu;
    uint32_t _size = 0;
    uint32_t _next = 0;          // optimistic: what to send, not what landed
    uint32_t _endLast = kNoFrame;   // end of the frame sent one call ago
    uint32_t _endPrev = kNoFrame;   // end of the frame the next ack judges
    uint32_t _stalls = 0;
    uint32_t _resends = 0;
};

// ---- Status overlay accessors -----------------------------------------------
// One writer and one reader for the flash fields, so neither end transcribes an
// offset (T20).

inline void writeFlashStatus(uint8_t* frame, uint32_t want, uint8_t result,
                             uint8_t detail) {
    frame[motionlink::kFlashStatusOffWant + 0] = uint8_t(want);
    frame[motionlink::kFlashStatusOffWant + 1] = uint8_t(want >> 8);
    frame[motionlink::kFlashStatusOffWant + 2] = uint8_t(want >> 16);
    frame[motionlink::kFlashStatusOffWant + 3] = uint8_t(want >> 24);
    frame[motionlink::kFlashStatusOffResult] = result;
    frame[motionlink::kFlashStatusOffDetail] = detail;
}

inline uint32_t readFlashWant(const uint8_t* frame) {
    return uint32_t(frame[motionlink::kFlashStatusOffWant + 0]) |
           (uint32_t(frame[motionlink::kFlashStatusOffWant + 1]) << 8) |
           (uint32_t(frame[motionlink::kFlashStatusOffWant + 2]) << 16) |
           (uint32_t(frame[motionlink::kFlashStatusOffWant + 3]) << 24);
}

}  // namespace rpflash
