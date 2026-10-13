#include "PdfViewerWidget.h"
#include <QWheelEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <algorithm>

namespace ui {

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QScrollArea(parent) {
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

bool PdfViewerWidget::openDocument(const QString& filePath) {
    if (!m_document) {
        m_document = pdf::createDocument();
    }

    if (!m_document->loadFromFile(filePath.toStdString())) {
        return false;
    }

    m_currentPage = 0;
    m_zoom = 1.0;
    m_activePage = m_document->getPage(m_currentPage);

    Q_EMIT documentLoaded(QFileInfo(filePath).fileName(), pageCount());
    Q_EMIT pageChanged(m_currentPage + 1, pageCount());
    Q_EMIT zoomChanged(m_zoom);

    updateRender();
    return true;
}

void PdfViewerWidget::closeDocument() {
    if (m_document) {
        m_document->close();
    }
    m_activePage.reset();
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
    m_currentPage = pageIndex;
    m_activePage = m_document->getPage(m_currentPage);
    updateRender();
    Q_EMIT pageChanged(m_currentPage + 1, pageCount());
}

void PdfViewerWidget::setZoom(double zoom) {
    double clamped = std::clamp(zoom, 0.25, 5.0);
    if (std::abs(clamped - m_zoom) > 0.001) {
        m_zoom = clamped;
        updateRender();
        Q_EMIT zoomChanged(m_zoom);
    }
}

void PdfViewerWidget::zoomIn() {
    setZoom(m_zoom * 1.25);
}

void PdfViewerWidget::zoomOut() {
    setZoom(m_zoom / 1.25);
}

void PdfViewerWidget::zoomReset() {
    setZoom(1.0);
}

void PdfViewerWidget::zoomFitWidth() {
    if (!m_activePage || m_activePage->widthPoints() <= 0.0) {
        return;
    }
    // Margin of 40px for scrollbars and padding
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
    QImage rendered = m_activePage->render(m_zoom, dpr);
    if (!rendered.isNull()) {
        QPixmap pixmap = QPixmap::fromImage(rendered);
        pixmap.setDevicePixelRatio(dpr);
        m_pageLabel->setPixmap(pixmap);
        m_pageLabel->adjustSize();
    }
}

void PdfViewerWidget::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
}

void PdfViewerWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() > 0) {
            zoomIn();
        } else if (event->angleDelta().y() < 0) {
            zoomOut();
        }
        event->accept();
    } else {
        QScrollArea::wheelEvent(event);
    }
}

} // namespace ui
