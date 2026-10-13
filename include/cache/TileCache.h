#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include "engine/PixelBuffer.h"

namespace pdf {

/**
 * @brief Identifies a unique rendered tile across documents, pages, grids, zoom steps, and DPR.
 * DPR is quantized to milli-units (1.5 -> 1500) to ensure reliable hashing and comparison.
 */
struct TileKey {
    int64_t documentId{0};
    int pageIndex{0};
    int col{0};
    int row{0};
    int zoomStep{0};
    int dprMilli{1000}; // Scaled by 1000 to eliminate floating-point hash jitter

    static TileKey create(int64_t docId, int pageIdx, int c, int r, int zStep, double dpr) {
        return TileKey{
            docId,
            pageIdx,
            c,
            r,
            zStep,
            static_cast<int>(std::round((dpr > 0.0 ? dpr : 1.0) * 1000.0))
        };
    }

    bool operator==(const TileKey& other) const noexcept {
        return documentId == other.documentId &&
               pageIndex == other.pageIndex &&
               col == other.col &&
               row == other.row &&
               zoomStep == other.zoomStep &&
               dprMilli == other.dprMilli;
    }
};

struct TileKeyHash {
    size_t operator()(const TileKey& k) const noexcept {
        size_t h = 0;
        auto combine = [&h](size_t v) {
            h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        combine(std::hash<int64_t>{}(k.documentId));
        combine(std::hash<int>{}(k.pageIndex));
        combine(std::hash<int>{}(k.col));
        combine(std::hash<int>{}(k.row));
        combine(std::hash<int>{}(k.zoomStep));
        combine(std::hash<int>{}(k.dprMilli));
        return h;
    }
};

struct TileCacheEntry {
    PixelBuffer buffer;
    size_t bytesUsed{0};
    bool isFailed{false};
};

/**
 * @brief High-performance, Qt-free, PDFium-free in-memory tile cache.
 * Implements Least-Recently-Used (LRU) eviction constrained by an exact byte budget.
 * Supports pinning tiles currently visible in the viewport to guarantee they are immune to eviction.
 * Remembers rendering failures (tombstones) so corrupt or unreadable pages do not cause repeated re-render storms.
 */
class TileCache {
public:
    explicit TileCache(size_t maxBytesBudget = 128 * 1024 * 1024); // 128 MB default budget
    ~TileCache() = default;

    TileCache(const TileCache&) = delete;
    TileCache& operator=(const TileCache&) = delete;
    TileCache(TileCache&&) noexcept = default;
    TileCache& operator=(TileCache&&) noexcept = default;

    /**
     * @brief Checks whether a tile (valid or tombstone) exists in the cache.
     * Does NOT update LRU access order.
     */
    bool contains(const TileKey& key) const;

    /**
     * @brief Checks if a tile is marked as a remembered rendering failure.
     * Does NOT update LRU access order.
     */
    bool isFailed(const TileKey& key) const;

    /**
     * @brief Retrieves a pointer to the cached pixel buffer and promotes the tile to Most Recently Used.
     * @return Pointer to PixelBuffer if found and not failed; nullptr otherwise.
     */
    const PixelBuffer* get(const TileKey& key);

    /**
     * @brief Inserts or updates a rendered tile in the cache, promoting it to Most Recently Used.
     * Automatically evicts unpinned tiles if the total memory exceeds the byte budget.
     */
    void put(const TileKey& key, PixelBuffer buffer);

    /**
     * @brief Marks a tile as a remembered failure (tombstone) with 0 byte weight.
     * Prevents UI components from repeatedly re-requesting unrenderable tiles.
     */
    void markFailed(const TileKey& key);

    /**
     * @brief Pins a single tile, making it immune to LRU cache eviction.
     */
    void pin(const TileKey& key);

    /**
     * @brief Unpins a single tile.
     */
    void unpin(const TileKey& key);

    /**
     * @brief Atomically updates the set of pinned tiles to exactly the provided keys.
     * Any previous pins not in the new set are released.
     */
    void setPinnedKeys(const std::vector<TileKey>& keys);

    /**
     * @brief Clears all pinned status. Triggers eviction if cache was temporarily over budget.
     */
    void clearPinned();

    /**
     * @return True if the tile is currently pinned in cache.
     */
    bool isPinned(const TileKey& key) const;

    /**
     * @brief Updates the maximum memory budget in bytes, evicting unpinned tiles if necessary.
     */
    void setByteBudget(size_t maxBytes);

    /**
     * @return Maximum byte budget.
     */
    size_t byteBudget() const noexcept { return m_maxBytesBudget; }

    /**
     * @return Current memory used by cached pixel buffers in bytes.
     */
    size_t bytesUsed() const noexcept { return m_currentBytes; }

    /**
     * @return Number of entries (valid + failed) currently cached.
     */
    size_t size() const noexcept { return m_cacheMap.size(); }

    /**
     * @brief Clears all cached tiles and pinned sets.
     */
    void clear();

private:
    void evictIfNeeded();

    struct MapEntry {
        TileCacheEntry entry;
        std::list<TileKey>::iterator lruIter;
    };

    size_t m_maxBytesBudget{128 * 1024 * 1024};
    size_t m_currentBytes{0};

    std::list<TileKey> m_lruList; // Front = MRU, Back = LRU
    std::unordered_map<TileKey, MapEntry, TileKeyHash> m_cacheMap;
    std::unordered_set<TileKey, TileKeyHash> m_pinnedSet;
};

} // namespace pdf
