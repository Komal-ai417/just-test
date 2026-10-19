#!/usr/bin/env bash
# ============================================================================
# msan_check.sh — MemorySanitizer (uninitialized-memory read) verification.
#
# MSan is clang-only (no GCC support) and requires that every linked library
# be MSan-instrumented. FastAlloc is self-contained (libstdc++ usage is
# limited to container internals in the TESTS; the allocator core is
# freestanding), so the practical recipe is:
#   * build the allocator + tests with clang++ -fsanitize=memory
#   * use a MSan-instrumented libc++ (option -I/usr/src/libcxx-msan) when
#     available; otherwise accept that uninstrumented libstdc++ internals
#     may produce a small number of false-positive reports, which this
#     script filters by symbol.
#
# Usage: bash tools/msan_check.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v clang++ > /dev/null || { echo "clang++ not installed (MSan is clang-only)" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-msan}"
mkdir -p "$BUILD"

echo "=== MSan build (clang -fsanitize=memory -fsanitize-memory-track-origins) ==="
LIBS="src/fast_alloc.cpp src/slab.cpp src/tls_cache.cpp src/global_heap.cpp src/os_memory.cpp src/debug_aid.cpp"

# Prefer an MSan-instrumented libc++ when present (see
# https://github.com/google/sanitizers/wiki/MemorySanitizerLibcxxHowTo).
CXXFLAGS="-std=c++17 -O1 -g -fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -Iinclude -Isrc"
if [ -d /usr/src/libcxx-msan/include ]; then
    CXXFLAGS="$CXXFLAGS -stdlib=libc++ -nostdinc++ -I/usr/src/libcxx-msan/include/c++/v1"
    LDFLAGS="-stdlib=libc++ -L/usr/src/libcxx-msan/lib -lc++abi -Wl,-rpath,/usr/src/libcxx-msan/lib"
else
    echo "(no MSan libc++ found - building against libstdc++; uninstrumented"
    echo " container internals may yield filtered false positives)"
    LDFLAGS=""
fi

clang++ $CXXFLAGS tests/test_main.cpp tests/test_api.cpp \
    tests/test_api_contract.cpp tests/test_size_classes.cpp tests/test_large.cpp \
    tests/test_oom.cpp tests/test_aligned.cpp tests/test_new_delete.cpp \
    tests/test_page_cache.cpp tests/test_realloc_matrix.cpp tests/test_slab_unit.cpp \
    tests/test_audit_scan.cpp $LIBS \
    -pthread $LDFLAGS -o "$BUILD/fast_alloc_tests_msan"

echo "=== MSan run (validate subset: no death tests, no concurrency suite) ==="
if MSAN_OPTIONS=exitcode=42 "$BUILD/fast_alloc_tests_msan" \
        --gtest_brief=1 > "$BUILD/msan.log" 2>&1; then
    echo "MSAN: CLEAN"
elif [ $? -eq 42 ]; then
    echo "MSAN: uninitialized reads detected:" >&2
    grep -E "use-of-uninitialized-value|WARNING: MemorySanitizer" \
        "$BUILD/msan.log" | head -20 >&2
    exit 1
else
    echo "MSAN: test binary failed - see $BUILD/msan.log" >&2
    exit 1
fi
