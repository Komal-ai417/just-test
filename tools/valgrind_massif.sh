#!/usr/bin/env bash
# ============================================================================
# valgrind_massif.sh — heap-usage / memory-growth profile of the memory
# stress benchmark. Produces massif.out.<pid> plus an ms_print text summary.
#
# Usage: bash tools/valgrind_massif.sh [threads] [allocs-per-thread] [size]
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v valgrind > /dev/null || { echo "valgrind not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-valgrind}"
THREADS="${1:-4}"
ALLOCS="${2:-20000}"
SIZE="${3:-512}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target fast_alloc_bench_memory > /dev/null

OUT="massif.out.$$"
echo "=== massif: bench_memory T=$THREADS allocs=$ALLOCS size=$SIZE ==="
valgrind --tool=massif --stacks=yes --time-unit=B \
    "$BUILD/fast_alloc_bench_memory" --threads "$THREADS" --allocs "$ALLOCS" \
    --size "$SIZE" > /dev/null 2>&1 || true
# massif writes massif.out.<pid>; grab the newest one
MFILE=$(ls -t massif.out.* 2>/dev/null | head -1) || MFILE=""
if [ -z "$MFILE" ]; then echo "no massif output produced" >&2; exit 1; fi
mv "$MFILE" "$OUT"
ms_print "$OUT" > "${OUT}.txt" 2>/dev/null || echo "(ms_print unavailable)"
echo "wrote $OUT ($(grep -c '^\s*#' "$OUT" 2>/dev/null || echo ?) snapshots)"
echo "peak heap snapshot:"
grep -A2 "mem_heap_B" "$OUT" | sort -t= -k2 -n | tail -3 || true
