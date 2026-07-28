# SlopSync fuzz gate (RFC-028)

**The obligation being proven:** every SlopSync parser, hub-side or
client-side, maps *any* byte string to accept-or-reject — no out-of-bounds
read, no unbounded allocation or recursion, no UB. Golden vectors prove
*correctness*; this proves *totality*.

Both directions are in scope. The hub parses HELLO/INTENT/bundles from
untrusted clients; a **client parses WELCOME/catalog/STATE from a possibly
hostile hub** — a client that auto-connects to any discovered
`_slopsync._tcp` beacon is one malicious hub away from parsing attacker
bytes, and the catalog (nested maps, a dozen variable-length strings per
field, index-aligned label pools) is the fattest client-side surface in the
protocol. RFC-028 §5 makes "a hostile hub MUST NOT be able to crash a
conforming client" normative.

---

## Running it (WSL — the Windows toolchain cannot do this)

The repo's Windows-side host has **no clang and no libFuzzer** (MinGW/GCC
only), so this gate runs in the WSL2 Arch instance:

```bash
# From the Windows side, one-shot:
wsl.exe -e bash -lc '<command>'
```

`/mnt/c` is slow — **build into and run corpora from a WSL-local directory**,
compiling against the repo over the mount:

```bash
# inside WSL
R=/mnt/c/Users/Atlan/Documents/SlopDrive-32
mkdir -p ~/fuzz && cd ~/fuzz

# build every target (mirrors build.sh; run it from ~/fuzz, not from /mnt/c)
for t in fuzz_cbor fuzz_catalog fuzz_frame fuzz_packed fuzz_bundle fuzz_blob fuzz_messages; do
  clang++ -std=c++2b -O1 -g -fno-omit-frame-pointer -Wall -Wextra \
    -Wno-unused-private-field \
    -I $R/lib/slopsync/include -I $R/test/fuzz \
    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
    $R/test/fuzz/$t.cc -o build/$t
done

# regenerate the seed corpus from the library's OWN encoders
clang++ -std=c++2b -O1 -g -I $R/lib/slopsync/include -I $R/test/fuzz \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  $R/test/fuzz/gen_seeds.cc -o build/gen_seeds
./build/gen_seeds corpus

# soak: 600 s per target
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
bash $R/test/fuzz/run.sh 600 ~/fuzz/build ~/fuzz/corpus ~/fuzz/work
```

`build.sh` and `run.sh` do the same thing with defaults; use them when you
are happy to write artifacts next to the sources (i.e. **not** when the
sources live on `/mnt/c`).

Deterministic replay of the committed corpus — the cheap check, run this
after any change to a decoder:

```bash
./build/fuzz_catalog $R/test/fuzz/corpus/catalog -runs=0
```

Reproducing one crash file:

```bash
ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer ./build/fuzz_cbor ./crash-<hash>
./build/fuzz_cbor -minimize_crash=1 -runs=100000 ./crash-<hash>
```

---

## The targets

