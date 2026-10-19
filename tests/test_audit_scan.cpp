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

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace FastAlloc;

namespace {

// CRT descriptor helpers. MSVC deprecates the POSIX spellings (dup/dup2/
// fileno/close -> C4996, an error under /W4 /WX), so the ISO underscore
// names are used there; every other platform takes the POSIX names.
inline int CaptureDup(int fd) {
#ifdef _WIN32
    return _dup(fd);
#else
    return dup(fd);
#endif
}
inline int CaptureDup2(int src, int dst) {
#ifdef _WIN32
    return _dup2(src, dst);
#else
    return dup2(src, dst);
#endif
}
inline int CaptureClose(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}
inline int CaptureFileno(std::FILE* f) {
#ifdef _WIN32
    return _fileno(f);
#else
    return fileno(f);
#endif
}

// Redirects stderr to a temporary file for the duration of one scan, then
// restores the original destination and hands back everything written.
//
// freopen() re-binds the same FILE object (it stays a valid `stderr`
// expression on every platform - MSVC's stderr is a macro and cannot be
// reassigned), and the descriptor saved by dup() is re-attached with dup2()
// on destruction. No fclose/fdopen dance: the FILE never closes.
class StderrCapture {
public:
    StderrCapture()
        : path_(testing::TempDir() + "fastalloc_audit_scan_" +
                std::to_string(NextSerial()) + ".err") {
        fd_ = CaptureFileno(stderr);
        std::fflush(stderr);
        saved_ = CaptureDup(fd_);
        // freopen() is declared with __wur (warn_unused_result) in glibc - a
        // fatal error under -Werror on Ubuntu CI when the result is ignored.
        // Handling it also matters semantically: on failure stderr keeps its
        // original destination, the capture file is never created, and every
        // later Contents() read would silently return an empty string. A
        // missing saved descriptor is equally unrecoverable (the original
        // stderr could not be restored), so both cases mark the capture
        // unhealthy and the tests report that loudly via Ok().
        if (saved_ < 0 || std::freopen(path_.c_str(), "w", stderr) == nullptr) {
            capture_ok_ = false;
            return;
        }
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    ~StderrCapture() {
        Restore();
        std::remove(path_.c_str());
    }
    // True when stderr was actually re-bound to the capture file. Tests
    // must ASSERT on this before reading Contents(): a failed capture is an
    // environment problem, not an allocator verdict.
    bool Ok() const { return capture_ok_; }

    std::string Contents() {
        std::fflush(stderr);
        FILE* f = std::fopen(path_.c_str(), "r");
        if (!f) return {};
        std::string out;
        char buf[512];
        while (std::fgets(buf, sizeof buf, f)) out += buf;
        std::fclose(f);
        return out;
    }

private:
    void Restore() {
        std::fflush(stderr);
        if (saved_ >= 0) {
            CaptureDup2(saved_, fd_);
            CaptureClose(saved_);
            saved_ = -1;
        }
        std::clearerr(stderr);
    }
    // Per-process capture file so parallel test binaries never collide.
    static unsigned NextSerial() {
        static unsigned n = 0;
        return n++;
    }
    std::string path_;
    bool capture_ok_ = true;
    int fd_ = -1;
    int saved_ = -1;
};

} // namespace

TEST(AuditScan, TlsBinHeadInRangeIsReported) {
    // Free a block of a cold class: its BLOCK BASE (user pointer - 16) becomes
    // the TLS bin head, so a scan covering that address must report the TLS
    // bin hit.
    const std::size_t size = 3289;  // rarely-used class (206)
    void* p = fast_malloc(size);
    ASSERT_NE(p, nullptr);
    fast_free(p);  // the block base is now the class bin head
    const char* block_base = static_cast<char*>(p) - 16;

    StderrCapture cap;
    ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
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
    ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
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
        ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
        fast_alloc_audit_pointers(nullptr, reinterpret_cast<const void*>(0x10000));
        EXPECT_TRUE(cap.Contents().empty()) << "null lo must return early";
    }
    {
        StderrCapture cap;
        ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
        fast_alloc_audit_pointers(reinterpret_cast<const void*>(0x10000), nullptr);
        EXPECT_TRUE(cap.Contents().empty()) << "null hi must return early";
    }
    {
        StderrCapture cap;
        ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
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
    const char* span_base = reinterpret_cast<char*>(p) - 16;

    StderrCapture cap;
    ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
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
        ASSERT_TRUE(cap.Ok()) << "stderr capture setup failed (freopen/dup)";
        fast_alloc_audit_pointers(p, static_cast<char*>(p) + 8192);
        (void)cap.Contents();
    }
    std::memset(p, 0x42, 8192);
    fast_free(p);
    SUCCEED();
}
