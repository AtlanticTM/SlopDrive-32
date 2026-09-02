// RpFlashLink -- the S3's half of the RP2350 firmware update, on the SPI link.
// Constraints:
// - NO safety gate of its own. OtaService::prepareForOta() is the ONE gate
//   that stops motion for an update (governance amendment 2026-08-06, sd-emy
//   item b); this class is only the byte pump underneath it.
// - EXCLUSIVE with MlinkServoDriver for the duration. main.cpp holds
//   motorTask off the link while active() is true, so exactly one owner drives
//   the bus; the shared esp32-hal bus mutex is the belt under that suspenders.
// - Runs on the task that received the bytes (commsTask), so no cross-core
//   queue exists to lose them. A forward blocks that task for one bridge chunk
//   (~2.3 ms typical, up to a sector erase at a boundary) with motion already
//   stopped -- the same standing the S3's own flash writes have.
// - Steady-state allocation is zero: every buffer is a fixed member.
// See: include/comms/RpFlashCore.h, include/comms/MotionLinkProtocol.h,
//      .claude/rules/build-test-deploy.md, dev board sd-4k1.3.
#pragma once

#include <cstddef>
#include <cstdint>

#include "comms/MotionLinkProtocol.h"
#include "comms/RpFlashCore.h"

class RpFlashLink {
public:
    // Claim the bus and put the RP into flash mode for an image of `size`
    // bytes. False means the RP refused (no partition table, image too big) or
    // never answered; detail() names which.
    bool begin(uint32_t size);

    // Forward one bridge chunk, already known to be the next one in order.
    // `base` is its absolute image offset. Blocks until the RP has acknowledged
    // every byte of it or the transfer fails.
    bool push(uint32_t base, const uint8_t* data, uint32_t len);

    // Ask the RP to verify the slot and boot it. False means the read-back
    // verify failed or the bootrom refused; the RP keeps its running image.
    bool end(uint32_t crc);

    void abort();

    bool     active()   const { return _active; }
    uint32_t want()     const { return _want; }
    uint8_t  detail()   const { return _detail; }
    uint32_t rewinds()  const { return _tx.rewinds(); }
    // NUL-terminated RP firmware string from the last kOpFlashVersion reply.
    // Empty until readVersion() has succeeded. This is the C-8 instrument for
    // the coprocessor: nothing else on the S3 knows what image the RP runs.
    const char* version() const { return _version; }
    bool readVersion();

private:
    void ensureBus();
    void xfer(uint8_t* out, uint8_t* in);
    // One transaction whose reply is parsed as a flash status. False = the
    // reply was not a CRC-valid flash status.
    bool transact(uint8_t* out);
    bool pollUntilReady(uint32_t timeout_ms);

    rpflash::Sender _tx;
    bool     _busUp   = false;
    bool     _active  = false;
    uint32_t _want    = 0;
    uint8_t  _seq     = 0;
    uint8_t  _result  = motionlink::kFlashIdle;
    uint8_t  _detail  = motionlink::kFlashDetailNone;
    char     _version[motionlink::kFlashVersionBytes + 1] = {};
};
