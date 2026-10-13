#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QLabel>
#include <memory>
#include "engine/IDocument.h"

namespace ui {

class PdfViewerWidget : public QScrollArea {
    Q_OBJECT

public:
    explicit PdfViewerWidget(QWidget* parent = nullptr);
    ~PdfViewerWidget() override = default;

    bool openDocument(const QString& filePath);
    void closeDocument();

    int currentPage() const { return m_currentPage; }
    int pageCount() const;
    double currentZoom() const { return m_zoom; }

public Q_SLOTS:
    void nextPage();
    void previousPage();
    void goToPage(int pageIndex);
    void setZoom(double zoom);
    void zoomIn();
    void zoomOut();
    void zoomFitWidth();
    void zoomReset();

Q_SIGNALS:
    void pageChanged(int currentPage, int totalPages);
    void zoomChanged(double zoom);
    void documentLoaded(const QString& title, int totalPages);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void updateRender();

    std::unique_ptr<pdf::IDocument> m_document;
    std::unique_ptr<pdf::IPage> m_activePage;

    QLabel* m_pageLabel{nullptr};
    int m_currentPage{0};
    double m_zoom{1.0}; // 1.0 = 100%
};

} // namespace ui
