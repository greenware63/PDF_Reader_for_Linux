#include "PdfViewerWidget.h"
#include "ImageUtils.h"
#include <QPainter>
#include <QWheelEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <algorithm>
#include <cmath>

namespace ui {

static constexpr double DESKTOP_100_PERCENT_SCALE = 96.0 / 72.0;

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QAbstractScrollArea(parent),
      m_layout(),
      m_scrollMapper(),
      m_cache(256 * 1024 * 1024), // 256 MB Tile Cache byte budget
      m_scale(DESKTOP_100_PERCENT_SCALE)
{
    m_document = pdf::createDocument();

    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_NoSystemBackground);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);

    setStyleSheet(QStringLiteral("QAbstractScrollArea { background-color: #323639; border: none; }"));
}

int PdfViewerWidget::pageCount() const {
    return (m_document && m_document->isOpen()) ? m_document->pageCount() : 0;
}

pdf::DocumentError PdfViewerWidget::openDocument(const QString& filePath) {
    if (!m_document) {
        m_document = pdf::createDocument();
    }

    // Load file without closing current active document first
    pdf::DocumentError err = m_document->loadFromFile(filePath.toStdString());
    if (err != pdf::DocumentError::None) {
        QString msg;
        switch (err) {
            case pdf::DocumentError::FileNotFound:
                msg = tr("File not found: %1").arg(filePath);
                break;
            case pdf::DocumentError::PasswordRequired:
                msg = tr("Document is password protected.");
                break;
            case pdf::DocumentError::BadPassword:
                msg = tr("Incorrect password entered.");
                break;
            case pdf::DocumentError::EmptyDocument:
                msg = tr("Document contains no pages.");
                break;
            case pdf::DocumentError::InvalidFormat:
            default:
                msg = tr("Corrupted or unsupported PDF format.");
                break;
        }
        Q_EMIT loadError(msg);
        return err; // Existing document remains intact on failure!
    }

    // Success: safely invalidate prior cache and advance document epoch
    m_cache.invalidateDocument(m_currentDocumentId);
    ++m_currentDocumentId;
    m_pageCache.clear();

    // Populate layout with all page sizes
    std::vector<std::pair<double, double>> sizes;
    sizes.reserve(m_document->pageCount());
    for (int i = 0; i < m_document->pageCount(); ++i) {
        sizes.push_back(m_document->pageSizePoints(i));
    }
    m_layout.setPageSizes(sizes);

    m_currentPage = 0;
    m_scale = DESKTOP_100_PERCENT_SCALE;

    updateLayoutGeometry();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);

    Q_EMIT documentLoaded(QFileInfo(filePath).fileName(), pageCount());
    Q_EMIT pageChanged(1, pageCount());
    Q_EMIT zoomChanged(1.0);

    viewport()->update();
    return pdf::DocumentError::None;
}

void PdfViewerWidget::closeDocument() {
    if (m_document && m_document->isOpen()) {
        m_cache.invalidateDocument(m_currentDocumentId);
        m_document->close();
    }
    ++m_currentDocumentId;
    m_pageCache.clear();
    m_layout.setPageSizes({});
    m_currentPage = 0;
    updateScrollBars();
    viewport()->update();
    Q_EMIT pageChanged(0, 0);
}

void PdfViewerWidget::nextPage() {
    if (m_currentPage + 1 < pageCount()) {
        goToPage(m_currentPage + 1);
    }
}

void PdfViewerWidget::previousPage() {
    if (m_currentPage > 0) {
        goToPage(m_currentPage - 1);
    }
}

void PdfViewerWidget::goToPage(int pageIndex) {
    if (!m_document || pageIndex < 0 || pageIndex >= pageCount()) {
        return;
    }
    m_currentPage = pageIndex;
    const auto& pl = m_layout.getPageLayout(pageIndex);
    int64_t targetDocY = std::max<int64_t>(0, pl.topPixel - 16);

    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    m_navigating = true;
    verticalScrollBar()->setValue(m_scrollMapper.docToSlider(targetDocY, m_layout.totalDocumentHeight(), vpH));
    m_navigating = false;

    viewport()->update();
    Q_EMIT pageChanged(m_currentPage + 1, pageCount());
}

