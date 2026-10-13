#include "MainWindow.h"
#include <QToolBar>
#include <QFileDialog>
#include <QStatusBar>
#include <QMenuBar>
#include <QAction>
#include <cmath>

namespace ui {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setupUi();
    setupActions();
    setWindowTitle(tr("PDF Reader - Linux Prototype Spike"));
    resize(1024, 768);
}

void MainWindow::setupUi() {
    m_viewer = new PdfViewerWidget(this);
    setCentralWidget(m_viewer);

    connect(m_viewer, &PdfViewerWidget::documentLoaded, this, &MainWindow::onDocumentLoaded);
    connect(m_viewer, &PdfViewerWidget::pageChanged, this, &MainWindow::onPageChanged);
    connect(m_viewer, &PdfViewerWidget::zoomChanged, this, &MainWindow::onZoomChanged);

    // Status bar setup
    statusBar()->showMessage(tr("Ready - Open a PDF to begin"));
}

void MainWindow::setupActions() {
    // Menu Bar
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    QAction* openAction = fileMenu->addAction(tr("&Open PDF..."), this, &MainWindow::openFileDialog);
    openAction->setShortcut(QKeySequence::Open);

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    exitAction->setShortcut(QKeySequence::Quit);

    // Toolbar
    QToolBar* toolbar = addToolBar(tr("Main Toolbar"));
    toolbar->setMovable(false);

    toolbar->addAction(openAction);
    toolbar->addSeparator();

    // Page navigation controls
    QAction* prevAction = toolbar->addAction(tr("◀ Prev"), m_viewer, &PdfViewerWidget::previousPage);
    prevAction->setShortcut(QKeySequence(Qt::Key_PageUp));

    m_pageStatusLabel = new QLabel(tr(" Page 0 / 0 "), this);
    toolbar->addWidget(m_pageStatusLabel);

    QAction* nextAction = toolbar->addAction(tr("Next ▶"), m_viewer, &PdfViewerWidget::nextPage);
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

bool MainWindow::openFile(const QString& filePath) {
    if (!m_viewer->openDocument(filePath)) {
        statusBar()->showMessage(tr("Failed to load PDF document: %1").arg(filePath), 5000);
        return false;
    }
    return true;
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
    setWindowTitle(tr("%1 - PDF Reader").arg(title));
    statusBar()->showMessage(tr("Loaded '%1' (%2 pages)").arg(title).arg(totalPages));
}

void MainWindow::onPageChanged(int currentPage, int totalPages) {
    if (totalPages > 0) {
        m_pageStatusLabel->setText(tr(" Page %1 / %2 ").arg(currentPage).arg(totalPages));
    } else {
        m_pageStatusLabel->setText(tr(" Page 0 / 0 "));
    }
}

void MainWindow::onZoomChanged(double zoom) {
    int percentage = static_cast<int>(std::round(zoom * 100.0));
    m_zoomStatusLabel->setText(tr(" %1% ").arg(percentage));
}

} // namespace ui
