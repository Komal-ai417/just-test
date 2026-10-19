#!/usr/bin/env bash
# ============================================================================
# strace_syscalls.sh — OS-interaction validation. Runs a deterministic
# map/churn/free/purge workload under strace and asserts the behavioural
# contract:
#   * FastAlloc grows the heap via mmap only - no brk() based growth inside
#     the FastAlloc workload window,
#   * purge actually releases cached spans back to the OS (munmap for direct
#     spans, MADV_DONTNEED for pooled spans),
#   * the probe completed (PROBE-START ... PROBE-DONE on stdout), so the
#     counts describe a full run rather than a crash mid-way.
#
# Methodology notes:
#   * The probe itself is free of <cstdio>, streams and STL containers:
#     glibc services printf buffering and std::vector growth from its own
#     brk()-grown heap, which a plain process-wide syscall count cannot
#     distinguish from an allocator brk.
#   * Even so, the dynamic runtime is NOT silent: ld.so, libc and libstdc++
#     perform startup allocations before main() (e.g. libstdc++'s static
#     init requests one ~72 KB block) and glibc's malloc queries the break
#     with brk(NULL). Those are outside the allocator's responsibility.
#     The probe therefore brackets the workload with two raw write(2)
#     markers (no stdio buffering involved), and every assertion is scoped
#     to the syscall lines BETWEEN those markers: inside that window the
#     only code that runs is FastAlloc plus plain computation, so a brk
#     there is attributable to the allocator under test.
#
# Usage: bash tools/strace_syscalls.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v strace > /dev/null || { echo "strace not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-tools}"

cat > /tmp/fastalloc_strace_probe.cpp <<'EOF'
// Deterministic syscall-shape probe: map, churn, free, purge.
// Allocation-free outside FastAlloc itself: static slot storage, raw
// write() markers/completion (no stdio buffering, no STL containers), so
// every heap request in the workload window belongs to the allocator.
#include "fast_alloc.h"
#include <cstring>
#include <unistd.h>

namespace {
constexpr int kSlots = 4000;
void* g_slots[kSlots];

bool emit(const char* msg, int len) {
    return write(STDOUT_FILENO, msg, (size_t)len) == len;
}
}  // namespace

int main() {
    // Marks the start of the FastAlloc workload window (see script header).
    if (!emit("PROBE-START\n", 12)) {
        return 1;
    }
    int filled = 0;
    for (int i = 0; i < kSlots; ++i) {
        void* p = FastAlloc::fast_malloc(4096);
        if (p != nullptr) {
            std::memset(p, 1, 64);
            g_slots[filled++] = p;
        }
    }
    for (int i = 0; i < filled; ++i) {
        FastAlloc::fast_free(g_slots[i]);
    }
    FastAlloc::fast_alloc_purge_thread_cache();
    FastAlloc::fast_alloc_purge();
    void* big = FastAlloc::fast_malloc(4ull << 20);
    if (big != nullptr) {
        std::memset(big, 2, 4096);
        FastAlloc::fast_free(big);
    }
    FastAlloc::fast_alloc_purge();
    // Marks the end of the workload window; also the completion proof.
    if (!emit("PROBE-DONE\n", 11)) {
        return 1;
    }
    return filled == kSlots ? 0 : 2;
}
EOF

g++ -std=c++17 -O2 -Iinclude -Isrc /tmp/fastalloc_strace_probe.cpp \
    src/fast_alloc.cpp src/slab.cpp src/tls_cache.cpp src/global_heap.cpp \
    src/os_memory.cpp src/debug_aid.cpp -pthread -o /tmp/fastalloc_strace_probe

LOG=/tmp/fastalloc_strace.log
OUT=/tmp/fastalloc_strace.out
echo "=== strace: syscall shape of a map/churn/free/purge probe ==="
# `write` is traced alongside the memory syscalls so the PROBE-START /
# PROBE-DONE markers appear in the log and can bracket the window.
if ! strace -f -e trace=memory,write -o "$LOG" /tmp/fastalloc_strace_probe > "$OUT"; then
    echo "!! probe exited non-zero (incomplete workload)" >&2
    exit 1
fi
for m in PROBE-START PROBE-DONE; do
    grep -q "$m" "$OUT" || {
        echo "!! probe did not complete ($m missing from stdout)" >&2
        exit 1
    }
    grep -q "write(1, \"$m" "$LOG" || {
        echo "!! $m marker not traced; cannot scope the workload window" >&2
        exit 1
    }
done

# Workload window: syscall lines between the two marker write()s.
WINDOW=$(awk '/write\(1, "PROBE-START/{flag=1; next}
              /write\(1, "PROBE-DONE/{flag=0}
              flag' "$LOG")
if [ -z "$WINDOW" ]; then
    echo "!! empty workload window in trace" >&2
    exit 1
fi

counts() { printf '%s\n' "$1" | grep -c "$2" || true; }
echo "  workload-window syscall shape (between PROBE markers):"
echo "    mmap calls:    $(counts "$WINDOW" 'mmap(')"
echo "    munmap calls:  $(counts "$WINDOW" 'munmap(')"
echo "    madvise calls: $(counts "$WINDOW" 'madvise(')"
echo "    mremap calls:  $(counts "$WINDOW" 'mremap(')"
echo "    brk calls:     $(counts "$WINDOW" 'brk(')"
echo "  (outside the window the C++ runtime may legitimately use brk; the"
echo "   contract under test covers the allocator path only)"
grep -E "mremap.*EINVAL|ENOMEM" "$LOG" | head -5 || true

# Behavioural assertions, all scoped to the workload window.
MADV=$(counts "$WINDOW" 'madvise(')
MUNMAP=$(counts "$WINDOW" 'munmap(')
BRK=$(counts "$WINDOW" 'brk(')
MMAP=$(counts "$WINDOW" 'mmap(')
FAIL=0
[ "$BRK" -eq 0 ] || { echo "!! brk observed inside the FastAlloc workload - the allocator must be mmap-only" >&2; FAIL=1; }
[ "$MMAP" -gt 0 ] || { echo "!! no mmap observed inside the workload - allocations are not mmap-sourced" >&2; FAIL=1; }
# Pooled spans release via MADV_DONTNEED; direct spans via munmap. A
# purge-heavy probe must produce at least one release syscall of either kind.
TOTAL_RELEASE=$((MADV + MUNMAP))
[ "$TOTAL_RELEASE" -gt 0 ] || { echo "!! no munmap/madvise observed - purge never releases memory" >&2; FAIL=1; }
if [ "$FAIL" -ne 0 ]; then exit 1; fi
echo "syscall shape OK (mmap-only growth, purge releases to OS)"
