#include "PdfViewerWidget.h"
#include "ImageUtils.h"
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace ui {

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QAbstractScrollArea(parent),
      m_layout(),
      m_scrollMapper(),
      m_cache(256 * 1024 * 1024), // 256 MB Tile Cache byte budget
      m_renderWorker(this),
      m_scale(DESKTOP_100_PERCENT_SCALE)
{
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_NoSystemBackground);
    viewport()->setMouseTracking(true);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);

    connect(&m_renderWorker, &pdf::RenderWorker::tileReady, this, &PdfViewerWidget::onTileReady, Qt::QueuedConnection);
    connect(&m_renderWorker, &pdf::RenderWorker::tileFailed, this, &PdfViewerWidget::onTileFailed, Qt::QueuedConnection);
    connect(&m_renderWorker, &pdf::RenderWorker::documentOpened, this, &PdfViewerWidget::onDocumentOpened, Qt::QueuedConnection);
    connect(&m_renderWorker, &pdf::RenderWorker::pageTextReady, this, &PdfViewerWidget::onPageTextReady, Qt::QueuedConnection);

    setStyleSheet(QStringLiteral("QAbstractScrollArea { background-color: #323639; border: none; }"));
}

PdfViewerWidget::~PdfViewerWidget() {
    // Clean teardown: stop worker and flush memory without touching scrollbars or emitting pageChanged during destruction
    m_renderWorker.stop();
    m_cache.clear();
    m_inFlightKeys.clear();
    m_pageTextCache.clear();
}

void PdfViewerWidget::setDevicePixelRatioOverride(double dpr) {
    m_dprOverride = dpr > 0.0 ? dpr : 0.0;
    updateLayoutGeometry();
}

double PdfViewerWidget::effectiveDpr() const {
    return (m_dprOverride > 0.0) ? m_dprOverride : pdf::quantizeDpr(devicePixelRatioF());
}

void PdfViewerWidget::retryFailedTiles() {
    m_cache.resetFailureHistory(); // explicit user retry also lifts the permanent-failure state
    m_cache.clearAllFailed();
    m_inFlightKeys.clear();
    viewport()->update();
}

void PdfViewerWidget::waitForRendering() {
    m_renderWorker.waitForIdle();
    QCoreApplication::processEvents();
}

QString PdfViewerWidget::openErrorMessage(pdf::DocumentError error, const QString& filePath) const {
    switch (error) {
        case pdf::DocumentError::FileNotFound:
            return tr("File not found: %1").arg(filePath);
        case pdf::DocumentError::PasswordRequired:
            return tr("Document is password protected.");
        case pdf::DocumentError::BadPassword:
            return tr("Incorrect password entered.");
        case pdf::DocumentError::EmptyDocument:
            return tr("Document contains no pages.");
        case pdf::DocumentError::InvalidFormat:
        default:
            return tr("Corrupted or unsupported PDF format.");
    }
}

void PdfViewerWidget::applyOpenedDocument(int64_t nextDocId, const QString& filePath, const pdf::OpenResult& openResult) {
    // Success: atomically cancel worker work, flush previous cache, and advance epochs
    m_renderWorker.cancelAll();
    m_cache.invalidateDocument(m_currentDocumentId);
    m_inFlightKeys.clear();
    m_pageTextCache.clear();
    clearSelection();
    m_currentDocumentId = nextDocId;
    ++m_currentGeneration;
    m_renderWorker.setGeneration(m_currentGeneration);

    m_hasDocument = true;
    m_pageCount = openResult.pageCount;
    m_layout.setPageSizes(openResult.pageSizes);

    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;
    m_scale = DESKTOP_100_PERCENT_SCALE;
    m_fallbackScales.clear(); // tiles of the previous document are gone; nothing to stand in

    const double dpr = effectiveDpr();
    m_lastVpWidthDevice = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    m_lastVpHeightDevice = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    updateLayoutGeometry();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);

    Q_EMIT documentLoaded(QFileInfo(filePath).fileName(), m_pageCount);
    Q_EMIT pageChanged(1, m_pageCount);
    Q_EMIT zoomChanged(1.0);

    viewport()->update();
}

