// ============================================================================
// Debug-instrumentation internals (FASTALLOC_DEBUG only): allocation
// registry data structure (insert/erase/duplicate/tombstone chains, shard
// growth, live accounting, test reset), poison pattern helpers, and the
// logging subsystem's actual emission behaviour (captured from stderr).
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "debug_aid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace FastAlloc;

#if FASTALLOC_DEBUG_ENABLED && !defined(_WIN32)

// ---------------------------------------------------------------------------
// Registry data structure
// ---------------------------------------------------------------------------
TEST(DebugRegistry, InsertEraseRoundtrip) {
    auto& reg = AllocRegistry::GetInstance();
    const std::size_t base_live = reg.live_count();
    AllocRecord rec{};
    rec.ptr = reinterpret_cast<void*>(0x0000500000001000ull);
    rec.size = 128;
    rec.usable = 144;
    rec.thread_id = CurrentThreadId();
    rec.class_or_large = 8;
    EXPECT_TRUE(reg.Insert(rec, nullptr));
    EXPECT_EQ(reg.live_count(), base_live + 1);

    AllocRecord out{};
    EXPECT_TRUE(reg.Erase(rec.ptr, &out));
    EXPECT_EQ(out.ptr, rec.ptr);
    EXPECT_EQ(out.size, 128u);
    EXPECT_EQ(out.usable, 144u);
    EXPECT_EQ(out.class_or_large, 8u);

    // Erasing again must fail (not live).
    EXPECT_FALSE(reg.Erase(rec.ptr, nullptr));
    EXPECT_EQ(reg.live_count(), base_live);
}

TEST(DebugRegistry, DuplicateInsertReportsExisting) {
    auto& reg = AllocRegistry::GetInstance();
    const std::size_t base_live = reg.live_count();
    AllocRecord a{};
    a.ptr = reinterpret_cast<void*>(0x0000500000002000ull);
    a.size = 64;
    a.usable = 64;
    a.class_or_large = 3;
    ASSERT_TRUE(reg.Insert(a, nullptr));

    AllocRecord b{};
    b.ptr = a.ptr;
    b.size = 999;  // conflicting second registration
    AllocRecord existing{};
    EXPECT_FALSE(reg.Insert(b, &existing));
    EXPECT_EQ(existing.ptr, a.ptr);
    EXPECT_EQ(existing.size, 64u) << "the original record must be reported";
    EXPECT_EQ(reg.live_count(), base_live + 1);

    EXPECT_TRUE(reg.Erase(a.ptr, nullptr));
}

TEST(DebugRegistry, TombstoneChainsKeepProbeIntegrity) {
    auto& reg = AllocRegistry::GetInstance();
    // Three pointers engineered to share a hash region: the shard is chosen
    // by (ptr >> 4) % 16, so pointers 64 bytes apart in the low bits land in
    // the same shard; their open-addressing slots are collision-prone.
    std::uintptr_t base = 0x0000500000004000ull;
    void* p1 = reinterpret_cast<void*>(base);
    void* p2 = reinterpret_cast<void*>(base + 4096 * 64);
    void* p3 = reinterpret_cast<void*>(base + 8192 * 64);
    AllocRecord r1{}, r2{}, r3{};
    r1.ptr = p1; r1.size = 16; r1.usable = 16;
    r2.ptr = p2; r2.size = 32; r2.usable = 32;
    r3.ptr = p3; r3.size = 48; r3.usable = 48;
    ASSERT_TRUE(reg.Insert(r1, nullptr));
    ASSERT_TRUE(reg.Insert(r2, nullptr));
    ASSERT_TRUE(reg.Insert(r3, nullptr));

    // Erase the first (tombstone) - the others must still be findable
    // through the tombstone chain.
    EXPECT_TRUE(reg.Erase(p1, nullptr));
    EXPECT_FALSE(reg.Erase(p1, nullptr));
    EXPECT_TRUE(reg.Erase(p2, nullptr));
    EXPECT_TRUE(reg.Erase(p3, nullptr));
}

