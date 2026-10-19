#!/usr/bin/env bash
# ============================================================================
# msan_check.sh — MemorySanitizer (uninitialized-memory read) verification.
#
# MSan is clang-only (GCC has no MSan) and requires that every linked
# library be MSan-instrumented. The gtest suites link gtest and the C++
# standard library runtime, neither of which a plain CI environment can
# provide instrumented, so a full gtest binary cannot be a reliable gate
# (uninstrumented container internals produce false positives). The gate is
# therefore the dedicated smoke harness tests/msan_harness.cpp - gtest-free
# and STL-container-free - which exercises every public API entry point:
# size-class boundaries, calloc-before-write, realloc chains across the
# small/large boundary, aligned allocations, free_sized hints, cross-thread
# hand-off through the pending queues, stats and purge.
#
# The build goes through CMake with -DFASTALLOC_SANITIZE=memory, which also
# turns off LTO (incompatible with sanitizer builds) - no raw clang++
# command lines that would drift from the project's build flags.
#
# Usage: bash tools/msan_check.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v clang++ > /dev/null ||
    { echo "clang++ not installed (MSan is clang-only)" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-msan}"

echo "=== MSan build (clang -fsanitize=memory, origin tracking level 2) ==="
# Tests/benchmarks stay off: the harness has no gtest dependency, so this
# configure needs no FetchContent downloads at all.
cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DFASTALLOC_SANITIZE=memory \
      -DFASTALLOC_BUILD_TESTS=OFF \
      -DFASTALLOC_BUILD_BENCHMARKS=OFF \
      -DFASTALLOC_BUILD_FUZZ=OFF \
      -DCMAKE_CXX_FLAGS="-fsanitize-memory-track-origins=2" \
      > /dev/null
cmake --build "$BUILD" --target fast_alloc_msan_harness -j"$(nproc)"

echo "=== MSan run (fast_alloc_msan_harness) ==="
# halt_on_error: the first uninit read aborts with a full origin trace
# rather than continuing (FastAlloc guards must never rely on uninit data,
# so any single report is a real bug).
MSAN_OPTIONS=halt_on_error=1:exitcode=99 \
    "$BUILD/fast_alloc_msan_harness"

echo "MSAN: CLEAN (harness exercised the public API under origin tracking)"
