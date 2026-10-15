#include "MainWindow.h"
#include "ThumbnailListWidget.h"
#include <QDockWidget>
#include <QTabWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QStackedWidget>
#include <QHeaderView>
#include <QToolBar>
#include <QFileDialog>
#include <QStatusBar>
#include <QMenuBar>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCloseEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
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

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setupUi();
    setupActions();
    setWindowTitle(tr("PDF Reader - Linux Prototype"));
    resize(1024, 768);
}

MainWindow::~MainWindow() {
    // The dock is destroyed by ~QMainWindow, after this object's own destructor has finished. Without this,
    // its visibilityChanged lambda would run on a half-destroyed MainWindow (UBSan: vptr check).
    if (m_sidebarDock) {
        QObject::disconnect(m_sidebarDock, nullptr, this, nullptr);
    }
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_findBar = new FindBar(central);
    m_findBar->hide();
    layout->addWidget(m_findBar);

    m_viewer = new PdfViewerWidget(central);
    layout->addWidget(m_viewer, 1);

    setCentralWidget(central);

    connect(m_viewer, &PdfViewerWidget::documentLoaded, this, &MainWindow::onDocumentLoaded);
    connect(m_viewer, &PdfViewerWidget::pageChanged, this, &MainWindow::onPageChanged);
    connect(m_viewer, &PdfViewerWidget::zoomChanged, this, &MainWindow::onZoomChanged);
    connect(m_viewer, &PdfViewerWidget::loadError, this, &MainWindow::onLoadError);
    connect(m_viewer, &PdfViewerWidget::documentOpenFailed, this, &MainWindow::onDocumentOpenFailed);
    connect(m_viewer, &PdfViewerWidget::loadingChanged, this, &MainWindow::onLoadingChanged);

    connect(m_findBar, &FindBar::searchRequested, m_viewer, &PdfViewerWidget::findText);
    connect(m_findBar, &FindBar::findNextRequested, m_viewer, &PdfViewerWidget::findNext);
    connect(m_findBar, &FindBar::findPreviousRequested, m_viewer, &PdfViewerWidget::findPrevious);
    connect(m_findBar, &FindBar::closed, this, [this]() {
        m_viewer->clearSearch();
        m_viewer->setFocus();
    });
    connect(m_viewer, &PdfViewerWidget::searchResultsChanged, m_findBar, &FindBar::setMatchCount);
    connect(m_viewer, &PdfViewerWidget::searchProgressChanged, m_findBar, &FindBar::setSearchingProgress);
    connect(m_viewer, &PdfViewerWidget::searchFinished, m_findBar, &FindBar::finishSearch);

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

    connect(m_viewer, &PdfViewerWidget::outlineReady, this, &MainWindow::onOutlineReady);

    statusBar()->showMessage(tr("Ready - Open a PDF to begin"));
}

void MainWindow::setupActions() {
    // Menu Bar
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* openAction = fileMenu->addAction(tr("&Open PDF..."), this, &MainWindow::openFileDialog);
    openAction->setShortcut(QKeySequence::Open);

    QAction* closeAction = fileMenu->addAction(tr("&Close PDF"), m_viewer, &PdfViewerWidget::closeDocument);
    closeAction->setShortcut(QKeySequence::Close);

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    QAction* copyAction = editMenu->addAction(tr("&Copy"), m_viewer, &PdfViewerWidget::copySelection);
    copyAction->setShortcut(QKeySequence::Copy);
    copyAction->setEnabled(false);
    connect(m_viewer, &PdfViewerWidget::selectionChanged, copyAction, [this, copyAction]() {
        copyAction->setEnabled(m_viewer->hasSelection());
    });

    QAction* findAction = editMenu->addAction(tr("&Find..."), this, &MainWindow::showFindBar);
    findAction->setShortcut(QKeySequence::Find);
    QAction* findNextAction = editMenu->addAction(tr("Find &Next"), m_viewer, &PdfViewerWidget::findNext);
    findNextAction->setShortcut(QKeySequence(Qt::Key_F3));
    QAction* findPreviousAction = editMenu->addAction(tr("Find &Previous"), m_viewer, &PdfViewerWidget::findPrevious);
    findPreviousAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    m_toggleSidebarAction = viewMenu->addAction(tr("&Sidebar"), this, [this]() {
        m_sidebarDock->setVisible(!m_sidebarDock->isVisible());
    });
    m_toggleSidebarAction->setShortcut(QKeySequence(Qt::Key_F4));

    // Toolbar
    QToolBar* toolbar = addToolBar(tr("Main Toolbar"));
    toolbar->setMovable(false);

    toolbar->addAction(openAction);
    toolbar->addSeparator();

    // Page navigation controls
    QAction* prevAction = toolbar->addAction(tr("\u25c0 Prev"), m_viewer, &PdfViewerWidget::previousPage);
    prevAction->setShortcut(QKeySequence(Qt::Key_PageUp));

    m_pageStatusLabel = new QLabel(tr(" Page 0 / 0 "), this);
    toolbar->addWidget(m_pageStatusLabel);

    QAction* nextAction = toolbar->addAction(tr("Next \u25b6"), m_viewer, &PdfViewerWidget::nextPage);
    nextAction->setShortcut(QKeySequence(Qt::Key_PageDown));

    toolbar->addSeparator();

    // Zoom controls
    QAction* zoomOutAction = toolbar->addAction(tr("Zoom -"), m_viewer, &PdfViewerWidget::zoomOut);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);

    m_zoomStatusLabel = new QLabel(tr(" 100% "), this);
    toolbar->addWidget(m_zoomStatusLabel);

    QAction* zoomInAction = toolbar->addAction(tr("Zoom +"), m_viewer, &PdfViewerWidget::zoomIn);
    zoomInAction->setShortcut(QKeySequence::ZoomIn);

    toolbar->addAction(tr("Fit Width"), m_viewer, &PdfViewerWidget::zoomFitWidth);
    toolbar->addAction(tr("100%"), m_viewer, &PdfViewerWidget::zoomReset);
}

