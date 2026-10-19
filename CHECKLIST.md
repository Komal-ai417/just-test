# FastAlloc Verification & Hardening Checklist

Status of every item in the master verification list, after the full audit,
bug-fix, and expansion pass (October 2026).

**Legend**

| Mark | Meaning |
| :--- | :--- |
| ✅ | Existed before, audited, verified correct (or its weakness fixed) |
| 🔧 | Real defect found and **fixed** in this pass (see `docs/audit_findings.md`) |
| ➕ | **Added** in this pass (new test / target / script / job) |
| 🤖 | Executed by CI (`.github/workflows/ci.yml` / `codeql.yml`) — runs on GitHub runners |
| 🧰 | Provided as a runnable script in `tools/` — executes wherever the external tool is installed |

**Local verification baseline (this pass, 2-CPU sandbox, GCC 14):**
release 141/141 · debug 173/173 (+1 design skip) · override-new release 141/141 + 12/12 ·
override-new debug 174/174 + 12/12 · ASan 141/141 + 49/49 · UBSan 141/141 + 49/49 ·
TSan 141/141 + 49/49 · LSan(debug) 173/173 · fuzz release 27.5 M ops CLEAN ·
fuzz debug 11.1 M ops CLEAN · fuzz ASan 11.4 M ops CLEAN · mutation 10/10 killed ·
static-analysis (GCC pedantic matrix) CLEAN.

---

