#pragma once

#include <QListWidget>
#include <QPointer>
#include <vector>
#include <unordered_set>

namespace ui {

class PdfViewerWidget;

/**
 * @brief High-performance, lazy-loading thumbnail sidebar list for document pages.
 * Displays miniature page previews with PDF page labels and two-way viewport synchronization.
 */
class ThumbnailListWidget : public QListWidget {
    Q_OBJECT

public:
    explicit ThumbnailListWidget(QWidget* parent = nullptr);
    ~ThumbnailListWidget() override = default;

    void setViewer(PdfViewerWidget* viewer);
    void setDocument(int totalPages, const std::vector<QString>& labels);
    void clearDocument();
    void setCurrentPage(int pageIndex);

    int totalPages() const noexcept { return m_totalPages; }
    bool isThumbnailLoaded(int pageIndex) const;

public Q_SLOTS:
    void requestVisibleThumbnails();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void onItemClicked(QListWidgetItem* item);
    void onThumbnailReady(int pageIndex, const QImage& image);
    void onThumbnailFailed(int pageIndex);

private:
    QPixmap createPlaceholderPixmap(int width, int height, int pageNumber) const;

    QPointer<PdfViewerWidget> m_viewer;
    int m_totalPages{0};
    std::vector<QString> m_labels;
    std::unordered_set<int> m_requestedPages;
    std::unordered_set<int> m_loadedPages;
    bool m_programmaticSelection{false};
};

} // namespace ui
