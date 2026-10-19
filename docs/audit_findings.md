# FastAlloc Audit Findings & Fix Report

**Scope:** full review of the allocator core, test suites, benchmarks, fuzz
harnesses, build system and CI; bug fixing; gap analysis against the master
verification checklist; and a verification-of-the-verification pass
(mutation testing). All findings below were fixed, verified, and re-run
through the full local matrix (release, debug, override-new × release/debug,
ASan, UBSan, TSan, LSan, three fuzz campaigns, mutation campaign).

---

## F-1 (High, allocator) — Incomplete global new/delete override set

**File:** `include/fast_alloc.h`, `src/fast_alloc.cpp`

The `FAST_ALLOC_OVERRIDE_NEW` mode replaced only the plain and sized forms
of `operator new`/`delete`. C++17 introduces additional mandatory overload
families; when a program performed an over-aligned `new` (any
`alignas(32+)` type, `std::vector<OverAligned>` buffers, etc.) the compiler
selected the **default** aligned `operator new`, while the matching
`operator delete` still routed the pointer into `fast_free`. FastAlloc then
read a header from memory that was never a FastAlloc block: at best the
pointer was dropped (leak), at worst the header misread as a live class
index and corrupted a bin.

**Fix:** completed the replacement set — aligned
`new/new[]/delete/delete[]` (+ sized + nothrow variants) routing through
`fast_aligned_alloc`/`fast_free`, plus plain nothrow `new/new[]` and their
matching deletes (nothrow OOM contract: `nullptr`, never an exception).

**Verified by:** `fast_alloc_override_tests` (12 cases: over-aligned 64 B
and 4096 B objects, aligned arrays, container workloads on over-aligned
types, nothrow-OOM returning `nullptr`, throwing-OOM raising
`std::bad_alloc`, multithreaded new/delete churn) run in both release and
debug override builds, plus the full validate suite executed with the
overrides active (141/141 and 173/173).

## F-2 (Medium, allocator) — Release double-free skewed live statistics

**File:** `src/fast_alloc.cpp`, `src/tls_cache.h`

The release free path decremented the live-block/byte counters **before**
`TLSCache::DeallocateBlock` ran, but the TLS push path contains a
duplicate-head guard that can absorb the push (adjacent double free). Each
absorbed duplicate therefore decremented the counters without a block being
returned — `current_live_blocks` underflowed (the repository's own audit
comments documented this as finding "F3", observed `2^64-99`).

**Fix:** `DeallocateBlock` now returns whether the block was actually
accepted, and the counters are only updated on acceptance. The class index
is captured **before** the push because an overflowing bin hands its batch
to the global heap inside the call, and an emptied slab's span can be
released to the OS within it — reading the header afterwards is a
use-after-unmap (this hazard was introduced and then caught by our own ASan
run during the fix; the final code reads the header only before the push).

**Verified by:** `AuditMemory.DoubleFreeReleaseAbsorbed` (existing
tolerance-based test, now exact), `MpscPending.AdjacentDoubleFreeDoesNotDoubleHandOut`
(new: pins that a guarded adjacent double free cannot cause a double
hand-out), full sanitizer matrix.

## F-3 (High, test bug) — MPSC test neither tested MPSC nor freed its blocks

**File:** `tests/audit/test_audit_concurrency.cpp`
(`AuditConcurrency.ManyProducersOneConsumerMPSC`)

The "many producers → one consumer" test stored each producer pointer
*inside the block itself* and then discarded it: ~20 000 live blocks (tens
of MB) were leaked per run, `fast_alloc_purge()` cannot free live blocks,
and the "consumer" never touched a producer block — it only malloc'd and
freed its own 64 B blocks. The test therefore exercised **zero** MPSC
behaviour while appearing to.

**Fix:** rewritten into a real bounded hand-off queue: producers fill a
tag-derived byte pattern, the single consumer verifies the pattern and
frees every block **cross-thread** (the actual MPSC pending hand-off),
with leak-freedom asserted at shutdown.

## F-4 (Low, test bug) — Ownership chain leaked the realloc result

**File:** `tests/audit/test_audit_concurrency.cpp`
(`AuditConcurrency.OwnershipChainABCD`)

