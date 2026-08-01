// HeapWatch — hardware watchpoint 0, armed from firmware, no debugger attached.
//
//   DIAGNOSTIC ONLY. Build-guarded behind SLOPSYNC_HEAP_BISECT; compiles to
//   nothing otherwise. Remove with the rest of the hunt (LEDGER THE QUEUE #0).
//
// Every other tool in the corruption hunt is a DETECTOR: it reports that the
// heap is already damaged, never who damaged it. A watchpoint reports the
// writing INSTRUCTION. The panic handler prints "Watchpoint 0 triggered"
// followed by the ordinary register dump, so the answer arrives through the
// /api/coredump + addr2line path that already exists — no OpenOCD, no CPU halt,
// and therefore no distortion of the network timing the reproduction needs.
//
// FOUR CONSTRAINTS, ALL LOAD-BEARING:
//  1. WATCHPOINT 1 IS NOT AVAILABLE. CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
//     re-arms it as the stack canary on every context switch, so anything we
//     put there is overwritten within microseconds. Watchpoint 0 is the only
//     one we get.
//  2. IT IS PER-CORE. xt_utils_set_watchpoint writes this core's DBREAKA /
//     DBREAKC. Arm from the same core the suspect write executes on. Everything
//     in this hunt (AsyncTCP, WiFi, lwIP, the hub) is Core 0; motion is Core 1
//     and is NOT covered by an arm() called from a comms context.
//  3. 64 BYTES, NATURALLY ALIGNED. SOC_CPU_WATCHPOINT_MAX_REGION_SIZE is 0x40
//     and esp_cpu_set_watchpoint rejects a start address that is not a multiple
//     of the region size. arm() applies both rules itself and reports the range
//     it actually armed, which is never silently the one you asked for.
//  4. IT MUST PROVE ITSELF BEFORE IT IS TRUSTED. selftest() exists because
//     TRAPS T28 item 6 cost a full session: heap tracing configured clean,
//     initialized clean, reported active, and produced zero records. A
//     watchpoint that never fires is ambiguous between "nothing wrote there"
//     and "the instrument is dead", and that ambiguity is worth more than the
//     twenty lines it takes to remove.
//
// WATCHING A FREED BLOCK DIRECTLY DOES NOT WORK, so do not try it: TLSF stores
// its free-list links in the first bytes of a free block's payload and rewrites
// them on every coalesce, and the next allocation hands the block to a new owner
// who writes it legitimately. The signal only comes out clean if the watched
// object is QUARANTINED — kept allocated, so nothing may legitimately touch it,
// while the code under suspicion believes it is dead.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace heapwatch {

// Arms watchpoint 0 on STOREs into a region covering `addr`. `size` is rounded
// DOWN to a legal power of two (max 64) and the start is aligned down to it, so
// the armed range is a superset-aligned window around `addr`, never nothing.
// Returns false and logs if the hardware refused; never fails quietly.
bool arm(const void* addr, size_t size);

void disarm();

// What is actually armed right now, for the readout. armedAt() is null when
// disarmed. arms() counts successful arm() calls since boot — a 0 here during a
// run means the arming site never executed, which is a different bug from the
// watchpoint not firing.
const void*  armedAt();
size_t       armedSize();
uint32_t     arms();

// Takes ownership of `mem` and DOES NOT RELEASE IT, then arms watchpoint 0 over
// its first bytes. The caller must have already run the object's destructor —
// this deals in raw storage and knows nothing about types.
//
// The point is to remove every legitimate writer. A block that is genuinely
// freed is written by TLSF (free-list links live in the payload) and then handed
// to its next owner, so a watchpoint on it fires constantly and says nothing.
// A quarantined block stays allocated: the allocator will not touch it and no
// one will be given it, so ANY store into it is by definition a write to an
// object its owner already believes is dead.
//
// Bounded: the ring releases the oldest block for real when it wraps, so the
// cost is a fixed few KB and not an unbounded leak. That does mean only the
// most recent kQuarantineDepth corpses are protected — which is the right bias,
// since the use-after-free window is immediately after the death.
void quarantine(void* mem, size_t size);

uint32_t quarantined();      // still held
uint32_t quarantineEvicted();  // released on wrap, no longer watched

// Arms on a private static and immediately stores into it. On working hardware
// this DOES NOT RETURN — the device panics with "Watchpoint 0 triggered" and the
// backtrace points at the store inside this function. If it returns, the
// instrument is dead and every negative result from it is worthless; it logs
// SLOGE and sets failed() in that case.
void selftest();
bool selftestFailed();

}  // namespace heapwatch
