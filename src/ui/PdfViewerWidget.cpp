#include "PdfViewerWidget.h"
#include "ImageUtils.h"
#include <QPainter>
#include <QWheelEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <QCoreApplication>
#include <algorithm>
#include <cmath>

namespace ui {

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QAbstractScrollArea(parent),
      m_layout(),
      m_scrollMapper(),
      m_cache(256 * 1024 * 1024), // 256 MB Tile Cache byte budget
      m_renderWorker(this),
      m_scale(DESKTOP_100_PERCENT_SCALE)
{
    m_document = pdf::createDocument();

    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_NoSystemBackground);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);

    connect(&m_renderWorker, &pdf::RenderWorker::tileReady, this, &PdfViewerWidget::onTileReady, Qt::QueuedConnection);
    connect(&m_renderWorker, &pdf::RenderWorker::tileFailed, this, &PdfViewerWidget::onTileFailed, Qt::QueuedConnection);

    setStyleSheet(QStringLiteral("QAbstractScrollArea { background-color: #323639; border: none; }"));
}

PdfViewerWidget::~PdfViewerWidget() {
    m_renderWorker.stop();
    closeDocument();
}

int PdfViewerWidget::pageCount() const {
    return (m_document && m_document->isOpen()) ? m_document->pageCount() : 0;
}

void PdfViewerWidget::setDevicePixelRatioOverride(double dpr) {
    m_dprOverride = dpr > 0.0 ? dpr : 0.0;
    updateLayoutGeometry();
}

double PdfViewerWidget::effectiveDpr() const {
    return (m_dprOverride > 0.0) ? m_dprOverride : pdf::quantizeDpr(devicePixelRatioF());
}

void PdfViewerWidget::retryFailedTiles() {
    m_cache.clearAllFailed();
    viewport()->update();
}

void PdfViewerWidget::waitForRendering() {
    m_renderWorker.waitForIdle();
    QCoreApplication::processEvents();
}

pdf::DocumentError PdfViewerWidget::openDocument(const QString& filePath) {
    if (filePath.isEmpty()) {
        return pdf::DocumentError::FileNotFound;
    }

    auto newDoc = pdf::createDocument();
    auto err = newDoc->loadFromFile(filePath.toStdString());
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
        return err; // Existing document remains intact on failure
    }

    // Success: atomically cancel worker work, flush previous cache, and advance epochs
    m_renderWorker.cancelAll();
    m_cache.invalidateDocument(m_currentDocumentId);
    ++m_currentDocumentId;
    ++m_currentGeneration;

    m_document = std::move(newDoc);
    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;

    std::vector<std::pair<double, double>> sizes;
    sizes.reserve(m_document->pageCount());
    for (int i = 0; i < m_document->pageCount(); ++i) {
        sizes.push_back(m_document->pageSizePoints(i));
    }
    m_layout.setPageSizes(sizes);

    m_renderWorker.setDocument(m_currentDocumentId, m_document);
    m_renderWorker.setGeneration(m_currentGeneration);

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
    m_renderWorker.cancelAll();
    m_renderWorker.setDocument(0, nullptr);

    if (m_document && m_document->isOpen()) {
        m_cache.invalidateDocument(m_currentDocumentId);
        m_document->close();
        m_document.reset();
    }
    ++m_currentDocumentId;
    ++m_currentGeneration;

    m_layout.setPageSizes({});
    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;

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
    if (!m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }
    if (pageIndex < 0 || pageIndex >= pageCount()) {
        return;
    }

    m_navigating = true;
    m_currentPage = pageIndex;
    const auto& pl = m_layout.getPageLayout(pageIndex);
    // Align page top to viewport top
    setDocScrollPosition(m_docScrollX, pl.topPixel);
    m_navigating = false;

    Q_EMIT pageChanged(m_currentPage + 1, pageCount());
    viewport()->update();
}

void PdfViewerWidget::setZoom(double zoom, const QPoint* anchorWidgetPos) {
    double clamped = std::clamp(zoom, 0.05, 30.0);
    double qScale = pdf::quantizeScale(clamped);

    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int64_t anchorVpX = (anchorWidgetPos ? static_cast<int64_t>(std::round(anchorWidgetPos->x() * dpr)) : vpW / 2);
    int64_t anchorVpY = (anchorWidgetPos ? static_cast<int64_t>(std::round(anchorWidgetPos->y() * dpr)) : vpH / 2);

    int64_t curDocX = m_docScrollX + anchorVpX;
    int64_t curDocY = m_docScrollY + anchorVpY;
    auto anchor = m_layout.createAnchor(curDocX, curDocY);

    ++m_currentGeneration;
    m_renderWorker.setGeneration(m_currentGeneration);

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
    const double dpr = effectiveDpr();
    double pagePtW = m_document->pageSizePoints(m_currentPage).first;
    if (pagePtW <= 0.0) {
        pagePtW = layout::DEFAULT_PLACEHOLDER_WIDTH_POINTS;
    }

    // Reserve vertical scrollbar width (in logical pixels) if not already visible
    int reservedScrollbarLogicalW = verticalScrollBar()->isVisible() ? 0 : verticalScrollBar()->sizeHint().width();
    int availLogicalW = viewport()->width() - reservedScrollbarLogicalW;

    // Layout margin is 16 device pixels each side = 32 device pixels total.
    int64_t availDeviceW = static_cast<int64_t>(std::round(availLogicalW * dpr)) - 32;

    // Leave 2 device pixels of slack to guarantee scale quantization never overflows viewport width
    availDeviceW -= 2;

    if (availDeviceW > 50) {
        double desiredScale = (static_cast<double>(availDeviceW) / dpr) / pagePtW;
        double flooredScale = std::floor(desiredScale * 1000.0) / 1000.0;
        setZoom(flooredScale);
    }
}

