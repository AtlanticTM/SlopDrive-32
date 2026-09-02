// RpFlashCore -- hardware-free suite for the S3-to-RP2350 update path.
// Constraints:
// - Exercises the SHIPPING state machines, not a model of them: the injected
//   flash sink is the only thing swapped out (architecture.md 1).
// - Drives the two halves against each other through real 32-byte frames, so
//   the go-back-N contract is tested end to end rather than per side.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "RpFlashCore.h"

using namespace rpflash;
namespace ml = motionlink;

namespace {

// Injected flash: a byte vector plus the call census the traps care about.
struct FakeFlash final : IFlashSink {
    std::vector<uint8_t> mem;
    uint32_t sectorWrites = 0;
    uint32_t failAtWrite = 0;   // 0 = never fail
    bool     readFails = false;

    explicit FakeFlash(uint32_t cap) : mem(cap, 0xFFu) {}

    uint32_t capacity() const override { return uint32_t(mem.size()); }

    bool writeSector(uint32_t offset, const uint8_t* data, uint32_t n) override {
        ++sectorWrites;
        if (failAtWrite != 0 && sectorWrites >= failAtWrite) return false;
        REQUIRE(offset % kFlashSectorBytes == 0);
        REQUIRE(n <= kFlashSectorBytes);
        REQUIRE(offset + n <= mem.size());
        memcpy(mem.data() + offset, data, n);
        return true;
    }

    bool read(uint32_t offset, uint8_t* out, uint32_t n) override {
        if (readFails) return false;
        REQUIRE(offset + n <= mem.size());
        memcpy(out, mem.data() + offset, n);
        return true;
    }
};

struct NoSlot final : IFlashSink {
    uint32_t capacity() const override { return 0; }
    bool writeSector(uint32_t, const uint8_t*, uint32_t) override { return false; }
    bool read(uint32_t, uint8_t*, uint32_t) override { return false; }
};

std::vector<uint8_t> makeImage(uint32_t n) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = uint8_t((i * 31u + (i >> 5)) & 0xFFu);
    return v;
}

uint32_t imageCrc(const std::vector<uint8_t>& v) {
    return crc32Final(crc32Update(crc32Init(), v.data(), v.size()));
}

// Drive one whole image through both halves over real frames, the way the S3
// forwarder does: one bridge chunk staged at a time, `want` read back from the
// receiver, service() run from "task context" between transactions.
// `dropEvery` corrupts every Nth frame's CRC to exercise the drop-and-resend
// path (0 = a clean wire).
struct Link {
    uint32_t transactions = 0;
    uint32_t serviced = 0;
    uint32_t polls = 0;

