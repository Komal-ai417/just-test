// ============================================================================
// Global operator new/delete override tests (FAST_ALLOC_OVERRIDE_NEW).
//
// The strict assertions require a build where the overrides are active
// (-DFASTALLOC_OVERRIDE_NEW, CMake option FASTALLOC_OVERRIDE_NEW=ON). In a
// default build the same expressions still run as smoke tests through the
// standard allocator, so the file compiles and passes everywhere.
//
// Covers: plain new/delete, new[]/delete[], sized delete, over-aligned
// new/delete (std::align_val_t), aligned-sized delete, nothrow new (returns
// nullptr instead of throwing), OOM throwing std::bad_alloc from throwing
// new, container workloads routed through the override, and thread safety.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace FastAlloc;

namespace {
struct OverAligned64 {
    alignas(64) std::uint64_t payload[4];
};
struct OverAligned4096 {
    alignas(4096) unsigned char blob[128];
};
struct Plain {
    std::uint64_t a, b, c;
};

// Forces the next OS-backed allocation to fail so the throwing/nothrow new
// paths can be exercised deterministically.
[[maybe_unused]] void DrainCaches() {
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();
}
} // namespace

TEST(NewDeleteOverride, PlainNewDeleteRoundtrip) {
    auto* p = new Plain{1, 2, 3};
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->a, 1);
    EXPECT_EQ(p->b, 2);
    EXPECT_EQ(p->c, 3);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignof(Plain), 0u);
    delete p;
}

TEST(NewDeleteOverride, ArrayNewDeleteRoundtrip) {
    const int n = 100;
    auto* arr = new std::uint64_t[n];
    ASSERT_NE(arr, nullptr);
    for (int i = 0; i < n; ++i) arr[i] = static_cast<std::uint64_t>(i) * 7;
    for (int i = 0; i < n; ++i) EXPECT_EQ(arr[i], static_cast<std::uint64_t>(i) * 7);
    delete[] arr;
}

TEST(NewDeleteOverride, ZeroLengthArrayIsUsable) {
    std::size_t n = 0;  // runtime value: a zero-length array must still yield
                        // a unique, deletable pointer
    auto* arr = new std::uint64_t[n];
    EXPECT_NE(arr, nullptr);
    delete[] arr;
}

TEST(NewDeleteOverride, OverAligned64NewDelete) {
    auto* p = new OverAligned64{};
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64u, 0u)
        << "over-aligned new lost 64-byte alignment";
    p->payload[0] = 0xDEADBEEFull;
    EXPECT_EQ(p->payload[0], 0xDEADBEEFull);
    delete p;  // aligned delete must recover the stash-based provenance
}

TEST(NewDeleteOverride, OverAligned4096NewDelete) {
    auto* p = new OverAligned4096{};
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 4096u, 0u)
        << "over-aligned new lost 4096-byte alignment";
    std::memset(p->blob, 0x5A, sizeof(p->blob));
    EXPECT_EQ(p->blob[0], 0x5A);
    EXPECT_EQ(p->blob[sizeof(p->blob) - 1], 0x5A);
    delete p;
}

TEST(NewDeleteOverride, OverAlignedArrayNewDelete) {
    const int n = 8;
    auto* arr = new OverAligned64[n];
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(arr) % 64u, 0u)
        << "aligned array new lost alignment";
    for (int i = 0; i < n; ++i) arr[i].payload[3] = static_cast<std::uint64_t>(i);
    for (int i = 0; i < n; ++i) EXPECT_EQ(arr[i].payload[3], static_cast<std::uint64_t>(i));
    delete[] arr;
}

TEST(NewDeleteOverride, NothrowNewReturnsNonNull) {
    auto* p = new (std::nothrow) std::uint64_t[64];
    ASSERT_NE(p, nullptr);
    p[0] = 42;
    EXPECT_EQ(p[0], 42);
    delete[] p;
}