TEST(DebugRegistry, ReinsertAfterEraseReusesTombstone) {
    auto& reg = AllocRegistry::GetInstance();
    void* p = reinterpret_cast<void*>(0x0000500000006000ull);
    AllocRecord r{};
    r.ptr = p; r.size = 100; r.usable = 112;
    ASSERT_TRUE(reg.Insert(r, nullptr));
    ASSERT_TRUE(reg.Erase(p, nullptr));
    // The slot is now a tombstone; a fresh insert of the same pointer must
    // succeed (freed-and-recycled address) and remain erasable.
    r.size = 200;
    ASSERT_TRUE(reg.Insert(r, nullptr));
    AllocRecord out{};
    EXPECT_TRUE(reg.Erase(p, &out));
    EXPECT_EQ(out.size, 200u) << "the re-inserted record must be found";
}

TEST(DebugRegistry, GrowthRehashPreservesEveryRecord) {
    auto& reg = AllocRegistry::GetInstance();
    // Insert enough records to cross the initial shard capacity (1024 per
    // shard, rehash at 70% load): 40k pointers spread over 16 shards.
    constexpr int kCount = 40000;
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        void* p = reinterpret_cast<void*>(0x0000500000000000ull +
                                          (static_cast<std::uintptr_t>(i) + 1) * 64ull);
        AllocRecord r{};
        r.ptr = p;
        r.size = static_cast<std::size_t>(i) + 1;
        r.usable = r.size;
        r.class_or_large = 1;
        ASSERT_TRUE(reg.Insert(r, nullptr)) << "i=" << i;
        ptrs.push_back(p);
    }
    EXPECT_GE(reg.live_count(), static_cast<std::size_t>(kCount));
    // Every record must survive the growth rehash and remain erasable with
    // its original payload.
    for (int i = 0; i < kCount; ++i) {
        AllocRecord out{};
        ASSERT_TRUE(reg.Erase(ptrs[static_cast<std::size_t>(i)], &out)) << "i=" << i;
        ASSERT_EQ(out.size, static_cast<std::size_t>(i) + 1) << "i=" << i;
    }
}

TEST(DebugRegistry, LiveBytesAccounting) {
    auto& reg = AllocRegistry::GetInstance();
    constexpr int kCount = 50;
    // Reserve every container BEFORE the baseline: in override-new builds the
    // vector's own growth allocations register too and would skew the delta.
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);
    const std::size_t base_bytes = reg.live_bytes();
    for (int i = 0; i < kCount; ++i) {
        void* p = reinterpret_cast<void*>(0x0000500010000000ull +
                                          (static_cast<std::uintptr_t>(i) + 1) * 64ull);
        AllocRecord r{};
        r.ptr = p;
        r.size = 256;
        r.usable = 256;
        ASSERT_TRUE(reg.Insert(r, nullptr));
        ptrs.push_back(p);
    }
    EXPECT_EQ(reg.live_bytes(), base_bytes + static_cast<std::size_t>(kCount) * 256);
    for (void* p : ptrs) reg.Erase(p, nullptr);
    EXPECT_EQ(reg.live_bytes(), base_bytes);
}

// ResetForTesting wipes the GLOBAL registry; it is only safe in processes
// where no other FastAlloc-tracked allocation is live. Override-new builds
// route every gtest/libstdc++ allocation through the tracked allocator, so
// the reset semantics are verified only in ordinary (non-override) builds.
#if !defined(FAST_ALLOC_OVERRIDE_NEW)
TEST(DebugRegistry, ResetForTestingClearsState) {
    auto& reg = AllocRegistry::GetInstance();
    const std::size_t base_live = reg.live_count();
    AllocRecord r{};
    r.ptr = reinterpret_cast<void*>(0x0000500000008000ull);
    r.size = 32;
    r.usable = 32;
    ASSERT_TRUE(reg.Insert(r, nullptr));
    EXPECT_EQ(reg.live_count(), base_live + 1);
    reg.ResetForTesting();
    EXPECT_EQ(reg.live_count(), 0u);
    EXPECT_EQ(reg.live_bytes(), 0u);
    // The cleared registry accepts the same pointer again.
    ASSERT_TRUE(reg.Insert(r, nullptr));
    EXPECT_TRUE(reg.Erase(r.ptr, nullptr));
    reg.ResetForTesting();
}
#endif

