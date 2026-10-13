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
static constexpr int SCROLL_INT_MAX = 1000000000;

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QAbstractScrollArea(parent),
      m_layout(),
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
    if (m_document && m_document->isOpen()) {
        closeDocument();
    }
    if (!m_document) {
        m_document = pdf::createDocument();
    }

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
        return err;
    }

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
    Q_EMIT zoomChanged(m_scale / DESKTOP_100_PERCENT_SCALE);

    viewport()->update();
    return pdf::DocumentError::None;
}

void PdfViewerWidget::closeDocument() {
    if (m_document && m_document->isOpen()) {
        m_cache.invalidateDocument(m_currentDocumentId);
        m_document->close();
    }
    ++m_currentDocumentId;
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
    verticalScrollBar()->setValue(docYToScrollBar(targetDocY));
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
    Q_EMIT zoomChanged(m_scale / DESKTOP_100_PERCENT_SCALE);
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
    int64_t availW = static_cast<int64_t>(std::round(viewport()->width() * dpr)) - 32;
    if (availW > 50) {
        setZoom((static_cast<double>(availW) / dpr) / pagePtW);
    }
}

void PdfViewerWidget::onScrollValueChanged() {
    if (!m_document || !m_document->isOpen() || pageCount() == 0) {
        return;
    }
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));
    const int64_t scrollDocY = currentDocScrollY();

    auto visPages = m_layout.getVisiblePages(scrollDocY, vpH);
    if (visPages.isValid() && visPages.firstVisiblePage != m_currentPage) {
        m_currentPage = visPages.firstVisiblePage;
        Q_EMIT pageChanged(m_currentPage + 1, pageCount());
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

int64_t PdfViewerWidget::maxScrollDocY() const {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));
    return std::max<int64_t>(0, m_layout.totalDocumentHeight() - vpH);
}

int64_t PdfViewerWidget::maxScrollDocX() const {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    return std::max<int64_t>(0, m_layout.maxDocumentWidth() - vpW);
}

int64_t PdfViewerWidget::scrollBarToDocY(int val) const {
    const int64_t maxY = maxScrollDocY();
    if (maxY <= SCROLL_INT_MAX) {
        return val;
    }
    return static_cast<int64_t>(std::round((static_cast<double>(val) / SCROLL_INT_MAX) * maxY));
}

int PdfViewerWidget::docYToScrollBar(int64_t docY) const {
    const int64_t maxY = maxScrollDocY();
    if (maxY <= 0) {
        return 0;
    }
    if (maxY <= SCROLL_INT_MAX) {
        return static_cast<int>(std::clamp<int64_t>(docY, 0, maxY));
    }
    double fraction = static_cast<double>(std::clamp<int64_t>(docY, 0, maxY)) / maxY;
    return static_cast<int>(std::round(fraction * SCROLL_INT_MAX));
}

int64_t PdfViewerWidget::scrollBarToDocX(int val) const {
    const int64_t maxX = maxScrollDocX();
    if (maxX <= SCROLL_INT_MAX) {
        return val;
    }
    return static_cast<int64_t>(std::round((static_cast<double>(val) / SCROLL_INT_MAX) * maxX));
}

int PdfViewerWidget::docXToScrollBar(int64_t docX) const {
    const int64_t maxX = maxScrollDocX();
    if (maxX <= 0) {
        return 0;
    }
    if (maxX <= SCROLL_INT_MAX) {
        return static_cast<int>(std::clamp<int64_t>(docX, 0, maxX));
    }
    double fraction = static_cast<double>(std::clamp<int64_t>(docX, 0, maxX)) / maxX;
    return static_cast<int>(std::round(fraction * SCROLL_INT_MAX));
}

int64_t PdfViewerWidget::currentDocScrollX() const {
    return scrollBarToDocX(horizontalScrollBar()->value());
}

int64_t PdfViewerWidget::currentDocScrollY() const {
    return scrollBarToDocY(verticalScrollBar()->value());
}

void PdfViewerWidget::setDocScrollPosition(int64_t docX, int64_t docY) {
    verticalScrollBar()->setValue(docYToScrollBar(docY));
    horizontalScrollBar()->setValue(docXToScrollBar(docX));
}

void PdfViewerWidget::updateScrollBars() {
    const double dpr = pdf::quantizeDpr(devicePixelRatioF());
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    const int64_t maxY = maxScrollDocY();
    const int64_t maxX = maxScrollDocX();

    QSignalBlocker blockV(verticalScrollBar());
    QSignalBlocker blockH(horizontalScrollBar());

    if (maxY <= SCROLL_INT_MAX) {
        verticalScrollBar()->setRange(0, static_cast<int>(maxY));
        verticalScrollBar()->setPageStep(static_cast<int>(std::min<int64_t>(vpH, maxY)));
        verticalScrollBar()->setSingleStep(static_cast<int>(std::round(40 * dpr)));
    } else {
        verticalScrollBar()->setRange(0, SCROLL_INT_MAX);
        int pageStep = static_cast<int>(std::round((static_cast<double>(vpH) / maxY) * SCROLL_INT_MAX));
        verticalScrollBar()->setPageStep(std::max(1, pageStep));
        verticalScrollBar()->setSingleStep(std::max(1, static_cast<int>(std::round((40.0 * dpr / maxY) * SCROLL_INT_MAX))));
    }

    if (maxX <= SCROLL_INT_MAX) {
        horizontalScrollBar()->setRange(0, static_cast<int>(maxX));
        horizontalScrollBar()->setPageStep(static_cast<int>(std::min<int64_t>(vpW, maxX)));
        horizontalScrollBar()->setSingleStep(static_cast<int>(std::round(40 * dpr)));
    } else {
        horizontalScrollBar()->setRange(0, SCROLL_INT_MAX);
        int pageStep = static_cast<int>(std::round((static_cast<double>(vpW) / maxX) * SCROLL_INT_MAX));
        horizontalScrollBar()->setPageStep(std::max(1, pageStep));
        horizontalScrollBar()->setSingleStep(std::max(1, static_cast<int>(std::round((40.0 * dpr / maxX) * SCROLL_INT_MAX))));
    }
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

    // Scale painter coordinates so all operations map directly 1:1 into device pixels
    painter.save();
    painter.scale(1.0 / dpr, 1.0 / dpr);

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
            // Synchronous UI thread render miss (Phase 2)
            auto page = m_document->getPage(t.pageIndex);
            if (!page) {
                m_cache.markFailed(key);
                drawErrorPlaceholder(painter, tileRect);
                continue;
            }

            pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
            pdf::PixelBuffer buffer = page->renderTile(pRect, qScale, qDpr);
            bool kept = m_cache.put(key, buffer);
            if (!kept || !buffer.isValid() || buffer.data() == nullptr) {
                drawErrorPlaceholder(painter, tileRect);
            } else {
                QImage img = toQImage(buffer);
                painter.drawImage(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), img);
            }
        }
    }

    painter.restore();
}

} // namespace ui
