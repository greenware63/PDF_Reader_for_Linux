#include "ThumbnailListWidget.h"
#include "PdfViewerWidget.h"
#include "ImageUtils.h"
#include "ColorMode.h"
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ui {

ThumbnailListWidget::ThumbnailListWidget(QWidget* parent)
    : QListWidget(parent) {
    setObjectName(QStringLiteral("ThumbnailListWidget"));
    setViewMode(QListView::IconMode);
    setIconSize(QSize(130, 170));
    setGridSize(QSize(150, 205));
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setWordWrap(true);
    setSpacing(8);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    setStyleSheet(QStringLiteral(
        "QListWidget {"
        "    background-color: #2b2b2b;"
        "    border: none;"
        "    color: #e0e0e0;"
        "    outline: none;"
        "}"
        "QListWidget::item {"
        "    color: #cccccc;"
        "    border: 2px solid transparent;"
        "    border-radius: 4px;"
        "    padding: 4px;"
        "}"
        "QListWidget::item:hover {"
        "    background-color: #383838;"
        "    border-color: #555555;"
        "}"
        "QListWidget::item:selected {"
        "    background-color: #1e3a5f;"
        "    border-color: #3574d4;"
        "    color: #ffffff;"
        "}"
    ));

    connect(this, &QListWidget::itemClicked, this, &ThumbnailListWidget::onItemClicked);

    if (verticalScrollBar()) {
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
            QTimer::singleShot(10, this, &ThumbnailListWidget::requestVisibleThumbnails);
        });
    }
}

void ThumbnailListWidget::setViewer(PdfViewerWidget* viewer) {
    if (m_viewer == viewer) {
        return;
    }
    if (m_viewer) {
        disconnect(m_viewer, nullptr, this, nullptr);
    }
    m_viewer = viewer;
    if (m_viewer) {
        connect(m_viewer, &PdfViewerWidget::thumbnailReady, this, &ThumbnailListWidget::onThumbnailReady);
        connect(m_viewer, &PdfViewerWidget::thumbnailFailed, this, &ThumbnailListWidget::onThumbnailFailed);
        connect(m_viewer, &PdfViewerWidget::rotationChanged, this, [this](int) {
            m_loadedPages.clear();
            m_requestedPages.clear();
            if (isVisible()) {
                requestVisibleThumbnails();
            }
        });
        connect(m_viewer, &PdfViewerWidget::colorModeChanged, this, [this](ColorMode) {
            m_loadedPages.clear();
            m_requestedPages.clear();
            if (isVisible()) {
                requestVisibleThumbnails();
            }
        });
    }
}

QPixmap ThumbnailListWidget::createPlaceholderPixmap(int width, int height, int pageNumber) const {
    QPixmap pixmap(width, height);
    const bool isDark = m_viewer ? m_viewer->isDarkMode() : false;
    const bool isSepia = m_viewer ? (m_viewer->colorMode() == ColorMode::Sepia) : false;

    QColor bgColor(245, 245, 245);
    QColor borderColor(200, 200, 200);
    QColor textColor(160, 160, 160);

    if (isDark) {
        bgColor = QColor(35, 35, 35);
        borderColor = QColor(60, 60, 60);
        textColor = QColor(140, 140, 140);
    } else if (isSepia) {
        bgColor = QColor(245, 235, 215);
        borderColor = QColor(210, 195, 170);
        textColor = QColor(140, 120, 95);
    }

    pixmap.fill(bgColor);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(borderColor, 1));
    painter.drawRect(0, 0, width - 1, height - 1);

    QFont font = painter.font();
    font.setPointSize(11);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(textColor);
    painter.drawText(QRect(0, 0, width, height), Qt::AlignCenter, QString::number(pageNumber));

    return pixmap;
}

void ThumbnailListWidget::setDocument(int totalPages, const std::vector<QString>& labels) {
    clearDocument();
    m_totalPages = totalPages;
    m_labels = labels;

    if (m_totalPages <= 0) {
        return;
    }

    QPixmap placeholder = createPlaceholderPixmap(130, 170, 1);
    for (int i = 0; i < m_totalPages; ++i) {
        QString labelText;
        if (i < static_cast<int>(m_labels.size()) && !m_labels[static_cast<size_t>(i)].isEmpty()) {
            labelText = m_labels[static_cast<size_t>(i)];
        } else {
            labelText = QString::number(i + 1);
        }

        auto* item = new QListWidgetItem(QIcon(placeholder), labelText, this);
        item->setData(Qt::UserRole, i);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    }

    QTimer::singleShot(20, this, &ThumbnailListWidget::requestVisibleThumbnails);
}

