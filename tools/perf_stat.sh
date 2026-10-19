#!/usr/bin/env bash
# ============================================================================
# perf stat — hardware/software counter summary for an allocator workload:
# cycles, instructions, IPC, branches + misses, cache refs/misses, page
# faults, context switches, CPU migrations.
#
# Usage: bash tools/perf_stat.sh [workload] [ops] [threads]
# Falls back to software counters when the environment blocks hardware ones.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v perf > /dev/null || { echo "perf not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-perf}"
WL="${1:-small-mixed}"
OPS="${2:-500000}"
TH="${3:-2}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

STATS="task-clock,cycles,instructions,branches,branch-misses,\
cache-references,cache-misses,page-faults,context-switches,cpu-migrations"

echo "=== perf stat: bench_suite $WL ops=$OPS threads=$TH (FastAlloc) ==="
if ! perf stat -e "$STATS" -- "$BUILD/bench_suite" --alloc fast --label perf \
        --workload "$WL" --threads "$TH" --ops "$OPS" --reps 1 2>&1 | tail -30; then
    echo "(hardware counters blocked; retrying with software counters only)"
    perf stat -e task-clock,page-faults,context-switches \
        -- "$BUILD/bench_suite" --alloc fast --label perf \
        --workload "$WL" --threads "$TH" --ops "$OPS" --reps 1 2>&1 | tail -15
fi

echo
echo "=== perf stat: same workload on glibc (baseline) ==="
perf stat -e task-clock,cycles,instructions,page-faults \
    -- "$BUILD/bench_suite" --alloc std --label glibc \
    --workload "$WL" --threads "$TH" --ops "$OPS" --reps 1 2>&1 | tail -20 || true
