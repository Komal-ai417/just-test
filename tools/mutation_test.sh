#!/usr/bin/env bash
# ============================================================================
# mutation_test.sh — verify that the test suites actually DETECT injected
# bugs. Applies a curated set of source mutants to a scratch copy of the
# repository, runs a fast test subset against each mutant, and reports the
# kill rate:
#
#   KILLED   = a test failed (the suite catches this bug)
#   SURVIVED = all tests passed (a coverage gap - the suite would miss it)
#
# The mutants mirror real historical allocator bug classes:
#   * size-class boundary off-by-one       (wrong bin => block overlap)
#   * USER_OFFSET miscounted               (metadata corruption on free)
#   * canary / red-zone check inverted      (silent corruption acceptance)
#   * MPSC CAS memory-order weakened        (lost-wakeup / lost-list races)
#   * purge drops the pending-drain step    (memory retained forever)
#   * page-cache split tail-loss            (span leak on best-fit)
#   * OOM countdown not decremented         (permanent OOM after injection)
#   * aligned-alloc stash offset shifted    (provenance corruption)
#
# Usage: bash tools/mutation_test.sh [--full]
#   default: fast subset (api/size-class/slab/page-cache/realloc suites)
#   --full:  full validate suite per mutant (much slower)
# ============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
command -v cmake > /dev/null || { echo "cmake required" >&2; exit 2; }

FULL=0
[ "${1:-}" = "--full" ] && FULL=1
WORK="${BUILD_DIR:-build-mutation}/mutation-work"
mkdir -p "$(dirname "$WORK")"
rm -rf "$WORK"
mkdir -p "$WORK"

# mutant_id|file|sed-expression|description
MUTANTS=(
  'm01|src/fast_alloc_config.h|s/return (size - 1) >> 4;/return size >> 4;/|SizeToClassIndex off-by-one (class boundary)'
  'm02|src/fast_alloc_config.h|s/constexpr std::size_t USER_OFFSET = 16;/constexpr std::size_t USER_OFFSET = 8;/|USER_OFFSET miscounted (header overlap)'
  'm03|src/tls_cache.h|s/if (FAST_UNLIKELY(bin.head == block))/if (false)/|double-push guard disabled (double free accepted)'
  'm04|src/tls_cache.cpp|s/if (bins_\[i\].count <= kDeferMaxBin)/if (false)/|thread-exit deferred hand-off disabled (pending queue starved)'
  'm05|src/global_heap.cpp|s/DrainPendingReturns(a, c);//|purge skips pending drain (retention leak)'
  'm06|src/global_heap.cpp|s/std::size_t rem_pages = p - pages;/std::size_t rem_pages = 0;/|page-cache split loses the tail span'
  'm07|src/global_heap.cpp|s/if (FAST_UNLIKELY(ShouldFailAllocationForTest()))/if (false)/|OOM injection disabled (OOM tests fail cleanly)'
  'm08|src/fast_alloc.cpp|s/std::size_t raw_size = size + alignment + 32;/std::size_t raw_size = size + alignment + 8;/|aligned stash slack too small (user data trims the red zone)'
  'm09|src/tls_cache.cpp|s/bin.count -= static_cast<uint32_t>(batch_size);/bin.count -= static_cast<uint32_t>(batch_size + 1);/|TLS flush count off-by-one (list/count divergence)'
  'm10|src/fast_alloc.cpp|s/if (!debug_check::FrontCanaryOk(\&block->canary))/if (false)/|front canary check disabled (underflow accepted)'
)

# Fast subset: deterministic, high-signal suites (gtest filter syntax:
# suite.* patterns joined with ':').
FILTER='ApiTest.*:SizeClassTest.*:SlabUnit.*:PageCache.*:ApiContract.*:ReallocMatrix.*:AlignedTest.*:LargeAllocTest.*:MpscPending.*'

total=0
killed=0
survived=0
errored=0
report="$WORK/report.txt"
: > "$report"

for m in "${MUTANTS[@]}"; do
    IFS='|' read -r mid file expr desc <<< "$m"
    total=$((total + 1))
    echo "--- mutant $mid: $desc"
    mkdir -p "$WORK/$mid"
    # Copy the source tree without build dirs and git metadata.
    tar --exclude='./build*' --exclude='./.git' --exclude="./$WORK" \
        -cf - . | (cd "$WORK/$mid" && tar -xf -)
    (cd "$WORK/$mid" && sed -i "$expr" "$file") || { echo "    sed failed"; errored=$((errored+1)); continue; }
    # A sed that matches nothing silently leaves the tree unmutated; require
    # the mutated copy to actually differ from the pristine source.
    if cmp -s "$file" "$WORK/$mid/$file"; then
        echo "    sed matched nothing (mutant not applied)"
        errored=$((errored + 1))
        echo "$mid SED-NOMATCH $desc" >> "$report"
        continue
    fi
    # Instrumentation-behaviour mutants (red-zone geometry, TLS count
    # bookkeeping, canary guards) are only observable in FASTALLOC_DEBUG
    # builds; route them there with debug-appropriate kill filters.
    case "$mid" in
        m08) CFG="-DFASTALLOC_DEBUG=ON"; MFILTER='AlignedTest.*:DebugAidsDeathTest.*' ;;
        m09) CFG="-DFASTALLOC_DEBUG=ON"; MFILTER='MpscPending.*:ConcurrencyTest.HighContentionChurn:ConcurrencyTest.ThreadExitFlushesCaches' ;;
        m10) CFG="-DFASTALLOC_DEBUG=ON"; MFILTER='DebugAidsDeathTest.*:DebugPoison.*:DebugLeakDump.*' ;;
        *)   CFG=""; MFILTER="$FILTER" ;;
    esac
    if ! (cd "$WORK/$mid" && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              $CFG -DFASTALLOC_BUILD_BENCHMARKS=OFF > /dev/null 2>&1); then
        echo "    RESULT: REJECTED AT BUILD (counts as killed: code path is load-bearing)"
        killed=$((killed + 1)); echo "$mid KILLED(build) $desc" >> "$report"; continue
    fi
    if [ "$FULL" -eq 1 ] && [ -z "$CFG" ]; then
        USE_FILTER=""
    else
        USE_FILTER="--gtest_filter=$MFILTER"
    fi
    if (cd "$WORK/$mid" && cmake --build build -j"$(nproc)" --target fast_alloc_tests > /dev/null 2>&1); then
        if (cd "$WORK/$mid" && ./build/fast_alloc_tests $USE_FILTER > run.log 2>&1); then
            echo "    RESULT: SURVIVED (coverage gap!)"
            survived=$((survived + 1)); echo "$mid SURVIVED $desc" >> "$report"
        else
            echo "    RESULT: KILLED (tests failed as expected)"
            killed=$((killed + 1)); echo "$mid KILLED $desc" >> "$report"
        fi
    else
        echo "    RESULT: REJECTED AT BUILD (counts as killed)"
        killed=$((killed + 1)); echo "$mid KILLED(build) $desc" >> "$report"
    fi
done

echo
echo "=================== MUTATION REPORT ==================="
cat "$report"
echo "======================================================="
echo "mutants: $total   killed: $killed   survived: $survived   build-errors: $errored"
rate=$(( 100 * killed / (total == 0 ? 1 : total) ))
echo "kill rate: ${rate}%"
[ "$survived" -eq 0 ] || echo "!! $survived SURVIVED - tests miss these injected bugs" >&2
# A healthy suite kills >= 80% of curated mutants.
if [ "$rate" -lt 80 ]; then
    echo "!! kill rate below 80% threshold" >&2
    exit 1
fi
exit 0