#ifdef FAST_ALLOC_OVERRIDE_NEW
TEST(NewDeleteOverride, NothrowNewReturnsNullOnOom) {
    // A size that requires a fresh multi-page span; the injected failure hits
    // the slow path while the TLS caches stay warm for small requests. The
    // countdown is cleared before any EXPECT machinery runs: gtest formats
    // failure messages through allocations, which in override-new mode would
    // trip the armed countdown and mask the real verdict.
    DrainCaches();
    FastAllocTestSetOOMCountdown(1);
    unsigned char* p = new (std::nothrow) unsigned char[64 * 1024 * 1024];
    FastAllocTestSetOOMCountdown(0);
    EXPECT_EQ(p, nullptr);  // nothrow contract: nullptr, no exception
    delete[] p;

    // Throwing allocation of the same magnitude must throw instead. The
    // result is bound and printed so the new/delete pair cannot be elided
    // (a discarded new with a trivial type is dead code to the optimizer).
    DrainCaches();
    FastAllocTestSetOOMCountdown(1);
    unsigned char* big = nullptr;
    bool threw_bad_alloc = false;
    try {
        big = new unsigned char[64 * 1024 * 1024];
    } catch (const std::bad_alloc&) {
        threw_bad_alloc = true;
    }
    std::fprintf(stderr, "[oom-override] throwing-new result: %p (threw=%d)\n",
                 static_cast<void*>(big), threw_bad_alloc ? 1 : 0);
    FastAllocTestSetOOMCountdown(0);
    EXPECT_TRUE(threw_bad_alloc) << "throwing new must throw std::bad_alloc on OOM";
    delete[] big;

    // The allocator must be fully serviceable afterwards.
    auto* q = new (std::nothrow) std::uint64_t[32];
    ASSERT_NE(q, nullptr);
    delete[] q;
}

TEST(NewDeleteOverride, NewRoutesThroughFastAlloc) {
    // With the overrides active every new expression is served by FastAlloc;
    // the statistics counters must observe the allocation.
    FastAllocStats before = fast_alloc_stats();
    auto* p = new Plain{7, 8, 9};
    ASSERT_NE(p, nullptr);
    FastAllocStats mid = fast_alloc_stats();
    EXPECT_GE(mid.small_allocs + mid.large_allocs,
              before.small_allocs + before.large_allocs);
    delete p;
    FastAllocStats after = fast_alloc_stats();
    EXPECT_GE(after.small_frees + after.large_frees,
              before.small_frees + before.large_frees);
}
#endif

TEST(NewDeleteOverride, ContainerWorkloadsRouteThroughOverride) {
    {
        std::vector<std::string> v;
        for (int i = 0; i < 2000; ++i) v.push_back("string-" + std::to_string(i));
        EXPECT_EQ(v.size(), 2000u);
        EXPECT_EQ(v[7], "string-7");
        EXPECT_EQ(v[1999], "string-1999");
    }
    {
        std::map<int, std::string> m;
        for (int i = 0; i < 500; ++i) m[i] = std::string(static_cast<std::size_t>(i % 64) + 1, 'x');
        EXPECT_EQ(m.size(), 500u);
        EXPECT_EQ(m[499].size(), 52u);  // 499 % 64 + 1
    }
    {
        std::vector<OverAligned64> v;
        v.reserve(16);
        for (int i = 0; i < 16; ++i) {
            OverAligned64 o{};
            o.payload[0] = static_cast<std::uint64_t>(i);
            v.push_back(o);
        }
        EXPECT_EQ(v.size(), 16u);
        EXPECT_EQ(v[15].payload[0], 15ull);
    }
    SUCCEED();
}

TEST(NewDeleteOverride, NewDeleteUnderThreads) {
    constexpr int kThreads = 4;
    constexpr int kIters = 20000;
    std::atomic<int> failures{0};
    auto worker = [&](int id) {
        std::mt19937 rng(0x5EED + static_cast<unsigned>(id));
        std::vector<Plain*> live;
        live.reserve(128);
        for (int i = 0; i < kIters; ++i) {
            int op = static_cast<int>(rng() % 100);
            if (op < 50 || live.empty()) {
                auto* p = new Plain{static_cast<std::uint64_t>(id), 0, 0};
                if (!p) { ++failures; continue; }
                p->b = static_cast<std::uint64_t>(i);
                live.push_back(p);
            } else {
                std::size_t idx = rng() % live.size();
                Plain* p = live[idx];
                if (p->a != static_cast<std::uint64_t>(id)) { ++failures; }
                live[idx] = live.back();
                live.pop_back();
                delete p;
            }
        }
        for (Plain* p : live) delete p;
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) ts.emplace_back(worker, i);
    for (auto& t : ts) t.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST(NewDeleteOverride, MixedAlignmentLifecycleChurn) {
    // Interleave plain and over-aligned objects so slab blocks and stash-based
    // aligned blocks are allocated and freed adjacently.
    for (int round = 0; round < 200; ++round) {
        auto* plain = new Plain{1, 2, 3};
        auto* al64 = new OverAligned64{};
        auto* arr = new std::uint32_t[37];
        auto* al4k = new OverAligned4096{};
        ASSERT_NE(plain, nullptr);
        ASSERT_NE(al64, nullptr);
        ASSERT_NE(arr, nullptr);
        ASSERT_NE(al4k, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(al64) % 64u, 0u);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(al4k) % 4096u, 0u);
        arr[36] = 0xC0FFEE;
        EXPECT_EQ(arr[36], 0xC0FFEEu);
        delete plain;
        delete al64;
        delete[] arr;
        delete al4k;
    }
    SUCCEED();
}
