#pragma once

#include <QAbstractScrollArea>
#include <memory>
#include <unordered_map>
#include "engine/IDocument.h"
#include "engine/IPage.h"
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
    int pageCount() const;
    double currentZoom() const { return m_scale; }

    const layout::DocumentLayout& layout() const { return m_layout; }
    const pdf::TileCache& cache() const { return m_cache; }
    pdf::TileCache& cache() { return m_cache; }

    int64_t currentDocScrollX() const;
    int64_t currentDocScrollY() const;

    void setDevicePixelRatioOverride(double dpr);
    double effectiveDpr() const;

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
    double m_scale{DESKTOP_100_PERCENT_SCALE};
    double m_dprOverride{0.0};
    bool m_navigating{false};
    int m_wheelDeltaAccumulator{0};
};

} // namespace ui
