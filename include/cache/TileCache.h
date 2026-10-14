#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
#include <optional>
#include <vector>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include "engine/PixelBuffer.h"

namespace pdf {

/**
 * @brief Fixed byte weight accounted for remembered failure tombstones.
 * Prevents malicious or corrupt PDFs with massive page counts from exhausting memory with tombstones.
 */
inline constexpr size_t TOMBSTONE_BYTE_WEIGHT = 256;

/**
 * @brief Clamps and quantizes device pixel ratio to milli-units (e.g. 1.25 -> 1.25, 1.5001 -> 1.500).
 * Prevents IEEE 754 precision wobble and ensures layout, rasterizer, and cache share the exact same grid.
 * Clamped strictly to [0.25, 10.0] to prevent arithmetic overflow on absurd inputs.
 */
inline double quantizeDpr(double dpr) {
    if (std::isnan(dpr) || std::isinf(dpr) || dpr <= 0.0) {
        return 1.0;
    }
    const double clamped = std::clamp(dpr, 0.25, 10.0);
    const int milli = static_cast<int>(std::round(clamped * 1000.0));
    return static_cast<double>(milli) / 1000.0;
}

inline int dprToMilli(double dpr) {
    const double q = quantizeDpr(dpr);
    return static_cast<int>(std::round(q * 1000.0));
}

/**
 * @brief Clamps and quantizes display scale to milli-units (e.g. 1.3333 -> 1.333).
 * Prevents floating-point hash wobble while expressing arbitrary zoom levels:
 * default 100% (96/72 = 1.333), Fit-Width, Fit-Page, typed percentage, or pinch zoom.
 * Clamped strictly to [0.05, 30.0] to prevent arithmetic overflow on absurd inputs.
 */
inline double quantizeScale(double scale) {
    if (std::isnan(scale) || std::isinf(scale) || scale <= 0.0) {
        return 1.0;
    }
    const double clamped = std::clamp(scale, 0.05, 30.0);
    const int milli = static_cast<int>(std::round(clamped * 1000.0));
    return static_cast<double>(milli) / 1000.0;
}

inline int scaleToMilli(double scale) {
    const double q = quantizeScale(scale);
    return static_cast<int>(std::round(q * 1000.0));
}

/**
 * @brief Identifies a unique rendered tile across documents, pages, grids, quantized scale, and DPR.
 * Both scale and DPR are quantized to milli-units (1.333 -> 1333, 1.5 -> 1500) to ensure reliable hashing and comparison.
 */
struct TileKey {
    int64_t documentId{0};
    int pageIndex{0};
    int col{0};
    int row{0};
    int scaleMilli{1000}; // Quantized scale (e.g. 1.333x -> 1333)
    int dprMilli{1000};   // Quantized DPR (e.g. 1.5x -> 1500)

    static TileKey create(int64_t docId, int pageIdx, int c, int r, double scale, double dpr) {
        return TileKey{
            docId,
            pageIdx,
            c,
            r,
            scaleToMilli(scale),
            dprToMilli(dpr)
        };
    }

    bool operator==(const TileKey& other) const noexcept {
        return documentId == other.documentId &&
               pageIndex == other.pageIndex &&
               col == other.col &&
               row == other.row &&
               scaleMilli == other.scaleMilli &&
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
        combine(std::hash<int>{}(k.scaleMilli));
        combine(std::hash<int>{}(k.dprMilli));
        return h;
    };
};

struct TileCacheEntry {
    PixelBuffer buffer;
    size_t bytesUsed{0};
    bool isFailed{false};
    int failCount{0};
    std::chrono::steady_clock::time_point lastFailedTime{};
};

/**
 * @brief High-performance, Qt-free, PDFium-free in-memory tile cache.
 * Implements Least-Recently-Used (LRU) eviction constrained by an exact byte budget.
 * Supports pinning tiles currently visible in the viewport to guarantee they are immune to eviction.
 * Remembers rendering failures (tombstones) with bounded byte overhead so corrupt or unreadable pages
 * do not cause repeated re-render storms.
 * Provides deliberate retry and invalidation paths with exponential backoff for transient failure recovery.
 *
 * @note Thread Safety: UI Thread ONLY.
 * This class is not thread-safe and must be accessed exclusively from the main UI thread.
 * Background render tasks or worker threads must not access TileCache directly.
 */
class TileCache {
public:
    static constexpr size_t TOMBSTONE_BYTE_WEIGHT = pdf::TOMBSTONE_BYTE_WEIGHT;

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
     * @brief Checks if a failed tile is eligible for retry based on exponential backoff.
     * @param key The tile key.
     * @param baseBackoff Base delay (default 1000ms).
     * @return true if the tile has failed and the backoff interval has elapsed; false otherwise.
     */
    bool canRetry(const TileKey& key, std::chrono::milliseconds baseBackoff = std::chrono::milliseconds(1000)) const;

