#pragma once

#include <cstdint>
#include <vector>
#include <unordered_set>
#include <optional>
#include <QRectF>

#include "cache/TileCache.h"
#include "ipc/IpcMessages.h"
#include "layout/DocumentLayout.h"

namespace pdf::ipc {
class WorkerClient;
}

namespace ui {

struct FallbackSlice {
    pdf::TileKey key;
    QRectF srcRect;
    QRectF dstRect;
};

enum class VisibleTileAction {
    DrawCached,
    DrawPendingNoFallback,
    DrawPendingWithFallback,
    DrawError
};

class ViewerTileController {
public:
    static constexpr size_t MAX_FALLBACK_SCALES = 3;
    static constexpr size_t MAX_PREFETCH_PER_PAINT = 16;
    static constexpr int TILE_SIZE = 512;
    static constexpr int MAX_SOURCE_TILES = 64;

    ViewerTileController() = default;
    ~ViewerTileController() = default;

    ViewerTileController(const ViewerTileController&) = delete;
    ViewerTileController& operator=(const ViewerTileController&) = delete;
    ViewerTileController(ViewerTileController&&) = delete;
    ViewerTileController& operator=(ViewerTileController&&) = delete;

    size_t inFlightCount() const noexcept { return m_inFlightKeys.size(); }
    size_t fallbackTilesDrawn() const noexcept { return m_fallbackTilesDrawn; }
    size_t prefetchRequestsIssued() const noexcept { return m_prefetchRequestsIssued; }
    bool isPrefetchEnabled() const noexcept { return m_prefetchEnabled; }
    void setPrefetchEnabled(bool enabled) noexcept { m_prefetchEnabled = enabled; }
    const std::vector<double>& fallbackScales() const noexcept { return m_fallbackScales; }
    const std::unordered_set<pdf::TileKey, pdf::TileKeyHash>& inFlightKeys() const noexcept { return m_inFlightKeys; }

    void clearInFlight() noexcept { m_inFlightKeys.clear(); }
    void clearFallbackScales() noexcept { m_fallbackScales.clear(); }
    void resetDocument() noexcept {
        m_inFlightKeys.clear();
        m_fallbackScales.clear();
    }
    void rememberFallbackScale(double previousScale);
    void recordFallbackTileDrawn() noexcept { ++m_fallbackTilesDrawn; }

    void retryFailedTiles(pdf::TileCache& cache);

    bool handleTileReady(int64_t documentId, int64_t currentDocumentId,
                         const pdf::TileKey& key, const pdf::PixelBuffer& buffer,
                         pdf::TileCache& cache);
    bool handleTileFailed(int64_t documentId, int64_t currentDocumentId,
                          const pdf::TileKey& key,
                          pdf::TileCache& cache);

    VisibleTileAction evaluateVisibleTile(const pdf::TileKey& key,
                                          const layout::VisibleTile& t,
                                          int64_t tileVpX, int64_t tileVpY,
                                          int64_t vpCenterX, int64_t vpCenterY,
                                          int64_t currentDocumentId, uint64_t currentGeneration,
                                          double qScale, double qDpr, int rotation,
                                          pdf::TileCache& cache,
                                          std::vector<pdf::TileRenderRequest>& outPendingRequests,
                                          std::optional<pdf::PixelBuffer>* outCachedBuffer = nullptr);

    std::vector<FallbackSlice> selectFallbackTiles(const layout::VisibleTile& t,
                                                   const QRectF& tileRect,
                                                   double qScale, double qDpr,
                                                   int rotation, int64_t currentDocumentId,
                                                   pdf::TileCache& cache) const;

    void schedulePrefetch(const std::vector<layout::VisibleTile>& ringTiles,
                          const std::unordered_set<pdf::TileKey, pdf::TileKeyHash>& visibleSet,
                          int64_t scrollDocX, int64_t scrollDocY,
                          int64_t vpCenterX, int64_t vpCenterY,
                          int64_t currentDocumentId, uint64_t currentGeneration,
                          double qScale, double qDpr, int rotation,
                          const pdf::TileCache& cache,
                          std::vector<pdf::TileRenderRequest>& outPendingRequests);

    void submitRequests(std::vector<pdf::TileRenderRequest> requests, pdf::ipc::WorkerClient& worker);

private:
    std::unordered_set<pdf::TileKey, pdf::TileKeyHash> m_inFlightKeys;
    std::vector<double> m_fallbackScales;
    size_t m_fallbackTilesDrawn{0};
    bool m_prefetchEnabled{true};
    size_t m_prefetchRequestsIssued{0};
};

} // namespace ui
