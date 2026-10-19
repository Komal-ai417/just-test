// ============================================================================
// MemorySanitizer smoke harness (also a portable ctest smoke test).
//
// MSan (clang-only) flags reads of uninitialized memory and requires every
// linked library to be instrumented. The gtest suites link gtest and the
// C++ runtime, which a plain CI environment cannot provide in an
// MSan-instrumented form, so this harness is deliberately freestanding: no
// gtest, no STL containers, no iostream, no std::thread - just the FastAlloc
// public API, libc string functions and pthreads (both fully intercepted by
// MSan). tools/msan_check.sh builds and runs it with
// clang -fsanitize=memory -fsanitize-memory-track-origins=2.
//
// The scenarios double as generic smoke checks (it runs as a normal ctest
// entry on every platform): every returned pointer is written across its
// full image and read back, calloc memory is read BEFORE any write (an
// allocator that relies on zero pages without an interceptor-visible memset
// is an MSan report), realloc prefixes are verified across the small/large
// boundary, and the cross-thread hand-off exercises the pending-queue paths.
// ============================================================================
#include "fast_alloc.h"
#include "fast_alloc_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace FastAlloc;

namespace {

int g_failures = 0;

#define HARNESS_CHECK(cond)                                                   \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "MSAN-HARNESS FAIL %s:%d: %s\n",             \
                         __FILE__, __LINE__, #cond);                          \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

void FillImage(unsigned char* p, std::size_t n, unsigned char seed) {
    for (std::size_t i = 0; i < n; ++i) {
        p[i] = static_cast<unsigned char>(seed + i);
    }
}

bool ImageMatches(const unsigned char* p, std::size_t n, unsigned char seed) {
    for (std::size_t i = 0; i < n; ++i) {
        if (p[i] != static_cast<unsigned char>(seed + i)) return false;
    }
    return true;
}

// malloc/free round-trips over size-class boundaries and both span paths.
// Writing then reading every byte means MSan checks that the allocator hands
// out memory whose read-after-write round-trip is clean (it must never make
// internal metadata decisions based on bytes it forgot to initialize).
void TestMallocFreeCycles() {
    const std::size_t sizes[] = {0,    1,     15,     16,     17,     24,
                                 32,   64,    100,    248,    256,    1000,
                                 4095, 4096,  8176,   8177,   8192,   65536,
                                 1u << 20, 4u * 1024 * 1024};
    for (std::size_t size : sizes) {
        void* p = fast_malloc(size);
        HARNESS_CHECK(p != nullptr);
        if (!p) continue;
        unsigned char* c = static_cast<unsigned char*>(p);
        FillImage(c, size, 0x5A);
        HARNESS_CHECK(ImageMatches(c, size, 0x5A));
        fast_free(p);
    }
}

// calloc must hand out memory that reads as zero WITHOUT any prior write:
// a fresh mmap is "uninitialized" as far as MSan's shadow is concerned, so
// an allocator that skips the memset because "mmap is already zero" is a
// guaranteed false-positive generator for instrumented callers.
void TestCallocZeroedBeforeWrite() {
    const std::size_t cases[][2] = {{0, 8},   {8, 0},     {1, 1},
                                    {16, 16}, {13, 61},   {64, 64},
                                    {31, 97}, {1, 1 << 20}};
    for (const auto& c : cases) {
        unsigned char* p = static_cast<unsigned char*>(fast_calloc(c[0], c[1]));
        HARNESS_CHECK(p != nullptr);
        if (!p) continue;
        const std::size_t n = c[0] * c[1];
        bool zeroed = true;
        for (std::size_t i = 0; i < n; ++i) {
            if (p[i] != 0) { zeroed = false; break; }
        }
        HARNESS_CHECK(zeroed);
        fast_free(p);
    }
}

// realloc chains across the small/large boundary and back; every prefix byte
// the caller previously wrote must stay initialized and identical.
void TestReallocChains() {
    const std::size_t steps[] = {2,      16,     248,    4096,  8176,
                                 8177,   65536,  1u << 20, 8177, 248,
                                 1};
    std::size_t size = 1;
    void* p = fast_malloc(size);
    HARNESS_CHECK(p != nullptr);
    if (!p) return;
    FillImage(static_cast<unsigned char*>(p), size, 0xC3);
    for (std::size_t next : steps) {
        void* q = fast_realloc(p, next);
        HARNESS_CHECK(q != nullptr);
        if (!q) { fast_free(p); return; }
        const std::size_t prefix = size < next ? size : next;
        HARNESS_CHECK(ImageMatches(static_cast<unsigned char*>(q), prefix, 0xC3));
        FillImage(static_cast<unsigned char*>(q), next, 0xC3);
        p = q;
        size = next;
    }
    // realloc(p, 0) frees the block (C11 semantics) and returns nullptr.
    HARNESS_CHECK(fast_realloc(p, 0) == nullptr);
}

// Aligned allocations across the full supported alignment range, plus the
// documented rejection cases.
void TestAlignedAlloc() {
    const std::size_t alignments[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    const std::size_t sizes[] = {0, 1, 17, 4097, 1u << 20};
    for (std::size_t align : alignments) {
        for (std::size_t size : sizes) {
            void* p = fast_aligned_alloc(align, size);
            HARNESS_CHECK(p != nullptr);
            if (!p) continue;
            HARNESS_CHECK((reinterpret_cast<std::uintptr_t>(p) % align) == 0);
            FillImage(static_cast<unsigned char*>(p), size, 0x77);
            HARNESS_CHECK(ImageMatches(static_cast<unsigned char*>(p), size, 0x77));
            fast_free(p);
        }
    }
    HARNESS_CHECK(fast_aligned_alloc(0, 8) == nullptr);    // not a power of two
    HARNESS_CHECK(fast_aligned_alloc(24, 8) == nullptr);   // not a power of two
    HARNESS_CHECK(fast_aligned_alloc(8192, 8) == nullptr); // beyond the cap
}

// Zero-size semantics and sized/wrong-hint frees.
void TestApiContracts() {
    void* a = fast_malloc(0);
    void* b = fast_malloc(0);
    HARNESS_CHECK(a != nullptr);
    HARNESS_CHECK(b != nullptr);
    HARNESS_CHECK(a != b);
    fast_free(nullptr);           // no-op
    fast_free_sized(nullptr, 0);  // no-op
    void* c = fast_malloc(64);
    HARNESS_CHECK(c != nullptr);
    FillImage(static_cast<unsigned char*>(c), 64, 0x31);
    fast_free_sized(c, 64);       // matching hint
    fast_free_sized(a, 0);        // zero-size block, zero hint
    fast_free_sized(b, 999);      // wrong hint must route to the true class
}

#ifndef _WIN32
// Cross-thread hand-off: a short-lived thread allocates blocks of a cold
// class and exits (its cache is deferred to the pending queue); the parent
// then READS the thread-written bytes, grows each block through realloc and
// frees it - driving the deferred hand-off, the pending drain and the
// slab-return paths under MSan's shadow tracking.
struct HandoffState {
    void* blocks[16];
    std::size_t count;
};

void* HandoffThreadMain(void* arg) {
    auto* st = static_cast<HandoffState*>(arg);
    st->count = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        void* p = fast_malloc(3289);  // cold class, slab-served
        if (!p) break;
        FillImage(static_cast<unsigned char*>(p), 32, 0x9D);
        st->blocks[st->count++] = p;
    }
    return nullptr;
}

void TestCrossThreadHandoff() {
    HandoffState st;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, HandoffThreadMain, &st) != 0) {
        std::fprintf(stderr, "MSAN-HARNESS: pthread_create failed; skipping\n");
        return;
    }
    pthread_join(thread, nullptr);
    for (std::size_t i = 0; i < st.count; ++i) {
        unsigned char* block = static_cast<unsigned char*>(st.blocks[i]);
        HARNESS_CHECK(ImageMatches(block, 32, 0x9D));  // parent reads child-written bytes
        void* grown = fast_realloc(st.blocks[i], 8000);
        HARNESS_CHECK(grown != nullptr);
        if (!grown) continue;
        HARNESS_CHECK(ImageMatches(static_cast<unsigned char*>(grown), 32, 0x9D));
        fast_free(grown);
    }
    // Purge drains the deferred blocks back into their slabs and releases
    // the emptied spans (also exercises the page-cache free path).
    fast_alloc_purge();
}
#endif

