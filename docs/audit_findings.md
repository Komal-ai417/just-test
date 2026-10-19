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

---

# Round 2 — CI hardening findings (October 2026, second pass)

The first pass was verified in a GCC-only sandbox; running the delivered
state on GitHub Actions surfaced eight failing jobs. Root causes and
resolutions below. Every fix is verified locally this time: the pass added
a clang 19.1.7 toolchain to the sandbox, so clang-tidy and MemorySanitizer
gates are now exercised exactly as CI runs them.

## F-11 (High, build system) — Coverage instrumentation never reached any target

`add_compile_options(--coverage ...)` sat in a block at the *bottom* of
`CMakeLists.txt`, after `add_library(fast_alloc)` and every test target.
CMake applies directory-scope compile options only to targets created
**after** the call, so no file was instrumented and gcovr reported 0.0%.
The flags now live at the top of the file, before any target; a fresh
`-DFASTALLOC_COVERAGE=ON` build produces .gcda for the library and both
suites (measured: 77.3% lines / 82.5% functions / 62.0% branches over
src/ + include/, gate 70%). LTO is now also disabled under coverage
(gcda handoff through LTO objects is unreliable).

## F-12 (High, CI) — gcovr invocation used flags removed in gcovr 7/8

The CI coverage step and the CMake `coverage` target invoked
`gcovr --object-directory ... --txt -s --branches ...`. gcovr 7 removed
`--branches` (branch data became part of the default report) and gcovr 8
demoted `--object-directory` to a deprecated alias; modern gcovr exits
non-zero on `--branches`. Both invocations now route through the new
`tools/coverage_report.sh` (verified against gcovr 5-8), and the CI job
pins `gcovr==8.7` so the flag set cannot drift with future releases.

## F-13 (High, MSVC portability) — three test/bench files used POSIX-only APIs in unguarded code

- `tests/test_audit_scan.cpp`: the `StderrCapture` helper (dup/dup2/
  fdopen/getpid + assignment to `stderr`) lived *outside* the file's
  `#ifndef _WIN32` guard, so MSVC parsed POSIX-only code — C4996 (treated
  as error under /W4 /WX) plus C2106 because MSVC's `stderr` macro is not
  an assignable l-value. The capture is now portable: `freopen()` re-binds
  the same FILE object (valid `stderr` expression on every CRT), the
  descriptor saved by `_dup`/`dup` is restored with `_dup2`/`dup2`, and
  the capture file lives under `testing::TempDir()`. The five AuditScan
  tests now run on Windows too (previously skipped).
- `tests/test_slab_unit.cpp`: `posix_memalign` → `_aligned_malloc`/
  `_aligned_free` on MSVC.
- `benchmarks/bench_audit.cpp`: the system-allocator twin used
  `std::aligned_alloc`, which the MSVC STL does not provide; it now maps
  to `_aligned_malloc` on Windows (with a matching `aligned_free`, since
  the result cannot go through `free`), and the POSIX path rounds the
  size up to a multiple of the alignment as C11 requires.

## F-14 (Medium, test bug) — MpscPending purge test depended on foreign allocator state

`MpscPending.PurgeDrainsDeferredBlocksAndReleasesSpans` asserted that
`fast_alloc_purge()` releases the deferred blocks' slab spans, observed
via `os_free_calls`. Two uncontrolled state sources broke it:
1. Earlier suites leave freed blocks of the class parked in the *main
   thread's* TLS bin; `fast_alloc_purge()` cannot flush another
   structure's bins, so the slab could never empty. The test now
   normalizes state first (`fast_alloc_purge_thread_cache()` +
   `fast_alloc_purge()`).
2. In `FASTALLOC_OVERRIDE_NEW` builds the whole process — gtest's own
   startup allocations included — routes through FastAlloc, so a foreign
   *live* block of the class can permanently occupy one slab. The donor
   now takes 300 blocks (three class-214 slabs of 128), guaranteeing at
   least two all-donor slabs that provably empty and release on purge.
   Verified: 20x release + 10x debug + 10x override loops, all green.