| binary | surface | why it is its own target |
|---|---|---|
| `fuzz_cbor` | `wire/cbor/cbor_reader.hpp` | the substrate every control-plane decoder sits on; two drivers (generic `skipValue` walk, byte-driven typed reads) |
| `fuzz_catalog` | `decodeCatalog` + downstream pool resolution + re-encode + etag | richest client-side surface; a catalog accepted into an inconsistent state is a bug that only fires downstream, so the target consumes it the way a client would |
| `fuzz_frame` | frame header, `Reassembler`, `fragmentFrame`, ESTOP, COBS, raw frames | **stateful**: one input replays a whole fragment sequence (slot reuse, eviction, out-of-order, pending-last, timeouts) |
| `fuzz_packed` | `decodeByLayout` / `appendOnlyRead` / `readStringField` / `applyStateFrame` | fuzzes DATA against a *fixed* catalog, so the fuzzer explores payloads rather than re-deriving a schema; includes RFC-026 `str16/32/64` offset math |
| `fuzz_bundle` | `BundleView::parse` + `BundleWriter` | the highest-rate untrusted input on the device (motion-input, ≤333 Hz); sample size `S` is fuzzer-chosen because that is the freedom a hostile peer actually has |
| `fuzz_blob` | `getBlobChunkHeader` + `ChunkReassembler` | **stateful**; `MaxChunks=8` deliberately (a small backing array puts an off-by-one in ASan's redzone instead of in slack) |
| `fuzz_messages` | all 15 control-plane decoders, both directions | selector byte in `data[0]`; they share the CborReader substrate and the seeds carry their own prefix |

### Adding a target

Include `fuzz_common.hpp`, define `LLVMFuzzerTestOneInput`, add the name to
`build.sh`'s `TARGETS` and to `.github/workflows/fuzz.yml`'s matrix. That is
the whole ceremony.

Three rules that are load-bearing, not style:

1. **Never assert on decoder *semantics*.** A decoder returning `Malformed`
   for something a human thinks is valid is a golden-vector question, not a
   fuzz finding. The only assertions worth making are TOTALITY invariants
   ("a successful decode's reported length never exceeds the input"), because
   those are memory-safety statements in disguise.
2. **TOUCH every zero-copy view a decoder hands back.** A `string_view` that
   escaped the input buffer is only a finding if something reads it. This is
   how finding #1 below was caught.
3. **Anything derived from fuzzer bytes that indexes memory must be bounded
   by the harness**, not by the library — otherwise a harness bug and a
   library finding look identical. (`sinkF`, not `uint32_t(float)`: casting a
   fuzzer-derived float to an integer is itself UB and will masquerade as a
   library bug.)

---

## The seed corpus

`corpus/` is generated by `gen_seeds.cc`, which runs **the library's own
encoders**. That matters: fuzzing raw random bytes against a deterministic
CBOR profile mostly produces first-byte rejects, so seeds that are already
inside the accepting region are worth more than any amount of extra CPU.

Sources: the frozen mini-catalog (733 bytes, etag-pinned — the generator
reproducing that length is itself a check), a *rich* catalog carrying the
RFC-009 annotation block / RFC-021 STORE descriptors / RFC-026 `str16/32/64`
fields / RFC-014 `stream_kind` (coverage the frozen fixture cannot grow,
because it is frozen), every control-plane message via its own encoder in
both a minimal and a maximal shape, real fragment sequences, real blob
transfers, and truncations of valid encodings.

The corpus is **committed, small, and deliberately not auto-updated** — CI
replays it and fuzzes into a scratch directory. To fold new coverage in,
download the nightly `fuzz-corpus-*` artifact, merge it locally
(`./build/<target> merged/ corpus/ downloaded/ -merge=1`), and commit only
what earns its bytes.

`fuzz_messages`' selector byte values are **stable by contract** — the
committed seeds encode them, so renumbering the enum silently invalidates the
corpus. Append only.

---

## First-pass results (2026-07-25)

600 s per target, **single worker each** (so the exec counts are directly
comparable), clang 22.1.8 on a 4-core WSL2 box, ASan+UBSan with
`-fno-sanitize-recover=undefined`. **Zero crash/leak/timeout/OOM artifacts
after the three fixes below.**

| target | executions | exec/s | grown corpus | edge cov | features |
|---|---:|---:|---:|---:|---:|
| `fuzz_cbor` | 309,164,578 | 514 k | 348 | 168 | 843 |
| `fuzz_catalog` | 120,174,127 | 200 k | 552 | 893 | 2498 |
| `fuzz_frame` | 117,180,290 | 195 k | 284 | 196 | 903 |
| `fuzz_packed` | 614,165,698 | 1.02 M | 45 | 74 | 166 |
| `fuzz_bundle` | 631,598,535 | 1.05 M | 66 | 48 | 179 |
| `fuzz_blob` | 151,322,267 | 252 k | 114 | 85 | 352 |
| `fuzz_messages` | 347,415,757 | 578 k | 879 | 1296 | 2572 |
| **total** | **2,291,021,252** | | | | |

"grown corpus" is how many distinct inputs libFuzzer kept beyond the ~11
committed seeds for that target — the corpus growing 4–25× is the evidence
that the seeds actually landed inside the accepting region and the mutator
had somewhere to go, rather than bouncing off a first-byte reject.

`fuzz_packed`/`fuzz_bundle` run an order of magnitude faster and plateau at
low coverage because their surfaces genuinely are small (a bounded field walk,
a header + span check); `fuzz_catalog`/`fuzz_messages` are the deep ones and
their numbers show it.

## Findings from the first pass (all fixed, all with regression tests)

| # | Where | What | Regression test |
|---|---|---|---|
| 1 | `CborReader::readTstr`/`readBstr` | `start + len > _in.size()` **overflows**. A head of `7B FF×8` (tstr claiming 2⁶⁴−1 bytes) wraps the sum to a value that passes the check, and the reader returns a 2⁶⁴−1-byte view into a 9-byte buffer while rewinding `_pos` backwards. Reachable from *every* message decoder and from `skipValue()` — i.e. the §4.3 unknown-key path, which is the attacker's preferred entry point. Fix: `arg > remaining`. | `test_slopsync_cbor` — 4 cases |
| 2 | `ChunkReassembler::begin` | A transfer `begin()` correctly REFUSED (chunk_count/total_bytes past capacity — RFC-028's know-the-size-before-you-allocate rule working) still stored the attacker's numbers, and `missingIndices()`/`assembled()` used them without consulting `active()`. UBSan: OOB read past an 8-element array. Fix: refusing a transfer refuses its NUMBERS too. | `test_slopsync_m3b` — 2 cases |
| 3 | `Reassembler::accept` | Unbounded `memcpy` into `pendingLastBytes` (504 B) in the "last fragment, unit size not yet known" branch — the one write in the class that did not go through the bounds-checking `placeFragment()`. ASan: WRITE of size 3998. Fix: report it as the `CapacityExceeded` it is. | `test_slopsync_transport` — 2 cases |

### The trap worth remembering

Finding #3 survived a **7.8-million-execution** fuzz run before being found by
hand. The overflow's spill lands in the *very next member of the same struct*
(`pendingLastBytes` → `data`), and an **intra-object overflow is invisible to
ASan** — only a write long enough to leave the entire enclosing object
produces a report. That is why `fuzz_frame` deliberately allows 8 KiB inputs
and why CI passes `-max_len=8192`. Do not lower it, and do not assume "the
fuzzer would have found it" for any bug between two arrays of one struct.

---

## What this gate does NOT cover

Stated plainly so nobody reads a green run as more than it is:

* **Stateful protocol sequences.** The targets fuzz decoders and the two
  stateful reassemblers. They do NOT drive `Hub`/`Client` through
  HELLO→WELCOME→GRANT→INTENT→GOODBYE orderings, so session-lifecycle bugs —
  the class the source-ownership leak belonged to — are out of reach here.
  Back-to-back sessions without a reboot remains a manual verification
  pattern, not something this replaces.
* **The firmware transports.** `SlopSyncWsTransport`, the ESP-NOW/BLE/serial
  adapters and everything in `src/comms/` are Arduino/FreeRTOS code and are
  not fuzzable in this harness. The library boundary is where the fuzzing
  stops; whatever a transport does to a buffer before calling into
  `lib/slopsync` is unproven by this gate.
* **Cross-task/concurrency behavior.** Single-threaded by construction.
* **Timing, rate limiting, token buckets, deadman policy.** Reachable in
  principle through a stateful hub harness; not attempted.
* **Semantic correctness.** Fuzzing proves nothing about whether a decoded
  value is the RIGHT value — that is the golden vectors' job, and the two are
  complements, not substitutes.
* **`decodeByLayout` against attacker-chosen SCHEMAS.** Deliberate: the brief
  fixes the catalog so the fuzzer explores data. A hostile hub choosing both
  the catalog and the payload is covered transitively (`fuzz_catalog` bounds
  what a catalog may declare) but not directly in one target.
* **`BundleView` does not check `t_off` monotonicity.** It is *total* — no
  input makes it read out of bounds — but it does not walk `t_off` for strict
  increase or `t_off[0]==0`, despite its file header once implying it did.
  The hub re-derives and checks both at ingress (`hub_impl.hpp` step 3b), so
  the device is covered; a **client** parsing an h2c STREAM bundle from a
  hostile hub must do the same walk itself. The header comment now says so.
