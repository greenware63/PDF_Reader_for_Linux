#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QPointer>
#include <QStringList>
#include "engine/IDocument.h"
#include "PdfViewerWidget.h"
#include "FindBar.h"
#include "ThumbnailListWidget.h"
#include "SearchResultsWidget.h"

class QDialog;
class QLineEdit;
class QLabel;
class QDockWidget;
class QTabWidget;
class QTreeView;
class QStandardItemModel;
class QStackedWidget;
class QModelIndex;
class QMenu;
class QToolBar;

namespace ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    static constexpr int MAX_RECENT_FILES = 10;

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Starts a non-blocking open. If inNewTab is true, opens in a newly created tab.
    bool openFile(const QString& filePath, bool inNewTab = false);

    /// Opens a document in a new tab. @return index of the new tab.
    int openFileInNewTab(const QString& filePath);

    /// Creates an empty untitled tab. @return index of the new tab.
    int addNewTab();

    /// Closes the tab at index.
    void closeTab(int index);

    /// Closes the currently active tab.
    void closeCurrentTab();

    /// Cycles to the next document tab.
    void selectNextTab();

    /// Cycles to the previous document tab.
    void selectPreviousTab();

    /// Number of open document tabs.
    int tabCount() const noexcept;

    /// Currently active PdfViewerWidget.
    PdfViewerWidget* currentViewer() const;

    /// PdfViewerWidget at specific tab index.
    PdfViewerWidget* viewerAt(int index) const;

    /// Creates and adds a new document viewer tab.
    PdfViewerWidget* createViewerTab(const QString& title = QString());

    /// Exports the currently active document to a PDF file. @return true on success.
    bool exportDocumentToPdf(const QString& outputPath, int fromPage = -1, int toPage = -1);

    /// Color mode / dark mode / reading theme status.
    ColorMode colorMode() const noexcept { return m_colorMode; }
    void setColorMode(ColorMode mode);
    bool isDarkMode() const noexcept { return m_colorMode == ColorMode::Inverted || m_colorMode == ColorMode::HighContrast; }
    layout::ViewMode viewMode() const noexcept { return m_viewMode; }
    /// Applies the page arrangement to every open tab and to tabs opened later; remembered across sessions.
    void setViewMode(layout::ViewMode mode);
    void setDarkMode(bool enabled);

    /// Reading position persistence across documents and sessions.
    void saveReadingPosition(PdfViewerWidget* viewer);
    void restoreReadingPosition(PdfViewerWidget* viewer, const QString& filePath);
    void clearReadingHistory();
    static QString readingPositionKey(const QString& filePath);
    static QString documentFingerprint(const QString& filePath);

    QTabWidget* documentTabs() const noexcept { return m_docTabs; }
    QDockWidget* sidebarDock() const noexcept { return m_sidebarDock; }
    QTabWidget* sidebarTabs() const noexcept { return m_sidebarTabs; }
    ThumbnailListWidget* thumbnailWidget() const noexcept { return m_thumbnailWidget; }
    SearchResultsWidget* searchResultsWidget() const noexcept { return m_searchResultsWidget; }
    QTreeView* outlineView() const noexcept { return m_outlineView; }
    QStandardItemModel* outlineModel() const noexcept { return m_outlineModel; }
    QMenu* recentFilesMenu() const noexcept { return m_recentFilesMenu; }
    QStringList recentFiles() const { return m_recentFiles; }

    void addRecentFile(const QString& filePath);
    void clearRecentFiles();

public Q_SLOTS:
    void openFileDialog();
    void openFileInNewTabDialog();
    void showFindBar();
    void printDocument();
    void exportAsPdf();
    void toggleRedactionMode();
    void markSelectionForRedaction();
    void clearRedactionMarks();
    void applyAndSaveRedactions();
    void saveDocument();
    void saveDocumentAs();
    void highlightSelection();
    void underlineSelection();
    void strikeOutSelection();
    void squigglySelection();
    void addStickyNote();
    void undoEdit();
    void redoEdit();

protected:
    void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
    void onDocumentLoaded(const QString& title, int totalPages);
    void onPageChanged(int currentPage, int totalPages);
    void onZoomChanged(double zoomRatio);
    void onLoadError(const QString& errorMessage);
    void onLoadingChanged(bool loading, const QString& fileName);
    void onDocumentOpenFailed(const QString& filePath, pdf::DocumentError error);
    void onOutlineReady(const std::vector<pdf::OutlineItem>& outline);
    void onOutlineItemClicked(const QModelIndex& index);
    void onCurrentTabChanged(int index);
    void onRedactionModeChanged(bool enabled);
    void onRedactionMarksChanged();
    void onEditLogDirtyChanged(bool dirty);

private:
    void setupUi();
    void setupActions();
    void showPasswordDialog(const QString& filePath, bool incorrectPassword);
    void closePasswordDialog();
    void loadRecentFiles();
    void updateRecentFilesMenu();
    void connectViewer(PdfViewerWidget* viewer);
    void updateStickyNoteAction();

    FindBar* m_findBar{nullptr};
    QTabWidget* m_docTabs{nullptr};
    PdfViewerWidget* m_viewer{nullptr};
    QLabel* m_pageStatusLabel{nullptr};
    QLabel* m_zoomStatusLabel{nullptr};
    QPointer<QDialog> m_passwordDialog;
    QLineEdit* m_passwordInput{nullptr};
    QLabel* m_passwordWarning{nullptr};
    QString m_passwordFilePath;
    QDockWidget* m_sidebarDock{nullptr};
    QTabWidget* m_sidebarTabs{nullptr};
    ThumbnailListWidget* m_thumbnailWidget{nullptr};
    SearchResultsWidget* m_searchResultsWidget{nullptr};
    QStackedWidget* m_sidebarStack{nullptr};
    QTreeView* m_outlineView{nullptr};
    QStandardItemModel* m_outlineModel{nullptr};
    QLabel* m_outlineEmptyLabel{nullptr};
    QAction* m_toggleSidebarAction{nullptr};
    QMenu* m_colorMenu{nullptr};
    QAction* m_colorModeActions[4]{nullptr, nullptr, nullptr, nullptr};
    QAction* m_darkModeAction{nullptr};
    QMenu* m_recentFilesMenu{nullptr};
    QStringList m_recentFiles;
    bool m_sidebarUserHidden{false};
    ColorMode m_colorMode{ColorMode::Normal};
    bool m_darkMode{false};
    layout::ViewMode m_viewMode{layout::ViewMode::Continuous};
    QAction* m_viewModeActions[4]{nullptr, nullptr, nullptr, nullptr};
    QAction* m_redactToggleAction{nullptr};
    QToolBar* m_redactToolBar{nullptr};
    QAction* m_markSelectionRedactAction{nullptr};
    QAction* m_clearRedactionsAction{nullptr};
    QAction* m_applyRedactionsAction{nullptr};
    QLabel* m_redactStatusLabel{nullptr};
    bool m_redactionBusy{false};
    QAction* m_saveAction{nullptr};
    QAction* m_saveAsAction{nullptr};
    QAction* m_undoAction{nullptr};
    QAction* m_redoAction{nullptr};
    QAction* m_highlightAction{nullptr};
    QAction* m_underlineAction{nullptr};
    QAction* m_strikeOutAction{nullptr};
    QAction* m_squigglyAction{nullptr};
    QAction* m_addNoteAction{nullptr};
};

} // namespace ui