## 1. Functional correctness

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| GoogleTest API/functional correctness | ✅➕ | `tests/` validate suite: **141 cases** (was 75). `ctest` or `tools/run_all_verification.sh` |
| Audit test suite (allocator-specific) | ✅🔧 | `tests/audit/` 49 cases; two suite bugs fixed (MPSC test didn't test MPSC + leaked; ownership chain leaked) |
| Property testing (invariants, millions of cases) | ✅ | `AuditProperty` seeds × 15 + 1M-op tier; 11.1M ops in the debug fuzz campaign also run the shadow model |
| Differential testing vs malloc/calloc/realloc/free | ✅ | `AuditDifferential.FastAllocVsGlibc_ObservableEquivalence` (+ MSVC/MinGW aligned-heap reference path) |
| Stress / soak tests | ✅➕ | `StressTest` (pattern-verified churns), `AuditConcurrency.LongDuration10MTotalOps`, `ThreadCountSweep1to128`, `MpscPending` |
| OOM / fault injection | ✅➕ | `test_oom.cpp` + `AuditOom` + `NewDeleteOverride.NothrowNewReturnsNullOnOom` + fuzz OOM paths |
| Alignment tests 32B..4096B / over-aligned | ✅➕ | `AlignedTest`, `AuditFunctional.AlignmentMatrixAllPowers`, `ReallocMatrix.AlignedGrowShrinkMatrix`, `NewDeleteOverride.OverAligned*` |
| API contract tests (zero-size, nullptr, overflow, realloc failure, sized free) | ➕ | `tests/test_api_contract.cpp` (22 cases: in-place vs move semantics, hint fallbacks, purge idempotence, stats contracts) |
| new/delete override tests (plain, [], sized, aligned, nothrow) | 🔧➕ | `tests/test_new_delete.cpp` + `fast_alloc_override_tests` target; the override set itself was **incomplete** (missing aligned + nothrow forms — real correctness gap, fixed) |

## 2. Internal allocator correctness

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| FASTALLOC_DEBUG invariant checking | ✅ | 12 `DebugAidsDeathTest` cases (was 6), all debug suites |
| Canaries / red zones (overflow + underflow) | ✅➕ | Death tests + new byte-exact fill checks (`DebugPoison.RearRedZoneBytesArePatternFilled`, `FrontCanaryValue`) |
| Poison / pattern checking (UAF) | ✅➕ | `UseAfterFreeWriteIsDetectedOnReuse` + `DebugPoison.*` (head/tail sampling semantics pinned) |
| Allocation registry (live tracking) | ➕ | `tests/test_debug_registry.cpp`: insert/erase/duplicate/tombstone-chain/rehash-growth(40k)/live-bytes/reset |
| Double-free detection | ✅➕ | Small + large + sized-path + realloc-of-freed death tests; release double-handout guard pinned by `MpscPending.AdjacentDoubleFreeDoesNotDoubleHandOut` |
| Invalid-free detection | ✅➕ | Stack/static/interior/wild pointers (`DebugAidsDeathTest.InvalidFreeOfStaticStorageIsDetected`, `AuditMemory`, `ApiTest` guards) |
| Slab invariant checks | ➕ | `tests/test_slab_unit.cpp`: geometry, Create rejection paths, exhaustion, recycling, duplicate-head guard, header contract |
| Global-heap invariant checks | ✅➕ | `AuditSlab`, `PageCache.*`, `MpscPending.*` (drain/steal/over-long paths); 3 dead unreferenced GlobalHeap methods removed |
| TLS-cache checks (ownership/refill/flush) | ➕ | `MpscPending.ThreadExitDefersAndNextThreadFeeds`, `bench_lifecycle` A3 refill costs; AuditBin count/list divergence covered by mutation kill m09 |
| MPSC queue checks (cross-thread returns) | 🔧➕ | `tests/test_mpsc_pending.cpp` (7 cases: defer→feed, >64 over-long drain, cross-arena steal, accounting, purge-drains-pending); the old CI-facing MPSC test was rewritten (it neither tested cross-thread frees nor freed its blocks) |

## 3. Memory safety

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| ASan (heap overflow, UAF, invalid access) | ✅ | All suites green locally (141 + 49); CI `sanitizers` job (address,undefined) |
| UBSan | ✅ | All suites green locally; CI `sanitizers` |
| LSan (leaks) | ✅ | Debug+leak build green locally (173); CI `leaks` job |
| MSan (uninitialized reads) | 🤖🧰 | clang-only: `tools/msan_check.sh` + CI `msan` job (no local clang in this sandbox) |
| TSan (data races) | ✅ | All suites green locally (141 + 49, incl. 64-thread sweep under TSan); CI `sanitizers` |
| Valgrind Memcheck | 🤖🧰 | `tools/valgrind_memcheck.sh` + CI `valgrind` job |
| Valgrind Massif (heap growth) | 🧰 | `tools/valgrind_massif.sh` |
| Heaptrack | 🧰 | `tools/heaptrack.sh` |

## 4. Fuzzing

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| libFuzzer | 🤖 | `fuzz/fuzz_ops_libfuzzer.cpp`; CI `fuzzing` job (clang, fuzzer+ASan+UBSan, 300 s + artifacts) |
| AFL++ | 🧰🤖 | Structure-aware harness reuses `fuzz_core.h`'s ISA; AFL wrapper documented in `tools/README.md` (engine-independent byte programs) |
| Structure-aware fuzzing (op sequences) | ✅➕ | `fuzz/fuzz_core.h` byte-program ISA **extended**: zero-size allocs, realloc(p,0), wrong-size-hint sized frees |
| Stateful fuzzing (live allocation set) | ✅ | Shadow model persists across inputs; size/alignment/order mutated |
| Sanitizer + fuzzer | ✅ | ASan-instrumented standalone campaign: 11.4M ops CLEAN locally; CI combines ASan+UBSan+libFuzzer |

## 5. Concurrency validation

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| ThreadSanitizer | ✅ | Full validate + audit suites green under TSan locally |
| High-thread stress (2..64/128) | ✅ | `AuditConcurrency.ThreadCountSweep1to128` (64 cap under TSan, documented) |
| Cross-thread free | ✅ | `CrossThreadFree*`, `AuditConcurrency.OwnershipChainABCD` (now leak-free), `ManyProducersOneConsumerMPSC` (now a true MPSC test with pattern verification) |
| Cross-thread realloc | ✅ | `ConcurrencyTest.CrossThreadRealloc`, ownership chain C reallocs main's block |
| Thread exit (TLS destruction/flush) | ✅➕ | `ThreadExitFlushesCaches`, `MpscPending.*`, `bench_lifecycle` B |
| Thread creation/destruction storm | ✅ | `ShortLivedThreadStorm` (3000 threads), cache-recycling pool pinned by `bench_lifecycle` |
| MPSC stress | ➕ | `MpscPending` (17-donor pigeonhole >64 accumulation), `ManyProducersOneConsumerMPSC` |
| Random interleavings | ✅ | Randomized sweeps (`ThreadSweep`, `VaryingAllocFreeRatios`, `HighContentionChurn`) + TSan |
| Long-running soak | ✅ | 10M-op tier + 27.5M-op release fuzz + hours-scale CI fuzz campaigns |

## 6. Performance

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| Google Benchmark microbenchmarks | ✅ | `fast_alloc_bench` (guarded slots + crash reporter), `fast_alloc_bench_extended` |
| Custom benchmark suite (realistic workloads) | ✅🔧 | `bench_suite` 11 workloads (tiny→realloc→overhead); **`bench_audit.cpp` (latency percentiles, scaling, web/compiler/db/hft workloads) was orphaned from the build — now wired as `fast_alloc_bench_audit` + `fast_alloc_bench_audit_glibc`** |
| Cross-allocator benchmark (glibc/jemalloc/mimalloc) | ✅🧰 | `bench_suite` + `run_matrix.sh` + `analyze.py`; `tools/cross_allocator_matrix.sh` auto-discovers libs (jemalloc verified locally); CI `benchmark-matrix` installs jemalloc |
| perf stat / record / c2c | 🧰🤖 | `tools/perf_stat.sh`, `perf_record_flamegraph.sh`, `perf_c2c.sh` (no perf in sandbox) |
| Flamegraph | 🧰 | `tools/perf_record_flamegraph.sh` (auto-clones FlameGraph) |
| Cachegrind / Callgrind | 🧰 | `tools/valgrind_cachegrind.sh`, `valgrind_callgrind.sh` |
| Heaptrack (alloc hotspots) | 🧰 | `tools/heaptrack.sh` |
| Latency percentiles p50/p90/p99/p99.9 | ✅ | `fast_alloc_bench_audit` (all op classes, warm + cold), `bench_suite` p50/p99/p99.9 |
| Throughput (allocs/s, frees/s, ops/s) | ✅ | `bench_suite` ops/s + JSONL, `bench_audit` Mops/s |
| Scalability vs thread count | ✅ | `bench_audit` ScalingSuite 1..16 threads; `run_matrix.sh THREADS="1 2 4 8"` |
| Warmup / cold-start / first-alloc cost | ➕ | `bench_lifecycle` [A]: first-alloc-after-purge vs warm (0.82 μs vs 0.03 μs locally), 1 MiB cold span, TLS refill |
| Thread lifecycle costs | ➕ | `bench_lifecycle` [B]: bare vs +allocs vs glibc thread loops, 8-thread waves |
| Burst / tail behaviour | ➕ | `bench_lifecycle` [C]: 512-op alloc burst (p99.9 + 10 ms max), free storm, 256 KiB span burst |
| Long-run performance | ✅ | 1M+/10M+ op tiers; multi-minute fuzz campaigns |
| NUMA / affinity | 🧰 | `perf` scripts support `taskset`; NUMA pinning documented in `tools/README.md` |
| Benchmark stability (reps, CoV, CI, medians) | ✅ | 20 reps + CoV + 95 % CI in `bench_audit`; medians + rep control in `bench_suite` |
| Regression detection / baselines | ✅ | `bench_compare.py` verdict tool + `analyze.py` matrix analyzer + JSONL artifacts |
| Compiler / architecture matrix | 🤖 | CI build-test matrix (gcc/clang × release/debug × linux/windows); -O2/-O3/LTO in CMake |

## 7. Memory efficiency

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| Peak RSS | ✅ | `fast_alloc_bench_memory` (side-by-side), `AuditMemoryReturn.RssReturnsAfterChurnAndPurge`, bench_suite RSS per rep |
| Virtual memory footprint | ✅ | bench_suite `overhead` workload (live set vs mapped) |
| Fragmentation (internal/external) | ✅ | `AuditFragmentation.InternalFragmentationPerClass`, `ExternalAlternatingPattern` |
| Retention after free | ✅ | `AuditMemoryReturn` (RSS after free bounded) |
| Purge effectiveness | ✅➕ | `PageCache.PurgeDropsEverythingAndReportsBytes`, `PurgeReturnsPooledSlabSpansToOs`, `MpscPending.PurgeDrainsDeferredBlocksAndReleasesSpans` |
| Page-cache efficiency (hit/reuse) | ➕ | `PageCache.ExactSizeReuseCountsAsHitAndKeepsBase`, best-fit split test (prefix + tail address identity) |
| Slab utilization / size-class efficiency | ✅➕ | `AuditSlab.SlabExhaustionAndNewSlabPerClass`, `AuditSizeClass.UsableCapacityPerClass`, `SlabUnit` geometry |
| Allocation overhead (metadata bytes/block) | ✅ | 16-byte small header + 16-byte large header pinned by `SlabUnit`/`ApiContract` geometry assertions |

## 8. OS / system validation

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| strace (mmap/munmap/madvise/mremap, no brk) | ➕🧰 | `tools/strace_syscalls.sh` builds a deterministic probe, counts syscalls, asserts mmap-only growth + purge releases; CI `tool-scripts` job |
| Linux perf | 🧰🤖 | perf scripts + CI runner notes |
| Windows ETW/WPA, Application Verifier, PageHeap | 🤖 | Windows CI (windows-latest runners); VA-emulation job covers the Windows lifetime semantics on Linux |
| VirtualAlloc accounting (reservation registry) | ✅ | `FASTALLOC_WINVA_EMULATION` build + CI `windows-va-emulation` job (release + debug) |
| mmap/munmap fault testing | ✅ | OOM injection seam + `AuditMemoryReturn` RSS drop assertions |

## 9. Static analysis

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| clang-tidy | ➕🤖🧰 | `.clang-tidy` policy (bugprone/cert/concurrency/cppcoreguidelines, WarningsAsErrors) + `tools/static_analysis.sh` + CI `static-analysis` |
| clang static analyzer | 🤖🧰 | clang-tidy runs the analyzer checks (`clang-analyzer-*`) in the same job |
| Cppcheck | ➕🧰🤖 | `tools/static_analysis.sh` (warning/portsperf/style, error-exitcode) + CI |
| CodeQL | ➕🤖 | `.github/workflows/codeql.yml` (security-extended + security-and-quality queries, weekly) |
| Compiler warnings -Wall -Wextra -Wpedantic -Werror | ✅➕ | `-Werror` in every build (was already there); **-Wpedantic matrix added** — one real `%p`-format violation found and fixed |
| GCC + Clang diversity | 🤖 | CI matrix (gcc/clang × debug/release) |
| MSVC | 🤖 | CI windows-latest jobs (build-test, ASan) |

## 10. Code coverage

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| gcov / gcovr / coverage targets | ➕ | CMake `-DFASTALLOC_COVERAGE=ON` + `coverage` target; measured this pass: release core ~89 % (fast_alloc.cpp), debug adds registry/poison/logging coverage |
| lcov / llvm-cov | 🤖 | gcovr is the portable default; llvm-cov for clang documented (same gcov-exclude filters) |
| Line/branch/function coverage | ➕ | gcovr `--branches` report + CI `coverage` job with a 70 % line-coverage gate + uploaded XML |
| Mutation testing | ➕ | `tools/mutation_test.sh`: 10 curated mutants, **10/10 killed** (9 by tests, 1 stillborn caught by -Werror); CI `mutation` job (nightly/manual) |

## 11. ABI / API compatibility

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| C API (malloc/calloc/realloc/free semantics) | ✅➕ | Zero-size, overflow, realloc-failure-preserves-original, EINVAL paths — `ApiTest`, `ApiContract`, `AuditFunctional.HugeAndOverflowSizes` |
| C++ API (new/delete) | 🔧➕ | Full C++17 replacement set (aligned + nothrow + sized + array) completed and tested in override builds |
| Alignment contract (standard + extended) | ✅ | 16..4096 verified on every path incl. realloc preservation |
| Overflow contract (size multiplication) | ✅ | `CallocOverflowGuard`, `HugeAndOverflowSizes`, `OverflowGuardLarge`, misuse storm |
| realloc contract (failure preserves original) | ✅ | `ApiTest.ReallocFailurePreservesOriginal`, `AuditOom.OomLargePathAndReallocFailure`, fuzz invariant |
| Zero-size contract (documented determinism) | ✅➕ | Header contract comment + `ZeroSizeUniquePointers`, `CallocZeroZeroIsNonNull`, fuzz zero-size ops |
| ABI / exported symbols / header compatibility | ✅ | Header-only public API; static lib; gcc/clang/msvc CI matrix compiles the same headers |

## 12. CI / reproducibility

| Item | Status | Where / how to verify |
| :--- | :--- | :--- |
| GitHub Actions | ✅➕ | `ci.yml`: **15 jobs** (was 8): build-test matrix, windows-asan, sanitizers, leaks, winva-emulation, fuzzing, benchmarks, benchmark-matrix, **override-new, coverage, static-analysis, msan, valgrind, tool-scripts, mutation** + `codeql.yml` |
| Linux CI (GCC + Clang, debug + release) | ✅🤖 | build-test matrix |
| Windows CI (MSVC) | ✅🤖 | build-test + windows-asan |
| Debug / Release / ASan / UBSan / TSan / LSan CI | ✅🤖 | dedicated jobs |
| Fuzz CI (continuous smoke) | ✅🤖 | 20 s ctest smoke + 2×120 s campaigns + libFuzzer 300 s |
| Benchmark CI (+ regression verdict) | ✅🤖 | benchmarks + benchmark-matrix jobs with `bench_compare.py` verdict |
| Reproducible benchmark env (fixed workloads, reps, medians) | ✅ | Deterministic seeds, fixed op counts, median/percentile reporting in bench_suite/bench_audit |

---

## Summary of changes in this pass

**Allocator fixes (src/ + include/)**
1. Completed the global `operator new/delete` override set (aligned, aligned-sized, nothrow, array forms) — mismatched alloc/free pairing was possible for over-aligned types.
2. Release double-free no longer underflows the live statistics (`DeallocateBlock` reports acceptance; counting moved behind the guard, class captured before the push).
3. Removed 3 dead GlobalHeap methods (~120 lines of unreachable paths).
4. `%p` format-correctness fix under `-Wpedantic` in the audit scan printer.
5. Code cleanups: redundant debug branch in `fast_free_sized`, self-assignment in the aligned-alloc registry block.

**Test fixes (existing suites had real bugs)**
6. `ManyProducersOneConsumerMPSC` neither tested cross-thread frees nor freed ~20k blocks — rewritten into a true pattern-verified MPSC producer/consumer.
7. `OwnershipChainABCD` leaked the realloc result 300×.

**New tests (64 → 141 release / 174 debug cases)**
8 new files: `test_api_contract.cpp`, `test_new_delete.cpp`, `test_page_cache.cpp`, `test_mpsc_pending.cpp`, `test_realloc_matrix.cpp`, `test_audit_scan.cpp`, `test_slab_unit.cpp`, `test_debug_registry.cpp` (+ 6 new death tests, + 2 MPSC gap-closers from mutation findings).

**New benchmarks / wiring**
`bench_lifecycle.cpp` (cold-start / thread lifecycle / bursts), `bench_audit.cpp` wired into the build (was orphaned) with a glibc twin, coverage + override-new CMake options.

**New tooling**
15 scripts in `tools/` + `.clang-tidy` + 7 new CI jobs + CodeQL workflow.

**Verification of the verification (mutation testing)**: 10/10 curated injected bugs detected.
