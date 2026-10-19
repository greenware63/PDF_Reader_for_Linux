#include "MainWindow.h"
#include "ThumbnailListWidget.h"
#include <QCoreApplication>
#include <QDockWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTreeView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QStackedWidget>
#include <QHeaderView>
#include <QToolBar>
#include <QToolButton>
#include <QFileDialog>
#include <QSaveFile>
#include <QStatusBar>
#include <QMenuBar>
#include <QAction>
#include <QActionGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCloseEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QSettings>
#include <QDir>
#include <QDateTime>
#include <QPrinter>
#include <QPrintDialog>
#include <QProgressDialog>
#include <QPainter>
#include <cmath>

namespace ui {

namespace {

class PasswordDialog final : public QDialog {
public:
    using QDialog::QDialog;

    void reject() override {
        if (m_dismissible) {
            QDialog::reject();
        }
    }

    void setDismissible(bool dismissible) {
        m_dismissible = dismissible;
    }

private:
    void closeEvent(QCloseEvent* event) override {
        if (m_dismissible) {
            QDialog::closeEvent(event);
        } else {
            event->ignore();
        }
    }

    bool m_dismissible{true};
};

static void addOutlineItemsRecursive(QStandardItem* parent, const std::vector<pdf::OutlineItem>& items) {
    for (const auto& item : items) {
        auto* treeItem = new QStandardItem(item.title);
        treeItem->setData(item.pageIndex, Qt::UserRole);
        treeItem->setData(item.targetYFraction, Qt::UserRole + 1);
        treeItem->setData(item.hasTargetY, Qt::UserRole + 2);
        treeItem->setToolTip(item.isValid()
                                 ? QObject::tr("%1 (Page %2)").arg(item.title).arg(item.pageIndex + 1)
                                 : item.title);
        if (!item.children.empty()) {
            addOutlineItemsRecursive(treeItem, item.children);
        }
        parent->appendRow(treeItem);
    }
}

static QStandardItem* findBestOutlineItem(QStandardItem* parent, int currentPage) {
    QStandardItem* best = nullptr;
    for (int r = 0; r < parent->rowCount(); ++r) {
        auto* child = parent->child(r);
        int page = child->data(Qt::UserRole).toInt();
        if (page >= 0 && page <= currentPage && (!best || page >= best->data(Qt::UserRole).toInt())) {
            best = child;
        }
        if (child->hasChildren()) {
            auto* deeper = findBestOutlineItem(child, currentPage);
            if (deeper && (!best || deeper->data(Qt::UserRole).toInt() >= best->data(Qt::UserRole).toInt())) {
                best = deeper;
            }
        }
    }
    return best;
}

static QString readingPositionKey(const QString& filePath) {
    if (filePath.isEmpty()) {
        return QString();
    }
    QString canonical = QFileInfo(filePath).canonicalFilePath();
    if (canonical.isEmpty()) {
        canonical = QDir::cleanPath(filePath);
    }
    return QString::fromUtf8(canonical.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    if (settings.contains(QStringLiteral("colorMode"))) {
        const int savedColorMode = settings.value(QStringLiteral("colorMode"), 0).toInt();
        if (savedColorMode >= 0 && savedColorMode <= static_cast<int>(ColorMode::HighContrast)) {
            m_colorMode = static_cast<ColorMode>(savedColorMode);
        }
    } else {
        m_colorMode = settings.value(QStringLiteral("darkMode"), false).toBool() ? ColorMode::Inverted : ColorMode::Normal;
    }
    m_darkMode = (m_colorMode == ColorMode::Inverted || m_colorMode == ColorMode::HighContrast);
    {
        const int savedMode = settings.value(QStringLiteral("viewMode"), 0).toInt();
        if (savedMode >= 0 && savedMode <= static_cast<int>(layout::ViewMode::TwoPageCover)) {
            m_viewMode = static_cast<layout::ViewMode>(savedMode);
        }
    }

    setupUi();
    setupActions();
    loadRecentFiles();
    connect(m_docTabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);
    setWindowTitle(tr("PDF Reader - Linux Prototype"));
    resize(1024, 768);
}

MainWindow::~MainWindow() {
    if (m_docTabs) {
        for (int i = 0; i < m_docTabs->count(); ++i) {
            if (auto* v = qobject_cast<PdfViewerWidget*>(m_docTabs->widget(i))) {
                saveReadingPosition(v);
            }
        }
    }
    // The dock and tabs are destroyed by ~QMainWindow, after this object's own destructor has finished.
    if (m_sidebarDock) {
        QObject::disconnect(m_sidebarDock, nullptr, this, nullptr);
    }
    if (m_docTabs) {
        QObject::disconnect(m_docTabs, nullptr, this, nullptr);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (m_docTabs) {
        for (int i = 0; i < m_docTabs->count(); ++i) {
            if (auto* v = qobject_cast<PdfViewerWidget*>(m_docTabs->widget(i))) {
                saveReadingPosition(v);
            }
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_findBar = new FindBar(central);
    m_findBar->hide();
    layout->addWidget(m_findBar);

    connect(m_findBar, &FindBar::searchRequested, this, [this](const QString& q, bool mc) {
        if (auto* v = currentViewer()) v->findText(q, mc);
    });
    connect(m_findBar, &FindBar::findNextRequested, this, [this]() {
        if (auto* v = currentViewer()) v->findNext();
    });
    connect(m_findBar, &FindBar::findPreviousRequested, this, [this]() {
        if (auto* v = currentViewer()) v->findPrevious();
    });
    connect(m_findBar, &FindBar::closed, this, [this]() {
        if (auto* v = currentViewer()) {
            v->clearSearch();
            v->setFocus();
        }
    });

    // Multi-document tab widget in central area
    m_docTabs = new QTabWidget(central);
    m_docTabs->setObjectName(QStringLiteral("DocumentTabs"));
    m_docTabs->setTabsClosable(true);
    m_docTabs->setMovable(true);
    m_docTabs->setDocumentMode(true);
    m_docTabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane {"
        "    border: none;"
        "    background-color: #323639;"
        "}"
        "QTabBar::tab {"
        "    background: #25282a;"
        "    color: #a0a0a0;"
        "    padding: 6px 14px;"
        "    border-top-left-radius: 4px;"
        "    border-top-right-radius: 4px;"
        "    margin-right: 2px;"
        "}"
        "QTabBar::tab:selected {"
        "    background: #323639;"
        "    color: #ffffff;"
        "    font-weight: bold;"
        "}"
        "QTabBar::tab:hover:!selected {"
        "    background: #2b2e31;"
        "    color: #d0d0d0;"
        "}"
    ));

    connect(m_docTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);

    // Initial default viewer tab
    m_viewer = createViewerTab(tr("Untitled"));
    layout->addWidget(m_docTabs, 1);
    setCentralWidget(central);

    // Sidebar dock with tabs: Thumbnails & Document Outline / Bookmarks
    m_sidebarDock = new QDockWidget(tr("Bookmarks"), this);
    m_sidebarDock->setObjectName(QStringLiteral("BookmarksDock"));
    m_sidebarDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    m_sidebarTabs = new QTabWidget(m_sidebarDock);
    m_sidebarTabs->setObjectName(QStringLiteral("SidebarTabs"));

    m_thumbnailWidget = new ThumbnailListWidget(m_sidebarTabs);
    m_thumbnailWidget->setViewer(m_viewer);

    m_sidebarStack = new QStackedWidget(m_sidebarTabs);

    m_outlineEmptyLabel = new QLabel(tr("No bookmarks in document"), m_sidebarStack);
    m_outlineEmptyLabel->setObjectName(QStringLiteral("OutlineEmptyLabel"));
    m_outlineEmptyLabel->setAlignment(Qt::AlignCenter);
    m_outlineEmptyLabel->setStyleSheet(QStringLiteral("color: #888; font-size: 13px; padding: 20px;"));

    m_outlineView = new QTreeView(m_sidebarStack);
    m_outlineView->setObjectName(QStringLiteral("OutlineView"));
    m_outlineView->setHeaderHidden(true);
    m_outlineView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_outlineView->setUniformRowHeights(true);
    m_outlineModel = new QStandardItemModel(this);
    m_outlineView->setModel(m_outlineModel);

    connect(m_outlineView, &QTreeView::clicked, this, &MainWindow::onOutlineItemClicked);

    m_sidebarStack->addWidget(m_outlineEmptyLabel); // index 0: empty
    m_sidebarStack->addWidget(m_outlineView);       // index 1: tree
    m_sidebarStack->setCurrentWidget(m_outlineEmptyLabel);

    m_sidebarTabs->addTab(m_thumbnailWidget, tr("Pages"));
    m_sidebarTabs->addTab(m_sidebarStack, tr("Bookmarks"));

    connect(m_sidebarTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (m_sidebarTabs->widget(index) == m_thumbnailWidget) {
            m_thumbnailWidget->requestVisibleThumbnails();
        }
    });

    m_sidebarDock->setWidget(m_sidebarTabs);
    addDockWidget(Qt::LeftDockWidgetArea, m_sidebarDock);
    m_sidebarDock->hide();
    connect(m_sidebarDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        m_sidebarUserHidden = !visible;
        if (visible && m_sidebarTabs->currentWidget() == m_thumbnailWidget) {
            m_thumbnailWidget->requestVisibleThumbnails();
        }
    });

    statusBar()->showMessage(tr("Ready - Open a PDF to begin"));
}

PdfViewerWidget* MainWindow::createViewerTab(const QString& title) {
    auto* viewer = new PdfViewerWidget(m_docTabs);
    viewer->setColorMode(m_colorMode);
    viewer->setViewMode(m_viewMode);
    connectViewer(viewer);
    const QString tabTitle = title.isEmpty() ? tr("Untitled") : title;
    m_docTabs->addTab(viewer, tabTitle);
    return viewer;
}

void MainWindow::connectViewer(PdfViewerWidget* viewer) {
    connect(viewer, &PdfViewerWidget::documentLoaded, this, &MainWindow::onDocumentLoaded);
    connect(viewer, &PdfViewerWidget::pageChanged, this, [this, viewer](int currentPage, int totalPages) {
        if (viewer == currentViewer()) {
            onPageChanged(currentPage, totalPages);
        }
    });
    connect(viewer, &PdfViewerWidget::zoomChanged, this, [this, viewer](double zoomRatio) {
        if (viewer == currentViewer()) {
            onZoomChanged(zoomRatio);
        }
    });
    connect(viewer, &PdfViewerWidget::loadError, this, [this, viewer](const QString& errorMessage) {
        if (viewer == currentViewer()) {
            onLoadError(errorMessage);
        }
    });
    connect(viewer, &PdfViewerWidget::documentOpenFailed, this, &MainWindow::onDocumentOpenFailed);
    connect(viewer, &PdfViewerWidget::loadingChanged, this, [this, viewer](bool loading, const QString& fileName) {
        if (viewer == currentViewer()) {
            onLoadingChanged(loading, fileName);
        }
    });
    connect(viewer, &PdfViewerWidget::outlineReady, this, [this, viewer](const std::vector<pdf::OutlineItem>& outline) {
        if (viewer == currentViewer()) {
            onOutlineReady(outline);
        }
    });
    connect(viewer, &PdfViewerWidget::searchResultsChanged, this, [this, viewer](int cur, int tot) {
        if (viewer == currentViewer()) {
            if (m_findBar) m_findBar->setMatchCount(cur, tot);
        }
    });
    connect(viewer, &PdfViewerWidget::searchProgressChanged, this, [this, viewer](int searched, int total) {
        if (viewer == currentViewer()) {
            if (m_findBar) m_findBar->setSearchingProgress(searched, total);
        }
    });
    connect(viewer, &PdfViewerWidget::searchFinished, this, [this, viewer](bool trunc) {
        if (viewer == currentViewer()) {
            if (m_findBar) m_findBar->finishSearch(trunc);
        }
    });
}

void MainWindow::setupActions() {
    // Menu Bar
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* newTabAction = fileMenu->addAction(tr("&New Tab"), this, &MainWindow::addNewTab);
    newTabAction->setShortcut(QKeySequence::AddTab);

    QAction* openAction = fileMenu->addAction(tr("&Open PDF..."), this, &MainWindow::openFileDialog);
    openAction->setShortcut(QKeySequence::Open);

    QAction* openNewTabAction = fileMenu->addAction(tr("Open in &New Tab..."), this, &MainWindow::openFileInNewTabDialog);
    openNewTabAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));