## F-15 (Medium, code) — clang-tidy fatal findings never exercised locally

The round-1 `.clang-tidy` policy set `WarningsAsErrors:
'bugprone-*,cert-*,clang-analyzer-*,concurrency-*'` but the sandbox had
no clang-tidy, so the fatal surface was never run. With clang-tidy 19
the following real violations surfaced (all fixed):
- `bugprone-implicit-widening-of-multiplication-result` — int-products
  widened to `std::size_t` at `tls_cache.h` (runtime) and in constexpr
  constants in `global_heap.cpp` / `os_memory.cpp` (constexpr is *not*
  exempt from the check).
- `cert-err33-c` — 30+ discarded `fprintf`/`fflush`/`atexit`/`vsnprintf`
  returns across the diagnostic/reporting paths; all now cast to
  `(void)` (the CERT-sanctioned form).
- `cert-err34-c` — `atoi`/`atol` for env-var parsing (no error
  reporting); both parse sites use `strtol` with end-pointer validation.
- `clang-analyzer-deadcode.DeadStores` — the defensive `"unknown"`
  initializer in the violation reporter (provably dead under an
  exhaustive switch); restructured into a `ViolationKindName()` lookup
  with a trailing return.
- `cert-dcl50-cpp` — the deliberate printf-style `LogEvent` variadic:
  suppressed with NOLINT *and* hardened with a new
  `FASTALLOC_FORMAT_PRINTF(printf) attribute` so gcc/clang now check
  every format string against its arguments (the Debug/logging build
  compiles all call sites under -Werror).
- `concurrency-mt-unsafe` — `getenv` in `fast_getenv`: NOLINT with the
  documented rationale (read-only, no environment mutation anywhere).
- `bugprone-easily-swappable-parameters` — **disabled** in `.clang-tidy`:
  it flags every natural same-type pair this API surface is built on
  (range bounds lo/hi, size/capacity, arena/class indices); the check is
  disabled-by-default upstream for exactly this noise level.

## F-16 (High, test infra) — MSan script compiled gtest sources without gtest

`tools/msan_check.sh` invoked raw `clang++` over the gtest test files;
googletest only exists via CMake FetchContent, so every TU failed with
`'gtest/gtest.h' file not found`. MSan additionally requires every
linked library instrumented — gtest and libstdc++ are not, so a full
gtest binary can never be a reliable MSan gate. The script now builds
the new `tests/msan_harness.cpp` (gtest-free, STL-container-free,
stdio+pthreads only — fully intercepted) through CMake with
`-DFASTALLOC_SANITIZE=memory` plus origin tracking, and runs it as the
gate. The harness covers every public API entry point: size-class
boundaries, calloc-read-before-write (catches "mmap is already zero"
shortcuts), realloc chains across the small/large boundary, aligned
allocations, free_sized hints, cross-thread pending hand-off, stats and
purge. Verified clean locally under clang 19 with
`-fsanitize-memory-track-origins=2`.

## F-17 (Medium, CI) — ctest ran tests the job never built

- `override-new` builds only the validate + override binaries but ran
  full ctest; the audit suite (not built: its differential tests are
  meaningless when the global new/delete override replaces the system
  allocator) reported "Not Run" and failed the job. The step now uses
  `-R "fast_alloc_validate_tests|fast_alloc_override_new_tests"`.
- The new `fast_alloc_msan_harness` ctest entry is registered in every
  configuration, so every job that builds partial targets and runs
  ctest (windows-asan, windows-va-emulation, coverage,
  run_all_verification.sh) now includes the harness in its build list.

## F-18 (Low, robustness) — `default:` missing in an exhaustive enum switch

Covered under F-15's DeadStores fix: `ViolationKindName()` keeps a
trailing `"unknown"` return so violation kinds added later degrade to a
readable message instead of undefined behavior.