void ThumbnailListWidget::clearDocument() {
    clear();
    m_totalPages = 0;
    m_labels.clear();
    m_requestedPages.clear();
    m_loadedPages.clear();
}

void ThumbnailListWidget::setCurrentPage(int pageIndex) {
    if (pageIndex < 0 || pageIndex >= count() || currentRow() == pageIndex) {
        return;
    }

    m_programmaticSelection = true;
    setCurrentRow(pageIndex);
    if (auto* current = item(pageIndex)) {
        scrollToItem(current, QAbstractItemView::EnsureVisible);
    }
    m_programmaticSelection = false;
    if (isVisible()) {
        requestVisibleThumbnails();
    }
}

bool ThumbnailListWidget::isThumbnailLoaded(int pageIndex) const {
    return m_loadedPages.find(pageIndex) != m_loadedPages.end();
}

void ThumbnailListWidget::resizeEvent(QResizeEvent* event) {
    QListWidget::resizeEvent(event);
    QTimer::singleShot(20, this, &ThumbnailListWidget::requestVisibleThumbnails);
}

void ThumbnailListWidget::showEvent(QShowEvent* event) {
    QListWidget::showEvent(event);
    QTimer::singleShot(20, this, &ThumbnailListWidget::requestVisibleThumbnails);
}

void ThumbnailListWidget::onItemClicked(QListWidgetItem* item) {
    if (!item || m_programmaticSelection) {
        return;
    }
    int pageIndex = item->data(Qt::UserRole).toInt();
    if (m_viewer) {
        m_viewer->goToPage(pageIndex);
    }
}

void ThumbnailListWidget::onThumbnailReady(int pageIndex, const QImage& image) {
    m_loadedPages.insert(pageIndex);
    if (pageIndex >= 0 && pageIndex < count()) {
        if (auto* it = item(pageIndex)) {
            const QImage transformed = m_viewer ? applyColorMode(image, m_viewer->colorMode()) : image;
            it->setIcon(QIcon(QPixmap::fromImage(transformed)));
        }
    }
}

void ThumbnailListWidget::onThumbnailFailed(int pageIndex) {
    m_requestedPages.erase(pageIndex);
}

void ThumbnailListWidget::requestVisibleThumbnails() {
    if (!m_viewer || count() == 0) {
        return;
    }

    const QRect vpRect = viewport()->rect();
    const bool hasLayout = vpRect.isValid() && vpRect.width() > 0 && vpRect.height() > 0;
    const QRect prefetchRect = hasLayout
        ? vpRect.adjusted(0, -250, 0, 250)
        : QRect(0, 0, 200, 800);

    int startIndex = 0;
    int endIndex = count() - 1;

    if (hasLayout && verticalScrollBar()) {
        const int itemH = std::max(1, gridSize().height() > 0 ? gridSize().height() : 205);
        const int itemW = std::max(1, gridSize().width() > 0 ? gridSize().width() : 150);
        const int cols = std::max(1, vpRect.width() / itemW);
        const int scrollY = verticalScrollBar()->value();
        const int startRow = std::max(0, (scrollY - 300) / itemH);
        const int endRow = (scrollY + vpRect.height() + 300) / itemH + 1;
        startIndex = std::clamp(startRow * cols, 0, count() - 1);
        endIndex = std::clamp(endRow * cols + cols, startIndex, count() - 1);
    }

    for (int i = startIndex; i <= endIndex; ++i) {
        auto* it = item(i);
        if (!it) {
            continue;
        }
        const QRect rect = visualItemRect(it);
        if (!hasLayout || rect.intersects(prefetchRect)) {
            const int pageIndex = it->data(Qt::UserRole).toInt();
            if (m_loadedPages.find(pageIndex) == m_loadedPages.end() &&
                m_requestedPages.insert(pageIndex).second) {
                m_viewer->requestThumbnail(pageIndex, 130);
            }
        }
    }
}

} // namespace ui