    m_recentFilesMenu = fileMenu->addMenu(tr("Open &Recent"));

    fileMenu->addSeparator();
    QAction* printAction = fileMenu->addAction(tr("&Print..."), this, &MainWindow::printDocument);
    printAction->setShortcut(QKeySequence::Print);

    QAction* exportPdfAction = fileMenu->addAction(tr("&Export as PDF..."), this, &MainWindow::exportAsPdf);
    exportPdfAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));

    fileMenu->addSeparator();
    QAction* closeAction = fileMenu->addAction(tr("&Close Tab"), this, &MainWindow::closeCurrentTab);
    closeAction->setShortcut(QKeySequence::Close);

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction* copyAction = editMenu->addAction(tr("&Copy"), this, [this]() {
        if (auto* v = currentViewer()) {
            v->copySelection();
        }
    });
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(false);

    QAction* findAction = editMenu->addAction(tr("&Find..."), this, &MainWindow::showFindBar);
    findAction->setShortcut(QKeySequence::Find);
    QAction* findNextAction = editMenu->addAction(tr("Find &Next"), this, [this]() {
        if (auto* v = currentViewer()) v->findNext();
    });
    findNextAction->setShortcut(QKeySequence(Qt::Key_F3));
    QAction* findPreviousAction = editMenu->addAction(tr("Find &Previous"), this, [this]() {
        if (auto* v = currentViewer()) v->findPrevious();
    });
    findPreviousAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    m_toggleSidebarAction = viewMenu->addAction(tr("&Sidebar"), this, [this]() {
        if (m_sidebarDock) m_sidebarDock->setVisible(!m_sidebarDock->isVisible());
    });
    m_toggleSidebarAction->setShortcut(QKeySequence(Qt::Key_F4));

    m_colorMenu = viewMenu->addMenu(tr("&Color Mode"));
    auto* colorGroup = new QActionGroup(this);
    colorGroup->setExclusive(true);

    struct ColorEntry { ColorMode mode; const char* text; const char* shortcut; };
    const ColorEntry colorEntries[] = {
        {ColorMode::Normal,       QT_TR_NOOP("&Normal (Light)"),         ""},
        {ColorMode::Inverted,     QT_TR_NOOP("&Dark Mode / Invert"),     "Ctrl+I"},
        {ColorMode::Sepia,        QT_TR_NOOP("&Sepia (Warm Reading)"),   "Ctrl+Alt+S"},
        {ColorMode::HighContrast, QT_TR_NOOP("&High Contrast"),          "Ctrl+Alt+H"},
    };

    for (const auto& e : colorEntries) {
        QAction* a = m_colorMenu->addAction(tr(e.text));
        a->setCheckable(true);
        if (e.shortcut && e.shortcut[0] != '\0') {
            a->setShortcut(QKeySequence(QString::fromLatin1(e.shortcut)));
        }
        a->setChecked(e.mode == m_colorMode);
        colorGroup->addAction(a);
        m_colorModeActions[static_cast<int>(e.mode)] = a;
        const ColorMode mode = e.mode;
        connect(a, &QAction::triggered, this, [this, mode]() { setColorMode(mode); });
    }
    m_darkModeAction = m_colorModeActions[static_cast<int>(ColorMode::Inverted)];

    viewMenu->addSeparator();
    {
        struct ModeEntry { layout::ViewMode mode; const char* text; int key; };
        const ModeEntry entries[] = {
            {layout::ViewMode::Continuous,    QT_TR_NOOP("&Continuous"),                 Qt::Key_1},
            {layout::ViewMode::SinglePage,    QT_TR_NOOP("Single &Page"),                Qt::Key_2},
            {layout::ViewMode::TwoPage,       QT_TR_NOOP("&Two Pages"),                  Qt::Key_3},
            {layout::ViewMode::TwoPageCover,  QT_TR_NOOP("Two Pages with Co&ver Page"),  Qt::Key_4},
        };
        auto* group = new QActionGroup(this);
        group->setExclusive(true);
        for (const auto& e : entries) {
            QAction* a = viewMenu->addAction(tr(e.text));
            a->setCheckable(true);
            a->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(e.key)));
            a->setChecked(e.mode == m_viewMode);
            group->addAction(a);
            m_viewModeActions[static_cast<int>(e.mode)] = a;
            const layout::ViewMode mode = e.mode;
            connect(a, &QAction::triggered, this, [this, mode]() { setViewMode(mode); });
        }
    }
    viewMenu->addSeparator();
    {
        QAction* fitWidth = viewMenu->addAction(tr("Fit &Width"), this, [this]() {
            if (auto* v = currentViewer()) v->zoomFitWidth();
        });
        fitWidth->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_8));
        QAction* fitPage = viewMenu->addAction(tr("&Fit Page"), this, [this]() {
            if (auto* v = currentViewer()) v->zoomFitPage();
        });
        fitPage->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_9));
    }
    viewMenu->addSeparator();
    {
        QAction* rotateCw = viewMenu->addAction(tr("Rotate &Clockwise"), this, [this]() {
            if (auto* v = currentViewer()) v->rotateClockwise();
        });
        rotateCw->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));

        QAction* rotateCcw = viewMenu->addAction(tr("Rotate Counter-&Clockwise"), this, [this]() {
            if (auto* v = currentViewer()) v->rotateCounterClockwise();
        });
        rotateCcw->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    }

    // Tab navigation shortcuts
    auto* nextTabAction = new QAction(this);
    nextTabAction->setShortcut(QKeySequence::NextChild);
    connect(nextTabAction, &QAction::triggered, this, &MainWindow::selectNextTab);
    addAction(nextTabAction);

    auto* prevTabAction = new QAction(this);
    prevTabAction->setShortcut(QKeySequence::PreviousChild);
    connect(prevTabAction, &QAction::triggered, this, &MainWindow::selectPreviousTab);
    addAction(prevTabAction);

    // Toolbar
    QToolBar* toolbar = addToolBar(tr("Main Toolbar"));
    toolbar->setMovable(false);

    toolbar->addAction(openAction);
    QAction* printToolbarAction = toolbar->addAction(tr("\U0001f5b6 Print"), this, &MainWindow::printDocument);
    printToolbarAction->setToolTip(tr("Print Document (Ctrl+P)"));
    toolbar->addSeparator();

    // Page navigation controls
    QAction* prevAction = toolbar->addAction(tr("\u25c0 Prev"), this, [this]() {
        if (auto* v = currentViewer()) v->previousPage();
    });
    prevAction->setShortcut(QKeySequence(Qt::Key_PageUp));

    m_pageStatusLabel = new QLabel(tr(" Page 0 / 0 "), this);
    toolbar->addWidget(m_pageStatusLabel);

    QAction* nextAction = toolbar->addAction(tr("Next \u25b6"), this, [this]() {
        if (auto* v = currentViewer()) v->nextPage();
    });
    nextAction->setShortcut(QKeySequence(Qt::Key_PageDown));

    toolbar->addSeparator();

    // Zoom controls
    QAction* zoomOutAction = toolbar->addAction(tr("Zoom -"), this, [this]() {
        if (auto* v = currentViewer()) v->zoomOut();
    });
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);

    m_zoomStatusLabel = new QLabel(tr(" 100% "), this);
    toolbar->addWidget(m_zoomStatusLabel);

    QAction* zoomInAction = toolbar->addAction(tr("Zoom +"), this, [this]() {
        if (auto* v = currentViewer()) v->zoomIn();
    });
    zoomInAction->setShortcut(QKeySequence::ZoomIn);

    toolbar->addAction(tr("Fit Width"), this, [this]() {
        if (auto* v = currentViewer()) v->zoomFitWidth();
    });
    toolbar->addAction(tr("Fit Page"), this, [this]() {
        if (auto* v = currentViewer()) v->zoomFitPage();
    });
    toolbar->addAction(tr("100%"), this, [this]() {
        if (auto* v = currentViewer()) v->zoomReset();
    });

    toolbar->addSeparator();
    QAction* rotateCwToolbarAction = toolbar->addAction(tr("Rotate CW"), this, [this]() {
        if (auto* v = currentViewer()) v->rotateClockwise();
    });
    rotateCwToolbarAction->setToolTip(tr("Rotate Clockwise (Ctrl+R)"));

    QAction* rotateCcwToolbarAction = toolbar->addAction(tr("Rotate CCW"), this, [this]() {
        if (auto* v = currentViewer()) v->rotateCounterClockwise();
    });
    rotateCcwToolbarAction->setToolTip(tr("Rotate Counter-Clockwise (Ctrl+Shift+R)"));

    toolbar->addSeparator();
    auto* themeButton = new QToolButton(toolbar);
    themeButton->setText(tr("🌙 Theme"));
    themeButton->setToolTip(tr("Color Mode / Reading Themes (Ctrl+I to toggle Dark Mode)"));
    if (m_colorMenu) {
        themeButton->setMenu(m_colorMenu);
        themeButton->setPopupMode(QToolButton::MenuButtonPopup);
    }
    connect(themeButton, &QToolButton::clicked, this, [this]() {
        if (m_colorMode == ColorMode::Normal) {
            setColorMode(ColorMode::Inverted);
        } else {
            setColorMode(ColorMode::Normal);
        }
    });
    toolbar->addWidget(themeButton);
}

