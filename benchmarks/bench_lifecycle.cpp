// ============================================================================
// bench_lifecycle — cold-start, thread-lifecycle and burst benchmarks that
// the throughput/latency suites do not isolate:
//
//   [A] cold-start: first-allocation cost after a full purge (fresh process
//       excluded - measure the steady process with cold caches), TLS cache
//       refill cost after a thread-cache flush, and first-touch page-fault
//       cost for a fresh span.
//   [B] thread lifecycle: create -> allocate a burst -> exit (cache flush,
//       pending hand-off, cache recycling) vs the same loop with glibc, and
//       the cost of a bare thread create/join as the baseline.
//   [C] burst behaviour: allocation bursts (fill bins), free storms (empty
//       them) and mixed bursts; measures worst-case burst latency.
//
// Usage:  bench_lifecycle [--quick]
// ============================================================================
#include "fast_alloc.h"
#include "fast_alloc_config.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
using namespace FastAlloc;

using clk = std::chrono::steady_clock;
static double NowUs() {
    return std::chrono::duration<double, std::micro>(clk::now().time_since_epoch()).count();
}

struct Sample {
    double mean;
    double p50;
    double p99;
    double p999;
    double max;
};

static Sample Summarize(std::vector<double>& xs) {
    std::sort(xs.begin(), xs.end());
    Sample s{};
    const double sum = std::accumulate(xs.begin(), xs.end(), 0.0);
    s.mean = xs.empty() ? 0 : sum / static_cast<double>(xs.size());
    auto q = [&](double p) {
        return xs.empty() ? 0 : xs[static_cast<std::size_t>(p * (xs.size() - 1))];
    };
    s.p50 = q(0.50);
    s.p99 = q(0.99);
    s.p999 = q(0.999);
    s.max = xs.empty() ? 0 : xs.back();
    return s;
}

static void Print(const char* tag, const Sample& s, const char* unit) {
    std::printf("  %-34s mean=%9.2f p50=%9.2f p99=%9.2f p99.9=%9.2f max=%10.2f %s\n",
                tag, s.mean, s.p50, s.p99, s.p999, s.max, unit);
}

// ---------------------------------------------------------------------------
// [A] cold-start
// ---------------------------------------------------------------------------
static void ColdStartSuite(bool quick) {
    std::printf("--- [A] cold-start (steady process, caches dropped) ---\n");
    const int reps = quick ? 40 : 200;

    // A1: first small allocation after a full purge (fresh TLS refill +
    // possible fresh slab; everything after comes from the warm bin).
    {
        std::vector<double> cold, warm;
        for (int r = 0; r < reps; ++r) {
            fast_alloc_purge_thread_cache();
            fast_alloc_purge();  // drains pending queues + drops the page cache
            double t0 = NowUs();
            void* p = fast_malloc(64);
            double t1 = NowUs();
            if (p) fast_free(p);
            cold.push_back(t1 - t0);
            // Warm second allocation in the same round.
            double t2 = NowUs();
            void* q = fast_malloc(64);
            double t3 = NowUs();
            if (q) fast_free(q);
            warm.push_back(t3 - t2);
        }
        Print("first alloc after purge", Summarize(cold), "us");
        Print("second (warm) alloc", Summarize(warm), "us");
    }

    // A2: first large allocation after a purge (fresh span mapping).
    {
        std::vector<double> v;
        for (int r = 0; r < (quick ? 20 : 100); ++r) {
            fast_alloc_purge_thread_cache();
            fast_alloc_purge();
            double t0 = NowUs();
            void* p = fast_malloc(1u << 20);
            double t1 = NowUs();
            if (p) {
                std::memset(p, 1, 1u << 20);  // first-touch cost included
                fast_free(p);
            }
            v.push_back(t1 - t0);
        }
        Print("first 1MiB alloc after purge", Summarize(v), "us");
    }

    // A3: TLS refill cost - drain one class's bin, measure the miss+refill
    // allocation against the cached hit.
    {
        std::vector<double> miss, hit;
        for (int r = 0; r < reps; ++r) {
            // Drain the class bin: allocate everything the bin holds.
            std::vector<void*> drain;
            for (int i = 0; i < 64; ++i) drain.push_back(fast_malloc(192));
            for (void* p : drain) fast_free(p);
            // Now the bin is full: hits.
            double t0 = NowUs();
            void* a = fast_malloc(192);
            double t1 = NowUs();
            fast_free(a);
            hit.push_back(t1 - t0);
            // Empty the bin entirely to force a miss+refill next time.
            std::vector<void*> drain2;
            for (int i = 0; i < 4096; ++i) drain2.push_back(fast_malloc(192));
            double t2 = NowUs();
            void* b = fast_malloc(192);
            double t3 = NowUs();
            for (void* p : drain2) fast_free(p);
            if (b) fast_free(b);
            miss.push_back(t3 - t2);
        }
        Print("TLS cache hit alloc (192B)", Summarize(hit), "us");
        Print("TLS cache miss+refill alloc", Summarize(miss), "us");
    }
}

