#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include "engine/IDocument.h"
#include "PdfViewerWidget.h"

class QDialog;
class QLineEdit;
class QLabel;

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
    void onDocumentOpenFailed(const QString& filePath, pdf::DocumentError error);

private:
    void setupUi();
    void setupActions();
    void showPasswordDialog(const QString& filePath, bool incorrectPassword);
    void closePasswordDialog();

    PdfViewerWidget* m_viewer{nullptr};
    QLabel* m_pageStatusLabel{nullptr};
    QLabel* m_zoomStatusLabel{nullptr};
    QPointer<QDialog> m_passwordDialog;
    QLineEdit* m_passwordInput{nullptr};
    QLabel* m_passwordWarning{nullptr};
    QString m_passwordFilePath;
};

} // namespace ui
