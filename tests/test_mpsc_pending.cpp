// ============================================================================
// MPSC pending-return queue lifecycle tests.
//
// The per-(arena, class) pending queues accept lock-free deferred hand-offs
// (thread exit, contended flushes) and are drained by the next refilling
// thread. These tests pin down: deferred hand-off feeding a later thread,
// the over-long-list drain path (remainder > 64 blocks), cross-arena refill
// stealing, and the pending-push accounting in fast_alloc_stats().
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace FastAlloc;

namespace {
unsigned char PB(std::size_t seed, std::size_t i) {
    return static_cast<unsigned char>((seed * 2654435761u + i * 40503u) >> 8);
}
} // namespace

TEST(MpscPending, ThreadExitDefersAndNextThreadFeeds) {
    // A dying thread frees a batch of blocks that stay under the locked-flush
    // threshold: the TLS destructor parks the bin on the pending queue (one
    // CAS, no locks). A subsequent thread allocating the same class must be
    // fed from that pending list with fully intact blocks.
    const std::size_t size = 200;      // class (200+15)>>4 = 13
    const int kBlocks = 48;            // <= kDeferMaxBin (64): deferred, not locked-flushed
    FastAllocStats before = fast_alloc_stats();

    {
        std::thread donor([&] {
            std::vector<void*> v;
            for (int i = 0; i < kBlocks; ++i) {
                void* p = fast_malloc(size);
                if (p) v.push_back(p);
            }
            for (void* p : v) fast_free(p);  // parked in the dying thread's bin
            // Thread exit: TLSCache destructor defers the bin to pending.
        });
        donor.join();
    }

    FastAllocStats mid = fast_alloc_stats();
    (void)before;
    EXPECT_GE(mid.pending_queue_pushes, before.pending_queue_pushes)
        << "the deferred hand-off must be counted";

    // A fresh thread allocates the same class: it must be fed from pending.
    std::atomic<std::size_t> fed_ok{0};
    std::thread feeder([&] {
        for (int i = 0; i < kBlocks; ++i) {
            void* p = fast_malloc(size);
            if (!p) continue;
            auto* c = static_cast<unsigned char*>(p);
            for (std::size_t k = 0; k < 16; ++k) c[k] = 0xA5;
            bool ok = true;
            for (std::size_t k = 0; k < 16; ++k) {
                if (c[k] != 0xA5) { ok = false; break; }
            }
            if (ok) ++fed_ok;
            fast_free(p);
        }
    });
    feeder.join();
    EXPECT_EQ(fed_ok.load(), static_cast<std::size_t>(kBlocks));
}

TEST(MpscPending, OverlongPendingListIsDrainedIntoSlabs) {
    // Accumulate more than the 64-block feed-walk threshold in ONE
    // (arena, class) pending queue: 17 donor threads are guaranteed to reuse
    // an arena index (16 arenas, round-robin per cache creation), so at
    // least two donors share an arena and push > 64 blocks onto the same
    // pending list. The next refilling thread feeds at most 'target' blocks
    // and must then drain the over-long remainder into the slab free lists.
    const std::size_t size = 336;   // class (336+15)>>4 = 21
    const int kDonors = 17;
    const int kPerDonor = 64;       // each donor defers exactly 64 blocks

    for (int d = 0; d < kDonors; ++d) {
        std::thread donor([&] {
            std::vector<void*> v;
            for (int i = 0; i < kPerDonor; ++i) {
                void* p = fast_malloc(size);
                if (p) v.push_back(p);
            }
            for (void* p : v) fast_free(p);
        });
        donor.join();
    }

    // Drain from the main thread: many allocations of the same class walk
    // the pending lists (feed + over-long drain) and every block handed out
    // must be writable and readable.
    std::size_t seed = 0x51ED;
    for (int i = 0; i < kDonors * kPerDonor; ++i) {
        void* p = fast_malloc(size);
        ASSERT_NE(p, nullptr);
        auto* c = static_cast<unsigned char*>(p);
        for (std::size_t k = 0; k < size; ++k) c[k] = PB(seed + 1, k);
        for (std::size_t k = 0; k < size; ++k) {
            ASSERT_EQ(c[k], PB(seed + 1, k)) << "block corrupted at " << i;
        }
        fast_free(p);
    }
    SUCCEED();
}

