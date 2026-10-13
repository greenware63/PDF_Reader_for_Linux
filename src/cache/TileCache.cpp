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

std::optional<PixelBuffer> TileCache::get(const TileKey& key) {
    auto it = m_cacheMap.find(key);
    if (it == m_cacheMap.end()) {
        return std::nullopt;
    }

    // Move to front of LRU list (Most Recently Used)
    m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);

    if (it->second.entry.isFailed) {
        return std::nullopt;
    }
    // Return a copy of PixelBuffer (shallow ref-counted copy via shared_ptr owner)
    return it->second.entry.buffer;
}

bool TileCache::put(const TileKey& key, PixelBuffer buffer) {
    if (!buffer.isValid() || buffer.data() == nullptr) {
        markFailed(key);
        return false;
    }

    const size_t newBytes = static_cast<size_t>(buffer.stride()) * buffer.height();

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
    return contains(key);
}

void TileCache::markFailed(const TileKey& key) {
    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        m_currentBytes -= it->second.entry.bytesUsed;
        m_currentBytes += TOMBSTONE_BYTE_WEIGHT;

        it->second.entry.buffer = PixelBuffer{};
        it->second.entry.bytesUsed = TOMBSTONE_BYTE_WEIGHT;
        it->second.entry.isFailed = true;

        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);
    } else {
        m_lruList.push_front(key);
        TileCacheEntry entry{PixelBuffer{}, TOMBSTONE_BYTE_WEIGHT, true};
        m_cacheMap.emplace(key, MapEntry{std::move(entry), m_lruList.begin()});
        m_currentBytes += TOMBSTONE_BYTE_WEIGHT;
    }

    evictIfNeeded();
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

void TileCache::invalidateDocument(int64_t documentId) {
    auto it = m_lruList.begin();
    while (it != m_lruList.end()) {
        if (it->documentId == documentId) {
            auto mapIt = m_cacheMap.find(*it);
            if (mapIt != m_cacheMap.end()) {
                m_currentBytes -= mapIt->second.entry.bytesUsed;
                m_cacheMap.erase(mapIt);
            }
            it = m_lruList.erase(it);
        } else {
            ++it;
        }
    }

    // Drop any pins associated with documentId
    for (auto pinIt = m_pinnedSet.begin(); pinIt != m_pinnedSet.end(); ) {
        if (pinIt->documentId == documentId) {
            pinIt = m_pinnedSet.erase(pinIt);
        } else {
            ++pinIt;
        }
    }
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
