#include <iostream>
#include <cassert>
#include <vector>
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
        cache.put(k1, std::move(buf));

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

        const pdf::PixelBuffer* retrieved = cache.get(k1);
        if (!retrieved || !retrieved->isValid() || retrieved->width() != 100 || retrieved->height() != 100) {
            std::cerr << "[FAIL] Failed to retrieve valid buffer" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Basic put/get/contains functions verified." << std::endl;
    }

    // Test 2: LRU access ordering
    std::cout << "[TEST 2] Testing LRU access ordering..." << std::endl;
    {
        // Each 100x100 tile is 40,000 bytes. Budget of 100,000 bytes holds exactly 2 tiles.
        pdf::TileCache cache(100000);

        auto kA = pdf::TileKey::create(1, 0, 0, 0, 0, 1.0);
        auto kB = pdf::TileKey::create(1, 0, 1, 0, 0, 1.0);
        auto kC = pdf::TileKey::create(1, 0, 2, 0, 0, 1.0);

        cache.put(kA, pdf::PixelBuffer::allocate(100, 100)); // 40KB
        cache.put(kB, pdf::PixelBuffer::allocate(100, 100)); // 80KB

        // Access kA to promote it to MRU (order now: kA, kB)
        const pdf::PixelBuffer* pA = cache.get(kA);
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

    // Test 3: Pinned tile immunity during eviction
    std::cout << "[TEST 3] Testing pinned tile immunity..." << std::endl;
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

    // Test 4: Atomically update pinned visible set
    std::cout << "[TEST 4] Testing setPinnedKeys batch pinning..." << std::endl;
    {
        pdf::TileCache cache(200000); // 200KB holds 5 tiles (40KB each)
        std::vector<pdf::TileKey> keys;
        for (int i = 0; i < 5; ++i) {
            keys.push_back(pdf::TileKey::create(1, 0, i, 0, 0, 1.0));
            cache.put(keys[i], pdf::PixelBuffer::allocate(100, 100));
        }

        // Pin keys 0 and 1 as visible
        cache.setPinnedKeys({keys[0], keys[1]});
        if (!cache.isPinned(keys[0]) || !cache.isPinned(keys[1]) || cache.isPinned(keys[2])) {
            std::cerr << "[FAIL] Pin status mismatch after setPinnedKeys" << std::endl;
            return 1;
        }

        // Now switch visible set to keys 2 and 3
        cache.setPinnedKeys({keys[2], keys[3]});
        if (cache.isPinned(keys[0]) || cache.isPinned(keys[1]) || !cache.isPinned(keys[2]) || !cache.isPinned(keys[3])) {
            std::cerr << "[FAIL] Pin status not properly updated on setPinnedKeys transition" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Batch pinned keys transition verified." << std::endl;
    }

    // Test 5: Remembered rendering failures (tombstones)
    std::cout << "[TEST 5] Testing remembered failures (tombstones)..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto badKey = pdf::TileKey::create(42, 99, 0, 0, 0, 1.0);

        cache.markFailed(badKey);

        if (!cache.contains(badKey)) {
            std::cerr << "[FAIL] Failed key should be present in cache as tombstone" << std::endl;
            return 1;
        }
        if (!cache.isFailed(badKey)) {
            std::cerr << "[FAIL] isFailed should return true for marked key" << std::endl;
            return 1;
        }
        if (cache.get(badKey) != nullptr) {
            std::cerr << "[FAIL] get() should return nullptr for failed key" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 0) {
            std::cerr << "[FAIL] Failed tombstone should consume 0 byte budget" << std::endl;
            return 1;
        }

        // If later successfully put, isFailed should reset to false
        cache.put(badKey, pdf::PixelBuffer::allocate(50, 50));
        if (cache.isFailed(badKey)) {
            std::cerr << "[FAIL] isFailed should be cleared after successful put" << std::endl;
            return 1;
        }
        if (cache.get(badKey) == nullptr) {
            std::cerr << "[FAIL] get() should return valid buffer after recovery" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Tombstones and recovery behavior verified." << std::endl;
    }

    // Test 6: Dynamic budget reduction
    std::cout << "[TEST 6] Testing dynamic budget reduction..." << std::endl;
    {
        pdf::TileCache cache(400000); // 400KB holds 10 tiles (40KB each)
        for (int i = 0; i < 8; ++i) {
            cache.put(pdf::TileKey::create(1, 0, i, 0, 0, 1.0), pdf::PixelBuffer::allocate(100, 100));
        }
        if (cache.size() != 8 || cache.bytesUsed() != 320000) {
            std::cerr << "[FAIL] Initial multi-tile allocation size incorrect" << std::endl;
            return 1;
        }

        // Dynamically shrink budget to 120KB (holds 3 tiles)
        cache.setByteBudget(120000);

        if (cache.size() != 3 || cache.bytesUsed() != 120000) {
            std::cerr << "[FAIL] Budget reduction did not trim to 3 tiles, size: " 
                      << cache.size() << ", bytes: " << cache.bytesUsed() << std::endl;
            return 1;
        }
        // Tiles 5, 6, 7 (the newest) should remain
        if (!cache.contains(pdf::TileKey::create(1, 0, 7, 0, 0, 1.0)) ||
            !cache.contains(pdf::TileKey::create(1, 0, 6, 0, 0, 1.0)) ||
            !cache.contains(pdf::TileKey::create(1, 0, 5, 0, 0, 1.0))) {
            std::cerr << "[FAIL] Most recent tiles were unexpectedly evicted during shrink" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Dynamic budget reduction trimmed older entries accurately." << std::endl;
    }

    // Test 7: DPR quantization and key discrimination
    std::cout << "[TEST 7] Testing DPR quantization and key differentiation..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);

        // DPR 1.5001 and 1.5002 both round to 1500 milli-DPR
        auto kA = pdf::TileKey::create(1, 0, 0, 0, 2, 1.5001);
        auto kB = pdf::TileKey::create(1, 0, 0, 0, 2, 1.5002);

        cache.put(kA, pdf::PixelBuffer::allocate(10, 10));

        if (!cache.contains(kB)) {
            std::cerr << "[FAIL] Float DPR jitter should quantize to matching cache key!" << std::endl;
            return 1;
        }

        // Different zoomStep should NOT match
        auto kDiffZoom = pdf::TileKey::create(1, 0, 0, 0, 3, 1.5001);
        if (cache.contains(kDiffZoom)) {
            std::cerr << "[FAIL] Different zoom step collided!" << std::endl;
            return 1;
        }

        // Different pageIndex should NOT match
        auto kDiffPage = pdf::TileKey::create(1, 1, 0, 0, 2, 1.5001);
        if (cache.contains(kDiffPage)) {
            std::cerr << "[FAIL] Different page index collided!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: DPR milli-quantization and key differentiation verified." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 7 TILE CACHE TESTS PASSED!         " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
