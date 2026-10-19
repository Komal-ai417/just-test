// ============================================================================
// API contract tests: the documented malloc/calloc/realloc/free contract
// corners that the functional suites do not already pin down — realloc
// in-place vs move semantics across class boundaries, sized-free hint
// fallbacks, zero-size interactions with realloc, idempotent purge calls and
// the statistics contract of the realloc counter.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <cstring>
#include <cstdint>
#include <vector>

using namespace FastAlloc;

namespace {
unsigned char PB(std::size_t seed, std::size_t i) {
    return static_cast<unsigned char>((seed * 2654435761u + i * 40503u) >> 8);
}
void Fill(void* p, std::size_t n, std::size_t seed) {
    auto* c = static_cast<unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i) c[i] = PB(seed, i);
}
bool Verify(const void* p, std::size_t n, std::size_t seed) {
    auto* c = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i)
        if (c[i] != PB(seed, i)) return false;
    return true;
}
} // namespace

// ---------------------------------------------------------------------------
// realloc in-place semantics on the small path
// ---------------------------------------------------------------------------
TEST(ApiContract, ReallocSameSizeKeepsPointerAndData) {
    void* p = fast_malloc(200);
    ASSERT_NE(p, nullptr);
    Fill(p, 200, 1);
    void* q = fast_realloc(p, 200);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "same-size realloc must not move the block";
    EXPECT_TRUE(Verify(q, 200, 1));
    fast_free(q);
}

TEST(ApiContract, ReallocGrowWithinClassKeepsPointer) {
    // Find a request pair (a, b>a) that maps to the same size class in the
    // active build mode (debug sizing reserves red-zone bytes, so the pair
    // is derived from RequestToClass itself): the block already has room,
    // so the growth must be served in place.
    std::size_t a = 0, b = 0;
    for (std::size_t base = 100; base < 400 && b == 0; base += 64) {
        for (std::size_t cand = base + 1; cand < base + 64; ++cand) {
            if (RequestToClass(base) == RequestToClass(cand)) {
                a = base;
                b = cand;
                break;
            }
        }
    }
    ASSERT_NE(b, 0u) << "no same-class pair found";
    ASSERT_EQ(RequestToClass(a), RequestToClass(b));
    void* p = fast_malloc(a);
    ASSERT_NE(p, nullptr);
    Fill(p, a, 2);
    void* q = fast_realloc(p, b);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "within-class growth should stay in place";
    EXPECT_TRUE(Verify(q, a, 2));
    fast_free(q);
}

TEST(ApiContract, ReallocShrinkOneClassKeepsPointer) {
    // Find a pair (big, small) whose classes are exactly adjacent in the
    // active build mode; the one-class hysteresis keeps the shrink in place.
    std::size_t big = 0, small = 0;
    for (std::size_t base = 190; base < 600 && big == 0; base += 64) {
        for (std::size_t cand = base - 16; cand > base - 64 && cand > 0; --cand) {
            if (RequestToClass(base) == RequestToClass(cand) + 1) {
                big = base;
                small = cand;
                break;
            }
        }
    }
    ASSERT_NE(big, 0u) << "no adjacent-class pair found";
    ASSERT_EQ(RequestToClass(big), RequestToClass(small) + 1);
    void* p = fast_malloc(big);
    ASSERT_NE(p, nullptr);
    Fill(p, small, 3);
    void* q = fast_realloc(p, small);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "one-class shrink must stay in place";
    EXPECT_TRUE(Verify(q, small, 3));
    fast_free(q);
}

TEST(ApiContract, ReallocShrinkTwoClassesPreservesData) {
    void* p = fast_malloc(200);
    ASSERT_NE(p, nullptr);
    Fill(p, 200, 4);
    void* q = fast_realloc(p, 144);  // two classes below: copy path may move
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(Verify(q, 144, 4)) << "shrunk prefix must survive";
    // The new size must be fully usable.
    Fill(q, 144, 5);
    EXPECT_TRUE(Verify(q, 144, 5));
    fast_free(q);
}

TEST(ApiContract, ReallocGrowAcrossClassPreservesData) {
    void* p = fast_malloc(100);
    ASSERT_NE(p, nullptr);
    Fill(p, 100, 6);
    void* q = fast_realloc(p, 117);  // next class up: copy path may move
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(Verify(q, 100, 6));
    Fill(q, 117, 7);
    EXPECT_TRUE(Verify(q, 117, 7));
    fast_free(q);
}

TEST(ApiContract, ReallocGeometricGrowthFullVerification) {
    // 17 bytes doubling to 1 MiB: every grow step must preserve the full
    // written prefix (max-size first-touch coverage on the way).
    void* p = fast_malloc(17);
    ASSERT_NE(p, nullptr);
    std::size_t cur = 17;
    Fill(p, cur, 8);
    while (cur < (1u << 20)) {
        std::size_t next = cur * 2;
        void* q = fast_realloc(p, next);
        ASSERT_NE(q, nullptr) << "grow " << cur << " -> " << next;
        ASSERT_TRUE(Verify(q, cur, 8)) << "prefix broken at grow to " << next;
        Fill(q, next, 8);  // rewrite the whole new size
        p = q;
        cur = next;
    }
    EXPECT_TRUE(Verify(p, cur, 8));
    fast_free(p);
}

