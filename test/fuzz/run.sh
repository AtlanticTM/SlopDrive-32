#!/usr/bin/env bash
# Run every SlopSync fuzz target for a fixed wall-clock budget each.
#
#   ./run.sh [seconds-per-target] [builddir] [corpusdir] [workdir] [workers]
#
# `workers` (default: nproc) fans one target across cores via libFuzzer's own
# -jobs/-workers. Set it to 1 for a single-process run whose exec counts are
# directly comparable between targets — which is what the RFC-028 first-pass
# numbers in README.md were measured with.
#
# Corpus policy: each target fuzzes into ITS OWN work directory seeded from
# the committed corpus. The committed corpus is never written to by a run —
# fold new coverage back deliberately (see README.md).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SECS="${1:-300}"
BUILD="${2:-$HERE/build}"
CORPUS="${3:-$HERE/corpus}"
WORK="${4:-$HERE/work}"
WORKERS="${5:-$(nproc 2>/dev/null || echo 1)}"

export ASAN_SYMBOLIZER_PATH="${ASAN_SYMBOLIZER_PATH:-/usr/bin/llvm-symbolizer}"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
export ASAN_OPTIONS="abort_on_error=1:detect_stack_use_after_return=1"

TARGETS=(fuzz_cbor fuzz_catalog fuzz_frame fuzz_packed fuzz_bundle fuzz_blob fuzz_messages)
declare -A SEEDDIR=(
  [fuzz_cbor]=cbor [fuzz_catalog]=catalog [fuzz_frame]=frame [fuzz_packed]=packed
  [fuzz_bundle]=bundle [fuzz_blob]=blob [fuzz_messages]=messages
)

fail=0
for t in "${TARGETS[@]}"; do
  d="$WORK/${SEEDDIR[$t]}"
  mkdir -p "$d"
  echo "===== $t (${SECS}s)"
  # -max_len well above the seeds: several findings here only reproduce with
  # inputs longer than the corpus's natural maximum (see fuzz_frame's note on
  # intra-object overflow).
  par=()
  if [ "$WORKERS" -gt 1 ]; then par=(-jobs="$WORKERS" -workers="$WORKERS"); fi
  "$BUILD/$t" "$d" "$CORPUS/${SEEDDIR[$t]}" \
      -max_total_time="$SECS" -max_len=8192 -print_final_stats=1 "${par[@]}" \
      -artifact_prefix="$WORK/" 2>&1 | grep -E 'stat::|ERROR|SUMMARY|runtime error|Done .* runs' || true
  rc=${PIPESTATUS[0]}
  if [ "$rc" -ne 0 ]; then echo "!!! $t exited $rc"; fail=1; fi
done
exit $fail
