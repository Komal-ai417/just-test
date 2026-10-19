#!/usr/bin/env bash
# ============================================================================
# cross_allocator_matrix.sh — full cross-allocator benchmark matrix
# (FastAlloc vs glibc vs jemalloc vs mimalloc) with automatic discovery of
# the allocator shared libraries. Wraps benchmarks/benchsuite/run_matrix.sh.
#
# Discovery order for each external allocator:
#   jemalloc : $JEMALLOC_SO, ldconfig cache, common distro paths
#   mimalloc : $MIMALLOC_SO, ldconfig cache, common distro paths
# Missing allocators are skipped with a note; FastAlloc and glibc always run.
#
# Usage: bash tools/cross_allocator_matrix.sh [out.jsonl] [reps] [threads...]
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="${1:-cross_allocator_results.jsonl}"
REPS="${2:-5}"
shift 2 2>/dev/null || shift "$#" 2>/dev/null || true
THREADS_L="${*:-1 2 4 8}"

BUILD="${BUILD_DIR:-build-xalloc}"
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

find_lib() {  # <name patterns...>
    local pat
    for pat in "$@"; do
        local hit
        hit=$(ldconfig -p 2>/dev/null | awk '{print $NF}' | grep -E "$pat" | head -1 || true)
        [ -n "$hit" ] && { echo "$hit"; return 0; }
        for d in /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu /usr/lib64; do
            hit=$(ls "$d"/$pat 2>/dev/null | head -1 || true)
            [ -n "$hit" ] && { echo "$hit"; return 0; }
        done
    done
    return 1
}

JEM="${JEMALLOC_SO:-$(find_lib 'libjemalloc\.so\.?[0-9]*$' || true)}"
MI="${MIMALLOC_SO:-$(find_lib 'libmimalloc(\.so|-debug\.so\.?[0-9]*)$' || true)}"

echo "allocators discovered:"
echo "  glibc    : system (always available)"
echo "  fastalloc: built from source"
[ -n "$JEM" ] && echo "  jemalloc : $JEM" || echo "  jemalloc : NOT FOUND (apt-get install libjemalloc2 to enable)"
[ -n "$MI" ]  && echo "  mimalloc : $MI"  || echo "  mimalloc : NOT FOUND (apt-get install libmimalloc to enable)"

cd benchmarks/benchsuite
BENCH_SUITE="$PWD/../../$BUILD/bench_suite" JEMALLOC_SO="$JEM" MIMALLOC_SO="$MI" \
    THREADS="$THREADS_L" bash ./run_matrix.sh "$PWD/$OUT" "$REPS"
python3 analyze.py "$PWD/$OUT" | tee "${OUT%.jsonl}_summary.md" || true
echo "results: benchmarks/benchsuite/$OUT"