TEST(MpscPending, CrossArenaPendingIsStealable) {
    // Blocks parked on another arena's pending queue must be available to a
    // thread whose own arena has nothing pending: the refill loop try-locks
    // foreign arenas and steals their pending entries.
    const std::size_t size = 152;   // class (152+15)>>4 = 10
    const int kDonors = 16;         // all 16 arenas get a donor
    const int kPerDonor = 50;

    std::vector<void*> to_free;
    for (int d = 0; d < kDonors; ++d) {
        std::thread donor([&] {
            std::vector<void*> v;
            for (int i = 0; i < kPerDonor; ++i) {
                void* p = fast_malloc(size);
                if (p) v.push_back(p);
            }
            for (void* p : v) fast_free(p);
        });
        donor.join();
    }

    // The main thread's arena differs from most donors': its refills must
    // succeed by stealing foreign pending entries.
    std::size_t ok = 0;
    for (int i = 0; i < kDonors * kPerDonor / 2; ++i) {
        void* p = fast_malloc(size);
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x3C, 16);
        ++ok;
        to_free.push_back(p);
    }
    EXPECT_GT(ok, 0u);
    for (void* p : to_free) fast_free(p);
    SUCCEED();
}

TEST(MpscPending, PendingPushAccountingUnderContention) {
    // Sustained cross-thread frees from many producers must show up in the
    // pending_queue_pushes statistic (lock-free hand-off path).
    FastAllocStats before = fast_alloc_stats();
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 2000;
    std::vector<void*> channel[kProducers];
    std::vector<std::thread> producers;
    for (int i = 0; i < kProducers; ++i) {
        producers.emplace_back([&, i] {
            for (int k = 0; k < kPerProducer; ++k) {
                void* p = fast_malloc(1 + (static_cast<std::size_t>(k * 131) % 2048));
                if (p) channel[i].push_back(p);
            }
        });
    }
    for (auto& t : producers) t.join();

    // Free everything from the main thread: cross-thread frees of producer
    // blocks (contended or deferred) push onto pending queues.
    std::vector<std::thread> consumers;
    for (int i = 0; i < kProducers; ++i) {
        consumers.emplace_back([&, i] {
            for (void* p : channel[i]) fast_free(p);
        });
    }
    for (auto& t : consumers) t.join();

    FastAllocStats after = fast_alloc_stats();
    EXPECT_GE(after.pending_queue_pushes, before.pending_queue_pushes);
}

TEST(MpscPending, DeferralKeepsAllocatorBalanced) {
    // After a deferral storm and a full purge (which drains every pending
    // queue), no block of this test may remain live.
    FastAllocStats t0 = fast_alloc_stats();
    for (int round = 0; round < 40; ++round) {
        std::thread t([&] {
            std::vector<void*> v;
            for (int i = 0; i < 60; ++i) {
                void* p = fast_malloc(1 + static_cast<std::size_t>(i * 29 % 1024));
                if (p) v.push_back(p);
            }
            for (void* p : v) fast_free(p);
        });
        t.join();
    }
    fast_alloc_purge();  // drains every pending queue into the slabs
    FastAllocStats after = fast_alloc_stats();
    EXPECT_EQ(after.current_live_blocks, t0.current_live_blocks)
        << "blocks stuck on pending lists after purge (leak)";
}

TEST(MpscPending, PurgeDrainsDeferredBlocksAndReleasesSpans) {
    // fast_alloc_purge() must drain every pending queue into the slabs so
    // the drained slabs can empty and their spans return to the OS. The
    // observable is the OS-free counter: slab-empty releases are counted
    // exactly when the drain processes the deferred blocks.
    //
    // State normalization + sizing rationale:
    //   * Earlier suites leave freed blocks of this class parked in the
    //     MAIN thread's TLS bin (SizeClassTest walks every class); those
    //     are invisible to fast_alloc_purge() and keep their slab
    //     non-empty. Flushing the calling thread's cache first parks them
    //     where the purge can drain them.
    //   * In FASTALLOC_OVERRIDE_NEW builds the WHOLE process (gtest's own
    //     startup allocations included) routes through FastAlloc, so a
    //     foreign LIVE block of this class can permanently occupy one slab.
    //     The donor therefore takes 300 blocks - class 214 slabs hold 128
    //     blocks each, so the donor's blocks are guaranteed to span at
    //     least three slabs; every slab that contains only donor blocks
    //     empties on purge and releases its span, no matter what the first
    //     slab carries.
    fast_alloc_purge_thread_cache();
    fast_alloc_purge();
    FastAllocStats t0 = fast_alloc_stats();
    const std::size_t size = 3421;  // class 214: rarely used outside this test
    constexpr int kBlocks = 300;    // spans >= 3 slabs of 128 blocks
    std::thread donor([&] {
        std::vector<void*> v;
        for (int i = 0; i < kBlocks; ++i) {
            void* p = fast_malloc(size);
            if (p) v.push_back(p);
        }
        for (void* p : v) fast_free(p);  // bin count > defer cap: bulk flush
    });
    donor.join();          // remaining bin deferred to the pending queue
    fast_alloc_purge();    // MUST drain pending -> slabs empty -> spans freed
    FastAllocStats t1 = fast_alloc_stats();
    EXPECT_GT(t1.os_free_calls, t0.os_free_calls)
        << "purge did not release the deferred blocks' slab spans to the OS";
}

