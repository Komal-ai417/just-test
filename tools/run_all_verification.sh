#!/usr/bin/env bash
# ============================================================================
# run_all_verification.sh — one-command full verification of FastAlloc.
#
# Runs, in order:
#   1. Release build  + full validate (fast_alloc_tests) + audit suites
#   2. Debug build    + full validate suite (FASTALLOC_DEBUG instrumentation)
#   3. Override-new build + operator-new override suite
#   4. ASan           + both suites        (LSan enabled by default on Linux)
#   5. UBSan          + both suites
#   6. TSan           + both suites        (slow: 10-40x instrumentation)
#   7. LSan-only debug build + validate suite
#   8. Coverage build + report             (if gcovr is present)
#   9. Fuzz smoke (standalone, release + debug semantics, 20 s each)
#  10. Benchmark smoke (bench_audit quick + bench_lifecycle quick + a
#      bench_suite cell per allocator path)
#
# Env:
#   BUILD_ROOT   (default: build-all)   directory receiving the build trees
#   SKIP_SLOW=1  skip TSan + fuzz + benchmarks (quick CI-ish mode)
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_ROOT="${BUILD_ROOT:-build-all}"
SKIP_SLOW="${SKIP_SLOW:-0}"
FAIL=0
STEP=""

note() { printf '\n=== %s ===\n' "$*"; }
fail() { echo "!! FAILURE during: $STEP" >&2; FAIL=1; }

run_suite() {  # <build-dir> [ctest filter regex or empty]
    local dir="$1"; shift
    local filter="${1:-}"
    if [ -n "$filter" ]; then
        (cd "$dir" && ctest --output-on-failure -R "$filter") || return 1
    else
        (cd "$dir" && ctest --output-on-failure) || return 1
    fi
}

configure() {  # <dir> [extra cmake args...]
    local dir="$1"; shift
    cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" "$@" \
        > /dev/null
}

STEP="1. release build + suites"
note "$STEP"
{
    CMAKE_BUILD_TYPE=Release configure "$BUILD_ROOT/rel" \
        -DFASTALLOC_BUILD_BENCHMARKS=ON -DFASTALLOC_BUILD_FUZZ=ON
    cmake --build "$BUILD_ROOT/rel" -j"$(nproc)" --target \
        fast_alloc_tests fast_alloc_audit_tests fast_alloc_override_tests \
        fast_alloc_fuzz_standalone fast_alloc_bench_audit \
        fast_alloc_bench_lifecycle bench_suite > /dev/null
    run_suite "$BUILD_ROOT/rel"
} || fail

STEP="2. debug build + validate suite"
note "$STEP"
{
    CMAKE_BUILD_TYPE=Debug configure "$BUILD_ROOT/dbg" \
        -DFASTALLOC_DEBUG=ON -DFASTALLOC_BUILD_BENCHMARKS=OFF
    cmake --build "$BUILD_ROOT/dbg" -j"$(nproc)" --target \
        fast_alloc_tests fast_alloc_override_tests > /dev/null
    run_suite "$BUILD_ROOT/dbg" fast_alloc_validate
} || fail

STEP="3. override-new build + suite"
note "$STEP"
{
    CMAKE_BUILD_TYPE=Release configure "$BUILD_ROOT/newo" \
        -DFASTALLOC_OVERRIDE_NEW=ON -DFASTALLOC_BUILD_BENCHMARKS=OFF
    cmake --build "$BUILD_ROOT/newo" -j"$(nproc)" --target \
        fast_alloc_tests fast_alloc_override_tests > /dev/null
    run_suite "$BUILD_ROOT/newo"
} || fail

if [ "$SKIP_SLOW" != "1" ]; then
    for SAN in address undefined thread; do
        STEP="4-6. $SAN build + suites"
        note "$STEP"
        {
            CMAKE_BUILD_TYPE=RelWithDebInfo configure "$BUILD_ROOT/san-$SAN" \
                -DFASTALLOC_SANITIZE="$SAN" -DFASTALLOC_BUILD_BENCHMARKS=OFF
            cmake --build "$BUILD_ROOT/san-$SAN" -j"$(nproc)" --target \
                fast_alloc_tests fast_alloc_audit_tests > /dev/null
            run_suite "$BUILD_ROOT/san-$SAN"
        } || fail
    done

    STEP="7. LSan-only debug build"
    note "$STEP"
    {
        CMAKE_BUILD_TYPE=Debug configure "$BUILD_ROOT/san-leak" \
            -DFASTALLOC_SANITIZE=leak -DFASTALLOC_DEBUG=ON \
            -DFASTALLOC_BUILD_BENCHMARKS=OFF
        cmake --build "$BUILD_ROOT/san-leak" -j"$(nproc)" --target \
            fast_alloc_tests > /dev/null
        run_suite "$BUILD_ROOT/san-leak" fast_alloc_validate
    } || fail
else
    echo "(SKIP_SLOW=1: sanitizer matrix skipped)"
fi

STEP="8. coverage build + report"
note "$STEP"
if command -v gcovr > /dev/null 2>&1; then
    {
        CMAKE_BUILD_TYPE=Debug configure "$BUILD_ROOT/cov" \
            -DFASTALLOC_COVERAGE=ON -DFASTALLOC_BUILD_BENCHMARKS=OFF
        # All three ctest-registered binaries (the coverage target runs the
        # full suite incl. fast_alloc_msan_harness).
        cmake --build "$BUILD_ROOT/cov" -j"$(nproc)" --target \
            fast_alloc_tests fast_alloc_audit_tests fast_alloc_msan_harness > /dev/null
        cmake --build "$BUILD_ROOT/cov" --target coverage || true
        echo "(coverage report above; HTML/CSV via gcovr flags if needed)"
    } || fail
else
    echo "gcovr not found - skipping coverage (pip install gcovr)"
fi

if [ "$SKIP_SLOW" != "1" ]; then
    STEP="9. fuzz smoke campaigns"
    note "$STEP"
    {
        "$BUILD_ROOT/rel/fast_alloc_fuzz_standalone" 20 20261019 | tee /tmp/fuzz-rel.log
        grep -q "FUZZ-CLEAN" /tmp/fuzz-rel.log
        CMAKE_BUILD_TYPE=Release configure "$BUILD_ROOT/fuzz-dbg" \
            -DFASTALLOC_DEBUG=ON -DFASTALLOC_BUILD_BENCHMARKS=OFF \
            -DFASTALLOC_BUILD_FUZZ=ON
        cmake --build "$BUILD_ROOT/fuzz-dbg" -j"$(nproc)" --target \
            fast_alloc_fuzz_standalone > /dev/null
        "$BUILD_ROOT/fuzz-dbg/fast_alloc_fuzz_standalone" 20 20261020 \
            | tee /tmp/fuzz-dbg.log
        grep -q "FUZZ-CLEAN" /tmp/fuzz-dbg.log
    } || fail

    STEP="10. benchmark smoke"
    note "$STEP"
    {
        "$BUILD_ROOT/rel/fast_alloc_bench_lifecycle" --quick | tee /tmp/bench-lifecycle.log
        "$BUILD_ROOT/rel/fast_alloc_bench_audit" | head -40 || true
        "$BUILD_ROOT/rel/bench_suite" --alloc fast --label smoke \
            --workload small-mixed --threads 2 --ops 50000 --reps 1 \
            --json /tmp/bench-smoke.jsonl
    } || fail
else
    echo "(SKIP_SLOW=1: fuzz + benchmarks skipped)"
fi

note "summary"
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAILURES PRESENT (see !! lines above)"
    exit 1
fi
echo "RESULT: ALL GREEN"