void MainWindow::setColorMode(ColorMode mode) {
    m_colorMode = mode;
    m_darkMode = (m_colorMode == ColorMode::Inverted || m_colorMode == ColorMode::HighContrast);
    const int idx = static_cast<int>(mode);
    if (idx >= 0 && idx < 4 && m_colorModeActions[idx] && !m_colorModeActions[idx]->isChecked()) {
        m_colorModeActions[idx]->setChecked(true);
    }
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    settings.setValue(QStringLiteral("colorMode"), static_cast<int>(m_colorMode));
    settings.setValue(QStringLiteral("darkMode"), m_darkMode);

    for (int i = 0; i < tabCount(); ++i) {
        if (auto* v = viewerAt(i)) {
            v->setColorMode(mode);
        }
    }
}

void MainWindow::setDarkMode(bool enabled) {
    setColorMode(enabled ? ColorMode::Inverted : ColorMode::Normal);
}

void MainWindow::setViewMode(layout::ViewMode mode) {
    m_viewMode = mode;
    const int idx = static_cast<int>(mode);
    if (idx >= 0 && idx < 4 && m_viewModeActions[idx] && !m_viewModeActions[idx]->isChecked()) {
        m_viewModeActions[idx]->setChecked(true);
    }
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    settings.setValue(QStringLiteral("viewMode"), idx);
    for (int i = 0; i < tabCount(); ++i) {
        if (auto* v = viewerAt(i)) {
            v->setViewMode(mode);
        }
    }
}

