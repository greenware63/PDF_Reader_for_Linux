#include "PdfViewerWidget.h"
#include "ImageUtils.h"
#include <QWheelEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <algorithm>

namespace ui {

// Baseline desktop 100% scale (96 DPI screen / 72 DPI PDF points)
static constexpr double DESKTOP_100_PERCENT_SCALE = 96.0 / 72.0;

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QScrollArea(parent), m_zoom(DESKTOP_100_PERCENT_SCALE) {
    m_document = pdf::createDocument();

    m_pageLabel = new QLabel(this);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_pageLabel->setStyleSheet("background-color: #525659; padding: 16px;");

    setWidget(m_pageLabel);
    setAlignment(Qt::AlignCenter);
    setStyleSheet("QScrollArea { background-color: #323639; border: none; }");
}

int PdfViewerWidget::pageCount() const {
    return m_document ? m_document->pageCount() : 0;
}

pdf::DocumentError PdfViewerWidget::openDocument(const QString& filePath) {
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

    // Document loaded successfully - safely release previous page before attaching new
    m_activePage.reset();
    m_currentPage = 0;
    m_zoom = DESKTOP_100_PERCENT_SCALE;
    m_activePage = m_document->getPage(m_currentPage);

    Q_EMIT documentLoaded(QFileInfo(filePath).fileName(), pageCount());
    Q_EMIT pageChanged(m_currentPage + 1, pageCount());
    Q_EMIT zoomChanged(m_zoom / DESKTOP_100_PERCENT_SCALE);

    updateRender();
    return pdf::DocumentError::None;
}

void PdfViewerWidget::closeDocument() {
    m_activePage.reset();
    if (m_document) {
        m_document->close();
    }
    m_pageLabel->clear();
    m_currentPage = 0;
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
    m_activePage.reset();
    m_currentPage = pageIndex;
    m_activePage = m_document->getPage(m_currentPage);
    updateRender();
    Q_EMIT pageChanged(m_currentPage + 1, pageCount());
}

void PdfViewerWidget::setZoom(double zoom) {
    double clamped = std::clamp(zoom, 0.2, 8.0);
    if (std::abs(clamped - m_zoom) > 0.001) {
        m_zoom = clamped;
        updateRender();
        Q_EMIT zoomChanged(m_zoom / DESKTOP_100_PERCENT_SCALE);
    }
}

void PdfViewerWidget::zoomIn() {
    setZoom(m_zoom * 1.2);
}

void PdfViewerWidget::zoomOut() {
    setZoom(m_zoom / 1.2);
}

void PdfViewerWidget::zoomReset() {
    setZoom(DESKTOP_100_PERCENT_SCALE);
}

void PdfViewerWidget::zoomFitWidth() {
    if (!m_activePage || m_activePage->widthPoints() <= 0.0) {
        return;
    }
    int viewportW = viewport()->width() - 40;
    if (viewportW > 50) {
        setZoom(static_cast<double>(viewportW) / m_activePage->widthPoints());
    }
}

void PdfViewerWidget::updateRender() {
    if (!m_activePage) {
        m_pageLabel->clear();
        return;
    }

    const double dpr = devicePixelRatioF();
    pdf::PixelBuffer buffer = m_activePage->renderWholePage(m_zoom, dpr);
    if (buffer.isValid()) {
        const double targetLogicalW = m_activePage->widthPoints() * m_zoom;
        const double effectiveDpr = static_cast<double>(buffer.width()) / targetLogicalW;
        QImage rendered = toQImage(buffer, effectiveDpr);
        QPixmap pixmap = QPixmap::fromImage(rendered);
        pixmap.setDevicePixelRatio(rendered.devicePixelRatio());
        m_pageLabel->setPixmap(pixmap);
        m_pageLabel->adjustSize();
    }
}

void PdfViewerWidget::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
}

void PdfViewerWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        m_wheelDeltaAccumulator += event->angleDelta().y();
        constexpr int DELTA_THRESHOLD = 120; // 1 mouse wheel click standard

        while (m_wheelDeltaAccumulator >= DELTA_THRESHOLD) {
            zoomIn();
            m_wheelDeltaAccumulator -= DELTA_THRESHOLD;
        }
        while (m_wheelDeltaAccumulator <= -DELTA_THRESHOLD) {
            zoomOut();
            m_wheelDeltaAccumulator += DELTA_THRESHOLD;
        }
        event->accept();
    } else {
        QScrollArea::wheelEvent(event);
    }
}

} // namespace ui
