#include "ThumbnailListWidget.h"
#include "PdfViewerWidget.h"
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
    }
}

QPixmap ThumbnailListWidget::createPlaceholderPixmap(int width, int height, int pageNumber) const {
    QPixmap pixmap(width, height);
    pixmap.fill(QColor(245, 245, 245));

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(200, 200, 200), 1));
    painter.drawRect(0, 0, width - 1, height - 1);

    QFont font = painter.font();
    font.setPointSize(11);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor(160, 160, 160));
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
    requestVisibleThumbnails();
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
            it->setIcon(QIcon(QPixmap::fromImage(image)));
        }
    }
}

void ThumbnailListWidget::onThumbnailFailed(int pageIndex) {
    m_requestedPages.erase(pageIndex);
}

void ThumbnailListWidget::requestVisibleThumbnails() {
    if (!m_viewer || count() == 0 || !isVisible()) {
        return;
    }

    const QRect vpRect = viewport()->rect();
    if (!vpRect.isValid() || vpRect.width() <= 0 || vpRect.height() <= 0) {
        return;
    }

    const int gridWidth = std::max(1, gridSize().width());
    const int gridHeight = std::max(1, gridSize().height());
    int firstVisible = count();
    int lastVisible = -1;
    for (int y = 0; y < vpRect.height(); y += gridHeight) {
        for (int x = 0; x < vpRect.width(); x += gridWidth) {
            const QPoint samplePoint(std::min(x + gridWidth / 2, vpRect.width() - 1),
                                     std::min(y + gridHeight / 2, vpRect.height() - 1));
            const QModelIndex index = indexAt(samplePoint);
            if (index.isValid()) {
                firstVisible = std::min(firstVisible, index.row());
                lastVisible = std::max(lastVisible, index.row());
            }
        }
    }
    if (lastVisible < firstVisible) {
        return;
    }

    const int columns = std::max(1, (vpRect.width() + gridWidth - 1) / gridWidth);
    const int prefetchItems = columns * (2 + 1);
    const int firstIndex = std::max(0, firstVisible - prefetchItems);
    const int lastIndex = std::min(count() - 1, lastVisible + prefetchItems);

    for (int i = firstIndex; i <= lastIndex; ++i) {
        auto* it = item(i);
        const QRect rect = visualItemRect(it);
        if (rect.intersects(vpRect.adjusted(0, -250, 0, 250))) {
            const int pageIndex = it->data(Qt::UserRole).toInt();
            if (m_loadedPages.find(pageIndex) == m_loadedPages.end() &&
                m_requestedPages.insert(pageIndex).second) {
                m_viewer->requestThumbnail(pageIndex, 130);
            }
        }
    }
}

} // namespace ui
