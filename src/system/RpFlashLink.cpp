// RpFlashLink -- byte pump for the RP2350 firmware update over the SPI link.
// Constraints:
// - GO-BACK-N sender (T33 rule 1): the next frame is always the offset the
//   RP's last CRC-valid reply asked for. Nothing is tracked as "outstanding",
//   so a stalled `want` heals in one transaction.
// - PACE ON PROGRESS, never on a timer (T32 rule 2). A `want` that has not
//   moved for a few frames means the RP is erasing a sector; the backoff stops
//   the flood into its 256-byte RX ring, and nothing here fires a retransmit
//   because a byte is merely unacknowledged.
// - Constraint set and ownership rules: include/system/RpFlashLink.h.
#include "system/RpFlashLink.h"

#include <Arduino.h>
#include "motion/MlinkServoDriver.h"
#include "system/CrashRing.h"
#include <cstring>

#include "sloplog/sloplog.h"

using namespace motionlink;

namespace {

// The S3 end of the link. These MUST match the pin set src/motion/
// MlinkServoDriver.cpp names and docs/rp2350-wiring.md records (SCK 7, MISO 10,
// MOSI 38, CS 48); that driver owns the bus in normal operation and this class
// only borrows it while motion is stopped.
// TODO(sd-4k1.3): one home for these once the driver and this file can move in
// the same commit -- two copies of a pin map is the T20 class.

// The slave block-resets its SPI per frame and its 20 kHz tick must fire
// inside the gap; 200 us is the value measured to stop tearing under load
// (MlinkServoDriver.cpp). Not shortened for the flash path: an unproven faster
// gap would be a second variable in an already hardware-unverified feature.
// Consecutive resends of one offset before backing off. Four frames is under
// the RP's 8-frame RX ring, so the ring never wraps during a stall.
constexpr uint32_t kStallBackoff = 4;
// Above the RP's worst sector erase plus the S3's own scheduling jitter. A
// bridge chunk is 10 frames, so this is many times the honest worst case.
constexpr uint32_t kChunkTimeoutMs = 3000;
constexpr uint32_t kReadyTimeoutMs = 2000;
// The verify reads the whole slot back before the reboot, so `end` waits far
// longer than any other step.
constexpr uint32_t kEndTimeoutMs = 15000;

}  // namespace

void RpFlashLink::ensureBus() {
    // The bus belongs to MlinkServoDriver's owner task. Post the stand-off and
    // WAIT for its acknowledgment: a second SPIClass on the same bus from the
    // other core took the S3 down with a task watchdog on the first live
    // attempt (2026-09-13). One bus object, one lock, one caller at a time.
    MlinkServoDriver::standoff(true);
    for (int i = 0; i < 200 && !MlinkServoDriver::standingOff(); ++i)
        vTaskDelay(pdMS_TO_TICKS(1));
    _busUp = MlinkServoDriver::standingOff();
    if (!_busUp) SLOGE("rpflash", "link owner never stood off; refusing the bus");
}

void RpFlashLink::release() {
    _active.store(false, std::memory_order_release);
    MlinkServoDriver::standoff(false);
    _busUp = false;
}

void RpFlashLink::xfer(uint8_t* out, uint8_t* in) {
    MlinkServoDriver::busXfer(*reinterpret_cast<uint8_t (*)[kFrameBytes]>(out),
                              *reinterpret_cast<uint8_t (*)[kFrameBytes]>(in));
}

bool RpFlashLink::transact(uint8_t* out) {
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    if (!crcOk(std::span<const uint8_t, kFrameBytes>(in, kFrameBytes)))
        return false;
    if (in[0] != kStateFlash) return false;
    _want   = rpflash::readFlashWant(in);
    _result = in[kFlashStatusOffResult];
    _detail = in[kFlashStatusOffDetail];
    memcpy(_version, &in[kFlashStatusOffVersion], kFlashVersionBytes);
    _version[kFlashVersionBytes] = '\0';
    return true;
}

bool RpFlashLink::pollUntilReady(uint32_t timeout_ms) {
    const uint32_t deadline = millis() + timeout_ms;
    while (int32_t(millis() - deadline) < 0) {
        uint8_t out[kFrameBytes] = {kOpFlashStatus, ++_seq};
        if (transact(out)) {
            if (_result == kFlashFailed) return false;
            if (_result == kFlashReady || _result == kFlashWriting ||
                _result == kFlashDone)
                return true;
        }
        delay(2);
    }
    return false;
}

