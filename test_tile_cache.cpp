#include "cache/TileCache.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cmath>
#include <random>
#include <algorithm>
#include <unordered_set>
#include <list>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " RUNNING TILE CACHE UNIT TESTS (10 TESTS) " << std::endl;
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

        // Pin k1 (even though k1 is older than k2)
        cache.pin(k1);

        // Adding k3 forces eviction. Since k1 is pinned, k2 MUST be evicted!
        cache.put(k3, pdf::PixelBuffer::allocate(100, 100));

        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Pinned tile k1 was evicted!" << std::endl;
            return 1;
        }
        if (cache.contains(k2)) {
            std::cerr << "[FAIL] Unpinned tile k2 should have been evicted instead of pinned k1" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Pinned tile was successfully protected from eviction." << std::endl;
    }

    // Test 5: setPinnedKeys batch operation
    std::cout << "[TEST 5] Testing setPinnedKeys batch replacement..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
        auto k2 = pdf::TileKey::create(1, 0, 1, 0, 1.0, 1.0);
        auto k3 = pdf::TileKey::create(1, 0, 2, 0, 1.0, 1.0);

        cache.pin(k1);
        if (!cache.isPinned(k1)) {
            std::cerr << "[FAIL] k1 should be pinned" << std::endl;
            return 1;
        }

        // Replace entire pinned set with {k2, k3}
        cache.setPinnedKeys({k2, k3});

        if (cache.isPinned(k1)) {
            std::cerr << "[FAIL] k1 should no longer be pinned after setPinnedKeys" << std::endl;
            return 1;
        }
        if (!cache.isPinned(k2) || !cache.isPinned(k3)) {
            std::cerr << "[FAIL] k2 and k3 should be pinned" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: setPinnedKeys atomically replaced pin set." << std::endl;
    }

    // Test 6: Tombstones for failed tiles and weight accounting
    std::cout << "[TEST 6] Testing markFailed, tombstones, and bounded byte weights..." << std::endl;
    {
        pdf::TileCache cache(1024 * 1024);
        auto k1 = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);

        cache.markFailed(k1);
        if (!cache.contains(k1)) {
            std::cerr << "[FAIL] Tombstone tile should be contained in cache" << std::endl;
            return 1;
        }
        if (!cache.isFailed(k1)) {
            std::cerr << "[FAIL] Tile should be marked failed" << std::endl;
            return 1;
        }
        if (cache.get(k1).has_value()) {
            std::cerr << "[FAIL] get() on failed tile should return nullopt" << std::endl;
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

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 10 TILE CACHE TESTS PASSED!        " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
