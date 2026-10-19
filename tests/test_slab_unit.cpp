// ============================================================================
// Slab unit tests: direct Slab::Create / Allocate / Deallocate exercises on
// caller-provided raw memory (the slab header is internal but the test suite
// compiles against src/, so the structure can be driven in isolation).
// Covers geometry computation, rejection of invalid inputs, exhaustion,
// recycling, and the duplicate-head guard on Deallocate.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "slab.h"

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace FastAlloc;

namespace {
// Page-aligned raw buffer for slab experiments.
struct RawSpan {
    explicit RawSpan(std::size_t bytes) : size_(bytes) {
        int rc = posix_memalign(&mem_, 4096, bytes);
        if (rc != 0) mem_ = nullptr;
    }
    ~RawSpan() { std::free(mem_); }
    RawSpan(const RawSpan&) = delete;
    RawSpan& operator=(const RawSpan&) = delete;
    void* mem_ = nullptr;
    std::size_t size_;
};
} // namespace

TEST(SlabUnit, CreateComputesExactBlockGeometry) {
    RawSpan span(64 * 1024);
    ASSERT_NE(span.mem_, nullptr);
    const std::size_t block_size = 128;
    Slab* slab = Slab::Create(span.mem_, span.size_, block_size, 0);
    ASSERT_NE(slab, nullptr);
    std::size_t offset = (sizeof(Slab) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    std::size_t expect_blocks = (span.size_ - offset) / block_size;
    EXPECT_EQ(slab->total_blocks, expect_blocks);
    EXPECT_EQ(slab->free_blocks, expect_blocks);
    EXPECT_EQ(slab->block_size, block_size);
    EXPECT_EQ(slab->first_block_offset, offset);
    EXPECT_EQ(slab->arena_index, 0u);
    EXPECT_EQ(slab->class_idx, SizeToClassIndex(block_size));
    EXPECT_TRUE(slab->IsEmpty());
    EXPECT_FALSE(slab->IsFull());
}

TEST(SlabUnit, CreateRejectsNullMemory) {
    EXPECT_EQ(Slab::Create(nullptr, 64 * 1024, 128, 0), nullptr);
}

TEST(SlabUnit, CreateRejectsTooSmallSpans) {
    // Not even room for the header plus one block.
    {
        RawSpan span(sizeof(Slab) + 16);
        ASSERT_NE(span.mem_, nullptr);
        EXPECT_EQ(Slab::Create(span.mem_, span.size_, 128, 0), nullptr);
    }
    // Room for the header plus exactly one block after alignment: the
    // minimum accepted geometry (the strict size check demands at least one
    // byte beyond header + block, so a 16-byte slack is added).
    {
        std::size_t offset = (sizeof(Slab) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        std::size_t bytes = offset + 128 + 16;
        RawSpan span(bytes);
        ASSERT_NE(span.mem_, nullptr);
        Slab* slab = Slab::Create(span.mem_, span.size_, 128, 0);
        ASSERT_NE(slab, nullptr);
        EXPECT_EQ(slab->total_blocks, 1u);
    }
    // Room for the header but not for a full block after alignment.
    {
        RawSpan span(sizeof(Slab) + 64);
        ASSERT_NE(span.mem_, nullptr);
        EXPECT_EQ(Slab::Create(span.mem_, span.size_, 128, 0), nullptr);
    }
}

TEST(SlabUnit, AllocateHandsOutEveryBlockExactlyOnce) {
    RawSpan span(64 * 1024);
    ASSERT_NE(span.mem_, nullptr);
    const std::size_t block_size = 64;
    Slab* slab = Slab::Create(span.mem_, span.size_, block_size, 3);
    ASSERT_NE(slab, nullptr);

    std::vector<void*> blocks;
    std::vector<std::uintptr_t> seen;
    for (std::size_t i = 0; i < slab->total_blocks; ++i) {
        void* p = slab->Allocate();  // returns the BLOCK base (FreeBlock*)
        ASSERT_NE(p, nullptr) << "block " << i;
        // Distinctness.
        for (std::uintptr_t a : seen) ASSERT_NE(reinterpret_cast<std::uintptr_t>(p), a);
        seen.push_back(reinterpret_cast<std::uintptr_t>(p));
        // Geometry: every block sits at a multiple of block_size from the
        // first block position.
        std::uintptr_t base = reinterpret_cast<std::uintptr_t>(slab) + slab->first_block_offset;
        std::uintptr_t off = reinterpret_cast<std::uintptr_t>(p) - base;
        ASSERT_EQ(off % block_size, 0u);
        // Header fields written by the carve/pop path.
        auto* fb = static_cast<FreeBlock*>(p);
        EXPECT_EQ(fb->slab, slab);
        EXPECT_EQ(fb->class_index, slab->class_idx);
        blocks.push_back(p);
    }
    EXPECT_TRUE(slab->IsFull());
#if !FASTALLOC_DEBUG_ENABLED
    EXPECT_EQ(slab->Allocate(), nullptr) << "exhausted slab must return null";
#else
    // Debug builds treat allocation from an exhausted slab as an internal
    // invariant violation (assert_fast) and abort; the allocator never calls
    // Allocate() on a full slab, so only the release contract promises null.
    EXPECT_EQ(slab->free_blocks, 0u);
#endif
    for (void* p : blocks) slab->Deallocate(p);
    EXPECT_TRUE(slab->IsEmpty());
}

TEST(SlabUnit, DeallocateRecyclesIntoFreeList) {
    RawSpan span(64 * 1024);
    ASSERT_NE(span.mem_, nullptr);
    Slab* slab = Slab::Create(span.mem_, span.size_, 96, 1);
    ASSERT_NE(slab, nullptr);

    void* a = slab->Allocate();
    void* b = slab->Allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    slab->Deallocate(a);
    // LIFO: the next allocation must reuse 'a'.
    void* c = slab->Allocate();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c, a);
    slab->Deallocate(b);
    slab->Deallocate(c);
    EXPECT_TRUE(slab->IsEmpty());
}

TEST(SlabUnit, DeallocateDuplicateHeadIsGuarded) {
    RawSpan span(64 * 1024);
    ASSERT_NE(span.mem_, nullptr);
    Slab* slab = Slab::Create(span.mem_, span.size_, 96, 0);
    ASSERT_NE(slab, nullptr);

    void* a = slab->Allocate();
    ASSERT_NE(a, nullptr);
    slab->Deallocate(a);
    std::size_t free_before = slab->free_blocks;
    ASSERT_GT(free_before, 0u);
#if !FASTALLOC_DEBUG_ENABLED
    // Returning the current free-list head again is a no-op (guarded).
    slab->Deallocate(a);
    EXPECT_EQ(slab->free_blocks, free_before)
        << "duplicate head return must not change the free count";
#else
    // Debug builds fatal on the double return by design (assert_fast in
    // Slab::Deallocate); the release guard is what the above exercises.
    EXPECT_EQ(slab->free_blocks, free_before);
#endif
    // The slab remains fully usable.
    void* b = slab->Allocate();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b, a);
    slab->Deallocate(b);
}

