#include "ui/ViewerTileController.h"

#include <algorithm>
#include <cmath>

#include "ipc/WorkerClient.h"

namespace ui {

void ViewerTileController::rememberFallbackScale(double previousScale) {
    m_fallbackScales.erase(std::remove(m_fallbackScales.begin(), m_fallbackScales.end(), previousScale), m_fallbackScales.end());
    m_fallbackScales.insert(m_fallbackScales.begin(), previousScale);
    if (m_fallbackScales.size() > MAX_FALLBACK_SCALES) {
        m_fallbackScales.resize(MAX_FALLBACK_SCALES);
    }
}

void ViewerTileController::retryFailedTiles(pdf::TileCache& cache) {
    cache.resetFailureHistory(); // explicit user retry also lifts the permanent-failure state
    cache.clearAllFailed();
    m_inFlightKeys.clear();
}

bool ViewerTileController::handleTileReady(int64_t documentId, int64_t currentDocumentId,
                                           const pdf::TileKey& key, const pdf::PixelBuffer& buffer,
                                           pdf::TileCache& cache) {
    m_inFlightKeys.erase(key);
    if (documentId != currentDocumentId) {
        return false;
    }
    cache.put(key, buffer);
    // Prefetched off-screen tiles do not change what is on screen: skip the repaint
    return cache.isPinned(key);
}

bool ViewerTileController::handleTileFailed(int64_t documentId, int64_t currentDocumentId,
                                            const pdf::TileKey& key,
                                            pdf::TileCache& cache) {
    m_inFlightKeys.erase(key);
    if (documentId != currentDocumentId) {
        return false;
    }
    cache.markFailed(key);
    return true;
}

VisibleTileAction ViewerTileController::evaluateVisibleTile(
    const pdf::TileKey& key,
    const layout::VisibleTile& t,
    int64_t tileVpX, int64_t tileVpY,
    int64_t vpCenterX, int64_t vpCenterY,
    int64_t currentDocumentId, uint64_t currentGeneration,
    double qScale, double qDpr, int rotation,
    pdf::TileCache& cache,
    std::vector<pdf::TileRenderRequest>& outPendingRequests,
    std::optional<pdf::PixelBuffer>* outCachedBuffer)
{
    auto cachedBuf = cache.get(key);
    if (cachedBuf.has_value()) {
        m_inFlightKeys.erase(key);
        if (outCachedBuffer) {
            *outCachedBuffer = std::move(cachedBuf);
        }
        return VisibleTileAction::DrawCached;
    }

    if (cache.isFailed(key)) {
        if (cache.canRetry(key)) {
            if (m_inFlightKeys.find(key) != m_inFlightKeys.end()) {
                // Already in flight to worker for retry; avoid re-enqueueing duplicate
                return VisibleTileAction::DrawPendingNoFallback;
            }
            // Backoff interval elapsed: allow retry
            cache.clearFailed(key);

            int64_t tileMidX = tileVpX + t.tileWidth / 2;
            int64_t tileMidY = tileVpY + t.tileHeight / 2;
            int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
            int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

            pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
            outPendingRequests.push_back({currentDocumentId, currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority, rotation});
            m_inFlightKeys.insert(key);
            return VisibleTileAction::DrawPendingNoFallback;
        }
        return VisibleTileAction::DrawError;
    }

    // Pending render miss: queue request to worker if not already in flight
    if (m_inFlightKeys.find(key) == m_inFlightKeys.end()) {
        int64_t tileMidX = tileVpX + t.tileWidth / 2;
        int64_t tileMidY = tileVpY + t.tileHeight / 2;
        int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
        int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

        pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
        outPendingRequests.push_back({currentDocumentId, currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority, rotation});
        m_inFlightKeys.insert(key);
    }
    return VisibleTileAction::DrawPendingWithFallback;
}

std::vector<FallbackSlice> ViewerTileController::selectFallbackTiles(
    const layout::VisibleTile& t,
    const QRectF& tileRect,
    double qScale, double qDpr,
    int rotation, int64_t currentDocumentId,
    pdf::TileCache& cache) const
{
    std::vector<FallbackSlice> slices;
    for (const double oldScale : m_fallbackScales) {
        if (oldScale == qScale) {
            continue;
        }
        // Region of this tile expressed in the old zoom's page-pixel grid
        const double r = oldScale / qScale;
        const double sx0 = t.tileXOnPage * r;
        const double sx1 = (t.tileXOnPage + t.tileWidth) * r;
        const double sy0 = t.tileYOnPage * r;
        const double sy1 = (t.tileYOnPage + t.tileHeight) * r;

        const int c0 = static_cast<int>(std::floor(sx0 / TILE_SIZE));
        const int c1 = static_cast<int>(std::floor((sx1 - 1e-6) / TILE_SIZE));
        const int r0 = static_cast<int>(std::floor(sy0 / TILE_SIZE));
        const int r1 = static_cast<int>(std::floor((sy1 - 1e-6) / TILE_SIZE));
        if (c1 < c0 || r1 < r0 || static_cast<int64_t>(c1 - c0 + 1) * (r1 - r0 + 1) > MAX_SOURCE_TILES) {
            continue;
        }

        bool fullyCovered = true;
        for (int row = r0; row <= r1; ++row) {
            for (int col = c0; col <= c1; ++col) {
                const auto key = pdf::TileKey::create(currentDocumentId, t.pageIndex, col, row, oldScale, qDpr, rotation);
                auto buf = cache.get(key);
                if (!buf.has_value()) {
                    fullyCovered = false;
                    continue;
                }
                const double ox = static_cast<double>(col) * TILE_SIZE;
                const double oy = static_cast<double>(row) * TILE_SIZE;
                const double ix0 = std::max(sx0, ox);
                const double iy0 = std::max(sy0, oy);
                const double ix1 = std::min(sx1, ox + buf->width());
                const double iy1 = std::min(sy1, oy + buf->height());
                if (ix1 <= ix0 || iy1 <= iy0) {
                    fullyCovered = false;
                    continue;
                }
                const QRectF src(ix0 - ox, iy0 - oy, ix1 - ix0, iy1 - iy0);
                const QRectF dst(tileRect.x() + (ix0 / r - t.tileXOnPage),
                                 tileRect.y() + (iy0 / r - t.tileYOnPage),
                                 (ix1 - ix0) / r,
                                 (iy1 - iy0) / r);
                slices.push_back({key, src, dst});
            }
        }
        if (fullyCovered) {
            break; // the most recent zoom level covers the whole tile; older ones would only be overdrawn
        }
    }
    return slices;
}

void ViewerTileController::schedulePrefetch(
    const std::vector<layout::VisibleTile>& ringTiles,
    const std::unordered_set<pdf::TileKey, pdf::TileKeyHash>& visibleSet,
    int64_t scrollDocX, int64_t scrollDocY,
    int64_t vpCenterX, int64_t vpCenterY,
    int64_t currentDocumentId, uint64_t currentGeneration,
    double qScale, double qDpr, int rotation,
    const pdf::TileCache& cache,
    std::vector<pdf::TileRenderRequest>& outPendingRequests)
{
    if (!m_prefetchEnabled || ringTiles.empty()) {
        return;
    }

    struct Candidate { int64_t distSq; layout::VisibleTile tile; pdf::TileKey key; };
    std::vector<Candidate> candidates;

    for (const auto& t : ringTiles) {
        const auto key = pdf::TileKey::create(currentDocumentId, t.pageIndex, t.col, t.row, qScale, qDpr, rotation);
        if (visibleSet.count(key) || m_inFlightKeys.count(key) || cache.contains(key)) {
            continue;
        }
        const int64_t dx = (t.docX - scrollDocX) + t.tileWidth / 2 - vpCenterX;
        const int64_t dy = (t.docY - scrollDocY) + t.tileHeight / 2 - vpCenterY;
        candidates.push_back({dx * dx + dy * dy, t, key});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });
    if (candidates.size() > MAX_PREFETCH_PER_PAINT) {
        candidates.resize(MAX_PREFETCH_PER_PAINT);
    }
    for (const auto& c : candidates) {
        const int priority = -1 - static_cast<int>(std::min<int64_t>(c.distSq >> 12, 1000000000));
        pdf::PixelRect pRect{c.tile.tileXOnPage, c.tile.tileYOnPage, c.tile.tileWidth, c.tile.tileHeight};
        outPendingRequests.push_back({currentDocumentId, currentGeneration, c.tile.pageIndex, pRect, qScale, qDpr, c.key, priority, rotation});
        m_inFlightKeys.insert(c.key);
        ++m_prefetchRequestsIssued;
    }
}

void ViewerTileController::submitRequests(std::vector<pdf::TileRenderRequest> requests, pdf::ipc::WorkerClient& worker) {
    if (!requests.empty()) {
        worker.enqueueRequests(std::move(requests));
    }
}

} // namespace ui