void PdfViewerWidget::setZoom(double zoom, const QPoint* anchorWidgetPos) {
    double clamped = std::clamp(zoom, 0.05, 30.0);
    double qScale = pdf::quantizeScale(clamped);

    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int64_t anchorVpX = (anchorWidgetPos ? static_cast<int64_t>(std::round(anchorWidgetPos->x() * dpr)) : vpW / 2);
    int64_t anchorVpY = (anchorWidgetPos ? static_cast<int64_t>(std::round(anchorWidgetPos->y() * dpr)) : vpH / 2);

    int64_t curDocX = currentDocScrollX() + anchorVpX;
    int64_t curDocY = currentDocScrollY() + anchorVpY;
    auto anchor = m_layout.createAnchor(curDocX, curDocY);

    m_scale = qScale;
    m_layout.updateLayout(m_scale, dpr, vpW);
    updateScrollBars();

    int64_t newDocX = 0, newDocY = 0;
    m_layout.resolveAnchor(anchor, anchorVpX, anchorVpY, vpW, vpH, newDocX, newDocY);
    setDocScrollPosition(newDocX, newDocY);

    viewport()->update();

    double roundedRatio = std::round((m_scale / DESKTOP_100_PERCENT_SCALE) * 1000.0) / 1000.0;
    Q_EMIT zoomChanged(roundedRatio);
}

void PdfViewerWidget::zoomIn() {
    setZoom(m_scale * 1.2);
}

void PdfViewerWidget::zoomOut() {
    setZoom(m_scale / 1.2);
}

void PdfViewerWidget::zoomReset() {
    setZoom(DESKTOP_100_PERCENT_SCALE);
}

void PdfViewerWidget::zoomFitWidth() {
    if (!m_document || pageCount() == 0) {
        return;
    }
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    double pagePtW = m_document->pageSizePoints(m_currentPage).first;
    if (pagePtW <= 0.0) {
        pagePtW = layout::DEFAULT_PLACEHOLDER_WIDTH_POINTS;
    }

    // Reserve vertical scrollbar width to prevent subsequent horizontal scrollbar popup
    int reservedScrollbarW = verticalScrollBar()->isVisible() ? 0 : verticalScrollBar()->sizeHint().width();
    int availLogicalW = viewport()->width() - reservedScrollbarW - 32; // 32 = 2 * 16px horizontal margins
    int64_t availWDevice = static_cast<int64_t>(std::round(availLogicalW * dpr));

    if (availWDevice > 50) {
        setZoom((static_cast<double>(availWDevice) / dpr) / pagePtW);
    }
}

void PdfViewerWidget::onScrollValueChanged() {
    if (m_navigating || !m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));
    const int64_t scrollDocY = currentDocScrollY();

    auto visPages = m_layout.getVisiblePages(scrollDocY, vpH);
    if (visPages.isValid()) {
        // If m_currentPage is outside visible range, track to topmost visible page
        if (m_currentPage < visPages.firstVisiblePage || m_currentPage > visPages.lastVisiblePage) {
            m_currentPage = visPages.firstVisiblePage;
            Q_EMIT pageChanged(m_currentPage + 1, pageCount());
        }
    }
    viewport()->update();
}

void PdfViewerWidget::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateLayoutGeometry();
}

void PdfViewerWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        m_wheelDeltaAccumulator += event->angleDelta().y();
        constexpr int DELTA_THRESHOLD = 120; // 1 mouse wheel click step

        QPoint pos = event->position().toPoint();
        while (m_wheelDeltaAccumulator >= DELTA_THRESHOLD) {
            setZoom(m_scale * 1.2, &pos);
            m_wheelDeltaAccumulator -= DELTA_THRESHOLD;
        }
        while (m_wheelDeltaAccumulator <= -DELTA_THRESHOLD) {
            setZoom(m_scale / 1.2, &pos);
            m_wheelDeltaAccumulator += DELTA_THRESHOLD;
        }
        event->accept();
    } else {
        QAbstractScrollArea::wheelEvent(event);
    }
}