void MainWindow::showFindBar() {
    if (m_viewer->isDocumentOpen()) {
        m_findBar->focusSearch();
    }
}

bool MainWindow::openFile(const QString& filePath) {
    if (filePath.isEmpty()) {
        return false;
    }
    // Non-blocking: the window stays responsive while the file loads; results arrive via
    // documentLoaded() / loadError().
    m_viewer->openDocumentAsync(filePath);
    return true;
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

void MainWindow::onDocumentLoaded(const QString& title, int totalPages) {
    closePasswordDialog();
    m_findBar->clear();
    m_findBar->hide();
    m_outlineModel->clear();
    m_outlineEmptyLabel->setText(tr("Loading bookmarks…"));
    m_sidebarStack->setCurrentWidget(m_outlineEmptyLabel);
    m_thumbnailWidget->setDocument(totalPages, m_viewer->pageLabels());
    setWindowTitle(tr("%1 - PDF Reader").arg(title));
    statusBar()->showMessage(tr("Loaded '%1' (%2 pages)").arg(title).arg(totalPages));
}

void MainWindow::onPageChanged(int currentPage, int totalPages) {
    if (totalPages > 0) {
        m_pageStatusLabel->setText(tr(" Page %1 / %2 ").arg(currentPage).arg(totalPages));
        const int zeroBasedPage = currentPage - 1;
        m_thumbnailWidget->setCurrentPage(zeroBasedPage);
        if (m_outlineModel && m_outlineModel->rowCount() > 0) {
            auto* item = findBestOutlineItem(m_outlineModel->invisibleRootItem(), zeroBasedPage);
            if (item) {
                m_outlineView->blockSignals(true);
                m_outlineView->setCurrentIndex(item->index());
                m_outlineView->blockSignals(false);
            }
        }
    } else {
        m_pageStatusLabel->setText(tr(" Page 0 / 0 "));
        setWindowTitle(tr("PDF Reader - Linux Prototype"));
        m_thumbnailWidget->clearDocument();
    }
}

void MainWindow::onZoomChanged(double zoomRatio) {
    int percentage = static_cast<int>(std::round(zoomRatio * 100.0));
    m_zoomStatusLabel->setText(tr(" %1% ").arg(percentage));
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
            m_viewer->openDocumentAsync(m_passwordFilePath, password);
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
    m_outlineModel->clear();
    if (outline.empty()) {
        m_outlineEmptyLabel->setText(tr("No bookmarks in document"));
        m_sidebarStack->setCurrentWidget(m_outlineEmptyLabel);
        return;
    }

    addOutlineItemsRecursive(m_outlineModel->invisibleRootItem(), outline);
    m_sidebarStack->setCurrentWidget(m_outlineView);
    m_sidebarTabs->setCurrentWidget(m_sidebarStack);
    m_outlineView->expandToDepth(0);
    if (m_viewer->pageCount() > 0) {
        onPageChanged(m_viewer->currentPage() + 1, m_viewer->pageCount());
    }
    if (!m_sidebarUserHidden) {
        m_sidebarDock->show();
    }
}

void MainWindow::onOutlineItemClicked(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    int pageIndex = index.data(Qt::UserRole).toInt();
    if (pageIndex >= 0 && pageIndex < m_viewer->pageCount()) {
        if (index.data(Qt::UserRole + 2).toBool()) {
            m_viewer->goToPage(pageIndex, index.data(Qt::UserRole + 1).toDouble());
        } else {
            m_viewer->goToPage(pageIndex);
        }
        m_outlineView->setCurrentIndex(index);
        m_viewer->setFocus();
    }
}

} // namespace ui
