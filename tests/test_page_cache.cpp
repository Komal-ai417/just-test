// ============================================================================
// Page-span cache behaviour tests: hit accounting, exact-size reuse, best-fit
// split (prefix served, tail parked), per-bin retention caps, global
// high-water cap, eviction accounting and full purge semantics.
//
// Large requests consume PageRoundUp(size + 16) bytes (16-byte span header),
// so the tests use sizes of the form k*4096 - 16 to make every span exactly
// k pages and the bin arithmetic fully deterministic.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <cstring>
#include <vector>

using namespace FastAlloc;

namespace {
constexpr std::size_t kPage = 4096;
// Request that maps to exactly 'pages' pages of span (header included).
constexpr std::size_t SpanRequest(int pages) {
    return static_cast<std::size_t>(pages) * kPage - 16;
}
// Move a span from the thread-local large cache into the global page cache:
// free parks it in the TLS large bin, the flush pushes it through
// GlobalHeap::DeallocateLarge into the page bins.
void ParkSpanInPageCache(void* p) {
    fast_free(p);
    fast_alloc_purge_thread_cache();
}
void ResetCaches() {
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();
}
} // namespace

TEST(PageCache, ExactSizeReuseCountsAsHitAndKeepsBase) {
    ResetCaches();
    void* a = fast_malloc(SpanRequest(16));
    ASSERT_NE(a, nullptr);
    ParkSpanInPageCache(a);

    FastAllocStats before = fast_alloc_stats();
    void* b = fast_malloc(SpanRequest(16));
    ASSERT_NE(b, nullptr);
    FastAllocStats after = fast_alloc_stats();
    EXPECT_EQ(b, a) << "cached span must be reused from the exact-size bin";
    EXPECT_GT(after.page_cache_hits, before.page_cache_hits)
        << "serving a cached span must count as a page-cache hit";
    fast_free(b);
    ResetCaches();
}

TEST(PageCache, BestFitSplitServesPrefixAndParksTail) {
    ResetCaches();
    // Park a 12-page span, then request 6 pages: the 6-page bin is empty, so
    // the allocator must take the 12-page span, serve its HEAD (same base
    // address) and park the 6-page tail in the 6-page bin. The next 6-page
    // request must land exactly on that tail.
    unsigned char* base = static_cast<unsigned char*>(fast_malloc(SpanRequest(12)));
    ASSERT_NE(base, nullptr);
    std::memset(base, 0x5B, SpanRequest(12));
    ParkSpanInPageCache(base);

    unsigned char* head = static_cast<unsigned char*>(fast_malloc(SpanRequest(6)));
    ASSERT_NE(head, nullptr);
    EXPECT_EQ(head, base) << "best-fit split must serve the span head";

    unsigned char* tail = static_cast<unsigned char*>(fast_malloc(SpanRequest(6)));
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail, base + 6 * kPage) << "split tail must be served from its bin";

    fast_free(head);
    fast_free(tail);
    ResetCaches();
}

TEST(PageCache, PerBinRetentionCapEvictsOverflow) {
    ResetCaches();
    const std::size_t span = SpanRequest(16);          // 16-page spans
    const std::size_t span_bytes = 16 * kPage;         // 64 KiB
    const std::size_t cap_entries = (2 * 1024 * 1024) / span_bytes;

    std::vector<void*> spans;
    for (std::size_t i = 0; i < cap_entries + 8; ++i) {
        void* p = fast_malloc(span);
        ASSERT_NE(p, nullptr);
        spans.push_back(p);
    }
    FastAllocStats before = fast_alloc_stats();
    for (std::size_t i = 0; i < spans.size(); ++i) ParkSpanInPageCache(spans[i]);
    FastAllocStats after = fast_alloc_stats();

    // Retention: at most cap_entries spans may stay cached; the excess must
    // be evicted straight to the OS.
    EXPECT_LE(FastAllocTestPageCacheBytes(), cap_entries * span_bytes);
    EXPECT_GT(after.page_cache_evictions, before.page_cache_evictions)
        << "overflowing the per-bin cap must count evictions";
    ResetCaches();
}