pdf::DocumentError PdfViewerWidget::openDocument(const QString& filePath) {
    if (filePath.isEmpty()) {
        return pdf::DocumentError::FileNotFound;
    }

    // A synchronous open supersedes any asynchronous open that is pending or queued.
    cancelPendingOpen();

    const int64_t nextDocId = m_currentDocumentId + 1;
    // Open serially on the engine worker thread - 0 PDFium calls on UI thread!
    auto openResult = m_renderWorker.openDocumentSync(nextDocId, filePath.toStdString());
    if (openResult.error != pdf::DocumentError::None) {
        Q_EMIT loadError(openErrorMessage(openResult.error, filePath));
        return openResult.error; // Existing document state remains intact on failure
    }

    applyOpenedDocument(nextDocId, filePath, openResult);
    return pdf::DocumentError::None;
}

void PdfViewerWidget::openDocumentAsync(const QString& filePath, const QString& password) {
    if (filePath.isEmpty()) {
        Q_EMIT loadError(openErrorMessage(pdf::DocumentError::FileNotFound, filePath));
        return;
    }
    if (m_loading) {
        // Coalesce: only the most recent request made during a load is opened afterwards
        m_queuedOpenPath = filePath;
        m_queuedOpenPassword = password;
        m_hasQueuedOpen = true;
        return;
    }
    startAsyncOpen(filePath, password);
}

void PdfViewerWidget::startAsyncOpen(const QString& filePath, const QString& password) {
    m_loading = true;
    m_pendingOpenId = m_currentDocumentId + 1;
    m_pendingOpenPath = filePath;
    Q_EMIT loadingChanged(true, QFileInfo(filePath).fileName());
    viewport()->update();
    m_renderWorker.openDocumentAsync(m_pendingOpenId, filePath.toStdString(), password.toStdString());
}

void PdfViewerWidget::cancelPendingOpen() {
    const bool wasLoading = m_loading;
    const QString name = QFileInfo(m_pendingOpenPath).fileName();
    m_loading = false;
    m_pendingOpenId = 0; // any result still in flight is ignored by onDocumentOpened()
    m_pendingOpenPath.clear();
    m_hasQueuedOpen = false;
    m_queuedOpenPath.clear();
    m_queuedOpenPassword.clear();
    if (wasLoading) {
        Q_EMIT loadingChanged(false, name);
        viewport()->update();
    }
}

void PdfViewerWidget::onDocumentOpened(int64_t documentId, pdf::OpenResult result) {
    if (!m_loading || documentId != m_pendingOpenId) {
        return; // cancelled by closeDocument()/openDocument() while the load was running
    }

    const QString path = m_pendingOpenPath;
    m_loading = false;
    m_pendingOpenId = 0;
    m_pendingOpenPath.clear();

    if (result.error != pdf::DocumentError::None) {
        Q_EMIT documentOpenFailed(path, result.error);
        Q_EMIT loadError(openErrorMessage(result.error, path));
    } else {
        applyOpenedDocument(documentId, path, result);
    }

    if (m_hasQueuedOpen) {
        const QString next = m_queuedOpenPath;
        const QString nextPassword = m_queuedOpenPassword;
        m_hasQueuedOpen = false;
        m_queuedOpenPath.clear();
        m_queuedOpenPassword.clear();
        startAsyncOpen(next, nextPassword); // stays "loading"; no intermediate loadingChanged(false)
        return;
    }

    Q_EMIT loadingChanged(false, QFileInfo(path).fileName());
    viewport()->update();
}

void PdfViewerWidget::closeDocument() {
    cancelPendingOpen();
    // Close document and page cache serially on worker thread
    m_renderWorker.closeDocumentSync();

    m_cache.invalidateDocument(m_currentDocumentId);
    m_inFlightKeys.clear();
    m_pageTextCache.clear();
    clearSelection();
    ++m_currentDocumentId;
    ++m_currentGeneration;

    m_hasDocument = false;
    m_fallbackScales.clear();
    m_pageCount = 0;
    m_layout.setPageSizes({});
    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;
    m_lastVpWidthDevice = 0;
    m_lastVpHeightDevice = 0;

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
    if (!isDocumentOpen()) {
        return;
    }
    if (pageIndex < 0 || pageIndex >= pageCount()) {
        return;
    }

    const auto& pl = m_layout.getPageLayout(pageIndex);

    m_navigating = true;
    setDocScrollPosition(m_docScrollX, pl.topPixel);
    m_navigating = false;

    if (m_currentPage != pageIndex) {
        m_currentPage = pageIndex;
        Q_EMIT pageChanged(m_currentPage + 1, pageCount());
    }

    viewport()->update();
}

