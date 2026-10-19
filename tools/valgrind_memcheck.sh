#!/usr/bin/env bash
# ============================================================================
# valgrind_memcheck.sh — independent memory-error validation of both gtest
# suites under valgrind memcheck (invalid reads/writes, invalid frees,
# undefined values, leaks). Complements the ASan/LSan matrix with a second,
# independent detector.
#
# The full valgrind + gtest output is STREAMED LIVE to the console (tee'd
# into valgrind_<label>.log as it happens): a hung or slow test is then
# visible in real time — which test is running, how far the suite got —
# instead of a silent gap followed by a post-mortem summary. stdbuf keeps
# the test binary's stdout line-buffered even though it writes into a pipe,
# so each [ RUN ] / [ OK ] line lands on the console the moment it is
# printed. gtest's brief mode is deliberately OFF so every test emits its
# own progress line.
#
# Usage: bash tools/valgrind_memcheck.sh [--quick]
#   --quick: validate suite only, audit suite skipped
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
    echo "=== memcheck: $label (live log; also captured in valgrind_$label.log) ==="
    # Everything valgrind and the test binary print is tee'd: console sees
    # it live, the file keeps the authoritative copy. pipefail + PIPESTATUS
    # preserve valgrind's exit code (42 = memory errors) through the pipe.
    # Death tests fork children that die by design; --child-silent-after-fork
    # keeps their expected abort output out of the way and their expected
    # SIGABRT exit does not affect the error exit code.
    local rc=0
    stdbuf -oL -eL valgrind $VG_OPTS "$bin" 2>&1 \
        | tee "valgrind_$label.log" || rc=${PIPESTATUS[0]}
    if [ "$rc" -eq 0 ]; then
        echo "--- $label: CLEAN ($(grep -c '^==.*==\s' "valgrind_$label.log" || true) valgrind log lines)"
    elif [ "$rc" -eq 42 ]; then
        echo "!! $label: MEMCHECK ERRORS" >&2
        grep -E "ERROR SUMMARY|Invalid|uninitialised|definitely lost" \
            "valgrind_$label.log" | head -20 >&2
        return 1
    else
        echo "!! $label: test binary failed rc=$rc (full log streamed above; also in valgrind_$label.log)" >&2
        return 1
    fi
    grep "ERROR SUMMARY" "valgrind_$label.log" | tail -1
}

run_one "$BUILD/fast_alloc_tests" validate
if [ "$QUICK" -ne 1 ]; then
    run_one "$BUILD/tests/audit/fast_alloc_audit_tests" audit
fi
echo "memcheck: ALL CLEAN"
