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

    bool openFile(const QString& filePath);

public Q_SLOTS:
    void openFileDialog();

private Q_SLOTS:
    void onDocumentLoaded(const QString& title, int totalPages);
    void onPageChanged(int currentPage, int totalPages);
    void onZoomChanged(double zoom);

private:
    void setupUi();
    void setupActions();

    PdfViewerWidget* m_viewer{nullptr};
    QLabel* m_pageStatusLabel{nullptr};
    QLabel* m_zoomStatusLabel{nullptr};
};

} // namespace ui
