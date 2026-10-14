#pragma once

#include <QAbstractScrollArea>
#include <QString>
#include <memory>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include "layout/DocumentLayout.h"
#include "layout/ScrollMapper.h"
#include "cache/TileCache.h"
#include "engine/RenderWorker.h"

namespace ui {

/**
 * @brief Professional, high-performance Qt6 PDF Viewer widget.
 * Implements continuous document scrolling, smooth zooming, exact 64-bit coordinate space,
 * compressed scrollbar mapping, sub-pixel rounding-safe fit-width, and asynchronous tiled background rendering.
 * PDFium operations run strictly on the background RenderWorker thread; 0 PDFium calls occur on UI thread.
 */
class PdfViewerWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    static constexpr double DESKTOP_100_PERCENT_SCALE = 96.0 / 72.0; // 1.3333333333333333

    explicit PdfViewerWidget(QWidget* parent = nullptr);
    ~PdfViewerWidget() override;

    /**
     * @brief Opens a document synchronously (blocks the caller until the worker has loaded it).
     * Cancels any pending openDocumentAsync(). Prefer openDocumentAsync() for interactive use.
     */
    pdf::DocumentError openDocument(const QString& filePath);

    /**
     * @brief Opens a document without blocking the UI thread.
     * The currently open document stays visible and interactive while the file loads; on success the
     * viewer switches to the new document and emits documentLoaded(), on failure it emits loadError()
     * and keeps the current document. Calls made while an open is in progress are coalesced:
     * only the most recent pending path is opened after the current one finishes.
     * loadingChanged() is emitted when loading starts and when the last queued open finishes.
     */
    void openDocumentAsync(const QString& filePath);

    /// @return true while an asynchronous open is in progress or queued.
    bool isLoading() const noexcept { return m_loading; }

    void closeDocument();

    bool isDocumentOpen() const noexcept { return m_hasDocument; }
    int pageCount() const noexcept { return m_pageCount; }
    int currentPage() const noexcept { return m_currentPage; }
    double currentZoom() const noexcept { return m_scale; }
    int64_t currentDocumentId() const noexcept { return m_currentDocumentId; }
    uint64_t currentGeneration() const noexcept { return m_currentGeneration; }

    int64_t currentDocScrollX() const noexcept { return m_docScrollX; }
    int64_t currentDocScrollY() const noexcept { return m_docScrollY; }

    void setZoom(double zoom, const QPoint* anchorWidgetPos = nullptr);
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void zoomFitWidth();

    void goToPage(int pageIndex);
    void nextPage();
    void previousPage();

    /**
     * @brief Manually triggers retry of all currently failed tiles in the viewport.
     */
    void retryFailedTiles();

    /**
     * @brief Injects an explicit Device Pixel Ratio override (for high-DPI testing and simulation).
     * Passing 0.0 or negative resets back to system devicePixelRatioF().
     */
    void setDevicePixelRatioOverride(double dpr);
    double effectiveDpr() const;

    /**
     * @brief Blocks until the background render worker has finished all pending requests and in-flight renders.
     * Useful for deterministic testing and flush operations.
     */
    void waitForRendering();

    pdf::RenderWorker& renderWorker() noexcept { return m_renderWorker; }
    pdf::TileCache& cache() noexcept { return m_cache; }
    const layout::DocumentLayout& layout() const noexcept { return m_layout; }
    size_t inFlightCount() const noexcept { return m_inFlightKeys.size(); }

    /// Tiles drawn from a previous zoom level (scaled) because the current-zoom tile was not ready yet.
    size_t fallbackTilesDrawn() const noexcept { return m_fallbackTilesDrawn; }
    /// Prefetch (off-screen) tile requests handed to the worker so far.
    size_t prefetchRequestsIssued() const noexcept { return m_prefetchRequestsIssued; }
    /// Enables/disables the off-screen prefetch ring (enabled by default).
    void setPrefetchEnabled(bool enabled) noexcept { m_prefetchEnabled = enabled; }

Q_SIGNALS:
    void documentLoaded(const QString& title, int pages);
    void loadError(const QString& errorMessage);
    void loadingChanged(bool loading, const QString& fileName);
    void pageChanged(int currentPage, int totalPages);
    void zoomChanged(double zoomRatio);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private Q_SLOTS:
    void onScrollValueChanged();
    void onTileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void onTileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);
    void onDocumentOpened(int64_t documentId, pdf::OpenResult result);

private:
    void updateLayoutGeometry();
    void updateScrollBars();

    void setDocScrollPosition(int64_t docX, int64_t docY);

    void drawErrorPlaceholder(QPainter& painter, const QRectF& rect);
    void drawPendingPlaceholder(QPainter& painter, const QRectF& rect);
    void drawLoadingOverlay(QPainter& painter);
    bool drawFallbackTile(QPainter& painter, const layout::VisibleTile& tile, const QRectF& tileRect, double qScale, double qDpr);
    void rememberFallbackScale(double previousScale);

    QString openErrorMessage(pdf::DocumentError error, const QString& filePath) const;
    void applyOpenedDocument(int64_t newDocumentId, const QString& filePath, const pdf::OpenResult& result);
    void startAsyncOpen(const QString& filePath);
    void cancelPendingOpen();

    layout::DocumentLayout m_layout;
    layout::ScrollMapper m_scrollMapper;
    pdf::TileCache m_cache;
    pdf::RenderWorker m_renderWorker;

    // Asynchronous open state (UI thread only)
    bool m_loading{false};
    int64_t m_pendingOpenId{0};
    QString m_pendingOpenPath;
    bool m_hasQueuedOpen{false};
    QString m_queuedOpenPath;

    bool m_hasDocument{false};
    int m_pageCount{0};
    int64_t m_currentDocumentId{1};
    uint64_t m_currentGeneration{1};
    int m_currentPage{0};
    double m_scale{DESKTOP_100_PERCENT_SCALE};
    double m_dprOverride{0.0};
    bool m_navigating{false};
    int m_wheelDeltaAccumulator{0};

    // Tracks tile requests currently in-flight on the worker to prevent duplicate enqueues
    std::unordered_set<pdf::TileKey, pdf::TileKeyHash> m_inFlightKeys;

    // Primary 64-bit document coordinates, immune to 32-bit slider resize quantization
    int64_t m_docScrollX{0};
    int64_t m_docScrollY{0};

    // Recently used zoom scales (newest first, quantized); their cached tiles serve as scaled stand-ins
    // while tiles for the current zoom are still rendering.
    static constexpr size_t MAX_FALLBACK_SCALES = 3;
    std::vector<double> m_fallbackScales;
    size_t m_fallbackTilesDrawn{0};

    // Off-screen prefetch ring (one viewport height above and below the visible area)
    bool m_prefetchEnabled{true};
    size_t m_prefetchRequestsIssued{0};

    // Cached previous viewport dimensions to anchor precisely during resizes
    int64_t m_lastVpWidthDevice{0};
    int64_t m_lastVpHeightDevice{0};
};

} // namespace ui
