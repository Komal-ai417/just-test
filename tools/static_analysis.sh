#!/usr/bin/env bash
# ============================================================================
# static_analysis.sh — independent static analysis of the whole codebase:
#   1. cppcheck (if installed)
#   2. clang-tidy (if installed) using the repo's .clang-tidy config
#   3. compiler warning matrix (GCC -Wall -Wextra -Wpedantic -Werror;
#      adds Clang when present)
# Fails when any analyser reports errors (warnings thresholds per tool).
#
# Usage: bash tools/static_analysis.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
FAIL=0

echo "=== [1/3] compiler warning matrix ==="
for CXX in g++ clang++; do
    command -v "$CXX" > /dev/null 2>&1 || continue
    echo "--- $CXX -Wall -Wextra -Wpedantic -Werror ---"
    for f in src/*.cpp; do
        if ! "$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
                -Iinclude -Isrc -fsyntax-only "$f"; then
            echo "!! $CXX rejected $f" >&2
            FAIL=1
        fi
    done
    for f in tests/*.cpp fuzz/*.cpp benchmarks/*.cpp benchmarks/benchsuite/*.cpp; do
        if ! "$CXX" -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
                -Ibenchmarks -fsyntax-only "$f" 2> /tmp/sa_warn.txt; then
            # Third-party FetchContent headers are not visible here; accept
            # "gtest/benchmark not found" as skip, everything else as fail.
            if grep -qE "benchmark/benchmark.h|gtest/gtest.h" /tmp/sa_warn.txt; then
                :
            else
                cat /tmp/sa_warn.txt >&2
                echo "!! $CXX rejected $f" >&2
                FAIL=1
            fi
        fi
    done
done

echo "=== [2/3] cppcheck ==="
if command -v cppcheck > /dev/null 2>&1; then
    cppcheck --enable=warning,portability,performance,style \
        --std=c++17 --inline-suppr --suppress=missingIncludeSystem \
        --suppress=unusedFunction --error-exitcode=42 \
        -Iinclude -Isrc --platform=unix64 \
        include/ src/ tests/ benchmarks/ fuzz/ 2>&1 | tail -30 || RC=${PIPESTATUS[0]}
    RC=${RC:-0}
    if [ "${RC}" -eq 42 ]; then
        echo "!! cppcheck reported errors" >&2
        FAIL=1
    fi
else
    echo "(cppcheck not installed - skipped)"
fi

echo "=== [3/3] clang-tidy ==="
if command -v clang-tidy > /dev/null 2>&1; then
    # clang-tidy needs a compilation database; generate a throwaway one.
    TIDY_BUILD="${BUILD_DIR:-build-tidy}"
    cmake -S . -B "$TIDY_BUILD" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DFASTALLOC_BUILD_BENCHMARKS=OFF > /dev/null
    # Diagnostics go to stdout; the per-file progress lines and the
    # "N warnings generated" counters go to stderr. Redirecting both into
    # ONE file with >f 2>&1 interleaves unbuffered stderr writes into the
    # middle of buffered stdout lines (observed: bytes clobbered mid-line),
    # so stderr is dropped instead and the log holds clean diagnostics.
    # Without the stdout redirect the log would be empty, the error report
    # below would silently print nothing, and the warning count would
    # always read 0.
    if clang-tidy -p "$TIDY_BUILD" --quiet src/*.cpp > /tmp/tidy.log 2>/dev/null; then
        :
    else
        echo "!! clang-tidy reported errors:" >&2
        grep -E "error:" /tmp/tidy.log | head -20 >&2
        FAIL=1
    fi
    # Summarise warnings but do not fail the run on style suggestions.
    # grep -c exits 1 on zero matches, which would abort this script under
    # set -e/pipefail - hence the || true and the plain echo.
    TIDY_WARN_COUNT=$(grep -cE "warning:" /tmp/tidy.log || true)
    echo "    ${TIDY_WARN_COUNT} clang-tidy warnings (see /tmp/tidy.log)"
    if [ "${TIDY_WARN_COUNT}" -gt 0 ]; then
        # Top offending checks, so a CI log reader can see the shape of the
        # noise without opening the full log.
        grep -oE '\[[a-z0-9-]+-[a-z0-9-]+-[a-z0-9-]+\]$|\[[a-z0-9-]+-[a-z0-9-]+\]$' \
            /tmp/tidy.log | sort | uniq -c | sort -rn | head -5 | \
            sed 's/^/      /'
    fi
else
    echo "(clang-tidy not installed - skipped)"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: STATIC ANALYSIS FAILURES" >&2
    exit 1
fi
echo "RESULT: STATIC ANALYSIS CLEAN"
