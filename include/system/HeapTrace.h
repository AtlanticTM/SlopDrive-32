#pragma once

// HeapTrace — allocation history for the corruption hunt (LEDGER THE QUEUE #0)
//
// WHY THIS EXISTS, and why it is not another detector:
//   heap_caps_check_integrity_all() answers "is the heap damaged NOW". It can
//   never answer "who damaged it", because by the time a walk notices a bad
//   header the writer is long gone. Session-granularity integrity probes were
//   tried on fw 2.3.2 and never fired — the window between the bad write and
//   the allocator tripping over it is shorter than the gap between probes.
//   Tracing records the alloc/free history WITH CALLER ADDRESSES, so a damaged
//   block can be traced back to who owned it and who freed it. That is a
//   cause, not a symptom.
//
// Constraints:
//   DIAGNOSTIC ONLY. Build-guarded behind SLOPSYNC_HEAP_BISECT; compiles to
//   nothing otherwise. Remove with the poisoning options once the write is
//   found.
//   The record buffer lives in PSRAM. It is ~8-12 KB for a useful depth and
//   internal RAM is the scarce resource this whole task exists to protect;
//   PSRAM has 8 MB sitting idle and tracing is not on a latency path.
//   Records are read by POLLING from the host (/api/heaptrace). They are NOT
//   dumped on panic: a panic in the allocator does not run our hooks, and the
//   buffer does not survive the reboot. The host samples continuously and the
//   last successful sample before the device dies is the evidence.
//   nowMs/dumping are not ISR-safe; call only from a task.

#include <stddef.h>
#include <stdint.h>

namespace heaptrace {

// Allocate the PSRAM record buffer and start recording. Safe to call once,
// after PSRAM is up. Returns false if the buffer or the tracer refused.
bool begin(size_t records = 256);

// True once begin() has succeeded and recording is live.
bool active();

// Total records the tracer has seen (wraps in the buffer; this is the raw
// count so a caller can tell whether it missed history between polls).
size_t seen();

// Serialize the most recent `limit` records as JSON into `out`. Returns bytes
// written. Shape per record: {a:<addr>,s:<size>,f:<freed 0|1>,c:[<caller pcs>]}
// Addresses are hex strings so a host can feed them straight to addr2line.
size_t dumpJson(char* out, size_t cap, size_t limit);

}  // namespace heaptrace
