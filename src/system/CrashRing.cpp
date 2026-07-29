#include "CrashRing.h"

#include <Arduino.h>
#include <string.h>
#include "esp_attr.h"
#include "sloplog/sloplog.h"

namespace crashring {

// One struct, fixed size, RTC_NOINIT: survives esp_restart() and the panic
// handler's reboot; a power cycle leaves garbage — which is exactly what the
// magic word screens out. Keep this comfortably under the 8 KB RTC slow
// segment; it is ~260 B today.
struct RingData {
    uint32_t magic;
    uint32_t bootSeq;
    uint32_t heapMin;
    uint32_t heapLast;
    uint32_t maxBlockLast;
    uint32_t crumbSeq;      // monotonically increasing; index = seq % kCrumbs
    Crumb    crumbs[12];
};

static constexpr uint32_t kMagic  = 0x53443332; // "SD32"
static constexpr uint32_t kCrumbs = 12;

static RTC_NOINIT_ATTR RingData s_ring;
static PrevReport s_prev; // BSS; zeroed => prevValid false until begin()

void begin(const char* reasonName, bool abnormal) {
    // Recover the previous life first — before this boot touches the ring.
    if (s_ring.magic == kMagic) {
        s_prev.prevValid    = true;
        s_prev.prevAbnormal = abnormal;
        s_prev.bootSeq      = s_ring.bootSeq;
        s_prev.heapMin      = s_ring.heapMin;
        s_prev.heapLast     = s_ring.heapLast;
        s_prev.maxBlockLast = s_ring.maxBlockLast;
        s_prev.crumbCount   = s_ring.crumbSeq;
        strncpy(s_prev.resetReason, reasonName, sizeof(s_prev.resetReason) - 1);

        // Oldest-to-newest: the ring index below crumbSeq wrapped kCrumbs times.
        const uint32_t n = s_ring.crumbSeq < kCrumbs ? s_ring.crumbSeq : kCrumbs;
        const uint32_t first = s_ring.crumbSeq - n;
        for (uint32_t i = 0; i < n; ++i) {
            const Crumb& c = s_ring.crumbs[(first + i) % kCrumbs];
            s_prev.crumbs[i] = c;
            s_prev.crumbs[i].tag[sizeof(c.tag) - 1] = '\0'; // torn-write guard
        }
        s_prev.crumbsRecovered = uint8_t(n);

        if (abnormal) {
            // The whole point of the ring: the previous boot's last words,
            // in the Warn ring where they survive long enough to be read.
            SLOGW("crash", "prev boot #%u died (%s): heap min=%u last=%u maxblock=%u, %u crumbs",
                  unsigned(s_prev.bootSeq), s_prev.resetReason,
                  unsigned(s_prev.heapMin), unsigned(s_prev.heapLast),
                  unsigned(s_prev.maxBlockLast), unsigned(s_prev.crumbCount));
            for (uint32_t i = 0; i < n; ++i) {
                SLOGW("crash", "  crumb[-%u] %-13s @%ums",
                      unsigned(n - i), s_prev.crumbs[i].tag, unsigned(s_prev.crumbs[i].tMs));
            }
        }
    }

    // Re-arm for this boot.
    const uint32_t seq = s_prev.prevValid ? s_prev.bootSeq + 1 : 1;
    memset(&s_ring, 0, sizeof(s_ring));
    s_ring.magic   = kMagic;
    s_ring.bootSeq = seq;
    s_ring.heapMin = UINT32_MAX;
}

void crumb(const char* tag) {
    const uint32_t seq = s_ring.crumbSeq;
    Crumb& c = s_ring.crumbs[seq % kCrumbs];
    strncpy(c.tag, tag, sizeof(c.tag) - 1);
    c.tag[sizeof(c.tag) - 1] = '\0';
    c.seq = uint16_t(seq);
    c.tMs = millis();
    s_ring.crumbSeq = seq + 1;
}

void heapSample(uint32_t freeNow, uint32_t maxBlockNow) {
    s_ring.heapLast     = freeNow;
    s_ring.maxBlockLast = maxBlockNow;
    if (freeNow < s_ring.heapMin) s_ring.heapMin = freeNow;
}

const PrevReport& prev() { return s_prev; }

} // namespace crashring