    /**
     * @brief Number of failures after which a tile is treated as permanently failed
     * (canRetry() returns false) until resetFailureHistory() is called.
     */
    static constexpr int MAX_RETRY_ATTEMPTS = 6;

    /**
     * @return true if the tile is currently a failure tombstone AND has failed MAX_RETRY_ATTEMPTS times or more.
     */
    bool isPermanentlyFailed(const TileKey& key) const;

    /**
     * @brief Forgets all recorded failure counts (explicit user retry). Does not remove tombstones;
     * call clearAllFailed() as well to make failed tiles eligible for rendering again.
     */
    void resetFailureHistory();

    /**
     * @brief Clears failure status on a specific tile to allow immediate retry.
     * Preserves failCount history across retries so exponential backoff grows on deterministic failure.
     * @return true if the tile was marked failed and was cleared; false otherwise.
     */
    bool clearFailed(const TileKey& key);

    /**
     * @brief Clears failure status on all failed tiles.
     */
    void clearAllFailed();

    /**
     * @brief Returns recorded failure count for a tile key (0 if never failed or successfully rendered).
     */
    int failCount(const TileKey& key) const;

    /**
     * @brief Retrieves a copy of the cached pixel buffer and promotes the tile to Most Recently Used.
     * Returning a value copy avoids lifetime hazards when subsequent cache operations evict the tile.
     * @return Optional containing PixelBuffer if found and not failed; std::nullopt otherwise.
     */
    std::optional<PixelBuffer> get(const TileKey& key);

    /**
     * @brief Inserts or updates a rendered tile in the cache, promoting it to Most Recently Used.
     * If the buffer is invalid, automatically calls markFailed(key) and returns false.
     * Automatically evicts unpinned tiles if the total memory exceeds the byte budget.
     * @note To prevent visible tiles from being evicted immediately when memory is tight,
     * callers should call pin() or setPinnedKeys() BEFORE put().
     * @return True if the tile is retained in cache; false if evicted immediately or invalid.
     */
    bool put(const TileKey& key, PixelBuffer buffer);

    /**
     * @brief Marks a tile as a remembered failure (tombstone) with TOMBSTONE_BYTE_WEIGHT overhead.
     * Records failure count and timestamp for exponential backoff.
     * Prevents UI components from repeatedly re-requesting unrenderable tiles.
     * Automatically evicts older unpinned tiles if over budget.
     */
    void markFailed(const TileKey& key);

    /**
     * @brief Pins a single tile, making it immune to LRU cache eviction.
     */
    void pin(const TileKey& key);

    /**
     * @brief Unpins a single tile. Triggers eviction if cache was temporarily over budget.
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
     * @brief Invalidates and evicts all tiles belonging to a specific document (e.g. on close).
     * Also drops any pins associated with documentId.
     */
    void invalidateDocument(int64_t documentId);

    /**
     * @brief Updates the maximum memory budget in bytes, evicting unpinned tiles if necessary.
     */
    void setByteBudget(size_t maxBytes);

    /**
     * @return Maximum byte budget.
     */
    size_t byteBudget() const noexcept { return m_maxBytesBudget; }

    /**
     * @return Current memory used by cached pixel buffers and tombstones in bytes.
     */
    size_t bytesUsed() const noexcept { return m_currentBytes; }

    /**
     * @return Number of entries (valid + failed) currently cached.
     */
    size_t size() const noexcept { return m_cacheMap.size(); }

    /**
     * @brief Clears all cached tiles, pinned sets, and failure history.
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
    std::unordered_map<TileKey, int, TileKeyHash> m_failHistory;
};

} // namespace pdf