void MainWindow::saveReadingPosition(PdfViewerWidget* viewer) {
    if (!viewer || !viewer->isDocumentOpen() || viewer->currentFilePath().isEmpty()) {
        return;
    }
    const QString key = readingPositionKey(viewer->currentFilePath());
    if (key.isEmpty()) {
        return;
    }
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    settings.beginGroup(QStringLiteral("readingHistory/") + key);
    settings.setValue(QStringLiteral("filePath"), viewer->currentFilePath());
    settings.setValue(QStringLiteral("page"), viewer->currentPage());
    settings.setValue(QStringLiteral("zoom"), viewer->currentZoom());
    // Scroll offsets are only meaningful in the continuous layout; other view modes restore by page.
    const bool continuous = viewer->viewMode() == layout::ViewMode::Continuous;
    settings.setValue(QStringLiteral("scrollX"), static_cast<qlonglong>(continuous ? viewer->currentDocScrollX() : 0));
    settings.setValue(QStringLiteral("scrollY"), static_cast<qlonglong>(continuous ? viewer->currentDocScrollY() : 0));
    settings.setValue(QStringLiteral("timestamp"), QDateTime::currentSecsSinceEpoch());
    settings.endGroup();
}

void MainWindow::restoreReadingPosition(PdfViewerWidget* viewer, const QString& filePath) {
    if (!viewer || filePath.isEmpty()) {
        return;
    }
    const QString key = readingPositionKey(filePath);
    if (key.isEmpty()) {
        return;
    }
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    const QString group = QStringLiteral("readingHistory/") + key;
    if (!settings.contains(group + QStringLiteral("/page"))) {
        return;
    }
    settings.beginGroup(group);
    int savedPage = settings.value(QStringLiteral("page"), 0).toInt();
    double savedZoom = settings.value(QStringLiteral("zoom"), viewer->currentZoom()).toDouble();
    int64_t scrollX = settings.value(QStringLiteral("scrollX"), 0).toLongLong();
    int64_t scrollY = settings.value(QStringLiteral("scrollY"), 0).toLongLong();
    settings.endGroup();

    viewer->restoreReadingPosition(savedPage, savedZoom, scrollX, scrollY);
}

