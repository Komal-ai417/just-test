#!/usr/bin/env bash
# ============================================================================
# valgrind_cachegrind.sh — cache-behaviour simulation (L1 I/D, LL misses)
# of a bench_suite workload. Reports the miss ratios that dominate the
# allocator's cycle cost.
#
# Usage: bash tools/valgrind_cachegrind.sh [workload] [ops] [threads]
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v valgrind > /dev/null || { echo "valgrind not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-valgrind}"
WL="${1:-cache-thrash}"
OPS="${2:-100000}"
TH="${3:-1}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

OUT="cachegrind.out.$$"
echo "=== cachegrind: bench_suite $WL ops=$OPS threads=$TH ==="
valgrind --tool=cachegrind --cachegrind-out-file="$OUT" \
    "$BUILD/bench_suite" --alloc fast --label cachegrind --workload "$WL" \
    --threads "$TH" --ops "$OPS" --reps 1 > /dev/null 2>&1 || true

if command -v cg_annotate > /dev/null 2>&1; then
    echo "--- summary + hottest lines ---"
    cg_annotate --auto=yes "$OUT" | head -60
else
    echo "cg_annotate unavailable; raw profile in $OUT"
    grep -E "I1|D1|LL" "$OUT" | head -20 || true
fi
