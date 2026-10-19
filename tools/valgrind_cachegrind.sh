#!/usr/bin/env bash
# ============================================================================
# valgrind_cachegrind.sh — cache-behaviour simulation (cachegrind) of a
# bench_suite workload. Reports instruction/data reference counts, L1 and
# last-level (LL) miss counts and miss ratios — the numbers that dominate
# the allocator's cycle cost — plus the hottest file:function pairs and
# source lines.
#
# Version robustness:
#   * --cache-sim=yes is passed explicitly: valgrind >= 3.24 defaults it
#     to "no", which would silently reduce the profile to bare instruction
#     counts (no misses at all).
#   * LL-miss event names differ across valgrind generations (>= 3.22 uses
#     ILmr/DLmr/DLmw, older ones ILdmr/DLdmr/DLdmw), so the sort keys for
#     the "hottest by LL misses" table are chosen from the events the
#     profile actually recorded.
#
# Profiling gate (soft): numbers vary with the runner's host cache model,
# so nothing asserts on the miss ratios themselves — but the script FAILS
# if the bench binary could not run or cachegrind produced no profile
# (configuration regression).
#
# Determinism: single thread, single rep, fixed op count.
#
# Usage: bash tools/valgrind_cachegrind.sh [workload] [ops] [threads]
#   defaults: cache-thrash 100000 1
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v valgrind > /dev/null || { echo "valgrind not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-valgrind}"
WL="${1:-cache-thrash}"
OPS="${2:-100000}"
TH="${3:-1}"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFASTALLOC_BUILD_BENCHMARKS=ON > /dev/null
cmake --build "$BUILD" -j"$(nproc)" --target bench_suite > /dev/null

OUT="$BUILD/cachegrind.out"
rm -f "$OUT"
echo "=== cachegrind: bench_suite $WL ops=$OPS threads=$TH ==="
# The workload's own one-line JSONL result streams live (tee); valgrind's
# chatter goes to a separate log; the profile itself lands in $OUT.
if ! stdbuf -oL valgrind --tool=cachegrind --cache-sim=yes \
        --cachegrind-out-file="$OUT" \
        "$BUILD/bench_suite" --alloc fast --label cachegrind --workload "$WL" \
        --threads "$TH" --ops "$OPS" --reps 1 2>"$BUILD/cachegrind.vg.log" \
        | tee "$BUILD/cachegrind.bench.log"; then
    echo "!! bench_suite failed under cachegrind (see $BUILD/cachegrind.vg.log)" >&2
    tail -10 "$BUILD/cachegrind.vg.log" >&2 || true
    exit 1
fi
if [ ! -s "$OUT" ]; then
    echo "!! cachegrind produced no profile ($OUT missing/empty)" >&2
    exit 1
fi

command -v cg_annotate > /dev/null 2>&1 || {
    echo "cg_annotate unavailable; raw profile in $OUT" >&2
    exit 0
}

# All cg_annotate invocations write to a file first: piping straight into
# sed/head makes cg_annotate die with a BrokenPipeError traceback once the
# reader closes early, which would spray noise over the CI log.
ANN="$BUILD/cachegrind.annot.txt"

echo "--- program totals ---"
cg_annotate --no-annotate --show-percs "$OUT" > "$ANN"
grep -m1 "PROGRAM TOTALS" "$ANN" | sed 's/^ */  /'
echo "--- miss ratios (derived from the totals above) ---"
awk '
    /^Ir_/ {
        n = split($0, labels, /[ _]+/)
        while ((getline) > 0 && $0 !~ /PROGRAM TOTALS/) { }
        if ($0 !~ /PROGRAM TOTALS/) { exit }
        j = 0
        for (i = 1; i <= NF; i++) {
            if (substr($i, 1, 1) == "(") continue
            if ($i == "PROGRAM" || $i == "TOTALS") break
            j++
            if (j <= n) {
                c = $i; gsub(/,/, "", c)
                val[labels[j]] = c + 0
            }
        }
        if ("Ir" in val)  printf "  I1  miss ratio: %8.3f%%  (%d / %d)\n", 100*val["I1mr"]/val["Ir"], val["I1mr"], val["Ir"]
        if ("ILmr" in val || "ILdmr" in val) {
            ll_i = ("ILmr" in val) ? val["ILmr"] : val["ILdmr"]
            printf "  LLi miss ratio: %8.3f%%  (%d / %d)\n", 100*ll_i/val["Ir"], ll_i, val["Ir"]
        }
        if ("Dr" in val)  printf "  D1r miss ratio: %8.3f%%  (%d / %d)\n", 100*val["D1mr"]/val["Dr"], val["D1mr"], val["Dr"]
        if ("DLmr" in val || "DLdmr" in val) {
            ll_r = ("DLmr" in val) ? val["DLmr"] : val["DLdmr"]
            printf "  LLr miss ratio: %8.3f%%  (%d / %d)\n", 100*ll_r/val["Dr"], ll_r, val["Dr"]
        }
        if ("Dw" in val)  printf "  D1w miss ratio: %8.3f%%  (%d / %d)\n", 100*val["D1mw"]/val["Dw"], val["D1mw"], val["Dw"]
        if ("DLmw" in val || "DLdmw" in val) {
            ll_w = ("DLmw" in val) ? val["DLmw"] : val["DLdmw"]
            printf "  LLw miss ratio: %8.3f%%  (%d / %d)\n", 100*ll_w/val["Dw"], ll_w, val["Dw"]
        }
        exit
    }' "$ANN"

# Hottest file:function pairs, sorted by last-level miss events when the
# profile has them (event names differ across valgrind generations:
# >= 3.22 uses ILmr/DLmr/DLmw, older ones ILdmr/DLdmr/DLdmw), else by the
# default (first) event.
RECORDED=$(sed -n 's/^events: //p' "$OUT")
SORT_KEYS=""
for e in ILmr DLmr DLmw ILdmr DLdmr DLdmw; do
    case " $RECORDED " in *" $e "*) SORT_KEYS="$SORT_KEYS,$e" ;; esac
done
if [ -n "$SORT_KEYS" ]; then
    cg_annotate --no-annotate --show-percs --sort="${SORT_KEYS#,}" \
        --threshold=1 "$OUT" > "$ANN"
else
    cg_annotate --no-annotate --show-percs --threshold=1 "$OUT" > "$ANN"
fi
echo "--- hottest file:function pairs (>=1% of sort event; LL misses first) ---"
sed -n '/File:function summary/,$p' "$ANN" | head -16

echo "--- hottest annotated source lines ---"
# --auto is accepted across generations (newer cg_annotate deprecates it in
# favour of --annotate but still honours it).
cg_annotate --auto=yes --threshold=1 "$OUT" > "$ANN" || true
sed -n '/^-- Annotated source/,$p' "$ANN" | head -40
echo "profile: $OUT  (raw valgrind log: $BUILD/cachegrind.vg.log)"