    bool run(Receiver& rx, Sender& tx, const std::vector<uint8_t>& img,
             uint32_t bridgeChunk, uint32_t dropEvery) {
        rx.begin(uint32_t(img.size()));
        tx.begin(uint32_t(img.size()));
        if (!rx.active()) return false;

        uint32_t base = 0;
        uint32_t guard = 0;
        uint32_t ack = 0;          // what the reply in hand reports
        uint32_t pending_ack = 0;  // what the next reply will report
        while (base < img.size()) {
            const uint32_t len =
                (img.size() - base < bridgeChunk) ? uint32_t(img.size() - base)
                                                  : bridgeChunk;
            // The ACK LAGS BY ONE TRANSACTION, exactly as the full-duplex link
            // does: the reply captured while frame i is clocked out was
            // preloaded before frame i existed. Without this the harness would
            // pass a sender that only works against an impossible receiver.
            for (;;) {
                if (++guard > 4000000u) return false;   // livelock tripwire
                uint8_t frame[ml::kFrameBytes] = {};
                const Step s = tx.step(ack, base, img.data() + base, len,
                                       uint8_t(transactions), frame);
                if (s == Step::kChunkDone) break;
                if (s == Step::kOutOfRange) return false;
                ++transactions;
                if (s == Step::kSend) {
                    ml::crcStamp(std::span<uint8_t, ml::kFrameBytes>(
                        frame, ml::kFrameBytes));
                    const bool corrupt =
                        dropEvery != 0 && (transactions % dropEvery) == 0;
                    if (corrupt) frame[ml::kCrcOffset] ^= 0xFFu;
                    // A frame that fails CRC is dropped WHOLE, exactly as
                    // processFrame does; nothing is partially parsed.
                    if (ml::crcOk(std::span<const uint8_t, ml::kFrameBytes>(
                            frame, ml::kFrameBytes))) {
                        const uint32_t off = uint32_t(frame[2]) |
                                             (uint32_t(frame[3]) << 8) |
                                             (uint32_t(frame[4]) << 16);
                        rx.data(off, frame + kFlashChunkOffset, frame[5]);
                    }
                } else {
                    ++polls;
                }
                ack = pending_ack;         // what the reply carried
                pending_ack = rx.want();   // what the NEXT reply will carry
                // Task-context half, between transactions.
                if (rx.pending()) {
                    ++serviced;
                    if (!rx.service()) return false;
                }
            }
            base += len;
        }
        while (rx.pending()) {
            ++serviced;
            if (!rx.service()) return false;
        }
        return true;
    }
};

}  // namespace

TEST_CASE("crc32 matches the canonical CRC-32/ISO-HDLC check value") {
    const char* v = "123456789";
    CHECK(crc32Final(crc32Update(crc32Init(),
                                 reinterpret_cast<const uint8_t*>(v), 9)) ==
          0xCBF43926u);
}

TEST_CASE("frame budget leaves the CRC field alone") {
    CHECK(kFlashChunkOffset + kFlashChunkBytes == ml::kCrcOffset);
    CHECK(kFlashChunkBytes == 24);
}

TEST_CASE("a clean transfer lands byte-exact and verifies") {
    auto img = makeImage(9000);   // straddles two sector boundaries
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    Sender tx;
    Link link;
    REQUIRE(link.run(rx, tx, img, 240, 0));
    CHECK(rx.want() == img.size());
    // Two sector writes happen mid-stream here and each one freezes the ack
    // while it runs, which the sender heals by rewinding. That is the FLOOR,
    // not loss; the sub-sector case below is the clean-wire proof.
    CHECK(tx.rewinds() <= 6);
    CHECK(rx.end(imageCrc(img)));
    CHECK(rx.result() == ml::kFlashDone);
    CHECK(memcmp(flash.mem.data(), img.data(), img.size()) == 0);
    // 9000 bytes is three sectors: two full, one short tail.
    CHECK(flash.sectorWrites == 3);
}

TEST_CASE("a clean transfer sends each byte ONCE") {
    auto img = makeImage(2000);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    Sender tx;
    Link link;
    REQUIRE(link.run(rx, tx, img, 240, 0));
    CHECK(tx.rewinds() == 0);
    // One frame per 24 bytes plus one poll per 240-byte bridge chunk. Judging
    // the ack against the frame just sent instead of the one before it would
    // double this and post a resend per chunk -- the sd-6kz.1 signature.
    const uint32_t frames = (uint32_t(img.size()) + kFlashChunkBytes - 1) /
                            uint32_t(kFlashChunkBytes);
    CHECK(link.polls == 9);
    CHECK(link.transactions == frames + link.polls);
}

TEST_CASE("a dropped frame costs a bounded rewind, never a hole") {
    auto img = makeImage(5000);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    Sender tx;
    Link link;
    REQUIRE(link.run(rx, tx, img, 240, 7));   // every 7th frame corrupted
    CHECK(rx.want() == img.size());
    CHECK(tx.rewinds() > 0);
    CHECK(rx.end(imageCrc(img)));
    CHECK(memcmp(flash.mem.data(), img.data(), img.size()) == 0);
}

