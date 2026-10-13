#pragma once

#include <QAbstractScrollArea>
#include <memory>
#include "engine/IDocument.h"
#include "engine/IPage.h"
#include "engine/RenderWorker.h"
#include "layout/DocumentLayout.h"
#include "layout/ScrollMapper.h"
#include "cache/TileCache.h"

namespace ui {

// Desktop 100% zoom standard: 96 DPI / 72 DPI = 1.3333333333333333
inline constexpr double DESKTOP_100_PERCENT_SCALE = 96.0 / 72.0;

class PdfViewerWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit PdfViewerWidget(QWidget* parent = nullptr);
    ~PdfViewerWidget() override;

    // Disallow copy/move
    PdfViewerWidget(const PdfViewerWidget&) = delete;
    PdfViewerWidget& operator=(const PdfViewerWidget&) = delete;

    pdf::DocumentError openDocument(const QString& filePath);
    void closeDocument();

    int currentPage() const { return m_currentPage; }
    int pageCount() const { return m_pageCount; }
    bool isDocumentOpen() const { return m_hasDocument && m_pageCount > 0; }
    double currentZoom() const { return m_scale; }
    int64_t currentDocumentId() const { return m_currentDocumentId; }

    const layout::DocumentLayout& layout() const { return m_layout; }
    const pdf::TileCache& cache() const { return m_cache; }
    pdf::TileCache& cache() { return m_cache; }

    int64_t currentDocScrollX() const { return m_docScrollX; }
    int64_t currentDocScrollY() const { return m_docScrollY; }

    void setDevicePixelRatioOverride(double dpr);
    double effectiveDpr() const;

    /**
     * @brief Retries any previously failed tiles by clearing failure tombstones.
     */
    void retryFailedTiles();

    /**
     * @brief Waits until background rendering is idle and flushes Qt event queue.
     * Useful for deterministic testing.
     */
    void waitForRendering();

    pdf::RenderWorker& renderWorker() { return m_renderWorker; }

public Q_SLOTS:
    void nextPage();
    void previousPage();
    void goToPage(int pageIndex);
    void setZoom(double zoom, const QPoint* anchorWidgetPos = nullptr);
    void zoomIn();
    void zoomOut();
    void zoomFitWidth();
    void zoomReset();

Q_SIGNALS:
    void pageChanged(int currentPage, int totalPages);
    void zoomChanged(double zoom);
    void documentLoaded(const QString& title, int totalPages);
    void loadError(const QString& errorMessage);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private Q_SLOTS:
    void onScrollValueChanged();
    void onTileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void onTileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);

private:
    void updateLayoutGeometry();
    void updateScrollBars();

    void setDocScrollPosition(int64_t docX, int64_t docY);

    void drawErrorPlaceholder(QPainter& painter, const QRectF& rect);
    void drawPendingPlaceholder(QPainter& painter, const QRectF& rect);

    layout::DocumentLayout m_layout;
    layout::ScrollMapper m_scrollMapper;
    pdf::TileCache m_cache;
    pdf::RenderWorker m_renderWorker;

    bool m_hasDocument{false};
    int m_pageCount{0};
    int64_t m_currentDocumentId{1};
    uint64_t m_currentGeneration{1};
    int m_currentPage{0};
    double m_scale{DESKTOP_100_PERCENT_SCALE};
    double m_dprOverride{0.0};
    bool m_navigating{false};
    int m_wheelDeltaAccumulator{0};

    // Primary 64-bit document coordinates, immune to 32-bit slider resize quantization
    int64_t m_docScrollX{0};
    int64_t m_docScrollY{0};
};

} // namespace ui
