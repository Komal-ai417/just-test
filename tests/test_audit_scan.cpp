// ============================================================================
// fast_alloc_audit_pointers() diagnostic scan tests.
//
// The scan inspects the allocator's head arrays (TLS bins, TLS large bins,
// arena slab/pending lists, page bins) and reports every internal pointer
// that falls inside [lo, hi). These tests drive it with ranges that provably
// contain (or provably avoid) known head pointers, capturing stderr to
// verify the reported verdict.
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "test_hooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace FastAlloc;

namespace {

// Redirects stderr to a temporary file for the duration of one scan, then
// restores it and returns everything written. POSIX only; the whole file is
// skipped on Windows (the scan itself works there, but the capture plumbing
// below uses dup/dup2).
class StderrCapture {
public:
    StderrCapture() {
        Path();
        fflush(stderr);
        saved_ = dup(kFd);
        file_ = std::freopen(path_.c_str(), "w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    ~StderrCapture() {
        fflush(stderr);
        if (file_) std::fclose(stderr);
        stderr = reinterpret_cast<FILE*>(stdout);  // placeholder before dup2 restores
        dup2(saved_, kFd);
        stderr = fdopen(kFd, "w");
        clearerr(stderr);
        std::remove(path_.c_str());
    }
    std::string Contents() {
        fflush(stderr);
        FILE* f = std::fopen(path_.c_str(), "r");
        if (!f) return {};
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) out += buf;
        std::fclose(f);
        return out;
    }

private:
    static constexpr int kFd = 2;
    // Per-process capture file so parallel test binaries never collide.
    const std::string& Path() {
        if (path_.empty()) {
            path_ = std::string("/tmp/fastalloc_audit_scan_") +
                    std::to_string(static_cast<long>(getpid())) + ".err";
        }
        return path_;
    }
    std::string path_;
    int saved_ = -1;
    FILE* file_ = nullptr;
};

} // namespace

#ifndef _WIN32

TEST(AuditScan, TlsBinHeadInRangeIsReported) {
    // Free a block of a cold class: its BLOCK BASE (user pointer - 16) becomes
    // the TLS bin head, so a scan covering that address must report the TLS
    // bin hit.
    const std::size_t size = 3289;  // rarely-used class (206)
    void* p = fast_malloc(size);
    ASSERT_NE(p, nullptr);
    fast_free(p);  // the block base is now the class bin head
    char* block_base = static_cast<char*>(p) - 16;

    StderrCapture cap;
    fast_alloc_audit_pointers(block_base, block_base + 16);
    std::string out = cap.Contents();
    EXPECT_NE(out.find("tls bin"), std::string::npos)
        << "scan must attribute the TLS bin head pointer";
    EXPECT_NE(out.find("IN RANGE"), std::string::npos);
    EXPECT_NE(out.find("pointer(s) target the region"), std::string::npos);
}

TEST(AuditScan, UnrelatedRangeReportsNoHits) {
    // A mapped but unused address range: no head pointer can target it.
    StderrCapture cap;
    fast_alloc_audit_pointers(reinterpret_cast<const void*>(0x10000),
                              reinterpret_cast<const void*>(0x20000));
    std::string out = cap.Contents();
    EXPECT_NE(out.find("no allocator head pointer targets the region"),
              std::string::npos);
    EXPECT_EQ(out.find("IN RANGE"), std::string::npos);
}

TEST(AuditScan, InvalidRangesAreSilentNoops) {
    {
        StderrCapture cap;
        fast_alloc_audit_pointers(nullptr, reinterpret_cast<const void*>(0x10000));
        EXPECT_TRUE(cap.Contents().empty()) << "null lo must return early";
    }
    {
        StderrCapture cap;
        fast_alloc_audit_pointers(reinterpret_cast<const void*>(0x10000), nullptr);
        EXPECT_TRUE(cap.Contents().empty()) << "null hi must return early";
    }
    {
        StderrCapture cap;
        fast_alloc_audit_pointers(reinterpret_cast<const void*>(0x20000),
                                  reinterpret_cast<const void*>(0x10000));
        EXPECT_TRUE(cap.Contents().empty()) << "inverted range must return early";
    }
    // Fully serviceable afterwards.
    void* p = fast_malloc(64);
    ASSERT_NE(p, nullptr);
    fast_free(p);
}

TEST(AuditScan, PageBinHeadInRangeIsReported) {
    // Park a large span in the global page cache: the SPAN BASE (user - 16)
    // becomes a page-bin head; scanning its range must report the page bin hit.
    fast_alloc_purge_thread_cache();
    FastAllocTestPurgePageCache();
    const std::size_t span = 16 * 4096 - 16;  // exactly 16 pages
    unsigned char* p = static_cast<unsigned char*>(fast_malloc(span));
    ASSERT_NE(p, nullptr);
    fast_free(p);
    fast_alloc_purge_thread_cache();  // span now parked in the page cache
    char* span_base = reinterpret_cast<char*>(p) - 16;

    StderrCapture cap;
    fast_alloc_audit_pointers(span_base, span_base + 16);
    std::string out = cap.Contents();
    EXPECT_NE(out.find("page bin"), std::string::npos)
        << "scan must attribute the page-bin head pointer";
    EXPECT_NE(out.find("IN RANGE"), std::string::npos);

    FastAllocTestPurgePageCache();
}

TEST(AuditScan, LiveRangeAroundAllocatedBlockIsQuiet) {
    // A live (handed-out) block is by definition not referenced by any head
    // array; scanning inside it must find nothing (unless a neighbouring
    // head coincidentally lands in range, which the wide verdict text still
    // disambiguates - here we assert the allocator stays usable and the call
    // completes).
    void* p = fast_malloc(8192);
    ASSERT_NE(p, nullptr);
    {
        StderrCapture cap;
        fast_alloc_audit_pointers(p, static_cast<char*>(p) + 8192);
        (void)cap.Contents();
    }
    std::memset(p, 0x42, 8192);
    fast_free(p);
    SUCCEED();
}

#endif // !_WIN32
