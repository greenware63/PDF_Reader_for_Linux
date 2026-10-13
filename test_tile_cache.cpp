#include <iostream>
#include <cassert>
#include <vector>
#include <random>
#include "cache/TileCache.h"

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running TileCache Unit Tests           " << std::endl;
    std::cout << "========================================" << std::endl;

    // Test 1: Basic insertion, retrieval, and presence check
    std::cout << "[TEST 1] Testing basic insertion and retrieval..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024); // 1 MB
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);

        if (cache.contains(k1)) {
            std::cerr << "[FAIL] Cache should not contain k1 yet" << std::endl;
            return 1;
        }

        pdf::PixelBuffer buf = pdf::PixelBuffer::allocate(100, 100);
        bool kept = cache.put(k1, std::move(buf));
        if (!kept) {
            std::cerr << "[FAIL] put() should have returned true for within-budget tile" << std::endl;
            return 1;
        }

        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Cache should contain k1 after put" << std::endl;
            return 1;
        }
        if (cache.size() != 1) {
            std::cerr << "[FAIL] Expected size 1, got " << cache.size() << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 100 * 100 * 4) {
            std::cerr << "[FAIL] Expected 40,000 bytes, got " << cache.bytesUsed() << std::endl;
            return 1;
        }

        std::optional<pdf::PixelBuffer> retrieved = cache.get(k1);
        if (!retrieved || !retrieved->isValid() || retrieved->width() != 100 || retrieved->height() != 100) {
            std::cerr << "[FAIL] Failed to retrieve valid buffer" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Basic put/get/contains functions verified." << std::endl;
    }

    // Test 2: Lifetime safety - retrieved buffer remains valid after eviction
    std::cout << "[TEST 2] Testing retrieved buffer lifetime safety after eviction..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);

        pdf::PixelBuffer buf = pdf::PixelBuffer::allocate(50, 50);
        // Fill with pattern
        uint32_t* raw = reinterpret_cast<uint32_t*>(buf.data());
        raw[0] = 0xDEADBEEF;
        raw[2499] = 0xCAFEBABE;
        cache.put(k1, std::move(buf));

        // Get value copy
        std::optional<pdf::PixelBuffer> heldCopy = cache.get(k1);
        if (!heldCopy || !heldCopy->isValid()) {
            std::cerr << "[FAIL] Failed to get valid buffer copy" << std::endl;
            return 1;
        }

        // Completely evict k1 by reducing budget to 0
        cache.setByteBudget(0);
        if (cache.contains(k1) || cache.size() != 0) {
            std::cerr << "[FAIL] Cache should be empty after budget reduced to 0" << std::endl;
            return 1;
        }

        // Access the held buffer: must NOT be dangling or use-after-free
        const uint32_t* checkRaw = reinterpret_cast<const uint32_t*>(heldCopy->data());
        if (checkRaw[0] != 0xDEADBEEF || checkRaw[2499] != 0xCAFEBABE) {
            std::cerr << "[FAIL] Buffer memory was corrupted after cache eviction!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Retrieved buffer remained completely valid and readable after cache eviction." << std::endl;
    }

    // Test 3: LRU access ordering
    std::cout << "[TEST 3] Testing LRU access ordering..." << std::endl;
    {
        // Each 100x100 tile is 40,000 bytes. Budget of 100,000 bytes holds exactly 2 tiles.
        pdf::TileCache cache(100000);

        auto kA = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);
        auto kB = pdf::TileKey::create(1, 0, 1, 0, 0, 1.0);
        auto kC = pdf::TileKey::create(1, 0, 2, 0, 0, 1.0);

        cache.put(kA, pdf::PixelBuffer::allocate(100, 100)); // 40KB
        cache.put(kB, pdf::PixelBuffer::allocate(100, 100)); // 80KB

        // Access kA to promote it to MRU (order now: kA, kB)
        auto pA = cache.get(kA);
        if (!pA) {
            std::cerr << "[FAIL] Failed to get kA" << std::endl;
            return 1;
        }

        // Inserting kC (40KB -> total 120KB > 100KB) must evict kB (the oldest), leaving kA and kC
        cache.put(kC, pdf::PixelBuffer::allocate(100, 100));

        if (!cache.contains(kA)) {
            std::cerr << "[FAIL] kA was accessed and should NOT have been evicted!" << std::endl;
            return 1;
        }
        if (cache.contains(kB)) {
            std::cerr << "[FAIL] kB was the least recently used and should have been evicted!" << std::endl;
            return 1;
        }
        if (!cache.contains(kC)) {
            std::cerr << "[FAIL] kC was just inserted and should be present!" << std::endl;
            return 1;
        }
        if (cache.size() != 2 || cache.bytesUsed() != 80000) {
            std::cerr << "[FAIL] Expected 2 items and 80,000 bytes, got " 
                      << cache.size() << " items, " << cache.bytesUsed() << " bytes" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: LRU promotion and eviction order verified." << std::endl;
    }

    // Test 4: Key replacement preserves exact byte count accounting
    std::cout << "[TEST 4] Testing key replacement byte count accounting..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);

        // Put 100x100 tile (40,000 bytes)
        cache.put(k, pdf::PixelBuffer::allocate(100, 100));
        if (cache.bytesUsed() != 40000) {
            std::cerr << "[FAIL] Expected 40,000 bytes, got " << cache.bytesUsed() << std::endl;
            return 1;
        }

        // Replace with 50x50 tile (10,000 bytes)
        cache.put(k, pdf::PixelBuffer::allocate(50, 50));
        if (cache.bytesUsed() != 10000 || cache.size() != 1) {
            std::cerr << "[FAIL] Replacement did not update byte count correctly: " 
                      << cache.bytesUsed() << " bytes, size=" << cache.size() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Key replacement accurately recalculated byte usage." << std::endl;
    }

    // Test 5: Pinned tile immunity during eviction
    std::cout << "[TEST 5] Testing pinned tile immunity..." << std::endl;
    {
        // Budget holds 2 tiles (100KB)
        pdf::TileCache cache(100000);

        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);
        auto k2 = pdf::TileKey::create(1, 0, 1, 0, 0, 1.0);
        auto k3 = pdf::TileKey::create(1, 0, 2, 0, 0, 1.0);

        cache.put(k1, pdf::PixelBuffer::allocate(100, 100)); // 40KB
        cache.put(k2, pdf::PixelBuffer::allocate(100, 100)); // 80KB

        // k1 is the oldest. But we pin k1!
        cache.pin(k1);
        if (!cache.isPinned(k1)) {
            std::cerr << "[FAIL] k1 should be marked pinned" << std::endl;
            return 1;
        }

        // Put k3 (40KB -> 120KB > 100KB budget).
        // Since k1 is pinned, k2 (even though newer than k1) must be evicted!
        cache.put(k3, pdf::PixelBuffer::allocate(100, 100));

        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Pinned tile k1 was evicted!" << std::endl;
            return 1;
        }
        if (cache.contains(k2)) {
            std::cerr << "[FAIL] Unpinned tile k2 should have been evicted!" << std::endl;
            return 1;
        }
        if (!cache.contains(k3)) {
            std::cerr << "[FAIL] Newly inserted tile k3 should be present!" << std::endl;
            return 1;
        }

        // Test budget reduced to 0: pinned tile k1 still survives!
        cache.setByteBudget(0);
        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Pinned tile k1 should survive even when budget is 0!" << std::endl;
            return 1;
        }
        if (cache.contains(k3)) {
            std::cerr << "[FAIL] Unpinned tile k3 should have been evicted under 0 budget!" << std::endl;
            return 1;
        }

        // Now unpin k1: it should immediately be evicted under 0 budget
        cache.unpin(k1);
        if (cache.contains(k1) || cache.size() != 0 || cache.bytesUsed() != 0) {
            std::cerr << "[FAIL] Unpinned tile should have been evicted when over budget!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Pinned tile retention and unpin-eviction verified." << std::endl;
    }

    // Test 6: Oversized tile on unpinned put returns false and is not retained
    std::cout << "[TEST 6] Testing oversized tile handling..." << std::endl;
    {
        pdf::TileCache cache(50000); // 50KB budget
        auto bigKey = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);

        // Try putting 200x200 tile (160,000 bytes > 50,000 budget)
        bool kept = cache.put(bigKey, pdf::PixelBuffer::allocate(200, 200));
        if (kept || cache.contains(bigKey)) {
            std::cerr << "[FAIL] Unpinned oversized tile should have been evicted immediately!" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 0 || cache.size() != 0) {
            std::cerr << "[FAIL] Oversized tile eviction leaked bytes: " << cache.bytesUsed() << std::endl;
            return 1;
        }

        // If pinned BEFORE put, the oversized tile IS retained
        cache.pin(bigKey);
        kept = cache.put(bigKey, pdf::PixelBuffer::allocate(200, 200));
        if (!kept || !cache.contains(bigKey)) {
            std::cerr << "[FAIL] Pinned oversized tile should be retained!" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 160000) {
            std::cerr << "[FAIL] Expected 160,000 bytes, got " << cache.bytesUsed() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Oversized tile put() return value and pin-first retention verified." << std::endl;
    }

    // Test 7: Remembered rendering failures (tombstones with byte weight and bounded eviction)
    std::cout << "[TEST 7] Testing bounded tombstone budget and eviction..." << std::endl;
    {
        // Budget of 1000 bytes holds exactly 3 tombstones (3 * 256 = 768 <= 1000)
        pdf::TileCache cache(1000);

        for (int i = 0; i < 5; ++i) {
            cache.markFailed(pdf::TileKey::create(1, i, 0, 0, 0, 1.0));
        }

        if (cache.size() != 3) {
            std::cerr << "[FAIL] Tombstones did not evict under budget! Expected 3, got " << cache.size() << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 3 * pdf::TOMBSTONE_BYTE_WEIGHT) {
            std::cerr << "[FAIL] Expected " << (3 * pdf::TOMBSTONE_BYTE_WEIGHT) 
                      << " bytes for tombstones, got " << cache.bytesUsed() << std::endl;
            return 1;
        }

        // Verify oldest tombstones 0 and 1 were evicted, while 2, 3, 4 remain
        if (cache.contains(pdf::TileKey::create(1, 0, 0, 0, 0, 1.0)) ||
            cache.contains(pdf::TileKey::create(1, 1, 0, 0, 0, 1.0))) {
            std::cerr << "[FAIL] Oldest tombstones were not evicted!" << std::endl;
            return 1;
        }
        auto k4 = pdf::TileKey::create(1, 4, 0, 0, 0, 1.0);
        if (!cache.contains(k4) || !cache.isFailed(k4) || cache.get(k4).has_value()) {
            std::cerr << "[FAIL] Tombstone query contract failed" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Bounded tombstones and LRU failure eviction verified." << std::endl;
    }

    // Test 8: Document invalidation
    std::cout << "[TEST 8] Testing invalidateDocument..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        cache.put(pdf::TileKey::create(101, 0, 0, 0, 0, 1.0), pdf::PixelBuffer::allocate(50, 50)); // Doc 101
        cache.put(pdf::TileKey::create(101, 1, 0, 0, 0, 1.0), pdf::PixelBuffer::allocate(50, 50)); // Doc 101
        cache.put(pdf::TileKey::create(202, 0, 0, 0, 0, 1.0), pdf::PixelBuffer::allocate(50, 50)); // Doc 202

        if (cache.size() != 3 || cache.bytesUsed() != 30000) {
            std::cerr << "[FAIL] Setup for invalidateDocument failed" << std::endl;
            return 1;
        }

        cache.invalidateDocument(101);

        if (cache.size() != 1 || cache.bytesUsed() != 10000) {
            std::cerr << "[FAIL] invalidateDocument(101) did not reduce size/bytes correctly: " 
                      << cache.size() << " items, " << cache.bytesUsed() << " bytes" << std::endl;
            return 1;
        }
        if (!cache.contains(pdf::TileKey::create(202, 0, 0, 0, 0, 1.0))) {
            std::cerr << "[FAIL] Other documents were improperly evicted during invalidateDocument!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Document invalidation accurately cleaned up targeted document." << std::endl;
    }

    // Test 9: DPR quantization and zoom mapping functions
    std::cout << "[TEST 9] Testing quantizeDpr, zoomStepToScale, and clamped DPR..." << std::endl;
    {
        // 1.5001 and 1.5002 should quantize to exactly 1.500
        double q1 = pdf::quantizeDpr(1.5001);
        double q2 = pdf::quantizeDpr(1.5002);
        if (q1 != 1.5 || q2 != 1.5) {
            std::cerr << "[FAIL] quantizeDpr failed: q1=" << q1 << ", q2=" << q2 << std::endl;
            return 1;
        }

        // Out-of-bounds / absurd DPR values clamped safely
        if (pdf::quantizeDpr(-5.0) != 1.0 || pdf::quantizeDpr(0.0) != 1.0) {
            std::cerr << "[FAIL] Non-positive DPR should fallback to 1.0" << std::endl;
            return 1;
        }
        if (pdf::quantizeDpr(100.0) != 10.0 || pdf::quantizeDpr(0.05) != 0.25) {
            std::cerr << "[FAIL] DPR clamping out of bounds" << std::endl;
            return 1;
        }

        // Canonical zoom steps
        if (std::abs(pdf::zoomStepToScale(0) - 1.0) > 1e-6) {
            std::cerr << "[FAIL] Zoom step 0 must be 1.0" << std::endl;
            return 1;
        }
        if (std::abs(pdf::zoomStepToScale(1) - 1.2) > 1e-6) {
            std::cerr << "[FAIL] Zoom step 1 must be 1.2" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: DPR quantization and canonical zoom mappings verified." << std::endl;
    }

    // Test 10: Randomized operations stress test with strict byte sum invariant
    std::cout << "[TEST 10] Testing randomized operations with strict byte sum invariant..." << std::endl;
    {
        constexpr size_t BUDGET = 150000;
        pdf::TileCache cache(BUDGET);
        std::mt19937 rng(1337);

        for (int op = 0; op < 500; ++op) {
            int action = rng() % 5;
            int page = rng() % 10;
            int col = rng() % 4;
            int row = rng() % 4;
            auto key = pdf::TileKey::create(1, page, col, row, 0, 1.0);

            if (action == 0 || action == 1) { // Put
                int dim = 20 + (rng() % 50);
                cache.put(key, pdf::PixelBuffer::allocate(dim, dim));
            } else if (action == 2) { // Get
                cache.get(key);
            } else if (action == 3) { // Pin / Unpin
                if (cache.isPinned(key)) cache.unpin(key);
                else cache.pin(key);
            } else if (action == 4) { // Mark failed
                cache.markFailed(key);
            }

            // Invariant 1: If not pinned, bytesUsed must not exceed budget
            if (cache.bytesUsed() > BUDGET) {
                // Must have at least one pinned item accounting for the excess
                bool hasPinned = false;
                for (int p = 0; p < 10; ++p) {
                    for (int c = 0; c < 4; ++c) {
                        for (int r = 0; r < 4; ++r) {
                            if (cache.isPinned(pdf::TileKey::create(1, p, c, r, 0, 1.0))) {
                                hasPinned = true;
                                break;
                            }
                        }
                    }
                }
                if (!hasPinned) {
                    std::cerr << "[FAIL] Cache exceeded budget without any pinned items: " 
                              << cache.bytesUsed() << " > " << BUDGET << std::endl;
                    return 1;
                }
            }
        }
        std::cout << "  -> Passed: 500 randomized operations completed with zero invariant violations." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 10 TILE CACHE TESTS PASSED!        " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