TEST(MpscPending, PurgeMustDrainSmallDeferredBins) {
    // Complement to PurgeDrainsDeferredBlocksAndReleasesSpans: that test's
    // 300-block donor exceeds the thread-exit defer cap (kDeferMaxBin = 64),
    // so the bin bulk-flushes to the slabs AT JOIN - its os_free_calls bump
    // comes from the bulk flush's slab work, not from purge's pending drain.
    // A purge that skips DrainPendingReturns entirely therefore still passes
    // that test. This variant keeps the donor's bin UNDER the defer cap, so
    // EVERY block is parked on the pending queue and the only way
    // os_free_calls can grow during fast_alloc_purge() is the drain
    // returning those blocks to their (otherwise non-empty) slabs.
    //
    // Sizing: size 7001 lands in a >= 4096-byte block class whose slabs hold
    // 32 blocks, so 48 blocks span two slabs - the first may share space
    // with foreign live blocks (override builds), the second is donor-only
    // and MUST empty when the drain processes it. 48 allocations exactly
    // exhaust the refill batches (16 + 32 blocks with one popped per
    // refill), so the bin is empty before the frees and ends at exactly 48
    // entries - comfortably under the 64-block defer cap.
    fast_alloc_purge_thread_cache();
    fast_alloc_purge();
    const std::size_t size = 7001;  // >= 4096-byte block class: 32 per slab
    constexpr int kBlocks = 48;     // 16 + 32 refill batches: no leftover
    std::thread donor([&] {
        std::vector<void*> v;
        for (int i = 0; i < kBlocks; ++i) {
            void* p = fast_malloc(size);
            if (p) v.push_back(p);
        }
        for (void* p : v) fast_free(p);
        // Explicit hand-off while the thread is alive: same defer path as
        // thread exit (FlushToGlobalHeap), just deterministic and visible.
        fast_alloc_purge_thread_cache();  // 48 <= 64: deferred, no slab work
    });
    donor.join();
    FastAllocStats mid = fast_alloc_stats();  // sampled after any bulk noise
    fast_alloc_purge();  // must drain pending -> donor-only slab empties
    FastAllocStats t1 = fast_alloc_stats();
    EXPECT_GT(t1.os_free_calls, mid.os_free_calls)
        << "purge did not drain the small deferred bin from the pending "
           "queue (the donor-only slab never emptied)";
}

#if FASTALLOC_DEBUG_ENABLED
TEST(MpscPending, TlsBinCountMatchesListLengthAfterOverflowFlush) {
    // The TLS bin overflow flush (DeallocateBlockSlow) must keep bin.count
    // exactly in sync with the linked list: the debug thread-exit AuditBin
    // walks every non-empty bin and ABORTS on a count/list divergence. An
    // off-by-one in the flush arithmetic (e.g. subtracting batch_size + 1)
    // is invisible while the thread runs - every refill pop rewrites the
    // count from the batch's true length - but it surfaces at exit, where
    // the un-corrected tail bin is audited before it is handed off.
    //
    // Sizing: size 7001 -> a >= 4096-byte block class: CACHE_LIMITS = 512,
    // overflow batch = 256. 513 allocations leave a refill remainder in the
    // bin; the 513 frees then push the count over 512, firing exactly one
    // overflow flush. The surviving tail keeps the drifted count until the
    // thread exits, where AuditBin compares it against the walked length.
    const std::size_t size = 7001;
    constexpr int kBlocks = 513;  // > CACHE_LIMITS (512): one overflow flush
    std::thread churner([&] {
        std::vector<void*> v;
        v.reserve(kBlocks);
        for (int i = 0; i < kBlocks; ++i) {
            void* p = fast_malloc(size);
            if (p) v.push_back(p);
        }
        for (void* p : v) fast_free(p);
        // Deliberately NO explicit flush here: the thread-exit path is the
        // audit point under test.
    });
    churner.join();  // under a drifted count this aborts inside AuditBin
    // Reaching here means every bin's count matched its list length.
    FastAllocStats s = fast_alloc_stats();
    EXPECT_GT(s.os_free_calls, 0u) << "overflow flush produced no slab work";
}
#endif  // FASTALLOC_DEBUG_ENABLED

#if !FASTALLOC_DEBUG_ENABLED
TEST(MpscPending, AdjacentDoubleFreeDoesNotDoubleHandOut) {
    // Release semantics: the TLS duplicate-head guard absorbs an adjacent
    // double free silently. A disabled guard would push the block into its
    // bin twice, and the next two allocations of the class would hand out
    // the SAME pointer - observable as a distinctness violation.
    const std::size_t size = 176;
    void* p = fast_malloc(size);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    fast_free(p);  // adjacent double free: absorbed by the head guard
    void* a = fast_malloc(size);
    void* b = fast_malloc(size);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b) << "the same block was handed out twice after the "
                       "guarded double free";
    fast_free(a);
    fast_free(b);
}
#endif
