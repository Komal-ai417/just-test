// ============================================================================
// realloc() path matrix: small in-place vs move semantics, small <-> large
// transitions, large in-place growth within page slack, large shrink on
// direct-mmap spans (mremap tail release), pooled-span copy path, aligned
// block grow/shrink with alignment preservation, and realloc(p, 0) across
// every block type.
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
// small <-> large transitions with full verification
// ---------------------------------------------------------------------------
TEST(ReallocMatrix, SmallLargeTransitionsPreserveData) {
    const std::size_t sizes[] = {100, 1u << 20, 100, 8176, 8177, 4096, 300, 16};
    void* p = fast_malloc(100);
    ASSERT_NE(p, nullptr);
    Fill(p, 100, 0xA1);
    std::size_t cur = 100;
    for (std::size_t next : sizes) {
        p = fast_realloc(p, next);
        ASSERT_NE(p, nullptr) << "transition to " << next;
        std::size_t check = std::min(cur, next);
        ASSERT_TRUE(Verify(p, check, 0xA1))
            << "prefix lost at transition " << cur << " -> " << next;
        Fill(p, next, 0xA1);  // rewrite the full new size for the next step
        cur = next;
    }
    fast_free(p);
}

// ---------------------------------------------------------------------------
// large-path specifics
// ---------------------------------------------------------------------------
TEST(ReallocMatrix, LargeGrowWithinPageSlackStaysInPlace) {
    // malloc(96 KiB) consumes PageRoundUp(96 KiB + 16) = 25 pages; requesting
    // 12 more bytes still fits inside the same page count.
    const std::size_t base = 96 * 1024;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(base));
    ASSERT_NE(p, nullptr);
    Fill(p, base, 0xB2);
    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, base + 12));
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "growth inside page slack must stay in place";
    EXPECT_TRUE(Verify(q, base, 0xB2));
    fast_free(q);
}

TEST(ReallocMatrix, LargeShrinkOnDirectMmapReleasesTailInPlace) {
    // Spans above 2 MiB never ride the chunk pool, so the shrink takes the
    // in-place mremap path: same base pointer, tail pages released to the OS.
    FastAllocStats before = fast_alloc_stats();
    const std::size_t big = 3 * 1024 * 1024;   // 768 pages, direct mmap
    const std::size_t small = 1 * 1024 * 1024;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(big));
    ASSERT_NE(p, nullptr);
    Fill(p, small, 0xC3);
    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, small));
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q, p) << "direct-mmap shrink must keep the front pages";
    EXPECT_TRUE(Verify(q, small, 0xC3));
    fast_free(q);
    FastAllocStats after = fast_alloc_stats();
    EXPECT_GE(after.os_free_calls, before.os_free_calls)
        << "in-place shrink must account the released tail pages";
}

TEST(ReallocMatrix, LargeGrowOnPooledSpanUsesCopyPath) {
    // A span <= 2 MiB is chunk-pool backed; growth must NOT mremap it (that
    // could punch holes in bump space the pool does not track) and instead
    // take the allocate-copy-free path. Data must survive.
    const std::size_t a = 1 * 1024 * 1024;
    const std::size_t b = 1536 * 1024;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(a));
    ASSERT_NE(p, nullptr);
    Fill(p, a, 0xD4);
    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, b));
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(Verify(q, a, 0xD4)) << "pooled-span growth lost data";
    Fill(q, b, 0xD4);
    EXPECT_TRUE(Verify(q, b, 0xD4));
    fast_free(q);
}

TEST(ReallocMatrix, LargeGrowDirectMmapPreservesData) {
    // 3 MiB -> 4 MiB on a direct-mmap span: mremap may move or grow in
    // place; either way the full old image must survive.
    const std::size_t a = 3 * 1024 * 1024;
    const std::size_t b = 4 * 1024 * 1024;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(a));
    ASSERT_NE(p, nullptr);
    Fill(p, a, 0xE5);
    unsigned char* q = static_cast<unsigned char*>(fast_realloc(p, b));
    ASSERT_NE(q, nullptr);
    EXPECT_TRUE(Verify(q, a, 0xE5));
    q[b - 1] = 0x5E;  // full new image mapped
    EXPECT_EQ(q[b - 1], 0x5E);
    fast_free(q);
}