bool RpFlashLink::begin(uint32_t size) {
    crashring::crumb("rpf-begin");
    _active.store(true, std::memory_order_release);   // the owner's stand-off cue
    ensureBus();
    if (!_busUp) {
        release();
        _detail = kFlashDetailNone;
        return false;
    }
    _tx.begin(size);
    _want   = 0;
    _result = kFlashIdle;
    _detail = kFlashDetailNone;

    uint8_t out[kFrameBytes] = {kOpFlashBegin, ++_seq};
    memcpy(&out[2], &size, 4);
    (void)transact(out);
    crashring::crumb("rpf-poll");
    if (!pollUntilReady(kReadyTimeoutMs)) {
        SLOGE("rpflash", "RP refused the update (result %u detail %u)",
              unsigned(_result), unsigned(_detail));
        release();
        return false;
    }
    crashring::crumb("rpf-ready");
    SLOGI("rpflash", "RP in flash mode for %u B, running fw '%s'",
          unsigned(size), _version);
    return true;
}

bool RpFlashLink::push(uint32_t base, const uint8_t* data, uint32_t len) {
    if (!active()) return false;
    const uint32_t deadline = millis() + kChunkTimeoutMs;
    for (;;) {
        uint8_t out[kFrameBytes] = {};
        const rpflash::Step st = _tx.step(_want, base, data, len, ++_seq, out);
        if (st == rpflash::Step::kChunkDone) return true;
        if (st == rpflash::Step::kPoll) {
            // Everything staged is on the wire; one status frame collects the
            // last ack. This is the whole cost of sending optimistically.
            out[0] = kOpFlashStatus;
            out[1] = ++_seq;
        }
        if (st == rpflash::Step::kOutOfRange) {
            SLOGE("rpflash", "RP rewound to %u behind chunk base %u -- the "
                  "upstream body is forward-only, so this cannot be served",
                  unsigned(_want), unsigned(base));
            release();
            return false;
        }
        transact(out);
        if (_result == kFlashFailed) {
            SLOGE("rpflash", "RP failed mid-image at %u (detail %u)",
                  unsigned(_want), unsigned(_detail));
            release();
            return false;
        }
        // Backpressure, not loss: the RP stops advancing `want` while a sector
        // write is owed, and its RX ring holds 8 frames.
        if (_tx.stalls() > kStallBackoff) delay(2);
        if (int32_t(millis() - deadline) > 0) {
            SLOGE("rpflash", "RP stalled at %u for %u ms (%u resends)",
                  unsigned(_want), unsigned(kChunkTimeoutMs), unsigned(rewinds()));
            release();
            return false;
        }
    }
}

bool RpFlashLink::end(uint32_t crc) {
    if (!active()) return false;
    uint8_t out[kFrameBytes] = {kOpFlashEnd, ++_seq};
    memcpy(&out[2], &crc, 4);
    (void)transact(out);

    // The RP reads the whole slot back before it answers, then reboots into it.
    const uint32_t deadline = millis() + kEndTimeoutMs;
    while (int32_t(millis() - deadline) < 0) {
        uint8_t poll[kFrameBytes] = {kOpFlashStatus, ++_seq};
        if (transact(poll)) {
            if (_result == kFlashDone) {
                SLOGI("rpflash", "RP verified the slot and is booting it "
                      "(%u rewinds; ~2 per 4 KB sector is backpressure, the "
                      "excess is wire loss)", unsigned(rewinds()));
                release();
                return true;
            }
            if (_result == kFlashFailed) {
                SLOGE("rpflash", "RP verify FAILED (detail %u) -- slot not "
                      "bought, running image intact", unsigned(_detail));
                release();
                return false;
            }
        }
        delay(5);
    }
    SLOGE("rpflash", "RP never reported a verify result");
    release();
    return false;
}

void RpFlashLink::abort() {
    if (!_busUp) return;
    uint8_t out[kFrameBytes] = {kOpFlashAbort, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(out, in);
    release();
    _result = kFlashIdle;
}

bool RpFlashLink::readVersion() {
    ensureBus();
    // TWO transactions: the slave preloads its reply AFTER processing a frame,
    // so the answer to a request rides the transaction that follows it.
    uint8_t req[kFrameBytes] = {kOpFlashVersion, ++_seq};
    uint8_t in[kFrameBytes] = {};
    xfer(req, in);
    uint8_t poll[kFrameBytes] = {kOpPing, ++_seq};
    xfer(poll, in);
    // The version overlays the tail of an ORDINARY status frame; only the CRC
    // gates whether the bytes are trustworthy.
    if (!crcOk(std::span<const uint8_t, kFrameBytes>(in, kFrameBytes)))
        return false;
    memcpy(_version, &in[kFlashStatusOffVersion], kFlashVersionBytes);
    _version[kFlashVersionBytes] = '\0';
    return _version[0] != '\0';
}
