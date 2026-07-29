#pragma once
// CrashRing — last-words diagnostics that survive a panic reboot.
//
// The 2026-07-29 incident (heap-starved httpTask -> the second unexplained
// PANIC on 2.1.86) died with no serial attached and nothing persisted: the
// only post-mortem fact was esp_reset_reason(). This ring lives in
// RTC_NOINIT memory — RAM in the RTC power domain that the panic handler's
// reboot does NOT clear (a full power cycle does) — so the NEXT boot can
// report what the previous life was doing when it died.
//
// Constraints:
// - crashringCrumb() must stay allocation-free and lock-free: it is called
//   from hot paths (WS accept on the AsyncTCP task, HTTP handlers on
//   httpTask). A torn crumb under a cross-task race costs one garbled tag in
//   a diagnostic ring; a lock here could cost a priority inversion in the
//   exact starvation scenarios this ring exists to record. Tolerated.
// - This is NOT a backtrace. A real backtrace needs a core-dump flash
//   partition, and partition tables do not change over OTA — that upgrade is
//   a serial-reflash bench item (see ledger, 2026-07-29 incident entry).
#include <stdint.h>

namespace crashring {

struct Crumb {
    char     tag[14]; // short checkpoint name, NUL-terminated
    uint16_t seq;     // low bits of the global crumb counter (wrap detection)
    uint32_t tMs;     // millis() at write
};

// Snapshot of the PREVIOUS boot's ring, valid only when prevValid is true.
struct PrevReport {
    bool     prevValid;
    bool     prevAbnormal;   // previous reset reason was not POWERON/SW
    uint32_t bootSeq;        // the previous boot's sequence number
    uint32_t heapMin;        // minimum internal free heap seen last boot
    uint32_t heapLast;       // last sampled free heap before death
    uint32_t maxBlockLast;   // last sampled largest allocatable block
    uint32_t crumbCount;     // total crumbs the previous boot wrote
    Crumb    crumbs[12];     // oldest-to-newest as recovered
    uint8_t  crumbsRecovered;
    char     resetReason[24]; // decoded reason of the reboot that ENDED that boot
};

// Call once in setup(), immediately after the reset-reason block: recovers
// the previous boot's ring (logging it via SlopLog when the reset was
// abnormal), then re-arms the ring for this boot. `reasonName` is the
// decoded esp_reset_reason() string; `abnormal` its POWERON/SW-or-not verdict.
void begin(const char* reasonName, bool abnormal);

// Drop a checkpoint. Cheap enough for per-request use; NOT for per-tick use.
void crumb(const char* tag);

// Feed the heap watermark tracker (piggybacks the existing 10 ms heap poll
// site — three u32 stores, no formatting).
void heapSample(uint32_t freeNow, uint32_t maxBlockNow);

// The recovered previous-boot report (stable for the whole boot).
const PrevReport& prev();

} // namespace crashring