TEST_CASE("the receiver is go-back-N: only the chunk at want is accepted") {
    auto img = makeImage(512);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    rx.begin(uint32_t(img.size()));
    REQUIRE(rx.want() == 0);
    // Ahead of want: a hole would brick the image, so it is refused.
    CHECK_FALSE(rx.data(kFlashChunkBytes, img.data(), uint8_t(kFlashChunkBytes)));
    CHECK(rx.want() == 0);
    CHECK(rx.data(0, img.data(), uint8_t(kFlashChunkBytes)));
    CHECK(rx.want() == kFlashChunkBytes);
    // Behind want: a lost-ack duplicate. Dropped, and want does not move back.
    CHECK_FALSE(rx.data(0, img.data(), uint8_t(kFlashChunkBytes)));
    CHECK(rx.want() == kFlashChunkBytes);
}

TEST_CASE("a pending sector write is backpressure, not loss") {
    auto img = makeImage(kFlashSectorBytes + 64);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    rx.begin(uint32_t(img.size()));
    uint32_t off = 0;
    while (!rx.pending()) {
        const uint32_t n = (img.size() - off < kFlashChunkBytes)
                               ? uint32_t(img.size() - off)
                               : uint32_t(kFlashChunkBytes);
        REQUIRE(rx.data(off, img.data() + off, uint8_t(n)));
        off += n;
    }
    CHECK(rx.want() == kFlashSectorBytes);   // stopped ON the sector boundary
    // Every further chunk is refused while the write is owed, and `want` holds
    // still: the sender reads that as "resend", never as "advance".
    const uint32_t held = rx.want();
    CHECK_FALSE(rx.data(held, img.data() + held, 8));
    CHECK(rx.want() == held);
    REQUIRE(rx.service());
    CHECK_FALSE(rx.pending());
    CHECK(rx.data(held, img.data() + held, 8));
    CHECK(rx.want() == held + 8);
}

TEST_CASE("a chunk straddling a sector boundary advances only to the boundary") {
    auto img = makeImage(kFlashSectorBytes + 256);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    rx.begin(uint32_t(img.size()));
    // Land want two bytes short of the boundary, then offer a full chunk.
    const uint32_t stop = kFlashSectorBytes - 2;
    uint32_t off = 0;
    while (off < stop) {
        uint32_t n = stop - off;
        if (n > kFlashChunkBytes) n = uint32_t(kFlashChunkBytes);
        REQUIRE(rx.data(off, img.data() + off, uint8_t(n)));
        off += n;
    }
    REQUIRE(rx.want() == stop);
    CHECK(rx.data(stop, img.data() + stop, uint8_t(kFlashChunkBytes)));
    CHECK(rx.want() == kFlashSectorBytes);   // head only; the tail is resent
    CHECK(rx.pending());
}

TEST_CASE("end refuses a short image and never marks it done") {
    auto img = makeImage(1000);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    rx.begin(uint32_t(img.size()));
    REQUIRE(rx.data(0, img.data(), uint8_t(kFlashChunkBytes)));
    CHECK_FALSE(rx.end(imageCrc(img)));
    CHECK(rx.result() == ml::kFlashFailed);
    CHECK(rx.detail() == ml::kFlashDetailShort);
}

TEST_CASE("a corrupted slot fails the read-back verify") {
    auto img = makeImage(3000);
    FakeFlash flash(64 * 1024);
    Receiver rx(flash);
    Sender tx;
    Link link;
    REQUIRE(link.run(rx, tx, img, 240, 0));
    flash.mem[1234] ^= 0x01u;   // the slot, not the wire
    CHECK_FALSE(rx.end(imageCrc(img)));
    CHECK(rx.result() == ml::kFlashFailed);
    CHECK(rx.detail() == ml::kFlashDetailCrc);
}

