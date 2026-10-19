#!/usr/bin/env bash
# ============================================================================
# coverage_report.sh — gcovr report over the allocator core (everything that
# lives under src/ plus the internal headers it pulls in) for a build
# configured with -DFASTALLOC_COVERAGE=ON whose tests have ALREADY run
# (the .gcda files are written when each test binary exits).
#
# The flag set is verified against gcovr 5.x through 8.x:
#   --gcov-object-directory   where the .gcda/.gcno files live (the
#                             pre-8 spelling --object-directory is still
#                             accepted as a deprecated alias)
#   -s / --txt-summary        prints the "lines: NN.N% ..." summary block
#   --branches is NOT used: gcovr 7 removed the flag (branch data is part
#   of the default report since then) and gcovr 8 rejects it outright.
#
# Usage:   bash tools/coverage_report.sh [build-dir]     (default: build)
# Output:  per-file text table + summary on stdout; coverage.xml in the
#          current directory (override with COVERAGE_XML=...).
# Exit:    0 report generated; 2 prerequisites missing; 3 no coverage data.
#
# CI gates on the "lines: NN%" figure from this script's stdout
# (.github/workflows/ci.yml, job "coverage", 70% floor).
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build}"

command -v gcovr > /dev/null ||
    { echo "!! gcovr not installed (pip install gcovr)" >&2; exit 2; }
[ -d "$BUILD_DIR" ] ||
    { echo "!! build directory '$BUILD_DIR' not found" >&2; exit 2; }

# shellcheck disable=SC2012
if [ -z "$(ls "$BUILD_DIR"/CMakeFiles/*/*.gcda "$BUILD_DIR"/CMakeFiles/*/*/*.gcda 2>/dev/null)" ]; then
    echo "!! no .gcda files under '$BUILD_DIR' - build with -DFASTALLOC_COVERAGE=ON" \
         "and run the tests (ctest) first" >&2
    exit 3
fi

echo "=== gcovr line/branch coverage (allocator core: src/, include/) ==="
gcovr --root . \
      --gcov-object-directory "$BUILD_DIR" \
      -e '.*tests/.*' -e '.*_deps/.*' -e '.*fuzz/.*' -e '.*CompilerId.*' \
      --gcov-ignore-parse-errors negative_hits.warn \
      --merge-mode-functions separate \
      --txt -s \
      --xml "${COVERAGE_XML:-coverage.xml}"
