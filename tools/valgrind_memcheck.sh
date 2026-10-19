#!/usr/bin/env bash
# ============================================================================
# valgrind_memcheck.sh — independent memory-error validation of both gtest
# suites under valgrind memcheck (invalid reads/writes, invalid frees,
# undefined values, leaks). Complements the ASan/LSan matrix with a second,
# independent detector.
#
# Usage: bash tools/valgrind_memcheck.sh [--quick]
#   --quick: validate suite only, leak check summary only
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v valgrind > /dev/null || { echo "valgrind not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-valgrind}"
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=OFF > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target fast_alloc_tests fast_alloc_audit_tests > /dev/null

VG_OPTS="--tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect \
--errors-for-leak-kinds=definite,indirect --track-origins=yes \
--error-exitcode=42 --child-silent-after-fork=yes --num-callers=30"

run_one() {  # <binary> <label>
    local bin="$1" label="$2"
    echo "=== memcheck: $label ==="
    # Death tests fork children that die by design; --child-silent-after-fork
    # keeps their expected abort output out of the way, and the expected
    # SIGABRT children do not affect the error exit code.
    if valgrind $VG_OPTS "$bin" --gtest_brief=1 > "valgrind_$label.log" 2>&1; then
        echo "    clean ($(wc -l < "valgrind_$label.log") log lines)"
    else
        rc=$?
        if [ "$rc" -eq 42 ]; then
            echo "    MEMCHECK ERRORS - see valgrind_$label.log" >&2
            grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" \
                "valgrind_$label.log" | head -20 >&2
            return 1
        fi
        echo "    test binary failed rc=$rc (see valgrind_$label.log)" >&2
        return 1
    fi
    grep "ERROR SUMMARY" "valgrind_$label.log" | tail -1
}

run_one "$BUILD/fast_alloc_tests" validate
if [ "$QUICK" -ne 1 ]; then
    run_one "$BUILD/tests/audit/fast_alloc_audit_tests" audit
fi
echo "memcheck: ALL CLEAN"