void PdfViewerWidget::setZoom(double zoom, const QPoint* anchorWidgetPos) {
    if (!isDocumentOpen()) {
        return;
    }

    // Strict zoom bounds: 10% to 1000%
    zoom = std::clamp(zoom, 0.1, 10.0);
    if (std::abs(zoom - m_scale) < 1e-4) {
        return;
    }

    rememberFallbackScale(pdf::quantizeScale(m_scale));

    const double dpr = effectiveDpr();
    const double qOldScale = pdf::quantizeScale(m_scale);
    const double qNewScale = pdf::quantizeScale(zoom);

    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    // Determine 2D anchor point
    int64_t anchorDocX = 0;
    int64_t anchorDocY = 0;
    int64_t widgetAnchorX = vpW / 2;
    int64_t widgetAnchorY = vpH / 2;

    if (anchorWidgetPos) {
        widgetAnchorX = static_cast<int64_t>(std::round(anchorWidgetPos->x() * dpr));
        widgetAnchorY = static_cast<int64_t>(std::round(anchorWidgetPos->y() * dpr));
        anchorDocX = m_docScrollX + widgetAnchorX;
        anchorDocY = m_docScrollY + widgetAnchorY;
    } else {
        anchorDocX = m_docScrollX + widgetAnchorX;
        anchorDocY = m_docScrollY + widgetAnchorY;
    }

    auto anchor = m_layout.createAnchor(anchorDocX, anchorDocY);

    m_scale = zoom;

    // Advance generation on zoom changes to prune obsolete tile renders immediately
    if (qNewScale != qOldScale) {
        ++m_currentGeneration;
        m_inFlightKeys.clear();
        m_renderWorker.setGeneration(m_currentGeneration);
    }

    m_layout.updateLayout(qNewScale, dpr, vpW);
    updateScrollBars();

    int64_t newScrollX = 0;
    int64_t newScrollY = 0;
    m_layout.resolveAnchor(anchor, widgetAnchorX, widgetAnchorY, vpW, vpH, newScrollX, newScrollY);

    setDocScrollPosition(newScrollX, newScrollY);

    Q_EMIT zoomChanged(m_scale / DESKTOP_100_PERCENT_SCALE);
    viewport()->update();
}

void PdfViewerWidget::zoomIn() {
    setZoom(m_scale * 1.25);
}

void PdfViewerWidget::zoomOut() {
    setZoom(m_scale / 1.25);
}

void PdfViewerWidget::zoomReset() {
    setZoom(DESKTOP_100_PERCENT_SCALE);
}

void PdfViewerWidget::zoomFitWidth() {
    if (!isDocumentOpen() || pageCount() == 0) {
        return;
    }

    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    constexpr int64_t HORIZONTAL_MARGIN = 16;
    const int64_t availableWidth = vpW - (2 * HORIZONTAL_MARGIN);

    if (availableWidth <= 0) {
        return;
    }

    double maxPageWidthPoints = 0.0;
    for (int i = 0; i < pageCount(); ++i) {
        maxPageWidthPoints = std::max(maxPageWidthPoints, m_layout.getPageLayout(i).widthPoints);
    }

    if (maxPageWidthPoints > 0.0) {
        double fitScale = static_cast<double>(availableWidth) / (maxPageWidthPoints * dpr);
        setZoom(fitScale);
    }
}

void PdfViewerWidget::onScrollValueChanged() {
    if (m_navigating || !isDocumentOpen()) {
        return;
    }
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int64_t newScrollX = m_scrollMapper.sliderToDoc(horizontalScrollBar()->value(), m_layout.maxDocumentWidth(), vpW);
    int64_t newScrollY = m_scrollMapper.sliderToDoc(verticalScrollBar()->value(), m_layout.totalDocumentHeight(), vpH);

    bool scrollChanged = (newScrollX != m_docScrollX || newScrollY != m_docScrollY);
    if (scrollChanged) {
        m_docScrollX = newScrollX;
        m_docScrollY = newScrollY;

        // Advance request generation on scroll to prune off-screen queued work
        ++m_currentGeneration;
        m_inFlightKeys.clear();
        m_renderWorker.setGeneration(m_currentGeneration);
    }

    m_lastVpWidthDevice = vpW;
    m_lastVpHeightDevice = vpH;

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

void PdfViewerWidget::onTileReady(int64_t documentId, uint64_t /*generation*/, const pdf::TileKey& key, const pdf::PixelBuffer& buffer) {
    m_inFlightKeys.erase(key);
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_cache.put(key, buffer);
    // Prefetched off-screen tiles do not change what is on screen: skip the repaint
    if (m_cache.isPinned(key)) {
        viewport()->update();
    }
}

void PdfViewerWidget::onTileFailed(int64_t documentId, uint64_t /*generation*/, const pdf::TileKey& key) {
    m_inFlightKeys.erase(key);
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_cache.markFailed(key);
    viewport()->update();
}

void PdfViewerWidget::onPageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text) {
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_pageTextCache[pageIndex] = std::move(text);
    viewport()->update();
}