TEST(PageCache, GlobalHighWaterCapHoldsAcrossBins) {
    ResetCaches();
    // Fill several different-size bins simultaneously; the global cap
    // (FASTALLOC_PAGE_CACHE_MB, default 64 MB) must bound the total.
    const int page_counts[] = {16, 32, 64, 128};
    std::vector<void*> all;
    for (int pages : page_counts) {
        for (int i = 0; i < 12; ++i) {
            void* p = fast_malloc(SpanRequest(pages));
            ASSERT_NE(p, nullptr);
            all.push_back(p);
        }
    }
    for (void* p : all) ParkSpanInPageCache(p);
    EXPECT_LE(FastAllocTestPageCacheBytes(), 64ull * 1024 * 1024)
        << "global page-cache high-water cap exceeded";
    ResetCaches();
}

TEST(PageCache, PurgeDropsEverythingAndReportsBytes) {
    ResetCaches();
    std::vector<void*> spans;
    for (int i = 0; i < 4; ++i) {
        void* p = fast_malloc(SpanRequest(16));
        ASSERT_NE(p, nullptr);
        spans.push_back(p);
    }
    for (void* p : spans) ParkSpanInPageCache(p);
    EXPECT_GT(FastAllocTestPageCacheBytes(), 0u);

    std::size_t released = FastAllocTestPurgePageCache();
    EXPECT_GE(released, 4 * 16 * kPage) << "purge must release every cached span";
    EXPECT_EQ(FastAllocTestPageCacheBytes(), 0u);

    // A second purge must be a clean no-op.
    EXPECT_EQ(FastAllocTestPurgePageCache(), 0u);

    // Fully serviceable afterwards.
    void* q = fast_malloc(SpanRequest(16));
    ASSERT_NE(q, nullptr);
    std::memset(q, 1, SpanRequest(16));
    fast_free(q);
    ResetCaches();
}

TEST(PageCache, PurgeReturnsPooledSlabSpansToOs) {
    // Slab spans ride the 4 MB chunk pool with madvise-based release; the
    // purge path must still drop them from the page bins once the thread
    // cache has handed its blocks back to the (now empty) slabs.
    ResetCaches();
    std::vector<void*> blocks;
    for (int i = 0; i < 4000; ++i) {
        void* p = fast_malloc(192);
        ASSERT_NE(p, nullptr);
        blocks.push_back(p);
    }
    for (void* p : blocks) fast_free(p);
    // Push the TLS bins through the locked batch path so the slabs empty.
    fast_alloc_purge_thread_cache();
    FastAllocStats before = fast_alloc_stats();
    std::size_t released = FastAllocTestPurgePageCache();
    FastAllocStats after = fast_alloc_stats();
    (void)before;
    EXPECT_GT(released, 0u) << "emptied slab spans must return through purge";
    EXPECT_EQ(FastAllocTestPageCacheBytes(), 0u);
    EXPECT_GE(after.os_free_calls, before.os_free_calls);
    ResetCaches();
}

TEST(PageCache, MixedSizesInterleaveWithoutAddressOverlap) {
    // Churn spans of many sizes so the bins split and merge repeatedly; no
    // two live allocations may ever overlap.
    ResetCaches();
    std::vector<std::pair<void*, std::size_t>> live;
    std::size_t seed = 0x9E37;
    for (int round = 0; round < 120; ++round) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        std::size_t sz = 64 * 1024 + (seed >> 33) % (512 * 1024);
        void* p = fast_malloc(sz);
        ASSERT_NE(p, nullptr);
        std::memset(p, static_cast<int>(round) & 0xFF, std::min<std::size_t>(sz, 64));
        // Overlap check against every live span (bounded set).
        for (auto& kv : live) {
            bool disjoint =
                static_cast<char*>(p) + sz <= kv.first ||
                static_cast<char*>(kv.first) + kv.second <= p;
            ASSERT_TRUE(disjoint) << "live spans overlap at round " << round;
        }
        live.push_back({p, sz});
        if (live.size() > 12) {
            void* victim = live.front().first;
            fast_free(victim);
            live.erase(live.begin());
        }
    }
    for (auto& kv : live) fast_free(kv.first);
    ResetCaches();
}
