#include "cache/TileCache.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <random>
#include <algorithm>
#include <unordered_set>
#include <list>
#include <thread>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " RUNNING TILE CACHE UNIT TESTS (13 TESTS) " << std::endl;
    std::cout << "========================================" << std::endl;

    // Test 1: Empty cache initialization & basic properties
    std::cout << "[TEST 1] Testing empty cache initialization..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024); // 1 MB
        if (cache.size() != 0) {
            std::cerr << "[FAIL] Cache should be empty initially" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 0) {
            std::cerr << "[FAIL] Initial bytesUsed should be 0" << std::endl;
            return 1;
        }
        if (cache.byteBudget() != 1024 * 1024) {
            std::cerr << "[FAIL] Incorrect initial byteBudget" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Initial properties verified." << std::endl;
    }

    // Test 2: Put and Get with PixelBuffer lifetime decoupling
    std::cout << "[TEST 2] Testing put/get and buffer preservation after eviction..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto key1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
        
        auto buf = pdf::PixelBuffer::allocate(100, 100);
        uint8_t* rawData = buf.data();
        rawData[0] = 0xAA;
        rawData[99] = 0xBB;

        bool kept = cache.put(key1, buf);
        if (!kept) {
            std::cerr << "[FAIL] Tile should be kept within budget" << std::endl;
            return 1;
        }

        auto retrieved = cache.get(key1);
        if (!retrieved.has_value()) {
            std::cerr << "[FAIL] get() failed to retrieve placed tile" << std::endl;
            return 1;
        }
        if (retrieved->data()[0] != 0xAA || retrieved->data()[99] != 0xBB) {
            std::cerr << "[FAIL] Pixel contents altered" << std::endl;
            return 1;
        }

        // Evict the tile from cache by dropping budget to 0
        cache.setByteBudget(0);
        if (cache.contains(key1)) {
            std::cerr << "[FAIL] Tile should be evicted" << std::endl;
            return 1;
        }

        // Even though cache evicted key1, retrieved buffer must remain valid and intact!
        if (retrieved->data()[0] != 0xAA || retrieved->data()[99] != 0xBB) {
            std::cerr << "[FAIL] Retrieved PixelBuffer memory invalidated on eviction" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: PixelBuffer remains completely valid after tile eviction." << std::endl;
    }

    // Test 3: Eviction order respects LRU
    std::cout << "[TEST 3] Testing LRU eviction order..." << std::endl;
    {
        // Allocate budget for exactly 2 tiles of 100x100 (40,000 bytes each)
        size_t tileSize = 100 * 100 * 4;
        pdf::TileCache cache(tileSize * 2 + 100);

        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
        auto k2 = pdf::TileKey::create(1, 0, 1, 0, 1.0, 1.0);
        auto k3 = pdf::TileKey::create(1, 0, 2, 0, 1.0, 1.0);

        cache.put(k1, pdf::PixelBuffer::allocate(100, 100));
        cache.put(k2, pdf::PixelBuffer::allocate(100, 100));

        // Touch k1 to make k2 the least recently used
        cache.get(k1);

        // Put k3 -> k2 should be evicted, k1 and k3 should stay
        cache.put(k3, pdf::PixelBuffer::allocate(100, 100));

        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] k1 was touched recently, should not have been evicted" << std::endl;
            return 1;
        }
        if (cache.contains(k2)) {
            std::cerr << "[FAIL] k2 was LRU, should have been evicted" << std::endl;
            return 1;
        }
        if (!cache.contains(k3)) {
            std::cerr << "[FAIL] k3 was just added, should be in cache" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: LRU touch and eviction order correctly maintained." << std::endl;
    }

    // Test 4: Pinning prevents eviction
    std::cout << "[TEST 4] Testing pinned tiles immunity to eviction..." << std::endl;
    {
        size_t tileSize = 100 * 100 * 4;
        pdf::TileCache cache(tileSize * 2 + 100);

        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
        auto k2 = pdf::TileKey::create(1, 0, 1, 0, 1.0, 1.0);
        auto k3 = pdf::TileKey::create(1, 0, 2, 0, 1.0, 1.0);

        cache.put(k1, pdf::PixelBuffer::allocate(100, 100));
        cache.put(k2, pdf::PixelBuffer::allocate(100, 100));

        // Pin k1
        cache.pin(k1);
        if (!cache.isPinned(k1)) {
            std::cerr << "[FAIL] k1 should be pinned" << std::endl;
            return 1;
        }

        // Put k3 -> k2 should be evicted because k1 is pinned
        cache.put(k3, pdf::PixelBuffer::allocate(100, 100));

        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Pinned tile k1 was evicted!" << std::endl;
            return 1;
        }
        if (cache.contains(k2)) {
            std::cerr << "[FAIL] Unpinned tile k2 should have been evicted" << std::endl;
            return 1;
        }
        if (!cache.contains(k3)) {
            std::cerr << "[FAIL] Newly added tile k3 should be in cache" << std::endl;
            return 1;
        }

        // Unpin k1 and insert k4 -> k1 should now be eligible for eviction
        cache.unpin(k1);
        auto k4 = pdf::TileKey::create(1, 0, 3, 0, 1.0, 1.0);
        cache.put(k4, pdf::PixelBuffer::allocate(100, 100));

        if (cache.contains(k1)) {
            std::cerr << "[FAIL] Unpinned tile k1 should have been evicted by k4" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Pin and unpin correctly guard tiles against eviction." << std::endl;
    }

    // Test 5: setPinnedKeys batch operation
    std::cout << "[TEST 5] Testing setPinnedKeys batch update..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
        auto k2 = pdf::TileKey::create(1, 0, 1, 0, 1.0, 1.0);
        auto k3 = pdf::TileKey::create(1, 0, 2, 0, 1.0, 1.0);

        cache.pin(k1);
        cache.setPinnedKeys({k2, k3});

        if (cache.isPinned(k1)) {
            std::cerr << "[FAIL] k1 should have been unpinned by setPinnedKeys" << std::endl;
            return 1;
        }
        if (!cache.isPinned(k2) || !cache.isPinned(k3)) {
            std::cerr << "[FAIL] k2 and k3 should be pinned by setPinnedKeys" << std::endl;
            return 1;
        }

        cache.clearPinned();
        if (cache.isPinned(k2) || cache.isPinned(k3)) {
            std::cerr << "[FAIL] clearPinned failed to unpin keys" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Batch pin and clearPinned verified." << std::endl;
    }

    // Test 6: Failure tombstones memory accounting and clearing
    std::cout << "[TEST 6] Testing failure tombstones and TOMBSTONE_BYTE_WEIGHT accounting..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);

        cache.markFailed(k1);

        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Cache should contain failed key" << std::endl;
            return 1;
        }
        if (!cache.isFailed(k1)) {
            std::cerr << "[FAIL] isFailed() should return true for failed key" << std::endl;
            return 1;
        }
        if (cache.get(k1).has_value()) {
            std::cerr << "[FAIL] get() on failed key should return nullopt" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != pdf::TileCache::TOMBSTONE_BYTE_WEIGHT) {
            std::cerr << "[FAIL] Tombstone must count for TOMBSTONE_BYTE_WEIGHT (" 
                      << pdf::TileCache::TOMBSTONE_BYTE_WEIGHT << " bytes), got " 
                      << cache.bytesUsed() << std::endl;
            return 1;
        }

        // Now put a valid buffer into the same key - should clear failed state
        cache.put(k1, pdf::PixelBuffer::allocate(10, 10));
        if (cache.isFailed(k1)) {
            std::cerr << "[FAIL] Valid put() should clear failed tombstone state" << std::endl;
            return 1;
        }
        if (!cache.get(k1).has_value()) {
            std::cerr << "[FAIL] Valid put() should allow retrieving pixel buffer" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Tombstones tracked, weighted, and cleared upon successful put." << std::endl;
    }

    // Test 7: put() with invalid buffer records failure and returns false
    std::cout << "[TEST 7] Testing put() with invalid buffer..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);

        // Put default-constructed invalid buffer
        pdf::PixelBuffer invalidBuf;
        bool kept = cache.put(k1, invalidBuf);
        if (kept) {
            std::cerr << "[FAIL] put() with invalid buffer should return false" << std::endl;
            return 1;
        }
        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Cache should contain failed key after invalid put" << std::endl;
            return 1;
        }
        if (!cache.isFailed(k1)) {
            std::cerr << "[FAIL] Invalid buffer put() must mark key as failed tombstone" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != pdf::TileCache::TOMBSTONE_BYTE_WEIGHT) {
            std::cerr << "[FAIL] Tombstone byte weight not counted on invalid buffer put" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Invalid buffer treated as markFailed and rejected." << std::endl;
    }

    // Test 8: Document invalidation cleans entries, bytes, and pins
    std::cout << "[TEST 8] Testing invalidateDocument and orphan pin cleanup..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto doc1_k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
        auto doc1_k2 = pdf::TileKey::create(1, 0, 1, 0, 1.0, 1.0);
        auto doc2_k1 = pdf::TileKey::create(2, 0, 0, 0, 1.0, 1.0);

        cache.put(doc1_k1, pdf::PixelBuffer::allocate(50, 50));
        cache.put(doc1_k2, pdf::PixelBuffer::allocate(50, 50));
        cache.put(doc2_k1, pdf::PixelBuffer::allocate(50, 50));

        // Pin doc1_k1 and an uncached doc1 key
        auto uncached_doc1 = pdf::TileKey::create(1, 9, 9, 9, 1.0, 1.0);
        cache.pin(doc1_k1);
        cache.pin(uncached_doc1);

        cache.invalidateDocument(1);

        if (cache.contains(doc1_k1) || cache.contains(doc1_k2)) {
            std::cerr << "[FAIL] Doc 1 tiles still present after invalidateDocument" << std::endl;
            return 1;
        }
        if (!cache.contains(doc2_k1)) {
            std::cerr << "[FAIL] Doc 2 tiles should not be removed" << std::endl;
            return 1;
        }
        if (cache.isPinned(doc1_k1) || cache.isPinned(uncached_doc1)) {
            std::cerr << "[FAIL] Pins for doc 1 should have been dropped" << std::endl;
            return 1;
        }
        if (cache.bytesUsed() != 50 * 50 * 4) {
            std::cerr << "[FAIL] bytesUsed inaccurate after document invalidation: " 
                      << cache.bytesUsed() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Document invalidation properly cleaned up tiles and orphan pins." << std::endl;
    }

    // Test 9: Quantized scale and DPR helpers
    std::cout << "[TEST 9] Testing quantizeScale and quantizeDpr..." << std::endl;
    {
        // Default desktop scale 96/72 = 1.333333333...
        double defaultScale = 96.0 / 72.0;
        double qScale = pdf::quantizeScale(defaultScale);
        if (qScale != 1.333) {
            std::cerr << "[FAIL] Expected 1.333 for default 100% zoom, got " << qScale << std::endl;
            return 1;
        }
        int scaleMilli = pdf::scaleToMilli(defaultScale);
        if (scaleMilli != 1333) {
            std::cerr << "[FAIL] Expected 1333 scaleMilli, got " << scaleMilli << std::endl;
            return 1;
        }

        // Out-of-bounds scales safely clamped
        if (pdf::quantizeScale(100.0) != 30.0 || pdf::quantizeScale(0.001) != 0.05) {
            std::cerr << "[FAIL] Scale clamping failed" << std::endl;
            return 1;
        }

        // Arbitrary pinch zoom scales
        double pinch = 2.4567;
        if (pdf::quantizeScale(pinch) != 2.457) {
            std::cerr << "[FAIL] Pinch zoom quantization failed: " << pdf::quantizeScale(pinch) << std::endl;
            return 1;
        }

        // Scale equality in TileKey
        auto kA = pdf::TileKey::create(1, 0, 0, 0, 1.3331, 1.5);
        auto kB = pdf::TileKey::create(1, 0, 0, 0, 1.3334, 1.5);
        if (!(kA == kB)) {
            std::cerr << "[FAIL] Near-identical scales should quantize to matching TileKey!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Arbitrary scale quantization and TileKey matching verified." << std::endl;
    }

    // Test 10: Randomized operations stress test with reference LRU model and strict invariants
    std::cout << "[TEST 10] Testing randomized operations compared against reference LRU model..." << std::endl;
    {
        struct ModelEntry {
            pdf::TileKey key;
            size_t bytesUsed{0};
            bool isFailed{false};
        };

        struct ReferenceLRU {
            size_t budget{100000};
            size_t bytesUsed{0};
            std::list<ModelEntry> lru; // front = MRU, back = LRU
            std::unordered_set<pdf::TileKey, pdf::TileKeyHash> pinned;

            void evict() {
                auto it = lru.end();
                while (bytesUsed > budget && it != lru.begin()) {
                    --it;
                    if (pinned.find(it->key) == pinned.end()) {
                        bytesUsed -= it->bytesUsed;
                        it = lru.erase(it);
                    }
                }
            }

            bool put(const pdf::TileKey& key, size_t bytes, bool isValid) {
                if (!isValid) {
                    markFailed(key);
                    return false;
                }
                auto it = std::find_if(lru.begin(), lru.end(), [&](const ModelEntry& e) { return e.key == key; });
                if (it != lru.end()) {
                    bytesUsed -= it->bytesUsed;
                    bytesUsed += bytes;
                    it->bytesUsed = bytes;
                    it->isFailed = false;
                    lru.splice(lru.begin(), lru, it);
                } else {
                    lru.push_front(ModelEntry{key, bytes, false});
                    bytesUsed += bytes;
                }
                evict();
                return contains(key);
            }

            void markFailed(const pdf::TileKey& key) {
                auto it = std::find_if(lru.begin(), lru.end(), [&](const ModelEntry& e) { return e.key == key; });
                if (it != lru.end()) {
                    bytesUsed -= it->bytesUsed;
                    bytesUsed += pdf::TileCache::TOMBSTONE_BYTE_WEIGHT;
                    it->bytesUsed = pdf::TileCache::TOMBSTONE_BYTE_WEIGHT;
                    it->isFailed = true;
                    lru.splice(lru.begin(), lru, it);
                } else {
                    lru.push_front(ModelEntry{key, pdf::TileCache::TOMBSTONE_BYTE_WEIGHT, true});
                    bytesUsed += pdf::TileCache::TOMBSTONE_BYTE_WEIGHT;
                }
                evict();
            }

            void pin(const pdf::TileKey& key) {
                pinned.insert(key);
            }

            void unpin(const pdf::TileKey& key) {
                pinned.erase(key);
                evict();
            }

            void setPinnedKeys(const std::vector<pdf::TileKey>& keys) {
                pinned.clear();
                for (const auto& k : keys) pinned.insert(k);
                evict();
            }

            void invalidateDocument(int64_t docId) {
                auto it = lru.begin();
                while (it != lru.end()) {
                    if (it->key.documentId == docId) {
                        bytesUsed -= it->bytesUsed;
                        it = lru.erase(it);
                    } else {
                        ++it;
                    }
                }
                for (auto pIt = pinned.begin(); pIt != pinned.end(); ) {
                    if (pIt->documentId == docId) pIt = pinned.erase(pIt);
                    else ++pIt;
                }
            }

            void setByteBudget(size_t newBudget) {
                budget = newBudget;
                evict();
            }

            bool contains(const pdf::TileKey& key) const {
                return std::any_of(lru.begin(), lru.end(), [&](const ModelEntry& e) { return e.key == key; });
            }

            bool isFailed(const pdf::TileKey& key) const {
                auto it = std::find_if(lru.begin(), lru.end(), [&](const ModelEntry& e) { return e.key == key; });
                return it != lru.end() && it->isFailed;
            }

            void touch(const pdf::TileKey& key) {
                auto it = std::find_if(lru.begin(), lru.end(), [&](const ModelEntry& e) { return e.key == key; });
                if (it != lru.end()) {
                    lru.splice(lru.begin(), lru, it);
                }
            }

            size_t size() const { return lru.size(); }
        };

        constexpr size_t INITIAL_BUDGET = 80000;
        pdf::TileCache cache(INITIAL_BUDGET);
        ReferenceLRU model;
        model.budget = INITIAL_BUDGET;

        std::mt19937 rng(42);
        std::vector<pdf::TileKey> allKeys;

        for (int op = 0; op < 600; ++op) {
            int action = rng() % 8;
            int docId = 1 + (rng() % 3);
            int page = rng() % 4;
            int col = rng() % 2;
            int row = rng() % 2;
            double scale = 1.0 + (rng() % 3) * 0.25;
            double dpr = 1.0 + (rng() % 2) * 0.5;
            auto key = pdf::TileKey::create(docId, page, col, row, scale, dpr);
            allKeys.push_back(key);

            if (action == 0 || action == 1) { // Put valid buffer
                int dim = 20 + (rng() % 30);
                size_t bytes = static_cast<size_t>(dim * 4) * dim;
                bool cKept = cache.put(key, pdf::PixelBuffer::allocate(dim, dim));
                bool mKept = model.put(key, bytes, true);
                if (cKept != mKept) {
                    std::cerr << "[FAIL] put return mismatch at op " << op << std::endl;
                    return 1;
                }
            } else if (action == 2) { // Put invalid buffer
                bool cKept = cache.put(key, pdf::PixelBuffer{});
                bool mKept = model.put(key, 0, false);
                if (cKept != mKept) {
                    std::cerr << "[FAIL] invalid put return mismatch at op " << op << std::endl;
                    return 1;
                }
            } else if (action == 3) { // Get
                auto cRes = cache.get(key);
                bool mContains = model.contains(key);
                bool mFailed = model.isFailed(key);
                if (mContains && !mFailed) {
                    if (!cRes.has_value()) {
                        std::cerr << "[FAIL] get() missed cached entry at op " << op << std::endl;
                        return 1;
                    }
                    model.touch(key);
                } else {
                    if (cRes.has_value()) {
                        std::cerr << "[FAIL] get() found unexpected entry at op " << op << std::endl;
                        return 1;
                    }
                }
            } else if (action == 4) { // Pin / Unpin
                if (cache.isPinned(key)) {
                    cache.unpin(key);
                    model.unpin(key);
                } else {
                    cache.pin(key);
                    model.pin(key);
                }
            } else if (action == 5) { // setPinnedKeys batch
                std::vector<pdf::TileKey> pinBatch;
                for (size_t i = 0; i < 5 && i < allKeys.size(); ++i) {
                    pinBatch.push_back(allKeys[rng() % allKeys.size()]);
                }
                cache.setPinnedKeys(pinBatch);
                model.setPinnedKeys(pinBatch);
            } else if (action == 6) { // Invalidate document
                cache.invalidateDocument(docId);
                model.invalidateDocument(docId);
            } else if (action == 7) { // Set budget variation
                size_t newBudget = 40000 + (rng() % 80000);
                cache.setByteBudget(newBudget);
                model.setByteBudget(newBudget);
            }

            // Verify model parity after EVERY operation
            if (cache.size() != model.size()) {
                std::cerr << "[FAIL] Cache size mismatch at op " << op 
                          << ": cache=" << cache.size() << ", model=" << model.size() << std::endl;
                return 1;
            }
            if (cache.bytesUsed() != model.bytesUsed) {
                std::cerr << "[FAIL] Cache bytesUsed mismatch at op " << op 
                          << ": cache=" << cache.bytesUsed() << ", model=" << model.bytesUsed << std::endl;
                return 1;
            }

            // Compare key-level properties against model
            for (const auto& k : allKeys) {
                if (cache.contains(k) != model.contains(k)) {
                    std::cerr << "[FAIL] contains mismatch at op " << op << std::endl;
                    return 1;
                }
                if (cache.isFailed(k) != model.isFailed(k)) {
                    std::cerr << "[FAIL] isFailed mismatch at op " << op << std::endl;
                    return 1;
                }
            }

            // Invariant: When cache is over budget, EVERY remaining entry in cache MUST be pinned!
            if (cache.bytesUsed() > cache.byteBudget()) {
                for (const auto& k : allKeys) {
                    if (cache.contains(k)) {
                        if (!cache.isPinned(k)) {
                            std::cerr << "[FAIL] Cache is over budget (" 
                                      << cache.bytesUsed() << " > " << cache.byteBudget() 
                                      << ") but contains unpinned entry!" << std::endl;
                            return 1;
                        }
                    }
                }
            }
        }
        std::cout << "  -> Passed: 600 operations fully matched ReferenceLRU model and strict all-pinned invariant." << std::endl;
    }

    // Test 11: Tile failure recovery, exponential backoff, and explicit clearing
    std::cout << "[TEST 11] Testing tile failure recovery, exponential backoff, and explicit clearing..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto key = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);

        // 1. Initial failure tombstone
        cache.markFailed(key);
        if (!cache.isFailed(key)) {
            std::cerr << "[FAIL] Key should be marked failed" << std::endl;
            return 1;
        }
        if (cache.get(key).has_value()) {
            std::cerr << "[FAIL] Failed tile must return nullopt on get()" << std::endl;
            return 1;
        }

        // 2. Exponential backoff check: immediately false, then true after sleep
        if (cache.canRetry(key, std::chrono::milliseconds(50))) {
            std::cerr << "[FAIL] canRetry should be false immediately after failure" << std::endl;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!cache.canRetry(key, std::chrono::milliseconds(50))) {
            std::cerr << "[FAIL] canRetry should be true after backoff interval elapses" << std::endl;
            return 1;
        }

        // 3. Overwriting failed tombstone with valid buffer clears failure and restores tile
        auto validBuf = pdf::PixelBuffer::allocate(50, 50);
        validBuf.data()[0] = 42;
        bool kept = cache.put(key, validBuf);
        if (!kept) {
            std::cerr << "[FAIL] put() of valid buffer should succeed on prior failure" << std::endl;
            return 1;
        }
        if (cache.isFailed(key)) {
            std::cerr << "[FAIL] Key should no longer be marked failed after valid put()" << std::endl;
            return 1;
        }
        auto retrieved = cache.get(key);
        if (!retrieved.has_value() || retrieved->data()[0] != 42) {
            std::cerr << "[FAIL] Retrieved buffer after recovery should match valid buffer" << std::endl;
            return 1;
        }

        // 4. Test explicit clearFailed
        auto key2 = pdf::TileKey::create(1, 1, 0, 0, 1.0, 1.0);
        cache.markFailed(key2);
        if (!cache.isFailed(key2)) {
            std::cerr << "[FAIL] key2 should be marked failed" << std::endl;
            return 1;
        }
        bool cleared = cache.clearFailed(key2);
        if (!cleared || cache.isFailed(key2) || cache.contains(key2)) {
            std::cerr << "[FAIL] clearFailed should completely clear failure tombstone" << std::endl;
            return 1;
        }

        // 5. Test clearAllFailed
        auto key3 = pdf::TileKey::create(2, 0, 0, 0, 1.0, 1.0);
        cache.markFailed(key3);
        if (!cache.isFailed(key3)) {
            std::cerr << "[FAIL] key3 should be marked failed" << std::endl;
            return 1;
        }
        cache.clearAllFailed();
        if (cache.isFailed(key3) || cache.contains(key3)) {
            std::cerr << "[FAIL] clearAllFailed should remove all failure tombstones" << std::endl;
            return 1;
        }
        // Valid tile key must still be present!
        if (!cache.contains(key) || !cache.get(key).has_value()) {
            std::cerr << "[FAIL] clearAllFailed must not remove valid tiles" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Transient failure recovery, exponential backoff, and explicit clearing verified." << std::endl;
    }

    // Test 12: Deterministic failure backoff intervals strictly grow across retries
    std::cout << "[TEST 12] Testing growing backoff intervals on deterministic failures..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto key = pdf::TileKey::create(42, 5, 0, 0, 1.0, 1.0);
        constexpr auto baseBackoff = std::chrono::milliseconds(40);

        // Failure 1: failCount = 1, delay = 40ms * 1 = 40ms
        cache.markFailed(key);
        if (cache.failCount(key) != 1) {
            std::cerr << "[FAIL] Expected failCount 1, got " << cache.failCount(key) << std::endl;
            return 1;
        }
        if (cache.canRetry(key, baseBackoff)) {
            std::cerr << "[FAIL] canRetry should be false immediately" << std::endl;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(45));
        if (!cache.canRetry(key, baseBackoff)) {
            std::cerr << "[FAIL] canRetry should be true after 45ms for failCount 1" << std::endl;
            return 1;
        }

        // Retry 1 occurs: clearFailed is called
        cache.clearFailed(key);
        if (cache.isFailed(key)) {
            std::cerr << "[FAIL] clearFailed should clear failed status" << std::endl;
            return 1;
        }
        // Deterministic failure occurs again: markFailed called
        cache.markFailed(key);
        if (cache.failCount(key) != 2) {
            std::cerr << "[FAIL] Expected failCount 2 after retry failed again, got " << cache.failCount(key) << std::endl;
            return 1;
        }
        // Failure 2 delay: baseBackoff * 2 = 80ms!
        // At 50ms, 50ms < 80ms -> must NOT be retryable yet!
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (cache.canRetry(key, baseBackoff)) {
            std::cerr << "[FAIL] Backoff was defeated! canRetry was true at 50ms, expected >= 80ms" << std::endl;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40)); // Total 90ms > 80ms
        if (!cache.canRetry(key, baseBackoff)) {
            std::cerr << "[FAIL] canRetry should be true after 90ms for failCount 2" << std::endl;
            return 1;
        }

        // Retry 2 occurs: clearFailed is called
        cache.clearFailed(key);
        // Fails a 3rd time
        cache.markFailed(key);
        if (cache.failCount(key) != 3) {
            std::cerr << "[FAIL] Expected failCount 3, got " << cache.failCount(key) << std::endl;
            return 1;
        }
        // Failure 3 delay: baseBackoff * 4 = 160ms!
        std::this_thread::sleep_for(std::chrono::milliseconds(90)); // 90ms < 160ms
        if (cache.canRetry(key, baseBackoff)) {
            std::cerr << "[FAIL] Backoff was defeated! canRetry was true at 90ms, expected >= 160ms" << std::endl;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80)); // Total 170ms > 160ms
        if (!cache.canRetry(key, baseBackoff)) {
            std::cerr << "[FAIL] canRetry should be true after 170ms for failCount 3" << std::endl;
            return 1;
        }

        // Successful put clears failure history
        auto validBuf = pdf::PixelBuffer::allocate(20, 20);
        cache.put(key, validBuf);
        if (cache.failCount(key) != 0) {
            std::cerr << "[FAIL] Successful put must reset failCount to 0" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Backoff intervals strictly grow (40ms -> 80ms -> 160ms) and reset on successful put." << std::endl;
    }

    // Test 13: tiles that fail MAX_RETRY_ATTEMPTS times become permanently failed until explicit reset
    std::cout << "[TEST 13] Testing permanent failure after MAX_RETRY_ATTEMPTS..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto key = pdf::TileKey::create(9, 0, 0, 0, 1.0, 1.0);
        const std::chrono::milliseconds zeroBackoff(0);

        for (int attempt = 1; attempt <= pdf::TileCache::MAX_RETRY_ATTEMPTS; ++attempt) {
            cache.markFailed(key);
            if (cache.failCount(key) != attempt) {
                std::cerr << "[FAIL] Expected failCount " << attempt << ", got " << cache.failCount(key) << std::endl;
                return 1;
            }
            const bool expectRetry = attempt < pdf::TileCache::MAX_RETRY_ATTEMPTS;
            if (cache.canRetry(key, zeroBackoff) != expectRetry) {
                std::cerr << "[FAIL] canRetry wrong at attempt " << attempt << std::endl;
                return 1;
            }
            if (expectRetry) {
                cache.clearFailed(key); // simulates the widget's retry
            }
        }
        if (!cache.isPermanentlyFailed(key) || !cache.isFailed(key)) {
            std::cerr << "[FAIL] Tile must be permanently failed after MAX_RETRY_ATTEMPTS" << std::endl;
            return 1;
        }

        // Explicit user retry lifts the permanent state
        cache.resetFailureHistory();
        if (cache.isPermanentlyFailed(key) || !cache.canRetry(key, zeroBackoff)) {
            std::cerr << "[FAIL] resetFailureHistory must make the tile retryable again" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Permanent failure after " << pdf::TileCache::MAX_RETRY_ATTEMPTS
                  << " attempts, lifted by resetFailureHistory()." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 13 TILE CACHE TESTS PASSED!        " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
