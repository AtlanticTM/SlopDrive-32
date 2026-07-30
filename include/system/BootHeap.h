#pragma once

// Boot heap attribution — one reading per init stage.
//
// WHY THIS EXISTS: a single post-boot total cannot say WHICH subsystem took the
// RAM. The linker map accounts for ~163 KB of static DRAM, leaving the heap
// starting near 258 KB, and the device reports ~32 KB free once setup() returns.
// That ~226 KB is spent by WiFi/lwIP/NimBLE/AsyncTCP/task stacks DURING init and
// none of it appears in any build artifact, so it can only be attributed by
// reading the heap between stages. Every memory fix before this one aimed at a
// number nobody had broken down. See LEDGER's "memory pressure" entry.
//
// Deliberately NOT behind a build flag: the boot that needs this is the one
// nobody planned for.
//
// Readings are RETAINED rather than only logged, because SlopLog's Info ring is
// 44 lines with a 104-byte message cap and SILENT truncation — lines emitted
// during boot recycle before they can be fetched. report() re-emits the whole
// table at a chosen moment.
//
// Header-only so both main.cpp's top-level stages and a subsystem's own
// sub-stages (SlopSyncHubService::init) feed ONE ordered table. Do not add a
// second table: the ordering across translation units is what makes the deltas
// mean anything.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "sloplog/sloplog.h"

namespace bootheap {

struct Stage {
    const char* name;
    uint32_t    free_after;
    int32_t     consumed;   // positive = this stage took it
};

// 8 top-level stages + subsystem sub-stages. Marks past the cap are counted but
// not stored, so the total stays honest even if someone adds stages and forgets
// to raise this.
inline constexpr size_t kMaxStages = 24;

inline Stage    g_stages[kMaxStages];
inline size_t   g_count = 0;      // stored
inline size_t   g_marks = 0;      // attempted
inline uint32_t g_prev_free = 0;

// Call from setup() and from a subsystem's init(), in execution order.
// `name` must have static storage duration — the table keeps the pointer.
inline void mark(const char* name) {
    const uint32_t now = ESP.getFreeHeap();
    const int32_t consumed = (g_marks == 0) ? 0 : int32_t(g_prev_free) - int32_t(now);
    if (g_count < kMaxStages) {
        g_stages[g_count++] = Stage{ name, now, consumed };
    }
    ++g_marks;
    g_prev_free = now;
}

inline void report() {
    for (size_t i = 0; i < g_count; ++i) {
        SLOGI("sys", "bootheap %-12s free=%u took=%+ld",
              g_stages[i].name, unsigned(g_stages[i].free_after),
              long(g_stages[i].consumed));
    }
    if (g_marks > g_count) {
        SLOGW("sys", "bootheap: %u marks dropped — raise kMaxStages",
              unsigned(g_marks - g_count));
    }
}

}  // namespace bootheap