// ---------------------------------------------------------------------------
// Poison helpers
// ---------------------------------------------------------------------------
TEST(DebugPoison, PoisonedRegionIsIntactUntilDisturbed) {
    std::vector<unsigned char> buf(1024, 0);
    void* p = buf.data();
    debug_check::PoisonFreed(p, buf.size());
    EXPECT_TRUE(debug_check::PoisonIntact(p, buf.size()));

    // Disturb the head sample -> detected.
    buf[8] = 0x00;
    EXPECT_FALSE(debug_check::PoisonIntact(p, buf.size()));
}

TEST(DebugPoison, TailSampleDisturbanceIsDetected) {
    std::vector<unsigned char> buf(2048, 0);
    void* p = buf.data();
    debug_check::PoisonFreed(p, buf.size());
    EXPECT_TRUE(debug_check::PoisonIntact(p, buf.size()));
    buf[2048 - 8] = 0x00;  // inside the tail 256-byte sample
    EXPECT_FALSE(debug_check::PoisonIntact(p, buf.size()));
}

TEST(DebugPoison, MiddleBytesAreSampledBounded) {
    // Poisoning samples head and tail 256 bytes; a disturbance in the
    // unsampled middle is intentionally not flagged (bounded cost design).
    std::vector<unsigned char> buf(4096, 0);
    void* p = buf.data();
    debug_check::PoisonFreed(p, buf.size());
    EXPECT_TRUE(debug_check::PoisonIntact(p, buf.size()));
    buf[2048] = 0x11;  // middle, outside both samples
    EXPECT_TRUE(debug_check::PoisonIntact(p, buf.size()));
}

TEST(DebugPoison, FreedUserBlockCarriesPoisonPattern) {
    // End-to-end: a real freed small block shows the 0xDD pattern in its
    // sampled region (offset 8.., past the free-list link).
    void* p = fast_malloc(200);
    ASSERT_NE(p, nullptr);
    fast_free(p);
    auto* c = static_cast<const unsigned char*>(p);
    for (std::size_t i = 8; i < 64; ++i) {
        EXPECT_EQ(c[i], debug::POISON_FREE) << "poison byte " << i << " disturbed";
    }
}

TEST(DebugPoison, RearRedZoneBytesArePatternFilled) {
    // A live allocation's slack (request end .. block end) is 0xBB filled,
    // including the RED_ZONE_RESERVE tail that guarantees a non-empty zone.
    const std::size_t req = 100;
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(req));
    ASSERT_NE(p, nullptr);
    std::size_t class_index = RequestToClass(req);
    std::size_t block_end = ClassIndexToSize(class_index) - USER_OFFSET;
    ASSERT_GT(block_end, req);
    for (std::size_t i = req; i < block_end; ++i) {
        ASSERT_EQ(static_cast<unsigned char>(p[i]), debug::RED_ZONE)
            << "red-zone byte " << i << " not 0xBB";
    }
    fast_free(p);
}

TEST(DebugPoison, FrontCanaryValue) {
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(40));
    ASSERT_NE(p, nullptr);
    uint32_t canary = *reinterpret_cast<const uint32_t*>(p - 4);
    EXPECT_EQ(canary, debug::FRONT_CANARY);
    fast_free(p);
}

