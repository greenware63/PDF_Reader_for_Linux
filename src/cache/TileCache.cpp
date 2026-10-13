#include "cache/TileCache.h"

namespace pdf {

TileCache::TileCache(size_t maxBytesBudget)
    : m_maxBytesBudget(maxBytesBudget), m_currentBytes(0) {}

bool TileCache::contains(const TileKey& key) const {
    return m_cacheMap.find(key) != m_cacheMap.end();
}

bool TileCache::isFailed(const TileKey& key) const {
    auto it = m_cacheMap.find(key);
    if (it == m_cacheMap.end()) {
        return false;
    }
    return it->second.entry.isFailed;
}

const PixelBuffer* TileCache::get(const TileKey& key) {
    auto it = m_cacheMap.find(key);
    if (it == m_cacheMap.end()) {
        return nullptr;
    }

    // Move to front of LRU list (Most Recently Used)
    m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);

    if (it->second.entry.isFailed) {
        return nullptr;
    }
    return &it->second.entry.buffer;
}

void TileCache::put(const TileKey& key, PixelBuffer buffer) {
    const size_t newBytes = buffer.isValid() ? (static_cast<size_t>(buffer.stride()) * buffer.height()) : 0;

    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        // Update existing entry
        m_currentBytes -= it->second.entry.bytesUsed;
        m_currentBytes += newBytes;

        it->second.entry.buffer = std::move(buffer);
        it->second.entry.bytesUsed = newBytes;
        it->second.entry.isFailed = false;

        // Move to front (MRU)
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);
    } else {
        // Insert new entry at front of LRU
        m_lruList.push_front(key);
        TileCacheEntry entry{std::move(buffer), newBytes, false};
        m_cacheMap.emplace(key, MapEntry{std::move(entry), m_lruList.begin()});
        m_currentBytes += newBytes;
    }

    evictIfNeeded();
}

void TileCache::markFailed(const TileKey& key) {
    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        m_currentBytes -= it->second.entry.bytesUsed;
        it->second.entry.buffer = PixelBuffer{};
        it->second.entry.bytesUsed = 0;
        it->second.entry.isFailed = true;

        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);
    } else {
        m_lruList.push_front(key);
        TileCacheEntry entry{PixelBuffer{}, 0, true};
        m_cacheMap.emplace(key, MapEntry{std::move(entry), m_lruList.begin()});
    }
}

void TileCache::pin(const TileKey& key) {
    m_pinnedSet.insert(key);
}

void TileCache::unpin(const TileKey& key) {
    m_pinnedSet.erase(key);
    evictIfNeeded();
}

void TileCache::setPinnedKeys(const std::vector<TileKey>& keys) {
    m_pinnedSet.clear();
    for (const auto& k : keys) {
        m_pinnedSet.insert(k);
    }
    evictIfNeeded();
}

void TileCache::clearPinned() {
    m_pinnedSet.clear();
    evictIfNeeded();
}

bool TileCache::isPinned(const TileKey& key) const {
    return m_pinnedSet.find(key) != m_pinnedSet.end();
}

void TileCache::setByteBudget(size_t maxBytes) {
    m_maxBytesBudget = maxBytes;
    evictIfNeeded();
}

void TileCache::clear() {
    m_cacheMap.clear();
    m_lruList.clear();
    m_pinnedSet.clear();
    m_currentBytes = 0;
}

void TileCache::evictIfNeeded() {
    auto it = m_lruList.end();
    while (m_currentBytes > m_maxBytesBudget && it != m_lruList.begin()) {
        --it;
        const TileKey& key = *it;
        if (m_pinnedSet.find(key) == m_pinnedSet.end()) {
            auto mapIt = m_cacheMap.find(key);
            if (mapIt != m_cacheMap.end()) {
                m_currentBytes -= mapIt->second.entry.bytesUsed;
                m_cacheMap.erase(mapIt);
            }
            it = m_lruList.erase(it);
        }
    }
}

} // namespace pdf