TEST(SlabUnit, InterleavedAllocateDeallocateStaysConsistent) {
    RawSpan span(128 * 1024);
    ASSERT_NE(span.mem_, nullptr);
    const std::size_t block_size = 160;
    Slab* slab = Slab::Create(span.mem_, span.size_, block_size, 7);
    ASSERT_NE(slab, nullptr);

    std::vector<std::pair<void*, unsigned char>> live;  // {block, first-byte tag}
    unsigned char tag = 0;
    for (int round = 0; round < 500; ++round) {
        if (round % 3 != 2) {
            void* p = slab->Allocate();
            if (!p) break;  // slab exhausted
            ++tag;
            // Write only the USER image (past the 16-byte block header).
            std::memset(static_cast<char*>(p) + USER_OFFSET, tag,
                        block_size - USER_OFFSET);
            live.push_back({p, tag});
        } else if (!live.empty()) {
            auto& b = live.back();
            auto* c = static_cast<unsigned char*>(b.first) + USER_OFFSET;
            // Verify the full writable image survived its live time.
            for (std::size_t k = 0; k < block_size - USER_OFFSET; ++k) {
                ASSERT_EQ(c[k], b.second) << "round " << round << " byte " << k;
            }
            slab->Deallocate(b.first);
            live.pop_back();
        }
        // Invariant: free bookkeeping always matches the live set.
        EXPECT_EQ(slab->free_blocks + live.size(), slab->total_blocks);
    }
    for (auto& b : live) slab->Deallocate(b.first);
    EXPECT_TRUE(slab->IsEmpty());
}

TEST(SlabUnit, BlocksAreWritableAcrossTheirFullImage) {
    RawSpan span(64 * 1024);
    ASSERT_NE(span.mem_, nullptr);
    const std::size_t block_size = 256;
    Slab* slab = Slab::Create(span.mem_, span.size_, block_size, 0);
    ASSERT_NE(slab, nullptr);
    const std::size_t usable = block_size - USER_OFFSET;  // user image bytes
    std::vector<void*> blocks;
    for (std::size_t i = 0; i < slab->total_blocks; ++i) {
        void* p = slab->Allocate();
        ASSERT_NE(p, nullptr);
        blocks.push_back(p);
        // The user image starts 16 bytes into the block (past the header).
        auto* c = static_cast<unsigned char*>(p) + USER_OFFSET;
        for (std::size_t k = 0; k < usable; ++k)
            c[k] = static_cast<unsigned char>((i * 31 + k) & 0xFF);
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        auto* c = static_cast<unsigned char*>(blocks[i]) + USER_OFFSET;
        for (std::size_t k = 0; k < usable; ++k) {
            ASSERT_EQ(c[k], static_cast<unsigned char>((i * 31 + k) & 0xFF))
                << "block " << i << " byte " << k;
        }
    }
    for (void* p : blocks) slab->Deallocate(p);
}

// The slabs the allocator itself creates share the same geometry contract;
// verify that a real user block of every size class really is served from a
// correctly-aligned slab position (integration cross-check).
TEST(SlabUnit, AllocatorBlocksHonourSlabGeometryContract) {
    for (std::size_t c = 1; c < NUM_SIZE_CLASSES; c += 41) {
        std::size_t block_size = ClassIndexToSize(c);
        std::size_t request = block_size - USER_OFFSET;
        void* p = fast_malloc(request);
        ASSERT_NE(p, nullptr) << "class " << c;
        auto* fb = reinterpret_cast<FreeBlock*>(static_cast<char*>(p) - USER_OFFSET);
        // The header must be self-consistent for the free path to work; the
        // serving class is derived from the request in the active build mode
        // (debug sizing adds a red-zone reserve).
        EXPECT_TRUE(IsPlausibleHeapPtr(fb->slab));
        EXPECT_EQ(fb->class_index, static_cast<uint32_t>(RequestToClass(request)));
        // Full image writable.
        std::memset(p, 0x6B, request);
        fast_free(p);
    }
}
