#pragma once

#include <QWidget>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <memory>
#include <unordered_map>
#include "engine/IDocument.h"
#include "layout/DocumentLayout.h"
#include "layout/ScrollMapper.h"
#include "cache/TileCache.h"

namespace ui {

/**
 * @brief High-performance Linux PDF viewer widget.
 * Subclasses QAbstractScrollArea to implement synchronous device-pixel tiled rendering,
 * continuous multi-page vertical layout, 64-bit integer mapped scrollbars,
 * 2D anchor zoom, high-DPI scaling, and error fallback rendering for unreadable pages.
 */
class PdfViewerWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    explicit PdfViewerWidget(QWidget* parent = nullptr);
    ~PdfViewerWidget() override = default;

    pdf::DocumentError openDocument(const QString& filePath);
    void closeDocument();

    int currentPage() const { return m_currentPage; }
    int pageCount() const;
    double currentZoom() const { return m_scale; }

    const layout::DocumentLayout& layout() const { return m_layout; }
    const pdf::TileCache& cache() const { return m_cache; }
    pdf::TileCache& cache() { return m_cache; }

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

private:
    void updateLayoutGeometry();
    void updateScrollBars();

    int64_t currentDocScrollX() const;
    int64_t currentDocScrollY() const;
    void setDocScrollPosition(int64_t docX, int64_t docY);

    void drawErrorPlaceholder(QPainter& painter, const QRectF& rect);

    std::unique_ptr<pdf::IDocument> m_document;
    layout::DocumentLayout m_layout;
    layout::ScrollMapper m_scrollMapper;
    pdf::TileCache m_cache;

    // Cache of open page handles for UI-thread rendering pass
    std::unordered_map<int, std::unique_ptr<pdf::IPage>> m_pageCache;

    int64_t m_currentDocumentId{1};
    int m_currentPage{0};
    double m_scale{96.0 / 72.0}; // Standard desktop 100% scale (96 DPI / 72 pt = 1.3333x)
    int m_wheelDeltaAccumulator{0};
    bool m_navigating{false};
};

} // namespace ui