void MainWindow::clearReadingHistory() {
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    settings.remove(QStringLiteral("readingHistory"));
}

PdfViewerWidget* MainWindow::currentViewer() const {
    if (m_docTabs && m_docTabs->currentIndex() >= 0) {
        if (auto* v = qobject_cast<PdfViewerWidget*>(m_docTabs->currentWidget())) {
            return v;
        }
    }
    return m_viewer;
}

PdfViewerWidget* MainWindow::viewerAt(int index) const {
    if (m_docTabs && index >= 0 && index < m_docTabs->count()) {
        return qobject_cast<PdfViewerWidget*>(m_docTabs->widget(index));
    }
    return nullptr;
}

int MainWindow::tabCount() const noexcept {
    return m_docTabs ? m_docTabs->count() : 0;
}

void MainWindow::showFindBar() {
    if (auto* v = currentViewer()) {
        if (v->isDocumentOpen() && m_findBar) {
            m_findBar->focusSearch();
        }
    }
}

bool MainWindow::openFile(const QString& filePath, bool inNewTab) {
    if (filePath.isEmpty()) {
        return false;
    }
    if (inNewTab) {
        openFileInNewTab(filePath);
        return true;
    }
    auto* viewer = currentViewer();
    if (!viewer) {
        viewer = createViewerTab();
    }
    viewer->openDocumentAsync(filePath);
    return true;
}

int MainWindow::openFileInNewTab(const QString& filePath) {
    if (filePath.isEmpty()) {
        return -1;
    }
    const QString title = QFileInfo(filePath).fileName();
    auto* viewer = createViewerTab(title);
    const int newIndex = m_docTabs->indexOf(viewer);
    m_docTabs->setCurrentIndex(newIndex);
    viewer->openDocumentAsync(filePath);
    return newIndex;
}

int MainWindow::addNewTab() {
    auto* viewer = createViewerTab(tr("Untitled"));
    const int newIndex = m_docTabs->indexOf(viewer);
    m_docTabs->setCurrentIndex(newIndex);
    return newIndex;
}

void MainWindow::closeCurrentTab() {
    if (m_docTabs) {
        closeTab(m_docTabs->currentIndex());
    }
}

void MainWindow::closeTab(int index) {
    if (!m_docTabs || index < 0 || index >= m_docTabs->count()) {
        return;
    }
    auto* viewer = qobject_cast<PdfViewerWidget*>(m_docTabs->widget(index));
    if (viewer) {
        saveReadingPosition(viewer);
    }
    if (m_docTabs->count() > 1) {
        if (viewer) {
            viewer->closeDocument();
        }
        m_docTabs->removeTab(index);
        if (viewer) {
            viewer->deleteLater();
        }
    } else {
        // Last remaining tab: keep it as empty/untitled
        m_docTabs->setTabText(0, tr("Untitled"));
        m_docTabs->setTabToolTip(0, QString());
        if (viewer) {
            viewer->closeDocument();
        }
        onCurrentTabChanged(0);
    }
}

void MainWindow::selectNextTab() {
    if (m_docTabs && m_docTabs->count() > 1) {
        m_docTabs->setCurrentIndex((m_docTabs->currentIndex() + 1) % m_docTabs->count());
    }
}

void MainWindow::selectPreviousTab() {
    if (m_docTabs && m_docTabs->count() > 1) {
        m_docTabs->setCurrentIndex((m_docTabs->currentIndex() - 1 + m_docTabs->count()) % m_docTabs->count());
    }
}