TEST_CASE("an oversized image is refused before anything is erased") {
    FakeFlash flash(4096);
    Receiver rx(flash);
    rx.begin(8192);
    CHECK(rx.result() == ml::kFlashFailed);
    CHECK(rx.detail() == ml::kFlashDetailTooBig);
    CHECK(flash.sectorWrites == 0);
    CHECK_FALSE(rx.data(0, flash.mem.data(), 4));
}

TEST_CASE("no partition table reports NoSlot rather than pretending to write") {
    NoSlot none;
    Receiver rx(none);
    rx.begin(1024);
    CHECK(rx.result() == ml::kFlashFailed);
    CHECK(rx.detail() == ml::kFlashDetailNoSlot);
}

TEST_CASE("a failing sector write ends the transfer, it does not spin") {
    auto img = makeImage(kFlashSectorBytes * 2);
    FakeFlash flash(64 * 1024);
    flash.failAtWrite = 1;
    Receiver rx(flash);
    Sender tx;
    Link link;
    CHECK_FALSE(link.run(rx, tx, img, 240, 0));
    CHECK(rx.result() == ml::kFlashFailed);
    CHECK(rx.detail() == ml::kFlashDetailWriteFail);
}

TEST_CASE("the sender rewinds nowhere: a want behind the chunk is fatal") {
    auto img = makeImage(1024);
    Sender tx;
    tx.begin(uint32_t(img.size()));
    uint8_t frame[ml::kFrameBytes] = {};
    // Staged chunk starts at 240; the receiver asking for 0 cannot be served
    // because the HTTP body upstream is forward-only.
    CHECK(tx.step(0, 240, img.data() + 240, 240, 0, frame) == Step::kOutOfRange);
    CHECK(tx.step(240, 240, img.data() + 240, 240, 0, frame) == Step::kSend);
    CHECK(tx.step(480, 240, img.data() + 240, 240, 0, frame) == Step::kChunkDone);
}

TEST_CASE("a frozen ack is counted so the caller can back off") {
    auto img = makeImage(1024);
    Sender tx;
    tx.begin(uint32_t(img.size()));
    uint8_t frame[ml::kFrameBytes] = {};
    // A frozen ack is what a sector erase looks like from here. The first two
    // calls carry no verdict yet: the ack is two frames behind by design.
    CHECK(tx.step(0, 0, img.data(), 240, 0, frame) == Step::kSend);
    CHECK(tx.stalls() == 0);
    for (int i = 0; i < 6; ++i) tx.step(0, 0, img.data(), 240, 0, frame);
    CHECK(tx.stalls() >= 4);
    CHECK(tx.rewinds() >= 4);
    tx.step(24, 0, img.data(), 240, 0, frame);
    tx.step(48, 0, img.data(), 240, 0, frame);
    CHECK(tx.stalls() == 0);   // progress resets the backoff
}

TEST_CASE("a short tail frame zeroes its padding under the CRC") {
    auto img = makeImage(10);
    Sender tx;
    tx.begin(10);
    uint8_t frame[ml::kFrameBytes];
    memset(frame, 0xAA, sizeof(frame));
    REQUIRE(tx.step(0, 0, img.data(), 10, 0, frame) == Step::kSend);
    CHECK(frame[5] == 10);
    for (size_t i = kFlashChunkOffset + 10; i < ml::kCrcOffset; ++i)
        CHECK(frame[i] == 0);
}

TEST_CASE("the status overlay round-trips want through a real frame") {
    uint8_t frame[ml::kFrameBytes] = {};
    writeFlashStatus(frame, 0x00ABCDEFu, ml::kFlashWriting, ml::kFlashDetailNone);
    CHECK(readFlashWant(frame) == 0x00ABCDEFu);
    CHECK(frame[ml::kFlashStatusOffResult] == ml::kFlashWriting);
    ml::crcStamp(std::span<uint8_t, ml::kFrameBytes>(frame, ml::kFrameBytes));
    CHECK(ml::crcOk(std::span<const uint8_t, ml::kFrameBytes>(frame,
                                                             ml::kFrameBytes)));
}