int64_t PdfViewerWidget::currentDocScrollX() const {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    return m_scrollMapper.sliderToDoc(horizontalScrollBar()->value(), m_layout.maxDocumentWidth(), vpW);
}

int64_t PdfViewerWidget::currentDocScrollY() const {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));
    return m_scrollMapper.sliderToDoc(verticalScrollBar()->value(), m_layout.totalDocumentHeight(), vpH);
}

void PdfViewerWidget::setDocScrollPosition(int64_t docX, int64_t docY) {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    verticalScrollBar()->setValue(m_scrollMapper.docToSlider(docY, m_layout.totalDocumentHeight(), vpH));
    horizontalScrollBar()->setValue(m_scrollMapper.docToSlider(docX, m_layout.maxDocumentWidth(), vpW));
}

void PdfViewerWidget::updateScrollBars() {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int32_t maxSliderV = 0, pageStepV = 0, singleStepV = 0;
    m_scrollMapper.computeRange(m_layout.totalDocumentHeight(), vpH, maxSliderV, pageStepV, singleStepV, static_cast<int64_t>(std::round(40 * dpr)));

    int32_t maxSliderH = 0, pageStepH = 0, singleStepH = 0;
    m_scrollMapper.computeRange(m_layout.maxDocumentWidth(), vpW, maxSliderH, pageStepH, singleStepH, static_cast<int64_t>(std::round(40 * dpr)));

    QSignalBlocker blockV(verticalScrollBar());
    QSignalBlocker blockH(horizontalScrollBar());

    verticalScrollBar()->setRange(0, maxSliderV);
    verticalScrollBar()->setPageStep(pageStepV);
    verticalScrollBar()->setSingleStep(singleStepV);

    horizontalScrollBar()->setRange(0, maxSliderH);
    horizontalScrollBar()->setPageStep(pageStepH);
    horizontalScrollBar()->setSingleStep(singleStepH);
}

