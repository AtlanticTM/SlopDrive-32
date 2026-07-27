#!/usr/bin/env bash
# Build every SlopSync fuzz target with libFuzzer + ASan + UBSan.
#
# The library is header-only and hardware-free, so each target is ONE
# translation unit and there is nothing for PlatformIO to add. Requires clang
# with compiler-rt (libFuzzer). See README.md for the WSL invocation used on
# the development host — the Windows-side MinGW toolchain cannot do this
# (no clang, no libFuzzer).
#
#   ./build.sh [outdir]      (default: ./build)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$HERE/build}"
CXX="${CXX:-clang++}"

TARGETS=(fuzz_cbor fuzz_catalog fuzz_frame fuzz_packed fuzz_bundle fuzz_blob fuzz_messages)

# -fno-sanitize-recover=undefined: UB must ABORT, not print and continue —
# otherwise a UBSan finding is a log line the fuzzer never reports.
COMMON=(
  -std=c++2b -O1 -g -fno-omit-frame-pointer
  # -Wno-unused-private-field: client.hpp's `_rng` is held for a future use and
  # is the library's ONE known benign warning under clang. Everything else must
  # stay clean — do not widen this list to silence a real diagnostic.
  -Wall -Wextra -Wno-unused-private-field
  -I "$REPO/lib/slopsync/include"
  -I "$HERE"
)
SAN=(-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)

mkdir -p "$OUT"
for t in "${TARGETS[@]}"; do
  echo "== $t"
  "$CXX" "${COMMON[@]}" "${SAN[@]}" "$HERE/$t.cc" -o "$OUT/$t"
done

# The seed generator is a plain program (no libFuzzer), but keep the
# sanitizers on: if an ENCODER is unsound the corpus is poisoned at birth.
echo "== gen_seeds"
"$CXX" "${COMMON[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  "$HERE/gen_seeds.cc" -o "$OUT/gen_seeds"

echo "built into $OUT"