// ---------------------------------------------------------------------------
// [B] thread lifecycle
// ---------------------------------------------------------------------------
static void ThreadLifecycleSuite(bool quick) {
    std::printf("--- [B] thread lifecycle (create -> burst -> exit) ---\n");
    const int rounds = quick ? 200 : 1000;

    // B1: bare thread create/join baseline (no allocator use in the thread).
    {
        std::vector<double> v;
        v.reserve(static_cast<std::size_t>(rounds));
        for (int r = 0; r < rounds; ++r) {
            double t0 = NowUs();
            std::thread t([] {});
            t.join();
            v.push_back(NowUs() - t0);
        }
        Print("bare create/join (no alloc)", Summarize(v), "us");
    }

    // B2: thread doing a 16-allocation burst (TLS init + refill) then exit
    // (cache flush / pending hand-off / cache recycling).
    {
        std::vector<double> v;
        v.reserve(static_cast<std::size_t>(rounds));
        for (int r = 0; r < rounds; ++r) {
            double t0 = NowUs();
            std::thread t([] {
                for (int i = 0; i < 16; ++i) {
                    void* p = fast_malloc(64 + static_cast<std::size_t>(i) * 32);
                    if (p) std::memset(p, 1, 16);
                }
                // Thread exits holding nothing: destructor flush path.
            });
            t.join();
            v.push_back(NowUs() - t0);
        }
        Print("thread + 16 allocs + exit", Summarize(v), "us");
    }

    // B3: same shape with std::malloc as the comparison baseline.
    {
        std::vector<double> v;
        v.reserve(static_cast<std::size_t>(rounds));
        for (int r = 0; r < rounds; ++r) {
            double t0 = NowUs();
            std::thread t([] {
                for (int i = 0; i < 16; ++i) {
                    void* p = std::malloc(64 + static_cast<std::size_t>(i) * 32);
                    if (p) std::memset(p, 1, 16);
                }
            });
            t.join();
            v.push_back(NowUs() - t0);
        }
        Print("thread + 16 mallocs + exit", Summarize(v), "us");
    }

    // B4: interleaved lifetime churn - 8 concurrent short-lived threads at a
    // time, repeatedly (exercises the cache recycling pool under pressure).
    {
        std::vector<double> v;
        const int rounds2 = quick ? 50 : 250;
        for (int r = 0; r < rounds2; ++r) {
            double t0 = NowUs();
            std::vector<std::thread> ts;
            for (int k = 0; k < 8; ++k) {
                ts.emplace_back([] {
                    for (int i = 0; i < 32; ++i) {
                        void* p = fast_malloc(1 + static_cast<std::size_t>(i) * 97);
                        if (p) fast_free(p);
                    }
                });
            }
            for (auto& t : ts) t.join();
            v.push_back(NowUs() - t0);
        }
        Print("8 concurrent short-lived threads", Summarize(v), "us/wave");
    }
}

// ---------------------------------------------------------------------------
// [C] bursts
// ---------------------------------------------------------------------------
static void BurstSuite(bool quick) {
    std::printf("--- [C] burst behaviour ---\n");
    const int reps = quick ? 20 : 100;

    // C1: allocation burst - 512 mixed-size allocations measured as one
    // burst (tail captures the refill/OS-mapping spikes).
    {
        std::vector<double> v;
        v.reserve(static_cast<std::size_t>(reps));
        for (int r = 0; r < reps; ++r) {
            std::vector<void*> keep(512);
            double t0 = NowUs();
            for (int i = 0; i < 512; ++i) {
                keep[static_cast<std::size_t>(i)] =
                    fast_malloc(1 + static_cast<std::size_t>((i * 131) % 8176));
            }
            double t1 = NowUs();
            for (void* p : keep) fast_free(p);
            v.push_back(t1 - t0);
        }
        Print("512-op mixed alloc burst", Summarize(v), "us/burst");
    }

    // C2: free storm - 512 pre-allocated blocks freed back-to-back.
    {
        std::vector<double> v;
        v.reserve(static_cast<std::size_t>(reps));
        for (int r = 0; r < reps; ++r) {
            std::vector<void*> keep;
            keep.reserve(512);
            for (int i = 0; i < 512; ++i)
                keep.push_back(fast_malloc(1 + static_cast<std::size_t>((i * 131) % 8176)));
            double t0 = NowUs();
            for (void* p : keep) fast_free(p);
            double t1 = NowUs();
            v.push_back(t1 - t0);
        }
        Print("512-op free storm", Summarize(v), "us/burst");
    }

    // C3: large-block burst (fresh spans; page mapping dominates the tail).
    {
        std::vector<double> v;
        v.reserve(static_cast<std::size_t>(reps));
        for (int r = 0; r < (quick ? 10 : 50); ++r) {
            std::vector<void*> keep(32);
            double t0 = NowUs();
            for (int i = 0; i < 32; ++i)
                keep[static_cast<std::size_t>(i)] = fast_malloc(256 * 1024);
            double t1 = NowUs();
            for (void* p : keep) {
                if (p) {
                    std::memset(p, 2, 4096);
                    fast_free(p);
                }
            }
            v.push_back(t1 - t0);
        }
        Print("32 x 256KiB span burst", Summarize(v), "us/burst");
    }
}

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--quick") quick = true;
    }
    std::printf("=== FastAlloc lifecycle benchmark (%s) ===\n",
                quick ? "quick" : "full");
    ColdStartSuite(quick);
    ThreadLifecycleSuite(quick);
    BurstSuite(quick);
    std::printf("=== end lifecycle benchmark ===\n");
    return 0;
}
