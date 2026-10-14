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

int TileCache::failCount(const TileKey& key) const {
    auto it = m_failHistory.find(key);
    if (it != m_failHistory.end()) {
        return it->second;
    }
    return 0;
}

bool TileCache::canRetry(const TileKey& key, std::chrono::milliseconds baseBackoff) const {
    auto it = m_cacheMap.find(key);
    if (it == m_cacheMap.end() || !it->second.entry.isFailed) {
        return false;
    }
    if (it->second.entry.failCount >= MAX_RETRY_ATTEMPTS) {
        return false; // Give up: deterministic failure, stop burning CPU on retries
    }
    int shift = std::min(std::max(0, it->second.entry.failCount - 1), 5);
    auto backoff = baseBackoff * (1 << shift);
    auto elapsed = std::chrono::steady_clock::now() - it->second.entry.lastFailedTime;
    return elapsed >= backoff;
}

bool TileCache::isPermanentlyFailed(const TileKey& key) const {
    auto it = m_cacheMap.find(key);
    return it != m_cacheMap.end() && it->second.entry.isFailed &&
           it->second.entry.failCount >= MAX_RETRY_ATTEMPTS;
}

void TileCache::resetFailureHistory() {
    m_failHistory.clear();
    for (auto& kv : m_cacheMap) {
        if (kv.second.entry.isFailed) {
            kv.second.entry.failCount = 1; // keep tombstone, restart backoff ladder
        }
    }
}

bool TileCache::clearFailed(const TileKey& key) {
    auto it = m_cacheMap.find(key);
    if (it == m_cacheMap.end() || !it->second.entry.isFailed) {
        return false;
    }
    m_currentBytes -= it->second.entry.bytesUsed;
    m_lruList.erase(it->second.lruIter);
    m_cacheMap.erase(it);
    // m_failHistory is intentionally preserved so exponential backoff grows across repeated retries
    return true;
}

void TileCache::clearAllFailed() {
    for (auto it = m_cacheMap.begin(); it != m_cacheMap.end(); ) {
        if (it->second.entry.isFailed) {
            m_currentBytes -= it->second.entry.bytesUsed;
            m_lruList.erase(it->second.lruIter);
            it = m_cacheMap.erase(it);
        } else {
            ++it;
        }
    }
    // m_failHistory is intentionally preserved so exponential backoff grows across repeated retries
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

    // Successful render: reset failure backoff history for this tile
    m_failHistory.erase(key);

    const size_t newBytes = static_cast<size_t>(buffer.stride()) * buffer.height();

    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        // Update existing entry
        m_currentBytes -= it->second.entry.bytesUsed;
        m_currentBytes += newBytes;

        it->second.entry.buffer = std::move(buffer);
        it->second.entry.bytesUsed = newBytes;
        it->second.entry.isFailed = false;
        it->second.entry.failCount = 0;

        // Move to front (MRU)
        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);
    } else {
        // Insert new entry at front of LRU
        m_lruList.push_front(key);
        TileCacheEntry entry{std::move(buffer), newBytes, false, 0, {}};
        m_cacheMap.emplace(key, MapEntry{std::move(entry), m_lruList.begin()});
        m_currentBytes += newBytes;
    }

    evictIfNeeded();
    return contains(key);
}

void TileCache::markFailed(const TileKey& key) {
    auto now = std::chrono::steady_clock::now();
    int count = ++m_failHistory[key];

    auto it = m_cacheMap.find(key);
    if (it != m_cacheMap.end()) {
        m_currentBytes -= it->second.entry.bytesUsed;
        m_currentBytes += TOMBSTONE_BYTE_WEIGHT;

        it->second.entry.buffer = PixelBuffer{};
        it->second.entry.bytesUsed = TOMBSTONE_BYTE_WEIGHT;
        it->second.entry.isFailed = true;
        it->second.entry.failCount = count;
        it->second.entry.lastFailedTime = now;

        m_lruList.splice(m_lruList.begin(), m_lruList, it->second.lruIter);
    } else {
        m_lruList.push_front(key);
        TileCacheEntry entry{PixelBuffer{}, TOMBSTONE_BYTE_WEIGHT, true, count, now};
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

    // Drop failure history associated with documentId
    for (auto failIt = m_failHistory.begin(); failIt != m_failHistory.end(); ) {
        if (failIt->first.documentId == documentId) {
            failIt = m_failHistory.erase(failIt);
        } else {
            ++failIt;
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
    m_failHistory.clear();
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
