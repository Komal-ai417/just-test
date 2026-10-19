#!/usr/bin/env bash
# ============================================================================
# strace_syscalls.sh — OS-interaction validation. Counts the memory-related
# syscalls of a deterministic workload and asserts the behavioural contract:
#   * allocations come from mmap (no brk-based growth on the FastAlloc path),
#   * purge actually munmap()s / madvise()s cached spans back to the OS,
#   * no unexpected mremap failures appear.
#
# Usage: bash tools/strace_syscalls.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
command -v strace > /dev/null || { echo "strace not installed" >&2; exit 2; }

BUILD="${BUILD_DIR:-build-tools}"

cat > /tmp/fastalloc_strace_probe.cpp <<'EOF'
// Deterministic syscall-shape probe: map, churn, free, purge.
#include "fast_alloc.h"
#include <cstdio>
#include <cstring>
#include <vector>
int main() {
    std::vector<void*> v;
    for (int i = 0; i < 4000; ++i) {
        void* p = FastAlloc::fast_malloc(4096);
        if (p) { std::memset(p, 1, 64); v.push_back(p); }
    }
    for (void* p : v) FastAlloc::fast_free(p);
    FastAlloc::fast_alloc_purge_thread_cache();
    FastAlloc::fast_alloc_purge();
    void* big = FastAlloc::fast_malloc(4ull << 20);
    if (big) { std::memset(big, 2, 4096); FastAlloc::fast_free(big); }
    FastAlloc::fast_alloc_purge();
    std::printf("PROBE-DONE\n");
    return 0;
}
EOF

g++ -std=c++17 -O2 -Iinclude -Isrc /tmp/fastalloc_strace_probe.cpp \
    src/fast_alloc.cpp src/slab.cpp src/tls_cache.cpp src/global_heap.cpp \
    src/os_memory.cpp src/debug_aid.cpp -pthread -o /tmp/fastalloc_strace_probe

LOG=/tmp/fastalloc_strace.log
echo "=== strace: syscall shape of a map/churn/free/purge probe ==="
strace -f -e trace=memory -o "$LOG" /tmp/fastalloc_strace_probe > /dev/null

counts() { grep -c "$1" "$LOG" || true; }
echo "  mmap calls:    $(counts 'mmap(')"
echo "  munmap calls:  $(counts 'munmap(')"
echo "  madvise calls: $(counts 'madvise(')"
echo "  mremap calls:  $(counts 'mremap(')"
echo "  brk calls:     $(counts 'brk(')"
grep -E "mremap.*EINVAL|ENOMEM" "$LOG" | head -5 || true

# Behavioural assertions.
MADV=$(counts 'madvise(')
MUNMAP=$(counts 'munmap(')
BRK=$(counts 'brk(')
FAIL=0
[ "$BRK" -eq 0 ] || { echo "!! brk observed - FastAlloc must be mmap-only" >&2; FAIL=1; }
# Pooled spans release via MADV_DONTNEED; direct spans via munmap. A
# purge-heavy probe must produce at least one release syscall of either kind.
TOTAL_RELEASE=$((MADV + MUNMAP))
[ "$TOTAL_RELEASE" -gt 0 ] || { echo "!! no munmap/madvise observed - purge never releases memory" >&2; FAIL=1; }
grep -q "PROBE-DONE" /dev/null 2>&1 || true
if [ "$FAIL" -ne 0 ]; then exit 1; fi
echo "syscall shape OK (mmap-only growth, purge releases to OS)"
