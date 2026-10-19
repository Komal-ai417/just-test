# FastAlloc verification & profiling tools

Every script here is self-contained: it configures/uses a build directory
(`build-tools` by default, override with `BUILD_DIR=...`), checks that the
required external tool exists, and exits non-zero on any detected problem.

Run them from the repository root:

```bash
bash tools/<script>.sh [args]
```

| Script | Requires | What it does |
| :--- | :--- | :--- |
| `run_all_verification.sh` | gcc, cmake | Master driver: release+debug tests, ASan/UBSan/TSan/LSan suites, fuzz smoke, coverage, benchmarks smoke. This is the one-command "verify everything" entry point. |
| `valgrind_memcheck.sh` | valgrind | Runs both gtest suites under `valgrind --leak-check=full --track-origins=yes`; the full log streams live to the console (tee'd into `valgrind_<suite>.log`) so hangs are visible in real time; fails on any invalid read/write/free or leak. |
| `valgrind_massif.sh` | valgrind | Heap-growth profile (`massif`) of the memory stress benchmark; produces `massif.out` + `ms_print` text summary. |
| `valgrind_callgrind.sh` | valgrind | Call-level profiling of a bench_suite workload; annotates hot functions. |
| `valgrind_cachegrind.sh` | valgrind, (cg_annotate) | Cache-behaviour simulation of a bench_suite workload: L1 I/D + LL miss counts and derived miss ratios, hottest file:function pairs (LL-miss sorted) and annotated source lines. Passes `--cache-sim=yes` explicitly (valgrind >= 3.24 defaults it off) and tolerates both event-name generations. |
| `perf_stat.sh` | perf | Hardware counter summary (cycles, instructions, IPC, branch & cache misses, faults) for a benchmark run. |
| `perf_record_flamegraph.sh` | perf, git (flamegraph repo) | CPU profile + SVG flame graph of the allocator hot paths. |
| `perf_c2c.sh` | perf (>= 3.x with c2c) | Cache-line-to-cache-line contention / false-sharing report for the multi-threaded churn. |
| `strace_syscalls.sh` | strace | Validates OS interaction: counts mmap/munmap/madvise/mremap (and brk absence) across a workload; asserts the purge path actually releases spans. |
| `heaptrack.sh` | heaptrack | Allocation-hotspot + heap-growth profile of the memory benchmark. |
| `msan_check.sh` | clang, cmake | MemorySanitizer build of the gtest-free/STL-free smoke harness (`tests/msan_harness.cpp`, every public API entry point incl. cross-thread hand-off) with origin tracking; clang-only - see the script header. |
| `coverage_report.sh` | gcovr | gcovr line/branch/function report over the allocator core for a `-DFASTALLOC_COVERAGE=ON` build whose tests already ran; flag set verified against gcovr 5-8 (CI pins 8.7). Writes `coverage.xml` + the summary the CI 70% gate parses. |
| `static_analysis.sh` | cppcheck and/or clang-tidy | Both analysers over `src/`, `include/`, `tests/`, `benchmarks/`, `fuzz/` with warning-as-failure thresholds. |
| `mutation_test.sh` | gcc, cmake, git | Applies ten curated source mutants (boundary flips, guard inversions, off-by-one count drift, disabled canaries) to scratch copies, runs a targeted test subset against each, and gates on an 80% kill rate. `--only m01,m05` runs a subset; `--full` runs the whole validate suite per mutant. |
| `page_cache_env_check.sh` | gcc | Builds a tiny probe that verifies the `FASTALLOC_PAGE_CACHE_MB` and `FASTALLOC_LOG_LEVEL` environment tunables actually change runtime behaviour. |
| `cross_allocator_matrix.sh` | cmake, optional jemalloc/mimalloc | Full cross-allocator benchmark matrix (wrapper over `benchmarks/benchsuite/run_matrix.sh` with auto-detected allocator libraries). |

## Environment notes

* `valgrind`, `perf`, `strace`, `heaptrack` are Linux-only; MSVC/Windows
  equivalents (ETW/WPA, Application Verifier, PageHeap) are run in CI on
  `windows-latest` runners (see `.github/workflows/ci.yml`).
* `perf` needs either a privileged runner (`perf_event_paranoid <= 2`) or
  `sudo`; the script degrades to software counters when hardware ones are
  unavailable.
* Scripts never modify the source tree; mutation testing works on a
  temporary copy under `build-mutation/mutation-work/` (googletest is
  cloned once into that work dir and shared between mutants).