void MainWindow::onCurrentTabChanged(int index) {
    if (index < 0 || !m_docTabs) {
        return;
    }
    if (m_viewer && m_viewer->isDocumentOpen()) {
        saveReadingPosition(m_viewer);
    }
    m_viewer = qobject_cast<PdfViewerWidget*>(m_docTabs->widget(index));
    if (!m_viewer) {
        return;
    }

    if (m_thumbnailWidget) {
        m_thumbnailWidget->setViewer(m_viewer);
    }

    if (m_viewer->isDocumentOpen()) {
        const QString fileName = QFileInfo(m_viewer->currentFilePath()).fileName();
        setWindowTitle(tr("%1 - PDF Reader").arg(fileName.isEmpty() ? m_docTabs->tabText(index) : fileName));
        if (m_pageStatusLabel) {
            onPageChanged(m_viewer->currentPage() + 1, m_viewer->pageCount());
        }
        if (m_zoomStatusLabel) {
            onZoomChanged(m_viewer->currentZoom());
        }
        if (m_thumbnailWidget) {
            m_thumbnailWidget->setDocument(m_viewer->pageCount(), m_viewer->pageLabels());
            m_thumbnailWidget->setCurrentPage(m_viewer->currentPage());
        }
        onOutlineReady(m_viewer->outline());
    } else {
        setWindowTitle(tr("PDF Reader - Linux Prototype"));
        if (m_pageStatusLabel) {
            m_pageStatusLabel->setText(tr(" Page 0 / 0 "));
        }
        if (m_zoomStatusLabel) {
            m_zoomStatusLabel->setText(tr(" 100% "));
        }
        if (m_thumbnailWidget) {
            m_thumbnailWidget->clearDocument();
        }
        if (m_outlineModel) {
            m_outlineModel->clear();
        }
        if (m_outlineEmptyLabel) {
            m_outlineEmptyLabel->setText(tr("No bookmarks in document"));
        }
        if (m_sidebarStack && m_outlineEmptyLabel) {
            m_sidebarStack->setCurrentWidget(m_outlineEmptyLabel);
        }
    }

    if (m_findBar && m_findBar->isVisible()) {
        m_findBar->clear();
        m_viewer->clearSearch();
    }
}

void MainWindow::loadRecentFiles() {
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    m_recentFiles = settings.value(QStringLiteral("recentFiles")).toStringList();
    m_recentFiles.erase(std::remove_if(m_recentFiles.begin(), m_recentFiles.end(),
        [](const QString& path) {
            return path.isEmpty() || !QFileInfo::exists(path);
        }), m_recentFiles.end());
    updateRecentFilesMenu();
}

void MainWindow::addRecentFile(const QString& filePath) {
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) {
        return;
    }
    QString canonical = QFileInfo(filePath).canonicalFilePath();
    if (canonical.isEmpty()) {
        canonical = QDir::cleanPath(filePath);
    }
    m_recentFiles.removeAll(canonical);
    m_recentFiles.prepend(canonical);
    while (m_recentFiles.size() > MAX_RECENT_FILES) {
        m_recentFiles.removeLast();
    }
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    settings.setValue(QStringLiteral("recentFiles"), m_recentFiles);
    updateRecentFilesMenu();
}

