#!/usr/bin/env bash
# ============================================================================
# valgrind_callgrind.sh — function-level execution-cost profile of a
# bench_suite workload (inclusive/exclusive costs, call counts).
#
# Usage: bash tools/valgrind_callgrind.sh [workload] [ops] [threads]
#   defaults: churn 200000 2
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v valgrind > /dev/null || { echo "valgrind not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-valgrind}"
WL="${1:-churn}"
OPS="${2:-200000}"
TH="${3:-2}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

OUT="callgrind.out.$$"
echo "=== callgrind: bench_suite $WL ops=$OPS threads=$TH (fast alloc path) ==="
valgrind --tool=callgrind --callgrind-out-file="$OUT" \
    "$BUILD/bench_suite" --alloc fast --label callgrind --workload "$WL" \
    --threads "$TH" --ops "$OPS" --reps 1 > /dev/null 2>&1 || true

echo "--- top functions (exclusive cost) ---"
if command -v callgrind_annotate > /dev/null 2>&1; then
    callgrind_annotate --auto=yes --inclusive=yes "$OUT" | head -45
    callgrind_annotate --auto=yes "$OUT" | head -45
else
    echo "callgrind_annotate unavailable; raw profile in $OUT"
fi