TEST(ReallocMatrix, LargeGrowShrinkGrowCycle) {
    const std::size_t sizes[] = {2 * 1024 * 1024 + 4096, 512 * 1024,
                                 3 * 1024 * 1024, 700 * 1024, 5 * 1024 * 1024};
    void* p = fast_malloc(1024 * 1024);
    ASSERT_NE(p, nullptr);
    Fill(p, 1024 * 1024, 0xF6);
    std::size_t cur = 1024 * 1024;
    for (std::size_t next : sizes) {
        p = fast_realloc(p, next);
        ASSERT_NE(p, nullptr) << "cycle to " << next;
        ASSERT_TRUE(Verify(p, std::min(cur, next), 0xF6))
            << "prefix lost at cycle step " << next;
        Fill(p, next, 0xF6);
        cur = next;
    }
    fast_free(p);
}

// ---------------------------------------------------------------------------
// aligned blocks
// ---------------------------------------------------------------------------
TEST(ReallocMatrix, AlignedGrowShrinkMatrix) {
    const std::size_t alignments[] = {32, 64, 256, 4096};
    for (std::size_t a : alignments) {
        SCOPED_TRACE(testing::Message() << "alignment=" << a);
        void* p = fast_aligned_alloc(a, 400);
        ASSERT_NE(p, nullptr);
        Fill(p, 400, 0x19 + a);

        // shrink (logical size only; span kept)
        p = fast_realloc(p, 200);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u);
        EXPECT_TRUE(Verify(p, 200, 0x19 + a));

        // grow beyond the old size: fresh aligned block, copy, free old
        p = fast_realloc(p, 5000);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u)
            << "grow lost the over-alignment";
        EXPECT_TRUE(Verify(p, 200, 0x19 + a));
        Fill(p, 5000, 0x19 + a);

        // grow again across the small/large boundary
        p = fast_realloc(p, 1u << 20);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % a, 0u)
            << "aligned grow to large lost the alignment";
        EXPECT_TRUE(Verify(p, 5000, 0x19 + a));
        fast_free(p);
    }
}

TEST(ReallocMatrix, AlignedReallocZeroFreesBlock) {
    void* p = fast_aligned_alloc(512, 1000);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(fast_realloc(p, 0), nullptr);
    // The span is recycled: a plain malloc of the raw size may reuse it.
    void* q = fast_malloc(1000 + 512 + 32);  // raw size the wrapper used
    ASSERT_NE(q, nullptr);
    fast_free(q);
}

// ---------------------------------------------------------------------------
// realloc(p, 0) across block types
// ---------------------------------------------------------------------------
TEST(ReallocMatrix, ReallocZeroFreesEachBlockType) {
    // small
    void* s = fast_malloc(300);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(fast_realloc(s, 0), nullptr);
    void* s2 = fast_malloc(300);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2, s) << "small block not recycled after realloc(p, 0)";
    fast_free(s2);

    // large
    void* l = fast_malloc(1u << 20);
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(fast_realloc(l, 0), nullptr);
    void* l2 = fast_malloc(1u << 20);
    ASSERT_NE(l2, nullptr);
    fast_free(l2);

    // zero-size block
    void* z = fast_malloc(0);
    ASSERT_NE(z, nullptr);
    EXPECT_EQ(fast_realloc(z, 0), nullptr);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// data integrity across repeated realloc on the same block
// ---------------------------------------------------------------------------
TEST(ReallocMatrix, RepeatedReallocKeepsEveryPrefixByte) {
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    std::size_t cur = 64;
    Fill(p, cur, 0x1234);
    for (int step = 0; step < 40; ++step) {
        std::size_t next = 16 + static_cast<std::size_t>(step * 211) % 8000;
        p = fast_realloc(p, next);
        ASSERT_NE(p, nullptr) << "step " << step;
        std::size_t check = std::min(cur, next);
        ASSERT_TRUE(Verify(p, check, 0x1234))
            << "prefix broken at step " << step << " (" << cur << " -> " << next << ")";
        // Extend the pattern to the new size so the next step verifies more.
        Fill(p, next, 0x1234);
        cur = next;
    }
    fast_free(p);
}