void MainWindow::clearRecentFiles() {
    m_recentFiles.clear();
    QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
    settings.setValue(QStringLiteral("recentFiles"), m_recentFiles);
    updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu() {
    if (!m_recentFilesMenu) {
        return;
    }
    m_recentFilesMenu->clear();
    if (m_recentFiles.isEmpty()) {
        auto* emptyAction = m_recentFilesMenu->addAction(tr("No Recent Files"));
        emptyAction->setEnabled(false);
        return;
    }

    for (int i = 0; i < m_recentFiles.size(); ++i) {
        const QString& path = m_recentFiles[i];
        QString text = tr("&%1 %2").arg(i + 1).arg(QFileInfo(path).fileName());
        auto* action = m_recentFilesMenu->addAction(text, this, [this, path]() {
            openFile(path);
        });
        action->setToolTip(path);
        action->setData(path);
    }

    m_recentFilesMenu->addSeparator();
    m_recentFilesMenu->addAction(tr("&Clear Recent Files"), this, &MainWindow::clearRecentFiles);
}

void MainWindow::onLoadingChanged(bool loading, const QString& fileName) {
    if (m_passwordDialog) {
        auto* dialog = static_cast<PasswordDialog*>(m_passwordDialog.data());
        dialog->setDismissible(!loading);
        if (m_passwordInput) {
            m_passwordInput->setEnabled(!loading);
        }
        if (auto* buttons = dialog->findChild<QDialogButtonBox*>()) {
            if (auto* cancel = buttons->button(QDialogButtonBox::Cancel)) {
                cancel->setEnabled(!loading);
            }
            if (auto* open = buttons->button(QDialogButtonBox::Ok)) {
                open->setEnabled(!loading);
            }
        }
    }
    if (loading) {
        statusBar()->showMessage(tr("Opening '%1'…").arg(fileName));
        setCursor(Qt::BusyCursor);
    } else {
        unsetCursor();
    }
}

void MainWindow::openFileDialog() {
    QString file = QFileDialog::getOpenFileName(
        this,
        tr("Open PDF Document"),
        QString(),
        tr("PDF Files (*.pdf)")
    );

    if (!file.isEmpty()) {
        openFile(file);
    }
}

void MainWindow::openFileInNewTabDialog() {
    QString file = QFileDialog::getOpenFileName(
        this,
        tr("Open PDF in New Tab"),
        QString(),
        tr("PDF Files (*.pdf)")
    );

    if (!file.isEmpty()) {
        openFile(file, true);
    }
}

bool MainWindow::exportDocumentToPdf(const QString& outputPath, int fromPage, int toPage) {
    auto* viewer = currentViewer();
    if (!viewer || !viewer->isDocumentOpen() || viewer->pageCount() <= 0) {
        return false;
    }
    if (outputPath.isEmpty()) {
        return false;
    }

    const int totalPages = viewer->pageCount();
    const int start = (fromPage >= 0) ? std::clamp(fromPage, 0, totalPages - 1) : 0;
    const int end = (toPage >= 0) ? std::clamp(toPage, start, totalPages - 1) : (totalPages - 1);

    QSaveFile out(outputPath);
    if (!out.open(QIODevice::WriteOnly)) {
        return false;
    }

    if (start == 0 && end == totalPages - 1 && !viewer->currentFilePath().isEmpty()) {
        // Whole document: copy the original bytes. This is lossless (outlines, forms, metadata,
        // encryption) and avoids pushing the entire file through the IPC channel.
        const QString source = viewer->currentFilePath();
        if (QFileInfo(source).canonicalFilePath() == QFileInfo(outputPath).canonicalFilePath()) {
            return false;
        }
        QFile in(source);
        if (!in.open(QIODevice::ReadOnly)) {
            return false;
        }
        while (!in.atEnd()) {
            const QByteArray chunk = in.read(1 << 20);
            if (chunk.isEmpty() || out.write(chunk) != chunk.size()) {
                return false;
            }
        }
    } else {
        // Page subset: PDFium copies the selected pages into a new document in the sandboxed worker.
        // The worker cannot write to user-chosen paths (Landlock), so the bytes come back over IPC.
        const QByteArray pdfBytes = viewer->extractPagesPdf(start, end);
        if (pdfBytes.isEmpty() || out.write(pdfBytes) != pdfBytes.size()) {
            return false;
        }
    }
    return out.commit();
}

void MainWindow::printDocument() {
    auto* viewer = currentViewer();
    if (!viewer || !viewer->isDocumentOpen() || viewer->pageCount() <= 0) {
        return;
    }

    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(QFileInfo(viewer->currentFilePath()).fileName());
    printer.setFromTo(1, viewer->pageCount());

    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print Document"));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    int fromPage = 0;
    int toPage = viewer->pageCount() - 1;
    if (printer.printRange() == QPrinter::PageRange) {
        fromPage = std::clamp(printer.fromPage() - 1, 0, viewer->pageCount() - 1);
        toPage = std::clamp(printer.toPage() - 1, fromPage, viewer->pageCount() - 1);
    } else if (printer.printRange() == QPrinter::CurrentPage) {
        fromPage = viewer->currentPage();
        toPage = viewer->currentPage();
    }

    const int numPages = toPage - fromPage + 1;
    QProgressDialog progress(tr("Printing document..."), tr("Cancel"), 0, numPages, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);

    QPainter painter;
    if (!painter.begin(&printer)) {
        statusBar()->showMessage(tr("Printing failed: Could not initialize printer."), 5000);
        return;
    }

    int step = 0;
    for (int p = fromPage; p <= toPage; ++p) {
        progress.setValue(step++);
        progress.setLabelText(tr("Printing page %1 of %2...").arg(p + 1).arg(viewer->pageCount()));
        QCoreApplication::processEvents();
        if (progress.wasCanceled()) {
            break;
        }

        if (p > fromPage) {
            printer.newPage();
        }

        QImage pageImg = viewer->renderPageForPrint(p, 1.0, 2.0);
        if (pageImg.isNull()) {
            continue;
        }

        const QRect targetRect = printer.pageLayout().paintRectPixels(printer.resolution());
        QImage scaled = pageImg.scaled(targetRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        int x = targetRect.left() + (targetRect.width() - scaled.width()) / 2;
        int y = targetRect.top() + (targetRect.height() - scaled.height()) / 2;
        painter.drawImage(x, y, scaled);
    }
    painter.end();
    progress.setValue(numPages);
    statusBar()->showMessage(tr("Document printed successfully."), 4000);
}

void MainWindow::exportAsPdf() {
    auto* viewer = currentViewer();
    if (!viewer || !viewer->isDocumentOpen() || viewer->pageCount() <= 0) {
        return;
    }

    QString defaultName = QFileInfo(viewer->currentFilePath()).completeBaseName() + QStringLiteral("_export.pdf");
    QString savePath = QFileDialog::getSaveFileName(
        this,
        tr("Export as PDF"),
        defaultName,
        tr("PDF Files (*.pdf)")
    );

    if (savePath.isEmpty()) {
        return;
    }

    if (!savePath.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)) {
        savePath += QStringLiteral(".pdf");
    }

    statusBar()->showMessage(tr("Exporting to '%1'…").arg(QFileInfo(savePath).fileName()));
    bool ok = exportDocumentToPdf(savePath);
    if (ok) {
        statusBar()->showMessage(tr("Successfully exported PDF to '%1'").arg(savePath), 5000);
    } else {
        statusBar()->showMessage(tr("Export failed."), 5000);
    }
}

void MainWindow::onDocumentLoaded(const QString& title, int totalPages) {
    auto* senderViewer = qobject_cast<PdfViewerWidget*>(sender());
    if (!senderViewer) {
        senderViewer = currentViewer();
    }

    // Update tab title and tooltip for this viewer
    if (m_docTabs && senderViewer) {
        int idx = m_docTabs->indexOf(senderViewer);
        if (idx >= 0) {
            m_docTabs->setTabText(idx, title);
            m_docTabs->setTabToolTip(idx, senderViewer->currentFilePath());
        }
    }

    // Record into recent files & restore reading position
    if (senderViewer && !senderViewer->currentFilePath().isEmpty()) {
        addRecentFile(senderViewer->currentFilePath());
        restoreReadingPosition(senderViewer, senderViewer->currentFilePath());
    }

    if (senderViewer == currentViewer()) {
        closePasswordDialog();
        if (m_findBar) {
            m_findBar->clear();
            m_findBar->hide();
        }
        if (m_outlineModel) {
            m_outlineModel->clear();
        }
        if (m_outlineEmptyLabel) {
            m_outlineEmptyLabel->setText(tr("Loading bookmarks…"));
        }
        if (m_sidebarStack && m_outlineEmptyLabel) {
            m_sidebarStack->setCurrentWidget(m_outlineEmptyLabel);
        }
        if (m_thumbnailWidget) {
            m_thumbnailWidget->setDocument(totalPages, senderViewer->pageLabels());
            m_thumbnailWidget->setCurrentPage(senderViewer->currentPage());
        }
        if (m_pageStatusLabel) {
            onPageChanged(senderViewer->currentPage() + 1, totalPages);
        }
        if (m_zoomStatusLabel) {
            onZoomChanged(senderViewer->currentZoom());
        }
        setWindowTitle(tr("%1 - PDF Reader").arg(title));
        statusBar()->showMessage(tr("Loaded '%1' (%2 pages)").arg(title).arg(totalPages));
    }
}

void MainWindow::onPageChanged(int currentPage, int totalPages) {
    if (totalPages > 0) {
        if (m_pageStatusLabel) {
            m_pageStatusLabel->setText(tr(" Page %1 / %2 ").arg(currentPage).arg(totalPages));
        }
        const int zeroBasedPage = currentPage - 1;
        if (m_thumbnailWidget) {
            m_thumbnailWidget->setCurrentPage(zeroBasedPage);
        }
        if (m_outlineModel && m_outlineModel->rowCount() > 0 && m_outlineView) {
            auto* item = findBestOutlineItem(m_outlineModel->invisibleRootItem(), zeroBasedPage);
            if (item) {
                m_outlineView->blockSignals(true);
                m_outlineView->setCurrentIndex(item->index());
                m_outlineView->blockSignals(false);
            }
        }
    } else {
        if (m_pageStatusLabel) {
            m_pageStatusLabel->setText(tr(" Page 0 / 0 "));
        }
        setWindowTitle(tr("PDF Reader - Linux Prototype"));
        if (m_thumbnailWidget) {
            m_thumbnailWidget->clearDocument();
        }
    }
}

void MainWindow::onZoomChanged(double zoomRatio) {
    if (m_zoomStatusLabel) {
        int percentage = static_cast<int>(std::round(zoomRatio * 100.0));
        m_zoomStatusLabel->setText(tr(" %1% ").arg(percentage));
    }
}

void MainWindow::onLoadError(const QString& errorMessage) {
    if (m_passwordDialog) {
        return;
    }
    statusBar()->showMessage(errorMessage, 6000);
}

void MainWindow::onDocumentOpenFailed(const QString& filePath, pdf::DocumentError error) {
    if (error == pdf::DocumentError::PasswordRequired || error == pdf::DocumentError::BadPassword) {
        showPasswordDialog(filePath, error == pdf::DocumentError::BadPassword);
    } else {
        closePasswordDialog();
    }
}

void MainWindow::showPasswordDialog(const QString& filePath, bool incorrectPassword) {
    if (!m_passwordDialog || m_passwordFilePath != filePath) {
        closePasswordDialog();
        auto* dialog = new PasswordDialog(this);
        dialog->setObjectName(QStringLiteral("passwordDialog"));
        dialog->setWindowTitle(tr("Password Required"));
        dialog->setModal(true);
        dialog->setAttribute(Qt::WA_DeleteOnClose);

        auto* prompt = new QLabel(tr("Enter the password for this PDF:"), dialog);
        m_passwordInput = new QLineEdit(dialog);
        m_passwordInput->setObjectName(QStringLiteral("passwordInput"));
        m_passwordInput->setEchoMode(QLineEdit::Password);
        m_passwordWarning = new QLabel(dialog);
        m_passwordWarning->setObjectName(QStringLiteral("passwordWarning"));
        m_passwordWarning->setStyleSheet(QStringLiteral("color: #b00020;"));
        m_passwordWarning->setVisible(false);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Open"));

        auto* layout = new QVBoxLayout(dialog);
        layout->addWidget(prompt);
        layout->addWidget(m_passwordInput);
        layout->addWidget(m_passwordWarning);
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, dialog, [this]() {
            const QString password = m_passwordInput->text();
            m_passwordInput->clear();
            m_passwordWarning->clear();
            m_passwordWarning->hide();
            if (auto* v = currentViewer()) {
                v->openDocumentAsync(m_passwordFilePath, password);
            }
        });
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(dialog, &QDialog::finished, this, [this]() {
            m_passwordDialog = nullptr;
            m_passwordInput = nullptr;
            m_passwordWarning = nullptr;
            m_passwordFilePath.clear();
        });

        m_passwordDialog = dialog;
        m_passwordFilePath = filePath;
    }

    m_passwordWarning->setText(incorrectPassword ? tr("Incorrect password") : QString());
    m_passwordWarning->setVisible(incorrectPassword);
    m_passwordInput->clear();
    m_passwordInput->setEnabled(true);
    m_passwordDialog->show();
    m_passwordInput->setFocus();
}

