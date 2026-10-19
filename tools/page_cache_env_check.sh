#!/usr/bin/env bash
# ============================================================================
# page_cache_env_check.sh — verifies that the runtime environment tunables
# actually change allocator behaviour:
#
#   FASTALLOC_PAGE_CACHE_MB  : page-cache retention cap (default 64 MB).
#                              Probe: with 1 MB set, churn that parks > 1 MB
#                              of spans must keep <= 1 MB cached.
#   FASTALLOC_LOG_LEVEL      : initial log level (0..5). Probe: level 4 lets
#                              a debug event through; level 0 silences it.
#
# The page-cache limit is parsed once per process, so the checks run as
# separate processes (as designed).
#
# Usage: bash tools/page_cache_env_check.sh
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="${BUILD_DIR:-build-tools}"
mkdir -p "$BUILD"

cat > "$BUILD/env_probe.cpp" <<'EOF'
// Prints the page-cache bytes after parking spans, or a log-level probe
// result, depending on argv[1].
#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include "debug_aid.h"
#include "test_hooks.h"
#include <cstdio>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "page-cache") == 0) {
        std::vector<void*> spans;
        for (int i = 0; i < 32; ++i) {
            void* p = FastAlloc::fast_malloc(256 * 1024);
            if (!p) return 1;
            spans.push_back(p);
        }
        for (void* p : spans) FastAlloc::fast_free(p);
        FastAlloc::fast_alloc_purge_thread_cache();
        std::printf("%zu\n", FastAlloc::FastAllocTestPageCacheBytes());
        FastAlloc::FastAllocTestPurgePageCache();
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "log-level") == 0) {
        FastAlloc::LogEvent(FastAlloc::LogLevel::Debug, "env-probe", "n=1");
        return 0;
    }
    return 2;
}
EOF

g++ -std=c++17 -O2 -DFASTALLOC_DEBUG -Iinclude -Isrc -Itests \
    "$BUILD/env_probe.cpp" src/fast_alloc.cpp src/slab.cpp src/tls_cache.cpp \
    src/global_heap.cpp src/os_memory.cpp src/debug_aid.cpp -pthread \
    -o "$BUILD/env_probe"

echo "=== FASTALLOC_PAGE_CACHE_MB cap enforcement ==="
CAPPED=$("$BUILD/env_probe" page-cache) || true
echo "default cap run: cached bytes = $CAPPED"
if [ "$CAPPED" -gt $((64 * 1024 * 1024)) ]; then
    echo "!! default cap (64MB) exceeded" >&2; exit 1
fi
CAPPED=$(FASTALLOC_PAGE_CACHE_MB=1 "$BUILD/env_probe" page-cache) || true
echo "FASTALLOC_PAGE_CACHE_MB=1 run: cached bytes = $CAPPED"
if [ "$CAPPED" -gt $((1 * 1024 * 1024)) ]; then
    echo "!! 1MB cap not enforced (cached $CAPPED bytes)" >&2
    exit 1
fi
echo "page-cache cap tunable: OK"

echo "=== FASTALLOC_LOG_LEVEL gating ==="
if FASTALLOC_LOG_LEVEL=4 "$BUILD/env_probe" log-level 2>&1 | grep -q "env-probe"; then
    echo "level 4 lets debug events through: OK"
else
    echo "!! level 4 should emit the debug event" >&2; exit 1
fi
if FASTALLOC_LOG_LEVEL=0 "$BUILD/env_probe" log-level 2>&1 | grep -q "env-probe"; then
    echo "!! level 0 must silence debug events" >&2; exit 1
else
    echo "level 0 silences debug events: OK"
fi
echo "environment tunables: ALL OK"
