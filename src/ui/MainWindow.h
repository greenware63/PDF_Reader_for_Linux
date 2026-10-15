#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include "engine/IDocument.h"
#include "PdfViewerWidget.h"
#include "FindBar.h"

class QDialog;
class QLineEdit;
class QLabel;
class QDockWidget;
class QTreeView;
class QStandardItemModel;
class QStackedWidget;
class QModelIndex;

namespace ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Starts a non-blocking open. @return false only if the path is empty; load errors arrive via loadError().
    bool openFile(const QString& filePath);
    QDockWidget* sidebarDock() const noexcept { return m_sidebarDock; }
    QTreeView* outlineView() const noexcept { return m_outlineView; }
    QStandardItemModel* outlineModel() const noexcept { return m_outlineModel; }

public Q_SLOTS:
    void openFileDialog();
    void showFindBar();

private Q_SLOTS:
    void onDocumentLoaded(const QString& title, int totalPages);
    void onPageChanged(int currentPage, int totalPages);
    void onZoomChanged(double zoomRatio);
    void onLoadError(const QString& errorMessage);
    void onLoadingChanged(bool loading, const QString& fileName);
    void onDocumentOpenFailed(const QString& filePath, pdf::DocumentError error);
    void onOutlineReady(const std::vector<pdf::OutlineItem>& outline);
    void onOutlineItemClicked(const QModelIndex& index);

private:
    void setupUi();
    void setupActions();
    void showPasswordDialog(const QString& filePath, bool incorrectPassword);
    void closePasswordDialog();

    FindBar* m_findBar{nullptr};
    PdfViewerWidget* m_viewer{nullptr};
    QLabel* m_pageStatusLabel{nullptr};
    QLabel* m_zoomStatusLabel{nullptr};
    QPointer<QDialog> m_passwordDialog;
    QLineEdit* m_passwordInput{nullptr};
    QLabel* m_passwordWarning{nullptr};
    QString m_passwordFilePath;
    QDockWidget* m_sidebarDock{nullptr};
    QStackedWidget* m_sidebarStack{nullptr};
    QTreeView* m_outlineView{nullptr};
    QStandardItemModel* m_outlineModel{nullptr};
    QLabel* m_outlineEmptyLabel{nullptr};
    QAction* m_toggleSidebarAction{nullptr};
    bool m_sidebarUserHidden{false};
};


} // namespace ui
