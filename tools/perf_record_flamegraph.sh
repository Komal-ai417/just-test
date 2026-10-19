#!/usr/bin/env bash
# ============================================================================
# perf record + flame graph — CPU hotspot profile of the allocator with an
# SVG flame graph (auto-clones FlameGraph if the repo is not present).
#
# Usage: bash tools/perf_record_flamegraph.sh [workload] [ops] [threads] [out.svg]
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v perf > /dev/null || { echo "perf not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-perf}"
WL="${1:-churn}"
OPS="${2:-2000000}"
TH="${3:-2}"
OUT="${4:-flamegraph.svg}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

echo "=== perf record: bench_suite $WL ops=$OPS threads=$TH ==="
perf record -F 999 -g --call-graph dwarf,16384 -o perf.data -- \
    "$BUILD/bench_suite" --alloc fast --label flame --workload "$WL" \
    --threads "$TH" --ops "$OPS" --reps 1 > /dev/null 2>&1 || true

# FlameGraph tooling
if [ ! -d build-flamegraph-tools ]; then
    git clone --depth 1 https://github.com/brendangregg/FlameGraph.git \
        build-flamegraph-tools > /dev/null 2>&1
fi

if [ -d build-flamegraph-tools ]; then
    perf script -i perf.data > perf.unfold 2>/dev/null || perf script > perf.unfold
    ./build-flamegraph-tools/stackcollapse-perf.pl perf.unfold > perf.folded
    ./build-flamegraph-tools/flamegraph.pl perf.folded > "$OUT"
    echo "wrote $OUT"
    echo "--- top symbols ---"
    awk '{ print $2, $1 }' perf.folded | sort -k2 -rn | head -15 || true
else
    echo "FlameGraph clone failed; falling back to perf report"
    perf report -i perf.data --stdio | head -40
fi