void PdfViewerWidget::updateLayoutGeometry() {
    if (!m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const double qScale = pdf::quantizeScale(m_scale);
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int64_t curDocX = currentDocScrollX() + vpW / 2;
    int64_t curDocY = currentDocScrollY() + vpH / 2;
    auto anchor = m_layout.createAnchor(curDocX, curDocY);

    m_layout.updateLayout(qScale, dpr, vpW);
    updateScrollBars();

    int64_t newDocX = 0, newDocY = 0;
    m_layout.resolveAnchor(anchor, vpW / 2, vpH / 2, vpW, vpH, newDocX, newDocY);
    setDocScrollPosition(newDocX, newDocY);

    viewport()->update();
}

void PdfViewerWidget::drawErrorPlaceholder(QPainter& painter, const QRectF& rect) {
    painter.fillRect(rect, QColor(245, 225, 225));
    painter.setPen(QPen(QColor(220, 60, 60), 1, Qt::DashLine));
    painter.drawRect(rect);
    painter.setPen(QColor(180, 30, 30));
    painter.drawText(rect, Qt::AlignCenter, tr("Page display error"));
}

void PdfViewerWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), QColor("#323639"));

    if (!m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }

    const double dpr = devicePixelRatioF();
    const double qDpr = pdf::quantizeDpr(dpr);
    const double qScale = pdf::quantizeScale(m_scale);

    const int64_t scrollDocX = currentDocScrollX();
    const int64_t scrollDocY = currentDocScrollY();
    const int64_t vpWidthDevice = static_cast<int64_t>(std::round(viewport()->width() * qDpr));
    const int64_t vpHeightDevice = static_cast<int64_t>(std::round(viewport()->height() * qDpr));

    layout::Rect64 vpRectDevice{scrollDocX, scrollDocY, vpWidthDevice, vpHeightDevice};

    // Scale painter coordinates so all operations map directly 1:1 into device pixels using quantized DPR
    painter.save();
    painter.scale(1.0 / qDpr, 1.0 / qDpr);

    // 1. Draw page paper background and drop-shadows
    auto visPages = m_layout.getVisiblePages(scrollDocY, vpHeightDevice);
    if (visPages.isValid()) {
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            const auto& pl = m_layout.getPageLayout(p);
            QRectF pageRect(
                static_cast<qreal>(pl.leftPixel - scrollDocX),
                static_cast<qreal>(pl.topPixel - scrollDocY),
                static_cast<qreal>(pl.widthPixel),
                static_cast<qreal>(pl.heightPixel)
            );

            // Drop shadow
            QRectF shadowRect = pageRect.adjusted(3, 3, 5, 5);
            painter.fillRect(shadowRect, QColor(0, 0, 0, 80));

            // White paper
            painter.fillRect(pageRect, Qt::white);

            // Subtle border
            painter.setPen(QColor(180, 180, 180));
            painter.drawRect(pageRect);
        }
    }

    // 2. Query visible tiles from layout
    auto tiles = m_layout.getVisibleTiles(vpRectDevice, 512);

    // 3. Atomically pin visible tiles in TileCache
    std::vector<pdf::TileKey> visibleKeys;
    visibleKeys.reserve(tiles.size());
    for (const auto& t : tiles) {
        visibleKeys.push_back(pdf::TileKey::create(m_currentDocumentId, t.pageIndex, t.col, t.row, qScale, qDpr));
    }
    m_cache.setPinnedKeys(visibleKeys);

    // 4. Render and draw tiles
    for (size_t i = 0; i < tiles.size(); ++i) {
        const auto& t = tiles[i];
        const auto& key = visibleKeys[i];
        const int64_t tileVpX = t.docX - scrollDocX;
        const int64_t tileVpY = t.docY - scrollDocY;
        QRectF tileRect(
            static_cast<qreal>(tileVpX),
            static_cast<qreal>(tileVpY),
            static_cast<qreal>(t.tileWidth),
            static_cast<qreal>(t.tileHeight)
        );

        if (m_cache.isFailed(key)) {
            drawErrorPlaceholder(painter, tileRect);
            continue;
        }

        auto cachedBuf = m_cache.get(key);
        if (cachedBuf.has_value()) {
            QImage img = toQImage(*cachedBuf);
            painter.drawImage(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), img);
        } else {
            // UI page cache lookup to prevent redundant getPage calls for tiles on same page
            pdf::IPage* page = nullptr;
            auto it = m_pageCache.find(t.pageIndex);
            if (it != m_pageCache.end()) {
                page = it->second.get();
            } else {
                auto newPage = m_document->getPage(t.pageIndex);
                if (newPage) {
                    page = newPage.get();
                    m_pageCache.emplace(t.pageIndex, std::move(newPage));
                }
            }

            if (!page) {
                m_cache.markFailed(key);
                drawErrorPlaceholder(painter, tileRect);
                continue;
            }

            pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
            pdf::PixelBuffer buffer = page->renderTile(pRect, qScale, qDpr);
            if (!buffer.isValid() || buffer.data() == nullptr) {
                m_cache.markFailed(key);
                drawErrorPlaceholder(painter, tileRect);
            } else {
                m_cache.put(key, buffer); // Put into cache
                QImage img = toQImage(buffer); // Draw rendered buffer even if cache immediately evicts
                painter.drawImage(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), img);
            }
        }
    }

    // Prune UI page cache if exceeding 16 open pages, retaining only currently visible pages
    if (m_pageCache.size() > 16 && visPages.isValid()) {
        for (auto it = m_pageCache.begin(); it != m_pageCache.end(); ) {
            if (it->first < visPages.firstVisiblePage || it->first > visPages.lastVisiblePage) {
                it = m_pageCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    painter.restore();
}

} // namespace ui
