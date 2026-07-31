#include "OomHook.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <Arduino.h>

#include "CrashRing.h"

namespace oomhook {
namespace {

// Plain statics, written from a context that may be an ISR. `volatile` on the
// counter only: it is the publication flag, and the fields are read by take()
// which tolerates a torn record far better than the alternative (a lock here
// could deadlock the very allocator failure it is reporting).
volatile uint32_t g_count = 0;
uint32_t          g_taken = 0;
Record            g_rec{};

void IRAM_ATTR onAllocFailed(size_t size, uint32_t caps, const char* fn) {
    g_rec.size        = (uint32_t)size;
    g_rec.caps        = caps;
    g_rec.free_int    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    g_rec.largest_int = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    g_rec.t_ms        = millis();
    g_rec.task[0]     = '\0';
    if (!xPortInIsrContext()) {
        const char* n = pcTaskGetName(nullptr);
        if (n) { strncpy(g_rec.task, n, sizeof(g_rec.task) - 1);
                 g_rec.task[sizeof(g_rec.task) - 1] = '\0'; }
    }
    g_rec.fn[0] = '\0';
    if (fn) { strncpy(g_rec.fn, fn, sizeof(g_rec.fn) - 1);
              g_rec.fn[sizeof(g_rec.fn) - 1] = '\0'; }
    g_count++;
    g_rec.count = g_count;
    // The one durable side effect: a crumb survives the reboot even if the
    // abort lands before anything can log.
    crashring::crumb("oom");
}

}  // namespace

void begin() { heap_caps_register_failed_alloc_callback(onAllocFailed); }

uint32_t count() { return g_count; }

bool peek(Record& out) {
    if (g_count == 0) return false;
    out = g_rec;
    out.count = g_count;
    return true;
}

bool take(Record& out) {
    const uint32_t c = g_count;
    if (c == g_taken) return false;
    out = g_rec;
    out.count = c;
    g_taken = c;
    return true;
}

}  // namespace oomhook