void PdfViewerWidget::onScrollValueChanged() {
    if (m_navigating || !m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    m_docScrollX = m_scrollMapper.sliderToDoc(horizontalScrollBar()->value(), m_layout.maxDocumentWidth(), vpW);
    m_docScrollY = m_scrollMapper.sliderToDoc(verticalScrollBar()->value(), m_layout.totalDocumentHeight(), vpH);

    auto visPages = m_layout.getVisiblePages(m_docScrollY, vpH);
    if (visPages.isValid()) {
        int bestPage = -1;
        int64_t maxOverlap = 0;
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            const auto& pl = m_layout.getPageLayout(p);
            int64_t overlapTop = std::max(m_docScrollY, pl.topPixel);
            int64_t overlapBottom = std::min(m_docScrollY + vpH, pl.bottomPixel());
            int64_t overlap = overlapBottom - overlapTop;
            if (overlap > maxOverlap) {
                maxOverlap = overlap;
                bestPage = p;
            }
        }
        if (bestPage >= 0 && bestPage != m_currentPage) {
            m_currentPage = bestPage;
            Q_EMIT pageChanged(m_currentPage + 1, pageCount());
        }
    }
    viewport()->update();
}

void PdfViewerWidget::onTileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer) {
    if (documentId != m_currentDocumentId || generation < m_currentGeneration) {
        return; // Discard stale tile result
    }
    m_cache.put(key, buffer);
    viewport()->update();
}

void PdfViewerWidget::onTileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key) {
    if (documentId != m_currentDocumentId || generation < m_currentGeneration) {
        return;
    }
    m_cache.markFailed(key);
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

void PdfViewerWidget::setDocScrollPosition(int64_t docX, int64_t docY) {
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    const int64_t maxX = std::max<int64_t>(0, m_layout.maxDocumentWidth() - vpW);
    const int64_t maxY = std::max<int64_t>(0, m_layout.totalDocumentHeight() - vpH);

    m_docScrollX = std::clamp<int64_t>(docX, 0, maxX);
    m_docScrollY = std::clamp<int64_t>(docY, 0, maxY);

    QSignalBlocker blockV(verticalScrollBar());
    QSignalBlocker blockH(horizontalScrollBar());

    verticalScrollBar()->setValue(m_scrollMapper.docToSlider(m_docScrollY, m_layout.totalDocumentHeight(), vpH));
    horizontalScrollBar()->setValue(m_scrollMapper.docToSlider(m_docScrollX, m_layout.maxDocumentWidth(), vpW));
}

void PdfViewerWidget::updateScrollBars() {
    const double dpr = effectiveDpr();
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
    const double dpr = effectiveDpr();
    const double qScale = pdf::quantizeScale(m_scale);
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int64_t curDocX = m_docScrollX + vpW / 2;
    int64_t curDocY = m_docScrollY + vpH / 2;
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

void PdfViewerWidget::drawPendingPlaceholder(QPainter& painter, const QRectF& rect) {
    painter.fillRect(rect, QColor(252, 252, 252));
}

void PdfViewerWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), QColor("#323639"));

    if (!m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }

    const double qDpr = effectiveDpr();
    const double qScale = pdf::quantizeScale(m_scale);

    const int64_t scrollDocX = m_docScrollX;
    const int64_t scrollDocY = m_docScrollY;
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

    // 4. Draw cached tiles or placeholders, and collect pending worker requests
    std::vector<pdf::TileRenderRequest> pendingRequests;
    const int64_t vpCenterX = vpWidthDevice / 2;
    const int64_t vpCenterY = vpHeightDevice / 2;

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

        auto cachedBuf = m_cache.get(key);
        if (cachedBuf.has_value()) {
            QImage img = toQImage(*cachedBuf);
            painter.drawImage(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), img);
        } else if (m_cache.isFailed(key)) {
            if (m_cache.canRetry(key)) {
                // Backoff interval elapsed: allow retry
                m_cache.clearFailed(key);
                drawPendingPlaceholder(painter, tileRect);

                int64_t tileMidX = tileVpX + t.tileWidth / 2;
                int64_t tileMidY = tileVpY + t.tileHeight / 2;
                int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
                int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

                pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
                pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority});
            } else {
                drawErrorPlaceholder(painter, tileRect);
            }
        } else {
            // Pending render miss: draw placeholder on UI thread and queue request to worker
            drawPendingPlaceholder(painter, tileRect);

            int64_t tileMidX = tileVpX + t.tileWidth / 2;
            int64_t tileMidY = tileVpY + t.tileHeight / 2;
            int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
            int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

            pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
            pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority});
        }
    }

    if (!pendingRequests.empty()) {
        m_renderWorker.enqueueRequests(std::move(pendingRequests));
    }

    painter.restore();
}

} // namespace ui