void PdfViewerWidget::ensurePageTextRequested(int pageIndex) {
    if (!isDocumentOpen() || pageIndex < 0 || pageIndex >= m_pageCount) {
        return;
    }
    if (m_pageTextCache.find(pageIndex) == m_pageTextCache.end()) {
        m_pageTextCache[pageIndex] = nullptr; // marked in-flight
        m_renderWorker.requestPageText(m_currentDocumentId, pageIndex);
    }
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

    m_lastVpWidthDevice = vpW;
    m_lastVpHeightDevice = vpH;

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
    if (!isDocumentOpen()) {
        return;
    }
    const double dpr = effectiveDpr();
    const double qScale = pdf::quantizeScale(m_scale);
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    const int64_t oldVpW = (m_lastVpWidthDevice > 0) ? m_lastVpWidthDevice : vpW;
    const int64_t oldVpH = (m_lastVpHeightDevice > 0) ? m_lastVpHeightDevice : vpH;

    int64_t curDocX = m_docScrollX + oldVpW / 2;
    int64_t curDocY = m_docScrollY + oldVpH / 2;
    auto anchor = m_layout.createAnchor(curDocX, curDocY);

    m_layout.updateLayout(qScale, dpr, vpW);
    updateScrollBars();

    int64_t newDocX = 0, newDocY = 0;
    m_layout.resolveAnchor(anchor, vpW / 2, vpH / 2, vpW, vpH, newDocX, newDocY);
    setDocScrollPosition(newDocX, newDocY);

    m_lastVpWidthDevice = vpW;
    m_lastVpHeightDevice = vpH;

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

void PdfViewerWidget::drawLoadingOverlay(QPainter& painter) {
    if (!m_loading) {
        return;
    }
    const QString text = tr("Opening %1\u2026").arg(QFileInfo(m_pendingOpenPath).fileName());
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() * 1.2);
    painter.setFont(font);

    const QFontMetrics fm(font);
    const QRect textRect = fm.boundingRect(text);
    QRect box(0, 0, textRect.width() + 40, textRect.height() + 24);
    box.moveCenter(viewport()->rect().center());

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 22, 24, 220));
    painter.drawRoundedRect(box, 8, 8);
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(box, Qt::AlignCenter, text);
}

void PdfViewerWidget::rememberFallbackScale(double previousScale) {
    m_fallbackScales.erase(std::remove(m_fallbackScales.begin(), m_fallbackScales.end(), previousScale), m_fallbackScales.end());
    m_fallbackScales.insert(m_fallbackScales.begin(), previousScale);
    if (m_fallbackScales.size() > MAX_FALLBACK_SCALES) {
        m_fallbackScales.resize(MAX_FALLBACK_SCALES);
    }
}

bool PdfViewerWidget::drawFallbackTile(QPainter& painter, const layout::VisibleTile& t, const QRectF& tileRect, double qScale, double qDpr) {
    constexpr int TILE = 512;
    constexpr int MAX_SOURCE_TILES = 64; // guards against huge zoom jumps touching thousands of old tiles
    bool drawnAny = false;

    painter.save();
    painter.setClipRect(tileRect);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

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

        const int c0 = static_cast<int>(std::floor(sx0 / TILE));
        const int c1 = static_cast<int>(std::floor((sx1 - 1e-6) / TILE));
        const int r0 = static_cast<int>(std::floor(sy0 / TILE));
        const int r1 = static_cast<int>(std::floor((sy1 - 1e-6) / TILE));
        if (c1 < c0 || r1 < r0 || static_cast<int64_t>(c1 - c0 + 1) * (r1 - r0 + 1) > MAX_SOURCE_TILES) {
            continue;
        }

        bool fullyCovered = true;
        for (int row = r0; row <= r1; ++row) {
            for (int col = c0; col <= c1; ++col) {
                auto buf = m_cache.get(pdf::TileKey::create(m_currentDocumentId, t.pageIndex, col, row, oldScale, qDpr));
                if (!buf.has_value()) {
                    fullyCovered = false;
                    continue;
                }
                const double ox = static_cast<double>(col) * TILE;
                const double oy = static_cast<double>(row) * TILE;
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
                painter.drawImage(dst, toQImage(*buf), src);
                drawnAny = true;
            }
        }
        if (fullyCovered) {
            break; // the most recent zoom level covers the whole tile; older ones would only be overdrawn
        }
    }

    painter.restore();
    if (drawnAny) {
        ++m_fallbackTilesDrawn;
    }
    return drawnAny;
}

