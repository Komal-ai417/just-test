#!/usr/bin/env bash
# ============================================================================
# heaptrack.sh — allocation-hotspot and heap-growth profiling of the memory
# stress benchmark. Produces heaptrack.<pid>.gz plus a text summary.
#
# Usage: bash tools/heaptrack.sh [threads] [allocs-per-thread] [size]
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v heaptrack > /dev/null || { echo "heaptrack not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-tools}"
THREADS="${1:-4}"
ALLOCS="${2:-20000}"
SIZE="${3:-512}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target fast_alloc_bench_memory > /dev/null

echo "=== heaptrack: bench_memory T=$THREADS allocs=$ALLOCS size=$SIZE ==="
heaptrack -o /tmp/fastalloc_heaptrack \
    "$BUILD/fast_alloc_bench_memory" --threads "$THREADS" \
    --allocs "$ALLOCS" --size "$SIZE" > /dev/null 2>&1 || true

RESULTS=$(ls -t /tmp/fastalloc_heaptrack*.gz 2>/dev/null | head -1)
if [ -z "$RESULTS" ]; then echo "no heaptrack results produced" >&2; exit 1; fi
echo "results: $RESULTS"
heaptrack_print "$RESULTS" 2>/dev/null | head -60 || \
    echo "(heaptrack_print unavailable; load $RESULTS into heaptrack_gui)"