// ---------------------------------------------------------------------------
// zero-size interactions
// ---------------------------------------------------------------------------
TEST(ApiContract, ReallocZeroSizeBlockGrows) {
    void* p = fast_malloc(0);  // unique 1-byte block
    ASSERT_NE(p, nullptr);
    *static_cast<unsigned char*>(p) = 0x7E;
    void* q = fast_realloc(p, 256);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(static_cast<unsigned char*>(q)[0], 0x7E) << "1-byte image lost";
    fast_free(q);
}

TEST(ApiContract, ReallocZeroOnZeroSizeBlockFrees) {
    void* p = fast_malloc(0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(fast_realloc(p, 0), nullptr);  // C11: free + null
    void* q = fast_malloc(0);
    ASSERT_NE(q, nullptr);
    fast_free(q);
}

TEST(ApiContract, CallocZeroZeroIsNonNull) {
    void* a = fast_calloc(0, 0);
    void* b = fast_calloc(0, 0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    fast_free(a);
    fast_free(b);
}

// ---------------------------------------------------------------------------
// sized-free hint contract
// ---------------------------------------------------------------------------
TEST(ApiContract, FreeSizedZeroHintFallsBackSafely) {
    // A zero hint maps to class 0, which no real request ever uses; the
    // header cross-check must route the block through the general path.
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    fast_free_sized(p, 0);
    void* q = fast_malloc(64);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "block not recycled after zero-hint sized free";
    fast_free(q);
}

TEST(ApiContract, FreeSizedHintOffByOneClassFallsBack) {
    for (std::size_t req : {96u, 160u, 320u, 640u, 1280u, 2560u, 5120u}) {
        SCOPED_TRACE(testing::Message() << "req=" << req);
        void* p = fast_malloc(req);
        ASSERT_NE(p, nullptr);
        std::size_t hint = req + 16;  // maps to the next class up
        ASSERT_NE(RequestToClass(hint), RequestToClass(req));
        fast_free_sized(p, hint);
        void* q = fast_malloc(req);
        ASSERT_NE(q, nullptr);
        EXPECT_EQ(q, p) << "wrong-hint sized free lost the block";
        fast_free(q);
    }
}

TEST(ApiContract, FreeSizedLargeHintOnSmallBlockIsSafe) {
    // Hint claims a large span while the block is small: the general path
    // must recover the true class from the header.
    for (std::size_t req : {24u, 248u, 1000u, 4096u, 8176u}) {
        void* p = fast_malloc(req);
        ASSERT_NE(p, nullptr);
        fast_free_sized(p, 1u << 20);
        void* q = fast_malloc(req);
        ASSERT_NE(q, nullptr);
        EXPECT_EQ(q, p);
        fast_free(q);
    }
}

TEST(ApiContract, FreeSizedOnLargeBlockRoutesThroughGeneralPath) {
    void* p = fast_malloc(1u << 20);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0x42, 1u << 20);
    fast_free_sized(p, 1u << 20);  // > MaxSmallRequest: general path
    void* q = fast_malloc(1u << 20);
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "large span not recycled through sized free";
    fast_free(q);
}

// ---------------------------------------------------------------------------
// purge + stats contracts
// ---------------------------------------------------------------------------
TEST(ApiContract, PurgeIsIdempotent) {
    fast_alloc_purge();
    fast_alloc_purge();
    fast_alloc_purge_thread_cache();
    fast_alloc_purge_thread_cache();
    fast_alloc_purge();
    void* p = fast_malloc(128);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    SUCCEED();
}

TEST(ApiContract, ReallocCounterTracksCalls) {
    FastAllocStats before = fast_alloc_stats();
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    p = fast_realloc(p, 128);
    ASSERT_NE(p, nullptr);
    p = fast_realloc(p, 256);
    ASSERT_NE(p, nullptr);
    p = fast_realloc(p, 512);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    FastAllocStats after = fast_alloc_stats();
    EXPECT_GE(after.reallocs, before.reallocs + 3);
}

TEST(ApiContract, OsCountersAdvanceOnFreshSpans) {
    FastAllocStats before = fast_alloc_stats();
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();
    void* p = fast_malloc(1u << 20);
    ASSERT_NE(p, nullptr);
    std::memset(p, 1, 1u << 20);
    fast_free(p);
    FastAllocStats after = fast_alloc_stats();
    EXPECT_GE(after.os_alloc_calls, before.os_alloc_calls + 1)
        << "a fresh 1 MiB span must produce at least one OS allocation";
}

TEST(ApiContract, LiveStatsBalancedAfterPairedOps) {
    // Batches of alloc/free pairs must return the live counters to their
    // starting values (exact in debug, approximate in release).
    FastAllocStats before = fast_alloc_stats();
    for (int round = 0; round < 20; ++round) {
        std::vector<void*> v;
        for (int i = 0; i < 500; ++i) {
            void* p = fast_malloc(1 + (static_cast<std::size_t>(i * 37) % 8176));
            ASSERT_NE(p, nullptr);
            v.push_back(p);
        }
        for (void* p : v) fast_free(p);
    }
    FastAllocStats after = fast_alloc_stats();
#if FASTALLOC_DEBUG_ENABLED
    EXPECT_EQ(after.current_live_blocks, before.current_live_blocks);
    EXPECT_EQ(after.current_live_bytes, before.current_live_bytes);
#else
    // Relaxed batching can lag by one flush window per thread.
    EXPECT_LE(after.current_live_blocks, before.current_live_blocks + 256);
#endif
}

TEST(ApiContract, MallocZeroThenFreeSizedZeroIsSafe) {
    void* p = fast_malloc(0);
    ASSERT_NE(p, nullptr);
    fast_free_sized(p, 1);  // correct class for the underlying 1-byte request
    SUCCEED();
}