void PdfViewerWidget::drawSelection(QPainter& painter, int pageIndex, const layout::PageLayout& pl, int64_t scrollDocX, int64_t scrollDocY, double effScale) {
    if (!hasSelection() || effScale <= 0.0) {
        return;
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    if (pageIndex < p0 || pageIndex > p1) {
        return;
    }

    auto it = m_pageTextCache.find(pageIndex);
    if (it == m_pageTextCache.end() || !it->second) {
        return;
    }
    const auto& pt = *it->second;

    int startChar = (pageIndex == p0) ? c0 : 0;
    int endChar = (pageIndex == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;

    auto rects = pt.getSelectionRects(startChar, endChar);
    QColor highlightColor(0, 120, 215, 90); // translucent selection overlay

    for (const auto& r : rects) {
        QRectF selRect(
            static_cast<qreal>(static_cast<double>(pl.leftPixel - scrollDocX) + r.left * effScale),
            static_cast<qreal>(static_cast<double>(pl.topPixel - scrollDocY) + r.top * effScale),
            static_cast<qreal>(r.width() * effScale),
            static_cast<qreal>(r.height() * effScale)
        );
        painter.fillRect(selRect, highlightColor);
    }
}

bool PdfViewerWidget::hasSelection() const noexcept {
    return m_selectionStart.isValid() && m_selectionEnd.isValid() &&
           (m_selectionStart.pageIndex != m_selectionEnd.pageIndex ||
            m_selectionStart.charIndex != m_selectionEnd.charIndex);
}

QString PdfViewerWidget::selectedText() const {
    if (!hasSelection()) {
        return QString();
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    QString result;
    for (int p = p0; p <= p1; ++p) {
        auto it = m_pageTextCache.find(p);
        if (it == m_pageTextCache.end() || !it->second) {
            continue;
        }
        const auto& pt = *it->second;
        int startChar = (p == p0) ? c0 : 0;
        int endChar = (p == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;
        auto selU16 = pt.getSelectedText(startChar, endChar);
        if (!selU16.empty()) {
            if (!result.isEmpty()) {
                result.append(QChar::LineFeed);
            }
            result.append(QString::fromStdU16String(selU16));
        }
    }
    return result;
}

void PdfViewerWidget::clearSelection() {
    if (m_selectionStart.isValid() || m_selectionEnd.isValid()) {
        m_selectionStart = {};
        m_selectionEnd = {};
        viewport()->update();
        Q_EMIT selectionChanged();
    }
}

void PdfViewerWidget::copySelection() {
    QString text = selectedText();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

std::shared_ptr<const pdf::PageText> PdfViewerWidget::getPageText(int pageIndex) const {
    auto it = m_pageTextCache.find(pageIndex);
    return (it != m_pageTextCache.end()) ? it->second : nullptr;
}

void PdfViewerWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));

        int page = m_layout.getPageAt(docX, docY);
        if (page >= 0) {
            double px = 0.0, py = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, px, py)) {
                auto it = m_pageTextCache.find(page);
                if (it != m_pageTextCache.end() && it->second) {
                    int ch = it->second->hitTest(px, py);
                    if (ch >= 0) {
                        m_selectionStart = {page, ch};
                        m_selectionEnd = {page, ch};
                        m_isSelecting = true;
                        viewport()->update();
                        Q_EMIT selectionChanged();
                        event->accept();
                        return;
                    }
                }
            }
        }
        clearSelection();
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void PdfViewerWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isSelecting && isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));

        int page = m_layout.getPageAt(docX, docY);
        if (page >= 0) {
            double px = 0.0, py = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, px, py)) {
                auto it = m_pageTextCache.find(page);
                if (it != m_pageTextCache.end() && it->second) {
                    int ch = it->second->hitTest(px, py);
                    if (ch >= 0) {
                        TextPosition newEnd{page, ch};
                        if (newEnd != m_selectionEnd) {
                            m_selectionEnd = newEnd;
                            viewport()->update();
                            Q_EMIT selectionChanged();
                        }
                        event->accept();
                        return;
                    }
                }
            }
        }
    } else if (isDocumentOpen()) {
        // Hover cursor: show IBeam if over text
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));

        int page = m_layout.getPageAt(docX, docY);
        bool overText = false;
        if (page >= 0) {
            double px = 0.0, py = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, px, py)) {
                auto it = m_pageTextCache.find(page);
                if (it != m_pageTextCache.end() && it->second) {
                    overText = (it->second->hitTest(px, py) >= 0);
                }
            }
        }
        viewport()->setCursor(overText ? Qt::IBeamCursor : Qt::ArrowCursor);
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PdfViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_isSelecting) {
        m_isSelecting = false;
        if (m_selectionStart == m_selectionEnd) {
            clearSelection();
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PdfViewerWidget::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PdfViewerWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), QColor("#323639"));

    if (!isDocumentOpen()) {
        drawLoadingOverlay(painter);
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
            ensurePageTextRequested(p);

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
            m_inFlightKeys.erase(key);
            QImage img = toQImage(*cachedBuf);
            painter.drawImage(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), img);
        } else if (m_cache.isFailed(key)) {
            if (m_cache.canRetry(key)) {
                if (m_inFlightKeys.find(key) != m_inFlightKeys.end()) {
                    // Already in flight to worker for retry; avoid re-enqueueing duplicate
                    drawPendingPlaceholder(painter, tileRect);
                } else {
                    // Backoff interval elapsed: allow retry
                    m_cache.clearFailed(key);
                    drawPendingPlaceholder(painter, tileRect);

                    int64_t tileMidX = tileVpX + t.tileWidth / 2;
                    int64_t tileMidY = tileVpY + t.tileHeight / 2;
                    int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
                    int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

                    pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
                    pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority});
                    m_inFlightKeys.insert(key);
                }
            } else {
                drawErrorPlaceholder(painter, tileRect);
            }
        } else {
            // Pending render miss: draw placeholder (plus a scaled earlier-zoom stand-in if we have one)
            // on the UI thread and queue request to worker if not already in flight
            drawPendingPlaceholder(painter, tileRect);
            if (!m_fallbackScales.empty()) {
                drawFallbackTile(painter, t, tileRect, qScale, qDpr);
            }

            if (m_inFlightKeys.find(key) == m_inFlightKeys.end()) {
                int64_t tileMidX = tileVpX + t.tileWidth / 2;
                int64_t tileMidY = tileVpY + t.tileHeight / 2;
                int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
                int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

                pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
                pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority});
                m_inFlightKeys.insert(key);
            }
        }
    }

    // 5. Prefetch ring: one viewport height above and below the visible area
    if (m_prefetchEnabled && !tiles.empty()) {
        constexpr size_t MAX_PREFETCH_PER_PAINT = 16;
        const int64_t ringTop = std::max<int64_t>(0, scrollDocY - vpHeightDevice);
        const int64_t ringBottom = std::min<int64_t>(m_layout.totalDocumentHeight(), scrollDocY + 2 * vpHeightDevice);
        if (ringBottom > ringTop) {
            std::unordered_set<pdf::TileKey, pdf::TileKeyHash> visibleSet(visibleKeys.begin(), visibleKeys.end());
            struct Candidate { int64_t distSq; layout::VisibleTile tile; pdf::TileKey key; };
            std::vector<Candidate> candidates;

            const auto ringTiles = m_layout.getVisibleTiles({scrollDocX, ringTop, vpWidthDevice, ringBottom - ringTop}, 512);
            for (const auto& t : ringTiles) {
                const auto key = pdf::TileKey::create(m_currentDocumentId, t.pageIndex, t.col, t.row, qScale, qDpr);
                if (visibleSet.count(key) || m_inFlightKeys.count(key) || m_cache.contains(key)) {
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
                pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, c.tile.pageIndex, pRect, qScale, qDpr, c.key, priority});
                m_inFlightKeys.insert(c.key);
                ++m_prefetchRequestsIssued;
            }
        }
    }

    if (!pendingRequests.empty()) {
        m_renderWorker.enqueueRequests(std::move(pendingRequests));
    }

    // 6. Draw selection highlights over rendered pages
    if (visPages.isValid() && hasSelection()) {
        const double effScale = qScale * qDpr;
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            const auto& pl = m_layout.getPageLayout(p);
            drawSelection(painter, p, pl, scrollDocX, scrollDocY, effScale);
        }
    }

    painter.restore();

    drawLoadingOverlay(painter);
}

} // namespace ui
