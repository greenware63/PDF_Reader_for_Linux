#pragma once

#include <QMainWindow>
#include <QLabel>
#include "PdfViewerWidget.h"

namespace ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override = default;

    /// Starts a non-blocking open. @return false only if the path is empty; load errors arrive via loadError().
    bool openFile(const QString& filePath);

public Q_SLOTS:
    void openFileDialog();

private Q_SLOTS:
    void onDocumentLoaded(const QString& title, int totalPages);
    void onPageChanged(int currentPage, int totalPages);
    void onZoomChanged(double zoomRatio);
    void onLoadError(const QString& errorMessage);
    void onLoadingChanged(bool loading, const QString& fileName);

private:
    void setupUi();
    void setupActions();

    PdfViewerWidget* m_viewer{nullptr};
    QLabel* m_pageStatusLabel{nullptr};
    QLabel* m_zoomStatusLabel{nullptr};
};

} // namespace ui
