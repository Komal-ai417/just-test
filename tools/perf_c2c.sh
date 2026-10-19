#!/usr/bin/env bash
# ============================================================================
# perf c2c — cache-line-to-cache-line contention report: false sharing and
# cache-line bouncing across the allocator's shared structures (arena locks,
# pending queue heads, page-bin heads, global counters).
#
# Usage: bash tools/perf_c2c.sh [threads] [seconds]
# Requires perf >= 3.17 with c2c support and (usually) a recent-enough
# kernel + privileged perf_event access.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v perf > /dev/null || { echo "perf not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-perf}"
TH="${1:-8}"
DUR="${2:-10}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

# A long-enough churn run so the sampler catches contention windows.
OPS=$((DUR * 200000))
echo "=== perf c2c: churn T=$TH (~${DUR}s) ==="
if ! perf c2c record -o perf_c2c.data -- \
        "$BUILD/bench_suite" --alloc fast --label c2c --workload churn \
        --threads "$TH" --ops "$OPS" --reps 1 > /dev/null 2>&1; then
    echo "perf c2c record failed (needs privileged perf / unsupported kernel)" >&2
    exit 3
fi
echo "=== perf c2c report (top cacheline contenders) ==="
perf c2c report -i perf_c2c.data --stdio 2>/dev/null | head -80 || true
echo "full report: perf c2c report -i perf_c2c.data"