`q` (realloc'd on thread C) was never freed — 300 leaked blocks per run.

**Fix:** freed at the end of each round.

## F-5 (Low, code quality) — Dead code in GlobalHeap

**File:** `src/global_heap.h`, `src/global_heap.cpp`

`GlobalHeap::AllocateBlock`, `GlobalHeap::DeallocateBlock(Slab*, void*)`
and `GlobalHeap::DeallocateBatch(FreeBlock*)` had no callers anywhere
(library, tests, benchmarks, fuzz) but accounted for ~120 lines of
unexercisable branches that polluted coverage and rotted silently.

**Fix:** removed (declaration + definition). The single-class and
deferred-return paths they duplicated remain fully covered.

## F-6 (Low, correctness under strict warnings) — `%p` format violations

**File:** `src/global_heap.cpp` (`GlobalHeap::AuditPointers`)

Two `fprintf(... "%p", const_cast<Slab*>(...))` calls passed `Slab*` where
`void*` is required — rejected by `-Wpedantic` (`-Wformat` in strict mode).

**Fix:** explicit two-step casts to `void*`. The new
`tools/static_analysis.sh` compiler matrix (`-Wall -Wextra -Wpedantic
-Werror` over src, tests, benchmarks, fuzz) now gates this permanently.

## F-7 (Medium, build system) — Benchmarks not wired into the build

**File:** `CMakeLists.txt`, `benchmarks/bench_audit.cpp`

`bench_audit.cpp` — 259 lines implementing latency percentiles (p50/p90/
p99/p99.9), warm-vs-cold suites, throughput scaling with CoV + 95 % CI,
and four realistic workload profiles — was **not referenced by any CMake
target**: it could not be built, run, or see CI.

**Fix:** wired as `fast_alloc_bench_audit` plus a `USE_GLIBC` twin
(`fast_alloc_bench_audit_glibc`) for same-binary differential output; both
built and run in this pass (see `docs/performance_report.md` numbers).

## F-8 (Low, CI hygiene) — CI referenced a non-existent script

**File:** `.github/workflows/ci.yml`

A fuzz-job comment pointed at `audit-scripts/build_fuzz.sh`, which is not
in the repository (historical leftover from a squashed branch).

**Fix:** comment corrected to describe the inline clang++ invocation.

## F-9 (Medium, test-infra interaction) — Override-mode OOM tests vs gtest

**File:** `tests/test_new_delete.cpp`

Two independent interactions surfaced while validating the new override
tests:

1. GCC elides a `new T[n]` expression whose result is discarded (both
   `(void)new ...` and an unused `auto* r = new ...` followed by `delete`
   compile to nothing at `-O1+`). A death-test-style probe around such an
   expression silently tests nothing.
2. With the overrides active, gtest formats **failure messages through
   allocations**; an armed OOM countdown at that moment makes gtest itself
   throw `std::bad_alloc` from inside its reporting path, masking the real
   verdict.

**Fix:** the OOM test binds and prints the pointer (defeats elision),
clears the countdown before any `EXPECT` machinery, and asserts the
throw/nothrow outcomes via explicit try/catch flags.

Additionally, in debug+override builds the fork-style gtest death-test
child's stderr capture proved unreliable for one case; that test now
selects the re-exec (`threadsafe`) death-test style explicitly. The
allocator's detection itself was proven correct three independent ways
(standalone binary, manual fork probe, re-exec death test).

## F-10 (Low, test-infra hazard) — `AllocRegistry::ResetForTesting` in a live process

**File:** `tests/test_debug_registry.cpp`

Resetting the registry wipes the **global** allocation table. In ordinary
builds the test binary's non-FastAlloc allocations are untracked, so this
was harmless; in override-new builds every gtest/libstdc++ allocation is
tracked, and the reset orphaned live records — the next ordinary free
fatal'd as a double free.

**Fix:** the registry unit tests were made delta-based (no reset), and the
reset-semantics test is compiled only in non-override builds with an
explanatory guard.

---

## Mutation testing (verification of the verification)

`tools/mutation_test.sh` applies 10 curated mutants mirroring real
allocator bug classes and runs the fast test subset against each:

| Mutant | Description | Result |
| :--- | :--- | :--- |
| m01 | `SizeToClassIndex` off-by-one (class boundary) | KILLED by tests |
| m02 | `USER_OFFSET` miscounted (header overlap) | KILLED by tests |
| m03 | TLS duplicate-push guard disabled (double hand-out) | KILLED by new `MpscPending` test (gap closed this pass) |
| m04 | Thread-exit deferred hand-off disabled | STILLBORN — rejected at build by `-Werror` (load-bearing constant) |
| m05 | Purge skips the pending drain (retention leak) | KILLED by new `MpscPending` test (gap closed this pass) |
| m06 | Page-cache split loses the tail span | KILLED by `PageCache.BestFitSplitServesPrefixAndParksTail` |
| m07 | OOM injection seam disabled | KILLED by the OOM suites |
| m08 | Aligned-stash slack too small (red-zone trim) | KILLED in debug build by `AlignedTest` (routing fixed this pass) |
| m09 | TLS flush count off-by-one (list/count divergence) | KILLED in debug build via `AuditBin` divergence fatal |
| m10 | Front-canary check disabled | KILLED by `DebugAidsDeathTest` |

**Kill rate: 10/10.** Two genuine coverage gaps (m03, m05) and one mutant
routing error (m08) were found *by this campaign* and closed with new
tests; the initial campaign also exposed two scripting bugs (gtest filter
separator, unterminated sed) which are fixed with a sed-verification guard.

## Verification matrix executed after all fixes

| Configuration | Result |
| :--- | :--- |
| Release validate suite | 141/141 PASS |
| Debug (FASTALLOC_DEBUG) validate suite | 173/173 PASS (+1 intentional skip) |
| Override-new release (validate + override suites) | 141/141 + 12/12 PASS |
| Override-new debug (validate + override suites) | 173/173 + 12/12 PASS |
| ASan (validate + audit) | 141/141 + 49/49 PASS |
| UBSan (validate + audit) | 141/141 + 49/49 PASS |
| TSan (validate + audit) | 141/141 + 49/49 PASS |
| LSan (debug validate) | 173/173 PASS |
| Fuzz, release semantics (60 s) | 566 203 inputs / 27.5 M ops CLEAN |
| Fuzz, debug semantics (45 s) | 229 376 inputs / 11.1 M ops CLEAN |
| Fuzz, ASan-instrumented (45 s) | 235 008 inputs / 11.4 M ops CLEAN |
| Static analysis (GCC pedantic matrix) | CLEAN |
| Environment tunables (`FASTALLOC_PAGE_CACHE_MB`, `FASTALLOC_LOG_LEVEL`) | OK (both directions) |
| Cross-allocator smoke (glibc / jemalloc / FastAlloc, checksummed) | OK |
| Mutation campaign | 10/10 killed |