// ---------------------------------------------------------------------------
// Logging emission
// ---------------------------------------------------------------------------
namespace {
class StderrRedirect {
public:
    StderrRedirect() {
        path_ = std::string("/tmp/fastalloc_log_test_") +
                std::to_string(static_cast<long>(::getpid())) + ".err";
        fflush(stderr);
        saved_ = dup(2);
        file_ = freopen(path_.c_str(), "w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    ~StderrRedirect() {
        fflush(stderr);
        if (file_) fclose(stderr);
        stderr = reinterpret_cast<FILE*>(stdout);
        dup2(saved_, 2);
        stderr = fdopen(2, "w");
        clearerr(stderr);
        remove(path_.c_str());
    }
    std::string Contents() {
        fflush(stderr);
        FILE* f = fopen(path_.c_str(), "r");
        if (!f) return {};
        std::string out;
        char buf[512];
        while (fgets(buf, sizeof buf, f)) out += buf;
        fclose(f);
        return out;
    }

private:
    std::string path_;
    int saved_ = -1;
    FILE* file_ = nullptr;
};
} // namespace

TEST(DebugLogging, LogEventEmitsFormattedMessage) {
    LogSetLevel(LogLevel::Trace);
    {
        StderrRedirect cap;
        LogEvent(LogLevel::Trace, "unit-probe", "value=%d tag=%s", 42, "xyz");
        std::string out = cap.Contents();
        EXPECT_NE(out.find("unit-probe"), std::string::npos);
        EXPECT_NE(out.find("value=42 tag=xyz"), std::string::npos);
        EXPECT_NE(out.find("[FastAlloc]"), std::string::npos);
        // Level gating: an Info event must NOT pass while the level is Error.
        LogSetLevel(LogLevel::Error);
        LogEvent(LogLevel::Info, "gated-probe", "n=1");
        out = cap.Contents();
        EXPECT_EQ(out.find("gated-probe"), std::string::npos);
    }
    LogSetLevel(LogLevel::Off);
}

TEST(DebugLogging, LevelGatingSuppressesLowerLevels) {
    LogSetLevel(LogLevel::Warning);
    {
        StderrRedirect cap;
        LogEvent(LogLevel::Error, "lvl-error", "x");
        LogEvent(LogLevel::Warning, "lvl-warn", "x");
        LogEvent(LogLevel::Info, "lvl-info", "x");
        LogEvent(LogLevel::Debug, "lvl-debug", "x");
        std::string out = cap.Contents();
        EXPECT_NE(out.find("lvl-error"), std::string::npos);
        EXPECT_NE(out.find("lvl-warn"), std::string::npos);
        EXPECT_EQ(out.find("lvl-info"), std::string::npos);
        EXPECT_EQ(out.find("lvl-debug"), std::string::npos);
    }
    LogSetLevel(LogLevel::Off);
}

TEST(DebugLogging, ApiLogLevelSetterClampsRange) {
    fast_alloc_log_set_level(99);   // clamps to 5
    EXPECT_EQ(static_cast<int>(LogGetLevel()), 5);
    fast_alloc_log_set_level(-7);   // clamps to 0
    EXPECT_EQ(static_cast<int>(LogGetLevel()), 0);
    fast_alloc_log_set_level(3);
    EXPECT_EQ(static_cast<int>(LogGetLevel()), 3);
    fast_alloc_log_set_level(0);
}

// ---------------------------------------------------------------------------
// Registry-backed leak dump format
// ---------------------------------------------------------------------------
TEST(DebugLeakDump, LeakReportListsLiveAllocations) {
    void* a = fast_malloc(111);
    void* b = fast_malloc(222);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    {
        StderrRedirect cap;
        fast_alloc_dump_leaks();
        std::string out = cap.Contents();
        EXPECT_NE(out.find("LEAK REPORT"), std::string::npos);
        EXPECT_NE(out.find("size=111"), std::string::npos);
        EXPECT_NE(out.find("size=222"), std::string::npos);
        EXPECT_NE(out.find("live allocations"), std::string::npos);
    }
    fast_free(a);
    fast_free(b);
    {
        StderrRedirect cap;
        fast_alloc_dump_leaks();
        std::string out = cap.Contents();
        EXPECT_NE(out.find("LEAK REPORT"), std::string::npos);
    }
}

// Sequence numbers must be strictly increasing across consecutive
// allocations (globally monotonic for live-block ordering).
TEST(DebugRegistry, SequenceNumbersAreMonotonic) {
    auto& reg = AllocRegistry::GetInstance();
    void* p1 = fast_malloc(64);
    void* p2 = fast_malloc(64);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    AllocRecord r1{}, r2{};
    ASSERT_TRUE(reg.Erase(p1, &r1));
    ASSERT_TRUE(reg.Erase(p2, &r2));
    EXPECT_GT(r2.sequence, r1.sequence) << "later allocation must get a larger seq";
    // Re-insert both so the fast_free paths validate normally.
    ASSERT_TRUE(reg.Insert(r1, nullptr));
    ASSERT_TRUE(reg.Insert(r2, nullptr));
    fast_free(p1);
    fast_free(p2);
}

#endif // FASTALLOC_DEBUG_ENABLED && !_WIN32
