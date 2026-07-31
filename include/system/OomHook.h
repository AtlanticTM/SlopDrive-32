#pragma once
// OomHook — name the allocation that killed us, at the moment it fails.
//
// WHY THIS EXISTS. Every heap death on this machine has arrived as the word
// PANIC and nothing else. The 2026-07-31 core dump finally decoded one
// (newlib lock_init_generic -> failed mutex -> abort) but that is a post-mortem
// of the VICTIM: the faulting task is whichever one next needed memory, never
// the one that took it. ESP-IDF offers a hook that fires on the failing
// allocation itself, before the abort, and it was simply never installed.
//
// ISR-SAFETY IS THE WHOLE DESIGN CONSTRAINT. The hook may fire in ANY context,
// including an ISR, so this records into plain statics and drops one
// crashring crumb (documented allocation-free and lock-free) and does NOTHING
// ELSE. It must not log: SlopLog is explicitly not ISR-safe, and a logging
// call from an ISR-context allocation failure would replace a diagnosable
// panic with an undiagnosable one. Reporting happens later, from a task, via
// oomTake().
#include <stdint.h>

namespace oomhook {

struct Record {
    uint32_t count;        // total failures since boot (0 = never fired)
    uint32_t size;         // bytes requested by the failing allocation
    uint32_t caps;         // MALLOC_CAP_* bitmask requested
    uint32_t free_int;     // free internal heap at failure
    uint32_t largest_int;  // largest internal block at failure
    uint32_t t_ms;
    char     task[16];     // task that was running, "" if ISR/unknown
    char     fn[24];       // IDF's reported caller, when it gives one
};

// Install the hook. Call once in setup(), after the crash ring is armed.
void begin();

// Copy out the newest record and report whether it is NEW since the last call.
// Returns false when nothing has failed since the previous take.
bool take(Record& out);

// Total failures since boot, for a readout that wants the count without
// consuming the "new" flag.
uint32_t count();

// Read the newest record WITHOUT consuming take()'s new-flag. A diagnostic
// readout must never swallow the one log line the failure was going to produce.
bool peek(Record& out);

}  // namespace oomhook
