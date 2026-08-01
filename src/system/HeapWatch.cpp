// HeapWatch — see include/system/HeapWatch.h for why this exists.

#include "HeapWatch.h"

#if defined(SLOPSYNC_HEAP_BISECT)

#include <stdlib.h>   // free() when the quarantine ring wraps

#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sloplog/sloplog.h"

namespace heapwatch {
namespace {

// Watchpoint 1 is the FreeRTOS stack canary (see the header). Naming it here
// rather than writing a bare 0 at the call site is the whole point.
constexpr int kOurWatchpoint = 0;

const void*        s_addr = nullptr;
size_t             s_size = 0;
uint32_t           s_arms = 0;
bool               s_selftestFailed = false;
volatile uint32_t  s_bait[16] __attribute__((aligned(64))) = {};

// Eight corpses held at ~200 B each is under 2 KB against ~40 KB free — small
// enough not to move the heap pressure the reproduction runs at, which would
// change the thing being measured (TRAPS T27's rule applied to memory rather
// than to CPU).
constexpr size_t kQuarantineDepth = 8;
void*    s_quarantine[kQuarantineDepth] = {};
size_t   s_qNext = 0;
uint32_t s_qHeld = 0;
uint32_t s_qEvicted = 0;

}  // namespace

bool arm(const void* addr, size_t size) {
    if (addr == nullptr) return false;

    // Round the region DOWN to a legal power of two. esp_cpu_set_watchpoint
    // rejects anything else outright, and a rejected arm is an instrument that
    // is off while the readout says it is on.
    size_t region = 1;
    while ((region << 1) <= size && (region << 1) <= 64) region <<= 1;

    // Natural alignment is required, so the window covers `addr` but generally
    // starts before it. Reported honestly by armedAt(); do not assume the arm
    // began where you asked.
    const uintptr_t start = uintptr_t(addr) & ~uintptr_t(region - 1);

    const esp_err_t err = esp_cpu_set_watchpoint(
        kOurWatchpoint, reinterpret_cast<const void*>(start), region, ESP_CPU_WATCHPOINT_STORE);
    if (err != ESP_OK) {
        SLOGE("heapwatch", "arm FAILED (err=0x%x) for 0x%08x size %u",
              int(err), unsigned(start), unsigned(region));
        return false;
    }

    s_addr = reinterpret_cast<const void*>(start);
    s_size = region;
    ++s_arms;
    return true;
}

void disarm() {
    esp_cpu_clear_watchpoint(kOurWatchpoint);
    s_addr = nullptr;
    s_size = 0;
}

void quarantine(void* mem, size_t size) {
    if (mem == nullptr) return;

    // Release the corpse this slot was holding — genuinely, with the operator
    // that matches the `new` its caller used. From here on it is an ordinary
    // freed block again and is no longer protected.
    // free(), not ::operator delete: the quarantining allocator hands out
    // heap_caps_aligned_alloc storage so the watchpoint window can sit at
    // offset 0, and that must come back through the C allocator.
    if (s_quarantine[s_qNext] != nullptr) {
        free(s_quarantine[s_qNext]);
        ++s_qEvicted;
        --s_qHeld;
    }
    s_quarantine[s_qNext] = mem;
    s_qNext = (s_qNext + 1) % kQuarantineDepth;
    ++s_qHeld;

    // ARM ONLY A WINDOW THAT LIES ENTIRELY INSIDE THE BLOCK. arm() aligns its
    // start DOWN (the hardware demands natural alignment), so an unaligned
    // block puts the window over the PREVIOUS heap block's header — which TLSF
    // rewrites on every coalesce. That cost 14 false positives in one run on
    // fw 2.3.10, every one of them landing in block_absorb and looking exactly
    // like a find. Refuse instead of arming something that cannot be trusted;
    // the caller's allocator is responsible for handing out aligned storage.
    //
    // Offset 0 ON PURPOSE once it is aligned: if this block were genuinely free,
    // TLSF's next_free / prev_free would live in exactly these first bytes —
    // the memory the surviving corruption smashes.
    if ((uintptr_t(mem) % 64u) != 0 || size < 64u) {
        SLOGW_EVERY_MS(2000, "heapwatch",
                       "NOT ARMED: block 0x%08x size %u is not 64-aligned",
                       unsigned(uintptr_t(mem)), unsigned(size));
        return;
    }
    arm(mem, 64);
}

uint32_t quarantined()       { return s_qHeld; }
uint32_t quarantineEvicted() { return s_qEvicted; }

const void* armedAt()   { return s_addr; }
size_t      armedSize() { return s_size; }
uint32_t    arms()      { return s_arms; }
bool        selftestFailed() { return s_selftestFailed; }

void selftest() {
    // Core is named in the log because the watchpoint is per-core: a selftest
    // that passes on Core 0 says nothing about Core 1.
    SLOGW("heapwatch", "SELFTEST arming on core %d — the device SHOULD panic now",
          int(xPortGetCoreID()));
    if (!arm(const_cast<const uint32_t*>(&s_bait[0]), 64)) {
        s_selftestFailed = true;
        SLOGE("heapwatch", "SELFTEST could not arm — instrument is dead");
        return;
    }

    s_bait[0] = 0xDEADBEEF;   // must trap here

    // Unreachable on working hardware.
    disarm();
    s_selftestFailed = true;
    SLOGE("heapwatch", "SELFTEST STORE DID NOT TRAP — watchpoint 0 is not working. "
                       "Every negative result from this instrument is worthless.");
}

}  // namespace heapwatch

#else   // !SLOPSYNC_HEAP_BISECT

namespace heapwatch {
bool        arm(const void*, size_t) { return false; }
void        disarm()                 {}
void        quarantine(void*, size_t) {}
uint32_t    quarantined()            { return 0; }
uint32_t    quarantineEvicted()      { return 0; }
const void* armedAt()                { return nullptr; }
size_t      armedSize()              { return 0; }
uint32_t    arms()                   { return 0; }
void        selftest()               {}
bool        selftestFailed()         { return false; }
}  // namespace heapwatch

#endif
