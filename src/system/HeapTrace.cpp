// HeapTrace — see include/system/HeapTrace.h for why this exists.

#include "HeapTrace.h"

// Outside the guard on purpose: the disabled-path stub at the bottom of this
// file also uses strlen/memcpy, and it had never been compiled until the heap
// diagnostics were stripped (2026-08-01) because the flag was always on.
#include <string.h>

#if defined(SLOPSYNC_HEAP_BISECT)

#include <stdarg.h>   // va_list in dumpJson's formatter
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "sloplog/sloplog.h"

namespace heaptrace {
namespace {

heap_trace_record_t* s_buf   = nullptr;
size_t               s_cap   = 0;
bool                 s_live  = false;

}  // namespace

bool begin(size_t records) {
    if (s_live) return true;

    // INTERNAL RAM, NOT PSRAM — and this is not a preference, it is required.
    // Trace records are written from INSIDE the allocator, under its lock and
    // in contexts where the PSRAM cache is not reliably usable. A PSRAM buffer
    // was tried first (fw 2.3.3): heap_trace_init_standalone and
    // heap_trace_start both returned ESP_OK, the endpoint reported
    // active=true, and heap_trace_get_count() stayed at 0 forever, under load
    // and at idle. It fails SILENTLY, which is the worst way for a diagnostic
    // to fail. Keep the record count small because this is scarce memory.
    const size_t bytes = records * sizeof(heap_trace_record_t);
    s_buf = static_cast<heap_trace_record_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_buf == nullptr) {
        SLOGE("heaptrace", "record buffer alloc FAILED (%u B in PSRAM)", unsigned(bytes));
        return false;
    }
    memset(s_buf, 0, bytes);
    s_cap = records;

    if (heap_trace_init_standalone(s_buf, records) != ESP_OK) {
        SLOGE("heaptrace", "heap_trace_init_standalone FAILED");
        heap_caps_free(s_buf);
        s_buf = nullptr;
        s_cap = 0;
        return false;
    }
    // HEAP_TRACE_ALL, not HEAP_TRACE_LEAKS: a double-free or an overrun is a
    // history question, and LEAKS discards the free records that answer it.
    if (heap_trace_start(HEAP_TRACE_ALL) != ESP_OK) {
        SLOGE("heaptrace", "heap_trace_start FAILED");
        return false;
    }
    s_live = true;
    SLOGW("heaptrace", "DIAGNOSTIC tracing started: %u records, %u B internal",
          unsigned(records), unsigned(bytes));
    return true;
}

bool active() { return s_live; }

size_t seen() { return s_live ? heap_trace_get_count() : 0; }

size_t dumpJson(char* out, size_t cap, size_t limit) {
    if (out == nullptr || cap == 0) return 0;
    size_t n = 0;
    auto put = [&](const char* fmt, ...) {
        if (n + 1 >= cap) return;
        va_list ap;
        va_start(ap, fmt);
        const int w = vsnprintf(out + n, cap - n, fmt, ap);
        va_end(ap);
        if (w > 0) n += size_t(w) < (cap - n) ? size_t(w) : (cap - n - 1);
    };

    if (!s_live) {
        put("{\"active\":false}");
        return n;
    }

    const size_t total = heap_trace_get_count();
    // Newest-first: the records adjacent to the corruption are the ones that
    // matter, and a truncated dump must never drop them.
    const size_t want  = (limit == 0 || limit > total) ? total : limit;
    const size_t start = total - want;

    put("{\"active\":true,\"total\":%u,\"returned\":%u,\"r\":[", unsigned(total), unsigned(want));
    for (size_t i = 0; i < want; ++i) {
        heap_trace_record_t rec;
        if (heap_trace_get(uint32_t(start + i), &rec) != ESP_OK) break;
        if (i) put(",");
        put("{\"a\":\"0x%08x\",\"s\":%u,\"f\":%d,\"c\":[",
            unsigned(uintptr_t(rec.address)), unsigned(rec.size),
            rec.freed_by[0] != nullptr ? 1 : 0);
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH; ++d) {
            if (rec.alloced_by[d] == nullptr) break;
            if (d) put(",");
            put("\"0x%08x\"", unsigned(uintptr_t(rec.alloced_by[d])));
        }
        put("],\"fb\":[");
        for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH; ++d) {
            if (rec.freed_by[d] == nullptr) break;
            if (d) put(",");
            put("\"0x%08x\"", unsigned(uintptr_t(rec.freed_by[d])));
        }
        put("]}");
        if (n + 64 >= cap) break;   // leave room to close the JSON honestly
    }
    put("]}");
    return n;
}

}  // namespace heaptrace

#else   // !SLOPSYNC_HEAP_BISECT

namespace heaptrace {
bool   begin(size_t)                     { return false; }
bool   active()                          { return false; }
size_t seen()                            { return 0; }
size_t dumpJson(char* out, size_t cap, size_t) {
    if (out && cap) { const char* s = "{\"active\":false}"; size_t n = strlen(s);
                      if (n >= cap) n = cap - 1; memcpy(out, s, n); out[n] = 0; return n; }
    return 0;
}
}  // namespace heaptrace

#endif