// Stats/purge observables and the debug-aid entry points (no-ops in release
// builds; must not touch uninitialized state in any build).
void TestDiagnostics() {
    fast_alloc_log_set_level(0);
    FastAllocStats s = fast_alloc_stats();
    HARNESS_CHECK(s.small_allocs + s.large_allocs >= 1);
    void* p = fast_malloc(4096);
    HARNESS_CHECK(p != nullptr);
    fast_alloc_purge_thread_cache();
    fast_alloc_purge();
    fast_free(p);
    fast_alloc_purge();
    fast_alloc_dump_leaks();
}

} // namespace

int main() {
    const FastAllocStats before = fast_alloc_stats();

    TestMallocFreeCycles();
    TestCallocZeroedBeforeWrite();
    TestReallocChains();
    TestAlignedAlloc();
    TestApiContracts();
#ifndef _WIN32
    TestCrossThreadHandoff();
#endif
    TestDiagnostics();

    const FastAllocStats after = fast_alloc_stats();
    // Every harness allocation was returned: the live-block counter must be
    // back at the pre-harness level (a mismatch means a leak or a stuck
    // pending queue).
    HARNESS_CHECK(after.current_live_blocks == before.current_live_blocks);

    if (g_failures != 0) {
        std::printf("MSAN-HARNESS: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("MSAN-HARNESS: OK\n");
    return 0;
}