void MainWindow::closePasswordDialog() {
    if (m_passwordDialog) {
        static_cast<PasswordDialog*>(m_passwordDialog.data())->setDismissible(true);
        m_passwordDialog->reject();
    }
}

void MainWindow::onOutlineReady(const std::vector<pdf::OutlineItem>& outline) {
    if (!m_outlineModel || !m_outlineEmptyLabel || !m_sidebarStack || !m_outlineView || !m_sidebarTabs) {
        return;
    }
    m_outlineModel->clear();
    if (outline.empty()) {
        m_outlineEmptyLabel->setText(tr("No bookmarks in document"));
        m_sidebarStack->setCurrentWidget(m_outlineEmptyLabel);
        return;
    }

    addOutlineItemsRecursive(m_outlineModel->invisibleRootItem(), outline);
    m_sidebarStack->setCurrentWidget(m_outlineView);
    if (m_sidebarTabs->currentWidget() != m_thumbnailWidget) {
        m_sidebarTabs->setCurrentWidget(m_sidebarStack);
    }
    m_outlineView->expandToDepth(0);
    if (auto* v = currentViewer()) {
        if (v->pageCount() > 0) {
            onPageChanged(v->currentPage() + 1, v->pageCount());
        }
    }
    if (!m_sidebarUserHidden && m_sidebarDock) {
        m_sidebarDock->show();
    }
}

void MainWindow::onOutlineItemClicked(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    auto* v = currentViewer();
    if (!v) {
        return;
    }
    int pageIndex = index.data(Qt::UserRole).toInt();
    if (pageIndex >= 0 && pageIndex < v->pageCount()) {
        if (index.data(Qt::UserRole + 2).toBool()) {
            v->goToPage(pageIndex, index.data(Qt::UserRole + 1).toDouble());
        } else {
            v->goToPage(pageIndex);
        }
        m_outlineView->setCurrentIndex(index);
        v->setFocus();
    }
}

} // namespace ui
