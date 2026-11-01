#include "PdfViewerWidget.h"
#include "ImageUtils.h"
#include <QFileInfo>
#include <QMenu>
#include <QContextMenuEvent>

namespace {

constexpr qint64 MAX_VALIDATED_PDF_BYTES = 512LL * 1024 * 1024;

// Rotates a page-point rectangle into displayed page-point coordinates [px, py, pw, ph]
void rotateRectPointsForDisplay(const pdf::PageCharBox& r, double pageW, double pageH, int rotation,
                                double& outPx, double& outPy, double& outPw, double& outPh) {
    const int normRot = ((rotation % 360) + 360) % 360;
    outPx = r.left;
    outPy = r.top;
    outPw = r.width();
    outPh = r.height();

    switch (normRot) {
        case 90:
            outPx = pageH - r.bottom;
            outPy = r.left;
            outPw = r.height();
            outPh = r.width();
            break;
        case 180:
            outPx = pageW - r.right;
            outPy = pageH - r.bottom;
            break;
        case 270:
            outPx = r.top;
            outPy = pageW - r.right;
            outPw = r.height();
            outPh = r.width();
            break;
        default:
            break;
    }
}

} // namespace
#include <QPainter>
#include <QFont>
#include <QFontMetrics>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFileInfo>
#include <QScrollBar>
#include <QTemporaryFile>
#include <QSaveFile>
#include <QUuid>
#include <QTimer>
#include <QDir>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QScopedValueRollback>
#include <QToolTip>
#include <QInputDialog>
#include <QHelpEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace ui {

PdfViewerWidget::PdfViewerWidget(QWidget* parent)
    : QAbstractScrollArea(parent),
      m_layout(),
      m_scrollMapper(),
      m_cache(256 * 1024 * 1024), // 256 MB Tile Cache byte budget
      m_renderWorker(this),
      m_scale(DESKTOP_100_PERCENT_SCALE)
{
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_NoSystemBackground);
    viewport()->setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setFocusPolicy(Qt::StrongFocus);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &PdfViewerWidget::onScrollValueChanged);

    connect(&m_renderWorker, &pdf::ipc::WorkerClient::tileReady, this, &PdfViewerWidget::onTileReady);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::tileFailed, this, &PdfViewerWidget::onTileFailed);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::documentOpened, this, &PdfViewerWidget::onDocumentOpened);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::documentOutlineReady, this, &PdfViewerWidget::onDocumentOutlineReady);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::pageTextReady, this, &PdfViewerWidget::onPageTextReady);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::searchMatchesFound, this, &PdfViewerWidget::onSearchMatchesFound);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::searchProgress, this, &PdfViewerWidget::onSearchProgress);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::searchCompleted, this, &PdfViewerWidget::onSearchCompleted);
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::thumbnailReady, this,
            [this](int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer) {
                if (documentId == m_currentDocumentId && buffer.isValid()) {
                    QImage img(reinterpret_cast<const uchar*>(buffer.data()),
                               buffer.width(), buffer.height(), static_cast<qsizetype>(buffer.stride()),
                               QImage::Format_RGB32);
                    Q_EMIT thumbnailReady(pageIndex, img.copy());
                }
            });
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::thumbnailFailed, this,
            [this](int64_t documentId, int pageIndex) {
                if (documentId == m_currentDocumentId) {
                    Q_EMIT thumbnailFailed(pageIndex);
                }
            });
    connect(&m_renderWorker, &pdf::ipc::WorkerClient::workerFailed, this, [this](const QString& message) {
        // Unsaved edits live in the UI-owned edit log; remember where to reopen the document so they can be
        // re-applied to a fresh worker (RFC section 3.1). Only one recovery attempt per failure.
        const bool wantRecovery = !m_recovering && m_hasDocument && !m_loading &&
                                  !m_currentFilePath.isEmpty() && m_editLog.isDirty();
        if (wantRecovery) {
            m_recoveryPath = m_currentFilePath;
        }
        const QString failedPath = m_pendingOpenPath;
        if (m_loading) {
            m_loading = false;
            m_pendingOpenId = 0;
            m_pendingOpenPath.clear();
            m_pendingOpenPassword.clear();
            Q_EMIT loadingChanged(false, QFileInfo(failedPath).fileName());
            Q_EMIT documentOpenFailed(failedPath, pdf::DocumentError::InvalidFormat);
        }
        m_workerErrorMessage = message;
        m_hasDocument = false;
        m_currentFilePath.clear();
        m_pageCount = 0;
        m_inFlightKeys.clear();
        Q_EMIT loadError(message);
        viewport()->update();
        if (wantRecovery) {
            QTimer::singleShot(0, this, &PdfViewerWidget::recoverAfterWorkerFailure);
        }
    });

    setStyleSheet(QStringLiteral("QAbstractScrollArea { background-color: #323639; border: none; }"));
}

PdfViewerWidget::~PdfViewerWidget() {
    // Clean teardown: stop worker and flush memory without touching scrollbars or emitting pageChanged during destruction
    m_renderWorker.stop();
    m_cache.clear();
    m_inFlightKeys.clear();
    m_pageTextCache.clear();
}

void PdfViewerWidget::setDevicePixelRatioOverride(double dpr) {
    m_dprOverride = dpr > 0.0 ? dpr : 0.0;
    updateLayoutGeometry();
}

double PdfViewerWidget::effectiveDpr() const {
    return (m_dprOverride > 0.0) ? m_dprOverride : pdf::quantizeDpr(devicePixelRatioF());
}

void PdfViewerWidget::retryFailedTiles() {
    m_cache.resetFailureHistory(); // explicit user retry also lifts the permanent-failure state
    m_cache.clearAllFailed();
    m_inFlightKeys.clear();
    m_transformedPixmapCache.clear();
    viewport()->update();
}

void PdfViewerWidget::waitForRendering() {
    m_renderWorker.waitForIdle();
    QCoreApplication::processEvents();
}

QString PdfViewerWidget::openErrorMessage(pdf::DocumentError error, const QString& filePath) const {
    switch (error) {
        case pdf::DocumentError::FileNotFound:
            return tr("File not found: %1").arg(filePath);
        case pdf::DocumentError::PasswordRequired:
            return tr("Document is password protected.");
        case pdf::DocumentError::BadPassword:
            return tr("Incorrect password entered.");
        case pdf::DocumentError::EmptyDocument:
            return tr("Document contains no pages.");
        case pdf::DocumentError::InvalidFormat:
        default:
            return tr("Corrupted or unsupported PDF format.");
    }
}

void PdfViewerWidget::applyOpenedDocument(int64_t nextDocId, const QString& filePath, const QString& password, const pdf::OpenResult& openResult) {
    m_workerErrorMessage.clear();
    // Success: atomically cancel worker work, flush previous cache, and advance epochs
    m_renderWorker.cancelAll();
    m_cache.invalidateDocument(m_currentDocumentId);
    m_inFlightKeys.clear();
    m_pageTextCache.clear();
    m_transformedPixmapCache.clear();
    clearSelection();
    clearSearch();
    if (!m_preserveEditLogOnOpen) {
        m_editLog.clear();
    }
    m_currentDocumentId = nextDocId;
    ++m_currentGeneration;
    m_renderWorker.setGeneration(m_currentGeneration);

    m_hasDocument = true;
    m_currentFilePath = filePath;
    m_currentPassword = password;
    const QFileInfo sourceInfo(filePath);
    m_sourceFileSize = sourceInfo.exists() ? sourceInfo.size() : -1;
    m_sourceFileModified = sourceInfo.exists() ? sourceInfo.lastModified() : QDateTime();
    m_pageCount = openResult.pageCount;
    m_layout.setFocusPage(0);
    m_layout.setPageSizes(openResult.pageSizes);
    m_pageLabels.clear();
    m_pageLabels.reserve(m_pageCount);
    if (!openResult.pageLabels.empty() && static_cast<int>(openResult.pageLabels.size()) == m_pageCount) {
        for (const auto& l : openResult.pageLabels) {
            m_pageLabels.push_back(QString::fromStdString(l));
        }
    } else {
        for (int i = 0; i < m_pageCount; ++i) {
            m_pageLabels.push_back(QString::number(i + 1));
        }
    }
    Q_EMIT pageLabelsReady(m_pageLabels);
    m_outline.clear();
    m_outlineRequestPending = true;

    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;
    m_scale = DESKTOP_100_PERCENT_SCALE;
    m_fallbackScales.clear(); // tiles of the previous document are gone; nothing to stand in

    const double dpr = effectiveDpr();
    m_lastVpWidthDevice = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    m_lastVpHeightDevice = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    updateLayoutGeometry();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);

    Q_EMIT documentLoaded(QFileInfo(filePath).fileName(), m_pageCount);
    Q_EMIT pageChanged(1, m_pageCount);
    Q_EMIT zoomChanged(1.0);

    viewport()->update();
}

pdf::DocumentError PdfViewerWidget::openDocument(const QString& filePath, const QString& password) {
    if (filePath.isEmpty()) {
        return pdf::DocumentError::FileNotFound;
    }
    if (m_saveInProgress || m_editInProgress ||
        (m_editLog.isDirty() && !m_preserveEditLogOnOpen)) {
        Q_EMIT loadError(tr("Save or discard the current edits before opening another document."));
        return pdf::DocumentError::InvalidFormat;
    }

    // A synchronous open supersedes any asynchronous open that is pending or queued.
    cancelPendingOpen();
    QScopedValueRollback<bool> openGuard(m_editInProgress, true);

    const int64_t nextDocId = m_currentDocumentId + 1;
    // Open serially on the engine worker thread - 0 PDFium calls on UI thread!
    auto openResult = m_renderWorker.openDocumentSync(nextDocId, filePath.toStdString(), password.toStdString());
    if (openResult.error != pdf::DocumentError::None) {
        Q_EMIT loadError(openErrorMessage(openResult.error, filePath));
        return openResult.error; // Existing document state remains intact on failure
    }

    applyOpenedDocument(nextDocId, filePath, password, openResult);
    return pdf::DocumentError::None;
}

void PdfViewerWidget::openDocumentAsync(const QString& filePath, const QString& password) {
    if (filePath.isEmpty()) {
        Q_EMIT loadError(openErrorMessage(pdf::DocumentError::FileNotFound, filePath));
        return;
    }
    if (m_saveInProgress || m_editInProgress ||
        (m_editLog.isDirty() && !m_preserveEditLogOnOpen)) {
        Q_EMIT loadError(tr("Save or discard the current edits before opening another document."));
        return;
    }
    if (m_loading) {
        // Coalesce: only the most recent request made during a load is opened afterwards
        m_queuedOpenPath = filePath;
        m_queuedOpenPassword = password;
        m_hasQueuedOpen = true;
        return;
    }
    startAsyncOpen(filePath, password);
}

void PdfViewerWidget::startAsyncOpen(const QString& filePath, const QString& password) {
    m_workerErrorMessage.clear();
    m_loading = true;
    m_pendingOpenId = m_currentDocumentId + 1;
    m_pendingOpenPath = filePath;
    m_pendingOpenPassword = password;
    Q_EMIT loadingChanged(true, QFileInfo(filePath).fileName());
    viewport()->update();
    m_renderWorker.openDocumentAsync(m_pendingOpenId, filePath.toStdString(), password.toStdString());
}

void PdfViewerWidget::cancelPendingOpen() {
    const bool wasLoading = m_loading;
    const QString name = QFileInfo(m_pendingOpenPath).fileName();
    m_loading = false;
    m_pendingOpenId = 0; // any result still in flight is ignored by onDocumentOpened()
    m_pendingOpenPath.clear();
    m_pendingOpenPassword.clear();
    m_hasQueuedOpen = false;
    m_queuedOpenPath.clear();
    m_queuedOpenPassword.clear();
    if (wasLoading) {
        Q_EMIT loadingChanged(false, name);
        viewport()->update();
    }
}

void PdfViewerWidget::onDocumentOpened(int64_t documentId, pdf::OpenResult result) {
    if (!m_loading || documentId != m_pendingOpenId) {
        return; // cancelled by closeDocument()/openDocument() while the load was running
    }

    const QString path = m_pendingOpenPath;
    const QString password = m_pendingOpenPassword;
    m_loading = false;
    m_pendingOpenId = 0;
    m_pendingOpenPath.clear();
    m_pendingOpenPassword.clear();

    if (result.error != pdf::DocumentError::None) {
        Q_EMIT documentOpenFailed(path, result.error);
        Q_EMIT loadError(openErrorMessage(result.error, path));
    } else {
        applyOpenedDocument(documentId, path, password, result);
    }

    if (m_hasQueuedOpen) {
        const QString next = m_queuedOpenPath;
        const QString nextPassword = m_queuedOpenPassword;
        m_hasQueuedOpen = false;
        m_queuedOpenPath.clear();
        m_queuedOpenPassword.clear();
        startAsyncOpen(next, nextPassword); // stays "loading"; no intermediate loadingChanged(false)
        return;
    }

    Q_EMIT loadingChanged(false, QFileInfo(path).fileName());
    viewport()->update();
}

void PdfViewerWidget::closeDocument() {
    if (m_saveInProgress || m_editInProgress || m_recovering) {
        return;
    }
    QScopedValueRollback<bool> closeGuard(m_editInProgress, true);
    clearStagedRedactions();
    m_isRedacting = false;
    m_redactStartPage = -1;
    m_workerErrorMessage.clear();
    cancelPendingOpen();
    // Close document and page cache serially on worker thread
    m_renderWorker.closeDocumentSync();

    m_cache.invalidateDocument(m_currentDocumentId);
    m_inFlightKeys.clear();
    m_pageTextCache.clear();
    m_transformedPixmapCache.clear();
    clearSelection();
    clearSearch();
    ++m_currentDocumentId;
    ++m_currentGeneration;

    m_rotation = 0;
    m_layout.setRotation(0);
    Q_EMIT rotationChanged(0);

    m_hasDocument = false;
    m_editLog.clear();
    m_currentFilePath.clear();
    m_currentPassword.clear();
    m_sourceFileSize = -1;
    m_sourceFileModified = QDateTime();
    m_fallbackScales.clear();
    m_pageCount = 0;
    m_layout.setPageSizes({});
    m_outline.clear();
    m_outlineRequestPending = false;
    m_pageLabels.clear();
    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;
    m_lastVpWidthDevice = 0;
    m_lastVpHeightDevice = 0;

    updateScrollBars();
    viewport()->update();
    Q_EMIT pageChanged(0, 0);
    Q_EMIT outlineReady(m_outline);
}

void PdfViewerWidget::setColorMode(ColorMode mode) {
    if (m_colorMode == mode) {
        return;
    }
    const bool wasDark = isDarkMode();
    m_colorMode = mode;
    m_darkMode = (m_colorMode == ColorMode::Inverted || m_colorMode == ColorMode::HighContrast);
    m_transformedPixmapCache.clear();
    const bool nowDark = isDarkMode();
    if (wasDark != nowDark) {
        Q_EMIT darkModeChanged(nowDark);
    }
    Q_EMIT colorModeChanged(m_colorMode);
    viewport()->update();
}

void PdfViewerWidget::setDarkMode(bool enabled) {
    setColorMode(enabled ? ColorMode::Inverted : ColorMode::Normal);
}

void PdfViewerWidget::toggleDarkMode() {
    setColorMode(m_colorMode == ColorMode::Normal ? ColorMode::Inverted : ColorMode::Normal);
}

void PdfViewerWidget::restoreReadingPosition(int pageIndex, double zoom, int64_t scrollX, int64_t scrollY) {
    if (!isDocumentOpen()) {
        return;
    }
    if (zoom >= 0.1 && zoom <= 10.0) {
        setZoom(zoom);
    }
    // Saved scroll offsets are in continuous-layout coordinates; other modes restore by page.
    if ((scrollY > 0 || scrollX > 0) && m_layout.viewMode() == layout::ViewMode::Continuous) {
        setDocScrollPosition(scrollX, scrollY);
        viewport()->update();
    } else if (pageIndex >= 0 && pageIndex < pageCount()) {
        goToPage(pageIndex);
    }
}

void PdfViewerWidget::restoreReadingPosition(int pageIndex, double zoom, double fractionX, double fractionY,
                                            int64_t fallbackScrollX, int64_t fallbackScrollY) {
    if (!isDocumentOpen()) {
        return;
    }
    if (zoom >= 0.1 && zoom <= 10.0) {
        setZoom(zoom);
    }
    // Continuous layout: restore exact fractional offset on page
    if (m_layout.viewMode() == layout::ViewMode::Continuous) {
        if (pageIndex >= 0 && pageIndex < pageCount() && std::isfinite(fractionY) && fractionY >= 0.0) {
            const auto& pl = m_layout.getPageLayout(pageIndex);
            int64_t targetY = pl.topPixel + static_cast<int64_t>(std::llround(fractionY * static_cast<double>(pl.heightPixel)));
            int64_t targetX = 0;
            if (std::isfinite(fractionX) && fractionX > 0.0) {
                targetX = pl.leftPixel + static_cast<int64_t>(std::llround(fractionX * static_cast<double>(pl.widthPixel)));
            } else if (fallbackScrollX > 0) {
                targetX = fallbackScrollX;
            }
            setDocScrollPosition(targetX, targetY);
            viewport()->update();
            return;
        } else if (fallbackScrollY > 0 || fallbackScrollX > 0) {
            setDocScrollPosition(fallbackScrollX, fallbackScrollY);
            viewport()->update();
            return;
        }
    }
    if (pageIndex >= 0 && pageIndex < pageCount()) {
        goToPage(pageIndex);
    }
}

bool PdfViewerWidget::getReadingPosition(int& outPage, double& outFractionX, double& outFractionY) const {
    if (!isDocumentOpen() || pageCount() <= 0) {
        return false;
    }
    if (m_layout.viewMode() == layout::ViewMode::Continuous) {
        auto anchor = m_layout.createAnchor(m_docScrollX, m_docScrollY);
        if (anchor.isValid() && anchor.pageIndex >= 0 && anchor.pageIndex < pageCount()) {
            outPage = anchor.pageIndex;
            const auto& pl = m_layout.getPageLayout(anchor.pageIndex);
            if (m_docScrollX > pl.leftPixel && pl.widthPixel > 0) {
                outFractionX = std::clamp(static_cast<double>(m_docScrollX - pl.leftPixel) / static_cast<double>(pl.widthPixel), 0.0, 1.0);
            } else {
                outFractionX = 0.0;
            }
            if (pl.heightPixel > 0) {
                if (m_docScrollY >= pl.topPixel) {
                    outFractionY = std::clamp(static_cast<double>(m_docScrollY - pl.topPixel) / static_cast<double>(pl.heightPixel), 0.0, 1.0);
                } else {
                    outFractionY = 0.0;
                }
            } else {
                outFractionY = 0.0;
            }
            return true;
        }
    }
    outPage = std::clamp(m_currentPage, 0, pageCount() - 1);
    outFractionX = 0.0;
    outFractionY = 0.0;
    return true;
}

QString PdfViewerWidget::pageLabel(int pageIndex) const {
    if (pageIndex >= 0 && pageIndex < static_cast<int>(m_pageLabels.size())) {
        return m_pageLabels[static_cast<size_t>(pageIndex)];
    }
    return QString::number(pageIndex + 1);
}

void PdfViewerWidget::requestThumbnail(int pageIndex, int targetWidth) {
    if (m_hasDocument && pageIndex >= 0 && pageIndex < m_pageCount) {
        m_renderWorker.requestThumbnail(m_currentDocumentId, pageIndex, targetWidth, m_rotation);
    }
}

void PdfViewerWidget::nextPage() {
    // In the two-page modes a "page" step moves by a whole spread.
    const int target = m_layout.rowFirstPage(m_currentPage) + m_layout.rowPageCount(m_currentPage);
    if (target < pageCount()) {
        goToPage(target);
    }
}

void PdfViewerWidget::previousPage() {
    const int rowFirst = m_layout.rowFirstPage(m_currentPage);
    if (rowFirst > 0) {
        goToPage(m_layout.rowFirstPage(rowFirst - 1));
    } else if (m_currentPage > rowFirst) {
        goToPage(rowFirst);
    }
}

void PdfViewerWidget::ensurePageShown(int pageIndex) {
    if (m_layout.viewMode() != layout::ViewMode::SinglePage || m_layout.focusPage() == pageIndex) {
        return;
    }
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    m_layout.setFocusPage(pageIndex);
    m_layout.updateLayout(pdf::quantizeScale(m_scale), dpr, vpW);
    updateScrollBars();
    // Document coordinates changed meaning; drop work queued for the previous page position.
    ++m_currentGeneration;
    m_inFlightKeys.clear();
    m_renderWorker.setGeneration(m_currentGeneration);
}

void PdfViewerWidget::setViewMode(layout::ViewMode mode) {
    if (mode == m_layout.viewMode()) {
        return;
    }
    const int page = std::clamp(m_currentPage, 0, std::max(0, pageCount() - 1));
    m_layout.setViewMode(mode);
    m_layout.setFocusPage(page);
    m_pageFlipAccumulator = 0;
    if (isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
        m_layout.updateLayout(pdf::quantizeScale(m_scale), dpr, vpW);
        updateScrollBars();
        ++m_currentGeneration;
        m_inFlightKeys.clear();
        m_renderWorker.setGeneration(m_currentGeneration);
        goToPage(page);
    }
    Q_EMIT viewModeChanged(mode);
    viewport()->update();
}

void PdfViewerWidget::setRotation(int degrees) {
    const int normalized = (degrees % 360 + 360) % 360;
    if (m_rotation == normalized) {
        return;
    }
    if (!isDocumentOpen()) {
        m_rotation = normalized;
        m_layout.setRotation(m_rotation);
        Q_EMIT rotationChanged(m_rotation);
        viewport()->update();
        return;
    }

    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));
    const int64_t centreDocX = m_docScrollX + vpW / 2;
    const int64_t centreDocY = m_docScrollY + vpH / 2;

    // Rotation is display-only, so the page-space point under the viewport centre is
    // invariant across it. Remember that point before the relayout and scroll back to
    // it afterwards, instead of jumping to the top of the current page.
    const int anchorPage = m_layout.getPageAt(centreDocX, centreDocY);
    double anchorPageX = 0.0;
    double anchorPageY = 0.0;
    const bool haveAnchor = anchorPage >= 0 &&
        m_layout.documentToPagePoints(anchorPage, centreDocX, centreDocY, anchorPageX, anchorPageY);

    m_rotation = normalized;
    m_layout.setRotation(m_rotation);

    ++m_currentGeneration;
    m_inFlightKeys.clear();
    m_transformedPixmapCache.clear();
    m_renderWorker.setGeneration(m_currentGeneration);

    m_layout.updateLayout(pdf::quantizeScale(m_scale), dpr, vpW);
    updateScrollBars();

    int64_t newDocX = m_docScrollX;
    int64_t newDocY = m_docScrollY;
    if (haveAnchor) {
        int64_t anchorDocX = 0;
        int64_t anchorDocY = 0;
        if (m_layout.pagePointsToDocument(anchorPage, anchorPageX, anchorPageY, anchorDocX, anchorDocY)) {
            newDocX = anchorDocX - vpW / 2;
            newDocY = anchorDocY - vpH / 2;
        }
    }
    setDocScrollPosition(newDocX, newDocY);

    Q_EMIT rotationChanged(m_rotation);
    viewport()->update();
}

void PdfViewerWidget::rotateClockwise() {
    setRotation(m_rotation + 90);
}

void PdfViewerWidget::rotateCounterClockwise() {
    setRotation(m_rotation - 90);
}

QImage PdfViewerWidget::renderPageForPrint(int pageIndex, double scale, double dpr) {
    if (!isDocumentOpen() || pageIndex < 0 || pageIndex >= m_pageCount) {
        return {};
    }
    pdf::PixelBuffer buf = m_renderWorker.renderPageForPrintSync(m_currentDocumentId, pageIndex, scale, dpr, m_rotation);
    return toQImage(buf, dpr).copy();
}

QByteArray PdfViewerWidget::extractPagesPdf(int firstPage, int lastPage) {
    if (!isDocumentOpen() || firstPage < 0 || lastPage < firstPage || lastPage >= m_pageCount) {
        return {};
    }
    return m_renderWorker.extractPagesSync(m_currentDocumentId, firstPage, lastPage);
}

void PdfViewerWidget::goToPage(int pageIndex) {
    goToPageImpl(pageIndex, std::nullopt);
}

void PdfViewerWidget::goToPage(int pageIndex, double targetYFraction) {
    goToPageImpl(pageIndex, targetYFraction);
}

void PdfViewerWidget::goToPageImpl(int pageIndex, std::optional<double> targetYFraction) {
    if (!isDocumentOpen()) {
        return;
    }
    if (pageIndex < 0 || pageIndex >= pageCount()) {
        return;
    }

    ensurePageShown(pageIndex);
    const auto& pl = m_layout.getPageLayout(pageIndex);

    const int64_t oldScrollX = m_docScrollX;
    const int64_t oldScrollY = m_docScrollY;

    int64_t targetDocY = pl.topPixel;
    if (targetYFraction && std::isfinite(*targetYFraction)) {
        const double topDownFraction = std::clamp(*targetYFraction, 0.0, 1.0);
        int64_t docX = 0, docY = 0;
        if (m_layout.pagePointsToDocument(pageIndex, 0.0, topDownFraction * pl.heightPoints, docX, docY)) {
            targetDocY = docY;
        } else {
            targetDocY += static_cast<int64_t>(std::llround(topDownFraction * static_cast<double>(pl.heightPixel)));
        }
    }

    m_navigating = true;
    setDocScrollPosition(m_docScrollX, targetDocY);
    m_navigating = false;

    if (m_docScrollX != oldScrollX || m_docScrollY != oldScrollY) {
        // setDocScrollPosition() blocks the scrollbar signals, so prune queued tiles of the old position
        // here (as onScrollValueChanged() does); otherwise a long jump renders stale tiles first.
        ++m_currentGeneration;
        m_inFlightKeys.clear();
        m_renderWorker.setGeneration(m_currentGeneration);
    }

    if (m_currentPage != pageIndex) {
        m_currentPage = pageIndex;
        Q_EMIT pageChanged(m_currentPage + 1, pageCount());
    }

    viewport()->update();
}

void PdfViewerWidget::setZoom(double zoom, const QPoint* anchorWidgetPos) {
    if (!isDocumentOpen()) {
        return;
    }

    // Zoom bounds in scale units (1.0 = 72 dpi, so 100% on screen is 96/72): 0.1 .. 10.0
    zoom = std::clamp(zoom, 0.1, 10.0);
    if (std::abs(zoom - m_scale) < 1e-4) {
        return;
    }

    m_transformedPixmapCache.clear();
    rememberFallbackScale(pdf::quantizeScale(m_scale));

    const double dpr = effectiveDpr();
    const double qOldScale = pdf::quantizeScale(m_scale);
    const double qNewScale = pdf::quantizeScale(zoom);

    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    // Determine 2D anchor point
    int64_t anchorDocX = 0;
    int64_t anchorDocY = 0;
    int64_t widgetAnchorX = vpW / 2;
    int64_t widgetAnchorY = vpH / 2;

    if (anchorWidgetPos) {
        widgetAnchorX = static_cast<int64_t>(std::round(anchorWidgetPos->x() * dpr));
        widgetAnchorY = static_cast<int64_t>(std::round(anchorWidgetPos->y() * dpr));
        anchorDocX = m_docScrollX + widgetAnchorX;
        anchorDocY = m_docScrollY + widgetAnchorY;
    } else {
        anchorDocX = m_docScrollX + widgetAnchorX;
        anchorDocY = m_docScrollY + widgetAnchorY;
    }

    auto anchor = m_layout.createAnchor(anchorDocX, anchorDocY);

    m_scale = zoom;

    // Advance generation on zoom changes to prune obsolete tile renders immediately
    if (qNewScale != qOldScale) {
        ++m_currentGeneration;
        m_inFlightKeys.clear();
        m_renderWorker.setGeneration(m_currentGeneration);
    }

    m_layout.updateLayout(qNewScale, dpr, vpW);
    updateScrollBars();

    int64_t newDocX = 0;
    int64_t newDocY = 0;
    m_layout.resolveAnchor(anchor, widgetAnchorX, widgetAnchorY, vpW, vpH, newDocX, newDocY);

    setDocScrollPosition(newDocX, newDocY);

    viewport()->update();
    Q_EMIT zoomChanged(m_scale / DESKTOP_100_PERCENT_SCALE);
}

void PdfViewerWidget::zoomIn() {
    setZoom(m_scale * ZOOM_STEP);
}

void PdfViewerWidget::zoomOut() {
    setZoom(m_scale / ZOOM_STEP);
}

void PdfViewerWidget::zoomReset() {
    setZoom(DESKTOP_100_PERCENT_SCALE);
}

void PdfViewerWidget::zoomFitWidth() {
    if (!isDocumentOpen() || pageCount() == 0) {
        return;
    }

    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    constexpr int64_t HORIZONTAL_MARGIN = 16;
    const bool twoUp = m_layout.viewMode() == layout::ViewMode::TwoPage ||
                       m_layout.viewMode() == layout::ViewMode::TwoPageCover;
    const int64_t availableWidth = vpW - (2 * HORIZONTAL_MARGIN) - (twoUp ? m_layout.columnGapPixels() : 0);

    if (availableWidth <= 0) {
        return;
    }

    // Widest page (continuous), widest spread (two-page modes) or the shown page (single page)
    const double widestPoints = m_layout.widestRowPoints();
    if (widestPoints > 0.0) {
        double fitScale = static_cast<double>(availableWidth) / (widestPoints * dpr);
        setZoom(fitScale);
    }
}

void PdfViewerWidget::zoomFitPage() {
    if (!isDocumentOpen() || pageCount() == 0) {
        return;
    }

    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));
    constexpr int64_t MARGIN = 16;
    const bool twoUp = m_layout.viewMode() == layout::ViewMode::TwoPage ||
                       m_layout.viewMode() == layout::ViewMode::TwoPageCover;
    const int64_t availW = vpW - (2 * MARGIN) - (twoUp ? m_layout.columnGapPixels() : 0);
    const int64_t availH = vpH - (2 * m_layout.columnGapPixels());
    if (availW <= 0 || availH <= 0) {
        return;
    }

    const int page = m_currentPage;
    const auto [rowW, rowH] = m_layout.rowSizePoints(page);
    if (rowW <= 0.0 || rowH <= 0.0) {
        return;
    }
    const double fitScale = std::min(static_cast<double>(availW) / (rowW * dpr),
                                     static_cast<double>(availH) / (rowH * dpr));
    setZoom(fitScale);
    goToPage(page);
}

void PdfViewerWidget::onScrollValueChanged() {
    if (m_navigating || !isDocumentOpen()) {
        return;
    }
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int64_t newScrollX = m_scrollMapper.sliderToDoc(horizontalScrollBar()->value(), m_layout.maxDocumentWidth(), vpW);
    int64_t newScrollY = m_scrollMapper.sliderToDoc(verticalScrollBar()->value(), m_layout.totalDocumentHeight(), vpH);

    bool scrollChanged = (newScrollX != m_docScrollX || newScrollY != m_docScrollY);
    if (scrollChanged) {
        m_docScrollX = newScrollX;
        m_docScrollY = newScrollY;

        // Advance request generation on scroll to prune off-screen queued work
        ++m_currentGeneration;
        m_inFlightKeys.clear();
        m_renderWorker.setGeneration(m_currentGeneration);
    }

    m_lastVpWidthDevice = vpW;
    m_lastVpHeightDevice = vpH;

    auto visPages = m_layout.getVisiblePages(m_docScrollY, vpH);
    if (visPages.isValid()) {
        int bestPage = -1;
        int64_t maxOverlap = 0;
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            const auto& pl = m_layout.getPageLayout(p);
            int64_t overlapTop = std::max(m_docScrollY, pl.topPixel);
            int64_t overlapBottom = std::min(m_docScrollY + vpH, pl.bottomPixel());
            int64_t overlap = overlapBottom - overlapTop;
            if (overlap > maxOverlap) {
                maxOverlap = overlap;
                bestPage = p;
            }
        }
        if (bestPage >= 0 && bestPage != m_currentPage) {
            m_currentPage = bestPage;
            Q_EMIT pageChanged(m_currentPage + 1, pageCount());
        }
    }
    viewport()->update();
}

void PdfViewerWidget::onTileReady(int64_t documentId, uint64_t /*generation*/, const pdf::TileKey& key, const pdf::PixelBuffer& buffer) {
    m_inFlightKeys.erase(key);
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_cache.put(key, buffer);
    // Prefetched off-screen tiles do not change what is on screen: skip the repaint
    if (m_cache.isPinned(key)) {
        viewport()->update();
    }
}

void PdfViewerWidget::onTileFailed(int64_t documentId, uint64_t /*generation*/, const pdf::TileKey& key) {
    m_inFlightKeys.erase(key);
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_cache.markFailed(key);
    viewport()->update();
}

void PdfViewerWidget::onPageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text) {
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_pageTextCache[pageIndex] = text ? std::move(text) : std::make_shared<pdf::PageText>();
    trimPageTextCache();
    viewport()->update();
}

void PdfViewerWidget::trimPageTextCache() {
    if (m_pageTextCache.size() <= MAX_CACHED_TEXT_PAGES) {
        return;
    }
    // Pages that must stay: the selection range (its text is needed for copy) ...
    int keepFirst = 1 << 30, keepLast = -1;
    if (m_selectionStart.isValid() && m_selectionEnd.isValid()) {
        keepFirst = std::min(m_selectionStart.pageIndex, m_selectionEnd.pageIndex);
        keepLast = std::max(m_selectionStart.pageIndex, m_selectionEnd.pageIndex);
    }
    // ... and, of the rest, the pages nearest to the one being viewed. Requests still in flight are kept.
    std::vector<int> evictable;
    evictable.reserve(m_pageTextCache.size());
    for (const auto& [page, text] : m_pageTextCache) {
        if (text && (page < keepFirst || page > keepLast)) {
            evictable.push_back(page);
        }
    }
    std::sort(evictable.begin(), evictable.end(), [this](int a, int b) {
        return std::abs(a - m_currentPage) > std::abs(b - m_currentPage); // farthest first
    });
    for (int page : evictable) {
        if (m_pageTextCache.size() <= MAX_CACHED_TEXT_PAGES) {
            break;
        }
        m_pageTextCache.erase(page);
    }
}

void PdfViewerWidget::ensurePageTextRequested(int pageIndex) {
    if (!isDocumentOpen() || pageIndex < 0 || pageIndex >= m_pageCount) {
        return;
    }
    if (m_pageTextCache.find(pageIndex) == m_pageTextCache.end()) {
        m_pageTextCache[pageIndex] = nullptr; // marked in-flight
        m_renderWorker.requestPageText(m_currentDocumentId, pageIndex);
    }
}

void PdfViewerWidget::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateLayoutGeometry();
}

void PdfViewerWidget::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        m_wheelDeltaAccumulator += event->angleDelta().y();
        constexpr int DELTA_THRESHOLD = 120; // 1 mouse wheel click step

        QPoint pos = event->position().toPoint();
        while (m_wheelDeltaAccumulator >= DELTA_THRESHOLD) {
            setZoom(m_scale * ZOOM_STEP, &pos);
            m_wheelDeltaAccumulator -= DELTA_THRESHOLD;
        }
        while (m_wheelDeltaAccumulator <= -DELTA_THRESHOLD) {
            setZoom(m_scale / ZOOM_STEP, &pos);
            m_wheelDeltaAccumulator += DELTA_THRESHOLD;
        }
        event->accept();
    } else if (m_layout.viewMode() == layout::ViewMode::SinglePage && isDocumentOpen() &&
               event->angleDelta().y() != 0) {
        // Scrolling past the top/bottom edge of the page flips to the neighbouring page after two wheel notches
        auto* vbar = verticalScrollBar();
        const int dy = event->angleDelta().y();
        const bool atBottom = vbar->value() >= vbar->maximum();
        const bool atTop = vbar->value() <= vbar->minimum();
        if (dy < 0 && atBottom) {
            m_pageFlipAccumulator = std::max(0, m_pageFlipAccumulator) - dy;
        } else if (dy > 0 && atTop) {
            m_pageFlipAccumulator = std::min(0, m_pageFlipAccumulator) - dy;
        } else {
            m_pageFlipAccumulator = 0;
        }
        constexpr int FLIP_THRESHOLD = 240;
        if (m_pageFlipAccumulator >= FLIP_THRESHOLD && m_currentPage + 1 < pageCount()) {
            m_pageFlipAccumulator = 0;
            goToPage(m_currentPage + 1);
            event->accept();
        } else if (m_pageFlipAccumulator <= -FLIP_THRESHOLD && m_currentPage > 0) {
            m_pageFlipAccumulator = 0;
            goToPage(m_currentPage - 1);
            vbar->setValue(vbar->maximum()); // arrive at the bottom of the previous page
            event->accept();
        } else {
            QAbstractScrollArea::wheelEvent(event);
        }
    } else {
        QAbstractScrollArea::wheelEvent(event);
    }
}

void PdfViewerWidget::setDocScrollPosition(int64_t docX, int64_t docY) {
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    const int64_t maxX = std::max<int64_t>(0, m_layout.maxDocumentWidth() - vpW);
    const int64_t maxY = std::max<int64_t>(0, m_layout.totalDocumentHeight() - vpH);

    m_docScrollX = std::clamp<int64_t>(docX, 0, maxX);
    m_docScrollY = std::clamp<int64_t>(docY, 0, maxY);

    m_lastVpWidthDevice = vpW;
    m_lastVpHeightDevice = vpH;

    QSignalBlocker blockV(verticalScrollBar());
    QSignalBlocker blockH(horizontalScrollBar());

    verticalScrollBar()->setValue(m_scrollMapper.docToSlider(m_docScrollY, m_layout.totalDocumentHeight(), vpH));
    horizontalScrollBar()->setValue(m_scrollMapper.docToSlider(m_docScrollX, m_layout.maxDocumentWidth(), vpW));

    if (!m_navigating) {
        auto visPages = m_layout.getVisiblePages(m_docScrollY, vpH);
        if (visPages.isValid()) {
            int bestPage = -1;
            int64_t maxOverlap = 0;
            for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
                const auto& pl = m_layout.getPageLayout(p);
                int64_t overlapTop = std::max(m_docScrollY, pl.topPixel);
                int64_t overlapBottom = std::min(m_docScrollY + vpH, pl.bottomPixel());
                int64_t overlap = overlapBottom - overlapTop;
                if (overlap > maxOverlap) {
                    maxOverlap = overlap;
                    bestPage = p;
                }
            }
            if (bestPage >= 0 && bestPage != m_currentPage) {
                m_currentPage = bestPage;
                Q_EMIT pageChanged(m_currentPage + 1, pageCount());
            }
        }
    }
}

void PdfViewerWidget::updateScrollBars() {
    const double dpr = effectiveDpr();
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    int32_t maxSliderV = 0, pageStepV = 0, singleStepV = 0;
    m_scrollMapper.computeRange(m_layout.totalDocumentHeight(), vpH, maxSliderV, pageStepV, singleStepV, static_cast<int64_t>(std::round(40 * dpr)));

    int32_t maxSliderH = 0, pageStepH = 0, singleStepH = 0;
    m_scrollMapper.computeRange(m_layout.maxDocumentWidth(), vpW, maxSliderH, pageStepH, singleStepH, static_cast<int64_t>(std::round(40 * dpr)));

    QSignalBlocker blockV(verticalScrollBar());
    QSignalBlocker blockH(horizontalScrollBar());

    verticalScrollBar()->setRange(0, maxSliderV);
    verticalScrollBar()->setPageStep(pageStepV);
    verticalScrollBar()->setSingleStep(singleStepV);

    horizontalScrollBar()->setRange(0, maxSliderH);
    horizontalScrollBar()->setPageStep(pageStepH);
    horizontalScrollBar()->setSingleStep(singleStepH);
}

void PdfViewerWidget::updateLayoutGeometry() {
    if (!isDocumentOpen()) {
        return;
    }
    const double dpr = effectiveDpr();
    const double qScale = pdf::quantizeScale(m_scale);
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    const int64_t oldVpW = (m_lastVpWidthDevice > 0) ? m_lastVpWidthDevice : vpW;
    const int64_t oldVpH = (m_lastVpHeightDevice > 0) ? m_lastVpHeightDevice : vpH;

    int64_t curDocX = m_docScrollX + oldVpW / 2;
    int64_t curDocY = m_docScrollY + oldVpH / 2;
    auto anchor = m_layout.createAnchor(curDocX, curDocY);

    m_layout.updateLayout(qScale, dpr, vpW);
    updateScrollBars();

    int64_t newDocX = 0, newDocY = 0;
    m_layout.resolveAnchor(anchor, vpW / 2, vpH / 2, vpW, vpH, newDocX, newDocY);
    setDocScrollPosition(newDocX, newDocY);

    m_lastVpWidthDevice = vpW;
    m_lastVpHeightDevice = vpH;

    viewport()->update();
}

void PdfViewerWidget::drawErrorPlaceholder(QPainter& painter, const QRectF& rect) {
    const bool isDark = isDarkMode();
    painter.fillRect(rect, isDark ? QColor(60, 20, 20) : QColor(245, 225, 225));
    painter.setPen(QPen(QColor(220, 60, 60), 1, Qt::DashLine));
    painter.drawRect(rect);
    painter.setPen(isDark ? QColor(255, 120, 120) : QColor(180, 30, 30));
    painter.drawText(rect, Qt::AlignCenter, tr("Page display error"));
}

void PdfViewerWidget::drawPendingPlaceholder(QPainter& painter, const QRectF& rect) {
    QColor placeholderColor;
    switch (m_colorMode) {
        case ColorMode::Inverted:     placeholderColor = Qt::black; break;
        case ColorMode::HighContrast: placeholderColor = QColor("#121212"); break;
        case ColorMode::Sepia:        placeholderColor = QColor("#fbf0d9"); break;
        case ColorMode::Normal:
        default:                      placeholderColor = QColor(252, 252, 252); break;
    }
    painter.fillRect(rect, placeholderColor);
}

void PdfViewerWidget::drawWorkerErrorBanner(QPainter& painter) {
    const int vpW = viewport()->width();
    const int vpH = viewport()->height();

    const int boxW = std::min(560, std::max(280, vpW - 40));
    const int boxH = 150;
    const QRect boxRect((vpW - boxW) / 2, (vpH - boxH) / 2, boxW, boxH);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(QColor(40, 44, 48));
    painter.setPen(QPen(QColor(220, 80, 80), 1.5));
    painter.drawRoundedRect(boxRect, 8, 8);

    QFont titleFont = painter.font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
    painter.setFont(titleFont);
    painter.setPen(QColor(240, 100, 100));
    painter.drawText(boxRect.adjusted(20, 16, -20, -90), Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Isolated Renderer Terminated"));

    QFont bodyFont = painter.font();
    bodyFont.setBold(false);
    bodyFont.setPointSizeF(bodyFont.pointSizeF() * 0.85);
    painter.setFont(bodyFont);
    painter.setPen(QColor(210, 215, 220));
    painter.drawText(boxRect.adjusted(20, 46, -20, -35), Qt::AlignLeft | Qt::TextWordWrap,
                     m_workerErrorMessage.isEmpty()
                         ? tr("The isolated rendering worker process crashed or exited unexpectedly. The main application was protected.")
                         : m_workerErrorMessage);

    painter.setPen(QColor(150, 155, 160));
    painter.drawText(boxRect.adjusted(20, 105, -20, -12), Qt::AlignLeft | Qt::AlignVCenter,
                     tr("Open another document or reload to restart the worker process."));
}

void PdfViewerWidget::drawLoadingOverlay(QPainter& painter) {
    if (!m_loading) {
        return;
    }
    const QString text = tr("Opening %1\u2026").arg(QFileInfo(m_pendingOpenPath).fileName());
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() * 1.2);
    painter.setFont(font);

    const QFontMetrics fm(font);
    const QRect textRect = fm.boundingRect(text);
    QRect box(0, 0, textRect.width() + 40, textRect.height() + 24);
    box.moveCenter(viewport()->rect().center());

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(20, 22, 24, 220));
    painter.drawRoundedRect(box, 8, 8);
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(box, Qt::AlignCenter, text);
}

void PdfViewerWidget::rememberFallbackScale(double previousScale) {
    m_fallbackScales.erase(std::remove(m_fallbackScales.begin(), m_fallbackScales.end(), previousScale), m_fallbackScales.end());
    m_fallbackScales.insert(m_fallbackScales.begin(), previousScale);
    if (m_fallbackScales.size() > MAX_FALLBACK_SCALES) {
        m_fallbackScales.resize(MAX_FALLBACK_SCALES);
    }
}

bool PdfViewerWidget::drawFallbackTile(QPainter& painter, const layout::VisibleTile& t, const QRectF& tileRect, double qScale, double qDpr) {
    constexpr int TILE = 512;
    constexpr int MAX_SOURCE_TILES = 64; // guards against huge zoom jumps touching thousands of old tiles
    bool drawnAny = false;

    painter.save();
    painter.setClipRect(tileRect);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    for (const double oldScale : m_fallbackScales) {
        if (oldScale == qScale) {
            continue;
        }
        // Region of this tile expressed in the old zoom's page-pixel grid
        const double r = oldScale / qScale;
        const double sx0 = t.tileXOnPage * r;
        const double sx1 = (t.tileXOnPage + t.tileWidth) * r;
        const double sy0 = t.tileYOnPage * r;
        const double sy1 = (t.tileYOnPage + t.tileHeight) * r;

        const int c0 = static_cast<int>(std::floor(sx0 / TILE));
        const int c1 = static_cast<int>(std::floor((sx1 - 1e-6) / TILE));
        const int r0 = static_cast<int>(std::floor(sy0 / TILE));
        const int r1 = static_cast<int>(std::floor((sy1 - 1e-6) / TILE));
        if (c1 < c0 || r1 < r0 || static_cast<int64_t>(c1 - c0 + 1) * (r1 - r0 + 1) > MAX_SOURCE_TILES) {
            continue;
        }

        bool fullyCovered = true;
        for (int row = r0; row <= r1; ++row) {
            for (int col = c0; col <= c1; ++col) {
                auto buf = m_cache.get(pdf::TileKey::create(m_currentDocumentId, t.pageIndex, col, row, oldScale, qDpr, m_rotation));
                if (!buf.has_value()) {
                    fullyCovered = false;
                    continue;
                }
                const double ox = static_cast<double>(col) * TILE;
                const double oy = static_cast<double>(row) * TILE;
                const double ix0 = std::max(sx0, ox);
                const double iy0 = std::max(sy0, oy);
                const double ix1 = std::min(sx1, ox + buf->width());
                const double iy1 = std::min(sy1, oy + buf->height());
                if (ix1 <= ix0 || iy1 <= iy0) {
                    fullyCovered = false;
                    continue;
                }
                const QRectF src(ix0 - ox, iy0 - oy, ix1 - ix0, iy1 - iy0);
                const QRectF dst(tileRect.x() + (ix0 / r - t.tileXOnPage),
                                 tileRect.y() + (iy0 / r - t.tileYOnPage),
                                 (ix1 - ix0) / r,
                                 (iy1 - iy0) / r);
                if (m_colorMode == ColorMode::Normal) {
                    painter.drawImage(dst, toQImage(*buf), src);
                } else {
                    QImage transformed = applyColorMode(toQImage(*buf), m_colorMode);
                    painter.drawImage(dst, transformed, src);
                }
                drawnAny = true;
            }
        }
        if (fullyCovered) {
            break; // the most recent zoom level covers the whole tile; older ones would only be overdrawn
        }
    }

    painter.restore();
    if (drawnAny) {
        ++m_fallbackTilesDrawn;
    }
    return drawnAny;
}

void PdfViewerWidget::drawSelection(QPainter& painter, int pageIndex, const layout::PageLayout& pl, int64_t scrollDocX, int64_t scrollDocY, double effScale) {
    if (!hasSelection() || effScale <= 0.0) {
        return;
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    if (pageIndex < p0 || pageIndex > p1) {
        return;
    }

    auto it = m_pageTextCache.find(pageIndex);
    if (it == m_pageTextCache.end() || !it->second) {
        return;
    }
    const auto& pt = *it->second;

    int startChar = (pageIndex == p0) ? c0 : 0;
    int endChar = (pageIndex == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;

    auto rects = pt.getSelectionRects(startChar, endChar);
    QColor highlightColor;
    switch (m_colorMode) {
        case ColorMode::Inverted:     highlightColor = QColor(0, 140, 255, 120); break;
        case ColorMode::HighContrast: highlightColor = QColor(255, 215, 0, 130); break;
        case ColorMode::Sepia:        highlightColor = QColor(180, 130, 60, 110); break;
        case ColorMode::Normal:
        default:                      highlightColor = QColor(0, 120, 215, 90); break;
    }

    for (const auto& r : rects) {
        double px = 0.0, py = 0.0, pw = 0.0, ph = 0.0;
        rotateRectPointsForDisplay(r, pl.widthPoints, pl.heightPoints, m_rotation, px, py, pw, ph);
        QRectF selRect(
            static_cast<qreal>(static_cast<double>(pl.leftPixel - scrollDocX) + px * effScale),
            static_cast<qreal>(static_cast<double>(pl.topPixel - scrollDocY) + py * effScale),
            static_cast<qreal>(pw * effScale),
            static_cast<qreal>(ph * effScale)
        );
        painter.fillRect(selRect, highlightColor);
    }
}

bool PdfViewerWidget::hasSelection() const noexcept {
    return m_selectionStart.isValid() && m_selectionEnd.isValid() &&
           (m_selectionStart.pageIndex != m_selectionEnd.pageIndex ||
            m_selectionStart.charIndex != m_selectionEnd.charIndex);
}

QString PdfViewerWidget::selectedText() const {
    if (!hasSelection()) {
        return QString();
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    QString result;
    for (int p = p0; p <= p1; ++p) {
        auto it = m_pageTextCache.find(p);
        if (it == m_pageTextCache.end() || !it->second) {
            continue;
        }
        const auto& pt = *it->second;
        int startChar = (p == p0) ? c0 : 0;
        int endChar = (p == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;
        auto selU16 = pt.getSelectedText(startChar, endChar);
        if (!selU16.empty()) {
            if (!result.isEmpty()) {
                result.append(QChar::LineFeed);
            }
            result.append(QString::fromStdU16String(selU16));
        }
    }
    return result;
}

void PdfViewerWidget::clearSelection() {
    if (m_selectionStart.isValid() || m_selectionEnd.isValid()) {
        m_selectionStart = {};
        m_selectionEnd = {};
        m_isSelecting = false;
        viewport()->update();
        Q_EMIT selectionChanged();
    }
}

void PdfViewerWidget::copySelection() {
    QString text = selectedText();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

std::shared_ptr<const pdf::PageText> PdfViewerWidget::getPageText(int pageIndex) const {
    auto it = m_pageTextCache.find(pageIndex);
    return (it != m_pageTextCache.end()) ? it->second : nullptr;
}

void PdfViewerWidget::drawSearchHighlights(QPainter& painter, int pageIndex, const layout::PageLayout& pl, int64_t scrollDocX, int64_t scrollDocY, double effScale) {
    if (m_searchMatches.empty() || effScale <= 0.0) {
        return;
    }

    const QColor matchColor(255, 235, 59, 130);      // translucent yellow for all matches
    const QColor activeMatchColor(255, 152, 0, 190);  // vivid orange for active match
    const QColor activeBorderColor(230, 81, 0, 240);  // strong outline for active match

    for (size_t i = 0; i < m_searchMatches.size(); ++i) {
        const auto& match = m_searchMatches[i];
        if (match.pageIndex != pageIndex) {
            continue;
        }

        const bool isActive = (static_cast<int>(i) == m_currentMatchIndex);
        for (const auto& r : match.rects) {
            double px = 0.0, py = 0.0, pw = 0.0, ph = 0.0;
            rotateRectPointsForDisplay(r, pl.widthPoints, pl.heightPoints, m_rotation, px, py, pw, ph);
            QRectF selRect(
                static_cast<qreal>(static_cast<double>(pl.leftPixel - scrollDocX) + px * effScale),
                static_cast<qreal>(static_cast<double>(pl.topPixel - scrollDocY) + py * effScale),
                static_cast<qreal>(pw * effScale),
                static_cast<qreal>(ph * effScale)
            );
            if (isActive) {
                painter.fillRect(selRect, activeMatchColor);
                painter.setPen(QPen(activeBorderColor, 1.5));
                painter.drawRect(selRect);
            } else {
                painter.fillRect(selRect, matchColor);
            }
        }
    }
}

void PdfViewerWidget::scrollToMatch(const pdf::SearchMatch& match) {
    if (!isDocumentOpen() || match.pageIndex < 0 || match.pageIndex >= pageCount()) {
        return;
    }
    ensurePageShown(match.pageIndex);
    const auto& pl = m_layout.getPageLayout(match.pageIndex);
    const double dpr = effectiveDpr();
    const double effScale = pdf::quantizeScale(m_scale) * dpr;
    const int64_t vpW = static_cast<int64_t>(std::round(viewport()->width() * dpr));
    const int64_t vpH = static_cast<int64_t>(std::round(viewport()->height() * dpr));

    double matchLeftPt = 0.0, matchRightPt = 20.0, matchTopPt = 0.0, matchBottomPt = 20.0;
    if (!match.rects.empty()) {
        matchLeftPt = 1e9; matchRightPt = -1e9; matchTopPt = 1e9; matchBottomPt = -1e9;
        for (const auto& rect : match.rects) {
            double px = 0.0, py = 0.0, pw = 0.0, ph = 0.0;
            rotateRectPointsForDisplay(rect, pl.widthPoints, pl.heightPoints, m_rotation, px, py, pw, ph);
            matchLeftPt = std::min(matchLeftPt, px);
            matchRightPt = std::max(matchRightPt, px + pw);
            matchTopPt = std::min(matchTopPt, py);
            matchBottomPt = std::max(matchBottomPt, py + ph);
        }
    }
    const int64_t matchDocX = pl.leftPixel + static_cast<int64_t>(std::round(matchLeftPt * effScale));
    const int64_t matchDocW = static_cast<int64_t>(std::round((matchRightPt - matchLeftPt) * effScale));
    int64_t matchDocY = pl.topPixel + static_cast<int64_t>(std::round(matchTopPt * effScale));
    int64_t matchDocH = static_cast<int64_t>(std::round((matchBottomPt - matchTopPt) * effScale));

    constexpr int64_t margin = 40;
    bool visibleX = (matchDocX >= m_docScrollX + margin) &&
                    (matchDocX + matchDocW <= m_docScrollX + vpW - margin);
    bool visibleY = (matchDocY >= m_docScrollY + margin) &&
                    (matchDocY + matchDocH <= m_docScrollY + vpH - margin);

    if (!visibleX || !visibleY) {
        int64_t targetX = m_docScrollX;
        if (!visibleX) {
            targetX = matchDocX - margin;
            targetX = std::clamp<int64_t>(targetX, 0, std::max<int64_t>(0, m_layout.maxDocumentWidth() - vpW));
        }
        int64_t targetY = matchDocY - (vpH / 3);
        targetY = std::clamp<int64_t>(targetY, 0, std::max<int64_t>(0, m_layout.totalDocumentHeight() - vpH));
        const int64_t oldScrollX = m_docScrollX;
        const int64_t oldScrollY = m_docScrollY;
        m_navigating = true;
        setDocScrollPosition(targetX, targetY);
        m_navigating = false;
        if (m_docScrollX != oldScrollX || m_docScrollY != oldScrollY) {
            ++m_currentGeneration;
            m_inFlightKeys.clear();
            m_renderWorker.setGeneration(m_currentGeneration);
        }
        if (m_currentPage != match.pageIndex) {
            m_currentPage = match.pageIndex;
            Q_EMIT pageChanged(m_currentPage + 1, pageCount());
        }
        viewport()->update();
    }
}

void PdfViewerWidget::findText(const QString& query, bool matchCase) {
    const int64_t searchId = ++m_currentSearchId;
    m_searchQuery = query;
    m_searchMatchCase = matchCase;
    m_searchStartPage = m_currentPage;
    m_searchMatches.clear();
    m_currentMatchIndex = -1;
    m_renderWorker.cancelSearch();

    if (query.trimmed().isEmpty() || !isDocumentOpen() || pageCount() <= 0) {
        Q_EMIT searchResultsChanged(0, 0);
        Q_EMIT searchProgressChanged(0, 0);
        Q_EMIT searchFinished(false);
        viewport()->update();
        return;
    }

    Q_EMIT searchProgressChanged(0, pageCount());
    m_renderWorker.startSearch(m_currentDocumentId, searchId, query, matchCase, pageCount(), m_currentPage);
    Q_EMIT searchResultsChanged(0, 0);
    viewport()->update();
}

void PdfViewerWidget::findNext() {
    if (m_searchMatches.empty()) {
        return;
    }
    m_currentMatchIndex = (m_currentMatchIndex + 1) % static_cast<int>(m_searchMatches.size());
    scrollToMatch(m_searchMatches[static_cast<size_t>(m_currentMatchIndex)]);
    Q_EMIT searchResultsChanged(m_currentMatchIndex + 1, static_cast<int>(m_searchMatches.size()));
    viewport()->update();
}

void PdfViewerWidget::findPrevious() {
    if (m_searchMatches.empty()) {
        return;
    }
    m_currentMatchIndex = (m_currentMatchIndex - 1 + static_cast<int>(m_searchMatches.size())) % static_cast<int>(m_searchMatches.size());
    scrollToMatch(m_searchMatches[static_cast<size_t>(m_currentMatchIndex)]);
    Q_EMIT searchResultsChanged(m_currentMatchIndex + 1, static_cast<int>(m_searchMatches.size()));
    viewport()->update();
}

void PdfViewerWidget::goToSearchMatch(int index) {
    if (index < 0 || index >= static_cast<int>(m_searchMatches.size())) {
        return;
    }
    m_currentMatchIndex = index;
    scrollToMatch(m_searchMatches[static_cast<size_t>(index)]);
    Q_EMIT searchResultsChanged(m_currentMatchIndex + 1, static_cast<int>(m_searchMatches.size()));
    viewport()->update();
}

void PdfViewerWidget::clearSearch() {
    ++m_currentSearchId;
    m_searchQuery.clear();
    m_searchMatches.clear();
    m_currentMatchIndex = -1;
    m_renderWorker.cancelSearch();
    Q_EMIT searchResultsChanged(0, 0);
    Q_EMIT searchProgressChanged(0, 0);
    Q_EMIT searchFinished(false);
    viewport()->update();
}

void PdfViewerWidget::onSearchMatchesFound(int64_t documentId, int64_t searchId, int /*pageIndex*/, const std::vector<pdf::SearchMatch>& matches) {
    if (documentId != m_currentDocumentId || searchId != m_currentSearchId) {
        return;
    }

    const bool hadSelection = m_currentMatchIndex >= 0 &&
        m_currentMatchIndex < static_cast<int>(m_searchMatches.size());
    const pdf::SearchMatch selectedMatch = hadSelection
        ? m_searchMatches[static_cast<size_t>(m_currentMatchIndex)]
        : pdf::SearchMatch{};
    const size_t remaining = static_cast<size_t>(std::max(0, pdf::MAX_SEARCH_MATCHES - static_cast<int>(m_searchMatches.size())));
    const size_t count = std::min(remaining, matches.size());
    m_searchMatches.insert(m_searchMatches.end(), matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(count));
    std::sort(m_searchMatches.begin(), m_searchMatches.end(), [](const pdf::SearchMatch& lhs, const pdf::SearchMatch& rhs) {
        if (lhs.pageIndex != rhs.pageIndex) {
            return lhs.pageIndex < rhs.pageIndex;
        }
        return lhs.charStartIndex < rhs.charStartIndex;
    });

    if (hadSelection) {
        const auto selectedIt = std::find(m_searchMatches.begin(), m_searchMatches.end(), selectedMatch);
        m_currentMatchIndex = selectedIt == m_searchMatches.end()
            ? -1
            : static_cast<int>(std::distance(m_searchMatches.begin(), selectedIt));
    } else if (!m_searchMatches.empty()) {
        const auto circularDistance = [this](int pageIndex) {
            return (pageIndex - m_searchStartPage + pageCount()) % pageCount();
        };
        const auto nearestIt = std::min_element(m_searchMatches.begin(), m_searchMatches.end(),
            [&circularDistance](const pdf::SearchMatch& lhs, const pdf::SearchMatch& rhs) {
                const int lhsDistance = circularDistance(lhs.pageIndex);
                const int rhsDistance = circularDistance(rhs.pageIndex);
                if (lhsDistance != rhsDistance) {
                    return lhsDistance < rhsDistance;
                }
                return lhs.charStartIndex < rhs.charStartIndex;
            });
        m_currentMatchIndex = static_cast<int>(std::distance(m_searchMatches.begin(), nearestIt));
        scrollToMatch(*nearestIt);
    }

    Q_EMIT searchResultsChanged(m_currentMatchIndex + 1, static_cast<int>(m_searchMatches.size()));
    viewport()->update();
}

void PdfViewerWidget::onSearchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages) {
    if (documentId != m_currentDocumentId || searchId != m_currentSearchId) {
        return;
    }
    Q_EMIT searchProgressChanged(pagesSearched, totalPages);
}

void PdfViewerWidget::onSearchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated) {
    if (documentId != m_currentDocumentId || searchId != m_currentSearchId) {
        return;
    }
    const int cur = (m_currentMatchIndex >= 0) ? (m_currentMatchIndex + 1) : 0;
    Q_EMIT searchResultsChanged(cur, std::min(totalMatches, pdf::MAX_SEARCH_MATCHES));
    Q_EMIT searchFinished(truncated);
    viewport()->update();
}

void PdfViewerWidget::onDocumentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline) {
    if (documentId != m_currentDocumentId) {
        return;
    }
    m_outline = outline;
    Q_EMIT outlineReady(m_outline);
}

void PdfViewerWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));

        if (m_redactionMode) {
            int page = m_layout.getPageAt(docX, docY);
            if (page >= 0) {
                m_isRedacting = true;
                m_redactStartPage = page;
                m_redactStartDocPos = QPointF(static_cast<qreal>(docX), static_cast<qreal>(docY));
                m_redactCurrentDocPos = m_redactStartDocPos;
                viewport()->update();
                event->accept();
                return;
            }
        }

        int page = m_layout.getPageAt(docX, docY);
        if (page >= 0) {
            double px = 0.0, py = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, px, py)) {
                auto it = m_pageTextCache.find(page);
                if (it != m_pageTextCache.end() && it->second) {
                    int ch = it->second->hitTest(px, py);
                    if (ch >= 0) {
                        m_selectionStart = {page, ch};
                        m_selectionEnd = {page, ch};
                        m_isSelecting = true;
                        viewport()->update();
                        Q_EMIT selectionChanged();
                        event->accept();
                        return;
                    }
                }
            }
        }
        clearSelection();
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void PdfViewerWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_isRedacting && isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));
        m_redactCurrentDocPos = QPointF(static_cast<qreal>(docX), static_cast<qreal>(docY));
        viewport()->update();
        event->accept();
        return;
    }
    if (m_isSelecting && isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));

        int page = m_layout.getPageAt(docX, docY);
        if (page < 0 && m_pageCount > 0) {
            // Find closest page vertically to gracefully handle inter-page gaps and outer margins
            for (int p = 0; p < m_pageCount; ++p) {
                const auto& pl = m_layout.getPageLayout(p);
                if (docY >= pl.topPixel && docY <= pl.bottomPixel()) {
                    page = p;
                    break;
                }
            }
            if (page < 0) {
                if (docY < m_layout.getPageLayout(0).topPixel) {
                    page = 0;
                } else if (docY > m_layout.getPageLayout(m_pageCount - 1).bottomPixel()) {
                    page = m_pageCount - 1;
                } else {
                    for (int p = 0; p + 1 < m_pageCount; ++p) {
                        if (docY > m_layout.getPageLayout(p).bottomPixel() &&
                            docY < m_layout.getPageLayout(p + 1).topPixel) {
                            page = (p >= m_selectionStart.pageIndex) ? (p + 1) : p;
                            break;
                        }
                    }
                }
            }
        }

        if (page >= 0 && page < m_pageCount) {
            double px = 0.0, py = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, px, py)) {
                auto it = m_pageTextCache.find(page);
                if (it != m_pageTextCache.end() && it->second && !it->second->empty()) {
                    int ch = it->second->hitTest(px, py);
                    if (ch < 0) {
                        if (py < it->second->chars.front().top) {
                            ch = 0;
                        } else if (py > it->second->chars.back().bottom) {
                            ch = static_cast<int>(it->second->charCount()) - 1;
                        } else {
                            double minDist = 1e9;
                            int nearest = 0;
                            for (size_t i = 0; i < it->second->charCount(); ++i) {
                                const auto& b = it->second->chars[i];
                                double cy = (b.top + b.bottom) * 0.5;
                                double d = std::abs(cy - py);
                                if (d < minDist) {
                                    minDist = d;
                                    nearest = static_cast<int>(i);
                                }
                            }
                            ch = nearest;
                        }
                    }
                    if (ch >= 0) {
                        TextPosition newEnd{page, ch};
                        if (newEnd != m_selectionEnd) {
                            m_selectionEnd = newEnd;
                            viewport()->update();
                            Q_EMIT selectionChanged();
                        }
                        event->accept();
                        return;
                    }
                }
            }
        }
    } else if (isDocumentOpen()) {
        // Hover cursor: show IBeam if over text
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));

        int page = m_layout.getPageAt(docX, docY);
        bool overText = false;
        if (page >= 0) {
            double px = 0.0, py = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, px, py)) {
                auto it = m_pageTextCache.find(page);
                if (it != m_pageTextCache.end() && it->second) {
                    overText = (it->second->hitTest(px, py) >= 0);
                }
            }
        }
        viewport()->setCursor(overText ? Qt::IBeamCursor : Qt::ArrowCursor);
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PdfViewerWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_isRedacting) {
        m_isRedacting = false;
        const int page = m_redactStartPage;
        if (page >= 0 && isDocumentOpen()) {
            // Marks are stored in canonical page space (unrotated, top-left origin, PDF points), the
            // space pdf_redact expects. The layout owns the view-rotation transform, so the same
            // code path is correct at every rotation. Both drag corners are mapped, then normalised.
            const auto& pl = m_layout.getPageLayout(page);
            double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
            const auto toDoc = [](const QPointF& p, int64_t& dx, int64_t& dy) {
                dx = static_cast<int64_t>(std::llround(p.x()));
                dy = static_cast<int64_t>(std::llround(p.y()));
            };
            int64_t sx = 0, sy = 0, ex = 0, ey = 0;
            toDoc(m_redactStartDocPos, sx, sy);
            toDoc(m_redactCurrentDocPos, ex, ey);
            if (m_layout.documentToPagePoints(page, sx, sy, ax, ay) &&
                m_layout.documentToPagePoints(page, ex, ey, bx, by)) {
                const double x0 = std::max(0.0, std::min(pl.widthPoints, std::min(ax, bx)));
                const double y0 = std::max(0.0, std::min(pl.heightPoints, std::min(ay, by)));
                const double x1 = std::max(0.0, std::min(pl.widthPoints, std::max(ax, bx)));
                const double y1 = std::max(0.0, std::min(pl.heightPoints, std::max(ay, by)));
                if ((x1 - x0) >= 3.0 && (y1 - y0) >= 3.0) {
                    addStagedRedaction(page, pdf::ipc::RedactRect{x0, y0, x1, y1});
                }
            }
        }
        m_redactStartPage = -1;
        viewport()->update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_isSelecting) {
        m_isSelecting = false;
        if (m_selectionStart == m_selectionEnd) {
            clearSelection();
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void PdfViewerWidget::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        event->accept();
        return;
    }

    if (!isDocumentOpen()) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }

    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();

    const int singleStepY = verticalScrollBar()->singleStep() > 0 ? verticalScrollBar()->singleStep() : 40;
    const int singleStepX = horizontalScrollBar()->singleStep() > 0 ? horizontalScrollBar()->singleStep() : 40;
    const int pageStepY = verticalScrollBar()->pageStep() > 0 ? verticalScrollBar()->pageStep() : viewport()->height();

    // 1b. SinglePage mode: paging past the edge of the page flips to the neighbouring page,
    //     and Left/Right flip when the page has no horizontal scroll range.
    if (m_layout.viewMode() == layout::ViewMode::SinglePage) {
        const bool forward = (key == Qt::Key_PageDown) || (key == Qt::Key_Space && !(mods & Qt::ShiftModifier));
        const bool backward = (key == Qt::Key_PageUp) || (key == Qt::Key_Space && (mods & Qt::ShiftModifier));
        auto* vbar = verticalScrollBar();
        if (forward && vbar->value() >= vbar->maximum()) {
            nextPage();
            event->accept();
            return;
        }
        if (backward && vbar->value() <= vbar->minimum()) {
            if (m_currentPage > 0) {
                goToPage(m_currentPage - 1);
                vbar->setValue(vbar->maximum());
            }
            event->accept();
            return;
        }
        const bool noHScroll = horizontalScrollBar()->maximum() == 0;
        if (noHScroll && mods == Qt::NoModifier && key == Qt::Key_Right) {
            nextPage();
            event->accept();
            return;
        }
        if (noHScroll && mods == Qt::NoModifier && key == Qt::Key_Left) {
            previousPage();
            event->accept();
            return;
        }
    }

    // 1. Home / End bounds navigation
    if (key == Qt::Key_Home && (mods == Qt::NoModifier || mods == Qt::ControlModifier)) {
        goToPage(0);
        event->accept();
        return;
    }
    if (key == Qt::Key_End && (mods == Qt::NoModifier || mods == Qt::ControlModifier)) {
        if (pageCount() > 0) {
            goToPage(pageCount() - 1);
        }
        event->accept();
        return;
    }

    // 2. PageDown / Space (forward) and PageUp / Shift+Space (backward)
    if (key == Qt::Key_PageDown) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + pageStepY);
        event->accept();
        return;
    }
    if (key == Qt::Key_PageUp) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - pageStepY);
        event->accept();
        return;
    }
    if (key == Qt::Key_Space) {
        if (mods & Qt::ShiftModifier) {
            verticalScrollBar()->setValue(verticalScrollBar()->value() - pageStepY);
        } else {
            verticalScrollBar()->setValue(verticalScrollBar()->value() + pageStepY);
        }
        event->accept();
        return;
    }

    // 3. Arrow keys & Vim navigation (J / K / L / H)
    if (key == Qt::Key_Down || (key == Qt::Key_J && (mods == Qt::NoModifier || mods == Qt::KeypadModifier))) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + singleStepY);
        event->accept();
        return;
    }
    if (key == Qt::Key_Up || (key == Qt::Key_K && (mods == Qt::NoModifier || mods == Qt::KeypadModifier))) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - singleStepY);
        event->accept();
        return;
    }
    if (key == Qt::Key_Right || (key == Qt::Key_L && (mods == Qt::NoModifier || mods == Qt::KeypadModifier))) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + singleStepX);
        event->accept();
        return;
    }
    if (key == Qt::Key_Left || (key == Qt::Key_H && (mods == Qt::NoModifier || mods == Qt::KeypadModifier))) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - singleStepX);
        event->accept();
        return;
    }

    QAbstractScrollArea::keyPressEvent(event);
}

void PdfViewerWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(viewport());
    QColor vpBg;
    switch (m_colorMode) {
        case ColorMode::Inverted:     vpBg = QColor("#181818"); break;
        case ColorMode::HighContrast: vpBg = QColor("#0a0a0a"); break;
        case ColorMode::Sepia:        vpBg = QColor("#342c24"); break;
        case ColorMode::Normal:
        default:                      vpBg = QColor("#323639"); break;
    }
    painter.fillRect(viewport()->rect(), vpBg);

    if (!isDocumentOpen()) {
        if (!m_workerErrorMessage.isEmpty()) {
            drawWorkerErrorBanner(painter);
        } else {
            drawLoadingOverlay(painter);
        }
        return;
    }

    const double qDpr = effectiveDpr();
    const double qScale = pdf::quantizeScale(m_scale);

    const int64_t scrollDocX = m_docScrollX;
    const int64_t scrollDocY = m_docScrollY;
    const int64_t vpWidthDevice = static_cast<int64_t>(std::round(viewport()->width() * qDpr));
    const int64_t vpHeightDevice = static_cast<int64_t>(std::round(viewport()->height() * qDpr));

    layout::Rect64 vpRectDevice{scrollDocX, scrollDocY, vpWidthDevice, vpHeightDevice};

    // Scale painter coordinates so all operations map directly 1:1 into device pixels using quantized DPR
    painter.save();
    painter.scale(1.0 / qDpr, 1.0 / qDpr);

    // 1. Draw page paper background and drop-shadows
    auto visPages = m_layout.getVisiblePages(scrollDocY, vpHeightDevice);
    if (visPages.isValid()) {
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            ensurePageTextRequested(p);

            const auto& pl = m_layout.getPageLayout(p);
            QRectF pageRect(
                static_cast<qreal>(pl.leftPixel - scrollDocX),
                static_cast<qreal>(pl.topPixel - scrollDocY),
                static_cast<qreal>(pl.widthPixel),
                static_cast<qreal>(pl.heightPixel)
            );

            QColor pageBg;
            QColor pageBorder;
            QColor shadowColor;
            switch (m_colorMode) {
                case ColorMode::Inverted:
                    pageBg = Qt::black;
                    pageBorder = QColor(50, 50, 50);
                    shadowColor = QColor(0, 0, 0, 140);
                    break;
                case ColorMode::HighContrast:
                    pageBg = QColor("#121212");
                    pageBorder = QColor(60, 60, 60);
                    shadowColor = QColor(0, 0, 0, 180);
                    break;
                case ColorMode::Sepia:
                    pageBg = QColor("#fbf0d9");
                    pageBorder = QColor(190, 175, 150);
                    shadowColor = QColor(0, 0, 0, 90);
                    break;
                case ColorMode::Normal:
                default:
                    pageBg = Qt::white;
                    pageBorder = QColor(180, 180, 180);
                    shadowColor = QColor(0, 0, 0, 80);
                    break;
            }

            // Drop shadow
            QRectF shadowRect = pageRect.adjusted(3, 3, 5, 5);
            painter.fillRect(shadowRect, shadowColor);

            // Paper background
            painter.fillRect(pageRect, pageBg);

            // Subtle border
            painter.setPen(pageBorder);
            painter.drawRect(pageRect);
        }
    }

    // 2. Query visible tiles from layout
    auto tiles = m_layout.getVisibleTiles(vpRectDevice, 512);

    // 3. Atomically pin visible tiles in TileCache
    std::vector<pdf::TileKey> visibleKeys;
    visibleKeys.reserve(tiles.size());
    for (const auto& t : tiles) {
        visibleKeys.push_back(pdf::TileKey::create(m_currentDocumentId, t.pageIndex, t.col, t.row, qScale, qDpr, m_rotation));
    }
    m_cache.setPinnedKeys(visibleKeys);

    // 4. Draw cached tiles or placeholders, and collect pending worker requests
    std::vector<pdf::TileRenderRequest> pendingRequests;
    const int64_t vpCenterX = vpWidthDevice / 2;
    const int64_t vpCenterY = vpHeightDevice / 2;

    for (size_t i = 0; i < tiles.size(); ++i) {
        const auto& t = tiles[i];
        const auto& key = visibleKeys[i];
        const int64_t tileVpX = t.docX - scrollDocX;
        const int64_t tileVpY = t.docY - scrollDocY;
        QRectF tileRect(
            static_cast<qreal>(tileVpX),
            static_cast<qreal>(tileVpY),
            static_cast<qreal>(t.tileWidth),
            static_cast<qreal>(t.tileHeight)
        );

        auto cachedBuf = m_cache.get(key);
        if (cachedBuf.has_value()) {
            m_inFlightKeys.erase(key);
            if (m_colorMode == ColorMode::Normal) {
                QImage img = toQImage(*cachedBuf);
                painter.drawImage(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), img);
            } else {
                auto it = m_transformedPixmapCache.find(key);
                if (it != m_transformedPixmapCache.end()) {
                    painter.drawPixmap(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), it->second);
                } else {
                    QImage img = applyColorMode(toQImage(*cachedBuf), m_colorMode);
                    QPixmap pixmap = QPixmap::fromImage(img);
                    if (m_transformedPixmapCache.size() >= 32) {
                        // Prune entries outside the visible set to strictly bound cache memory
                        for (auto iter = m_transformedPixmapCache.begin(); iter != m_transformedPixmapCache.end(); ) {
                            if (std::find(visibleKeys.begin(), visibleKeys.end(), iter->first) == visibleKeys.end()) {
                                iter = m_transformedPixmapCache.erase(iter);
                            } else {
                                ++iter;
                            }
                        }
                        if (m_transformedPixmapCache.size() >= 32) {
                            m_transformedPixmapCache.clear();
                        }
                    }
                    m_transformedPixmapCache.emplace(key, pixmap);
                    painter.drawPixmap(QPointF(static_cast<qreal>(tileVpX), static_cast<qreal>(tileVpY)), pixmap);
                }
            }
        } else if (m_cache.isFailed(key)) {
            if (m_cache.canRetry(key)) {
                if (m_inFlightKeys.find(key) != m_inFlightKeys.end()) {
                    // Already in flight to worker for retry; avoid re-enqueueing duplicate
                    drawPendingPlaceholder(painter, tileRect);
                } else {
                    // Backoff interval elapsed: allow retry
                    m_cache.clearFailed(key);
                    drawPendingPlaceholder(painter, tileRect);

                    int64_t tileMidX = tileVpX + t.tileWidth / 2;
                    int64_t tileMidY = tileVpY + t.tileHeight / 2;
                    int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
                    int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

                    pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
                    pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority, m_rotation});
                    m_inFlightKeys.insert(key);
                }
            } else {
                drawErrorPlaceholder(painter, tileRect);
            }
        } else {
            // Pending render miss: draw placeholder (plus a scaled earlier-zoom stand-in if we have one)
            // on the UI thread and queue request to worker if not already in flight
            drawPendingPlaceholder(painter, tileRect);
            if (!m_fallbackScales.empty()) {
                drawFallbackTile(painter, t, tileRect, qScale, qDpr);
            }

            if (m_inFlightKeys.find(key) == m_inFlightKeys.end()) {
                int64_t tileMidX = tileVpX + t.tileWidth / 2;
                int64_t tileMidY = tileVpY + t.tileHeight / 2;
                int64_t distSq = (tileMidX - vpCenterX) * (tileMidX - vpCenterX) + (tileMidY - vpCenterY) * (tileMidY - vpCenterY);
                int priority = static_cast<int>(std::max<int64_t>(0, 10000000 - distSq));

                pdf::PixelRect pRect{t.tileXOnPage, t.tileYOnPage, t.tileWidth, t.tileHeight};
                pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, t.pageIndex, pRect, qScale, qDpr, key, priority, m_rotation});
                m_inFlightKeys.insert(key);
            }
        }
    }

    // 5. Prefetch ring: one viewport height above and below the visible area
    if (m_prefetchEnabled && !tiles.empty()) {
        constexpr size_t MAX_PREFETCH_PER_PAINT = 16;
        const int64_t ringTop = std::max<int64_t>(0, scrollDocY - vpHeightDevice);
        const int64_t ringBottom = std::min<int64_t>(m_layout.totalDocumentHeight(), scrollDocY + 2 * vpHeightDevice);
        if (ringBottom > ringTop) {
            std::unordered_set<pdf::TileKey, pdf::TileKeyHash> visibleSet(visibleKeys.begin(), visibleKeys.end());
            struct Candidate { int64_t distSq; layout::VisibleTile tile; pdf::TileKey key; };
            std::vector<Candidate> candidates;

            const auto ringTiles = m_layout.getVisibleTiles({scrollDocX, ringTop, vpWidthDevice, ringBottom - ringTop}, 512);
            for (const auto& t : ringTiles) {
                const auto key = pdf::TileKey::create(m_currentDocumentId, t.pageIndex, t.col, t.row, qScale, qDpr, m_rotation);
                if (visibleSet.count(key) || m_inFlightKeys.count(key) || m_cache.contains(key)) {
                    continue;
                }
                const int64_t dx = (t.docX - scrollDocX) + t.tileWidth / 2 - vpCenterX;
                const int64_t dy = (t.docY - scrollDocY) + t.tileHeight / 2 - vpCenterY;
                candidates.push_back({dx * dx + dy * dy, t, key});
            }

            std::sort(candidates.begin(), candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.distSq < b.distSq; });
            if (candidates.size() > MAX_PREFETCH_PER_PAINT) {
                candidates.resize(MAX_PREFETCH_PER_PAINT);
            }
            for (const auto& c : candidates) {
                const int priority = -1 - static_cast<int>(std::min<int64_t>(c.distSq >> 12, 1000000000));
                pdf::PixelRect pRect{c.tile.tileXOnPage, c.tile.tileYOnPage, c.tile.tileWidth, c.tile.tileHeight};
                pendingRequests.push_back({m_currentDocumentId, m_currentGeneration, c.tile.pageIndex, pRect, qScale, qDpr, c.key, priority, m_rotation});
                m_inFlightKeys.insert(c.key);
                ++m_prefetchRequestsIssued;
            }
        }
    }

    if (!pendingRequests.empty()) {
        m_renderWorker.enqueueRequests(std::move(pendingRequests));
    }
    if (m_outlineRequestPending && !tiles.empty()) {
        m_outlineRequestPending = false;
        m_renderWorker.requestDocumentOutline(m_currentDocumentId);
    }

    // 6. Draw selection highlights over rendered pages
    if (visPages.isValid() && (hasSelection() || !m_searchMatches.empty())) {
        const double effScale = qScale * qDpr;
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            const auto& pl = m_layout.getPageLayout(p);
            drawSearchHighlights(painter, p, pl, scrollDocX, scrollDocY, effScale);
            if (hasSelection()) {
                drawSelection(painter, p, pl, scrollDocX, scrollDocY, effScale);
            }
        }
    }

    // 7. Draw staged redactions and active redaction rubber-band
    if (visPages.isValid()) {
        const double effScale = qScale * qDpr;
        drawRedactionOverlays(painter, visPages, scrollDocX, scrollDocY, effScale);
    }

    painter.restore();

    drawLoadingOverlay(painter);
}

void PdfViewerWidget::setRedactionMode(bool enabled) {
    if (m_redactionMode == enabled) {
        return;
    }
    m_redactionMode = enabled;
    if (m_redactionMode) {
        clearSelection();
    }
    viewport()->setCursor(m_redactionMode ? Qt::CrossCursor : Qt::IBeamCursor);
    viewport()->update();
    Q_EMIT redactionModeChanged(m_redactionMode);
}

void PdfViewerWidget::clearStagedRedactions() {
    if (m_stagedRedactions.empty()) {
        return;
    }
    m_stagedRedactions.clear();
    viewport()->update();
    Q_EMIT redactionMarksChanged();
}

int PdfViewerWidget::stagedRedactionCount() const noexcept {
    int count = 0;
    for (const auto& [_, rects] : m_stagedRedactions) {
        count += static_cast<int>(rects.size());
    }
    return count;
}

void PdfViewerWidget::addStagedRedaction(int pageIndex, const pdf::ipc::RedactRect& rect) {
    if (pageIndex < 0 || pageIndex >= m_pageCount) {
        return;
    }
    pdf::ipc::RedactRect norm{
        std::min(rect.left, rect.right),
        std::min(rect.top, rect.bottom),
        std::max(rect.left, rect.right),
        std::max(rect.top, rect.bottom)
    };
    if (norm.width() <= 0.0 || norm.height() <= 0.0) {
        return;
    }
    m_stagedRedactions[pageIndex].push_back(norm);
    viewport()->update();
    Q_EMIT redactionMarksChanged();
}

bool PdfViewerWidget::markSelectionForRedaction() {
    if (!hasSelection() || !isDocumentOpen()) {
        return false;
    }
    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    // Redaction must never be silently partial: every page in the range needs its text. Pages the
    // LRU text cache evicted (or that are still in flight) are fetched synchronously; if any page
    // still cannot be obtained, nothing is staged and the caller is told.
    std::vector<std::pair<int, std::shared_ptr<const pdf::PageText>>> texts;
    for (int p = p0; p <= p1; ++p) {
        auto it = m_pageTextCache.find(p);
        std::shared_ptr<const pdf::PageText> pt;
        if (it != m_pageTextCache.end() && it->second) {
            pt = it->second;
        } else {
            pt = m_renderWorker.getPageTextSync(m_currentDocumentId, p);
            if (!pt) {
                return false;
            }
        }
        texts.emplace_back(p, std::move(pt));
    }

    bool addedAny = false;
    for (const auto& [p, ptr] : texts) {
        const auto& pt = *ptr;
        const int startChar = (p == p0) ? c0 : 0;
        const int endChar = (p == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;
        const auto& pl = m_layout.getPageLayout(p);
        // Safety margin (1.5 pt, ~3.1 px at 150 DPI) ensuring font descenders (g, y, p, q),
        // diacritics/accents, and italic overhang are fully enveloped by the redaction box.
        constexpr double kSelectionPadPt = 1.5;
        // Selection rectangles are already canonical page points (unrotated, top-left origin).
        for (const auto& r : pt.getSelectionRects(startChar, endChar)) {
            const double rLeft = std::min(r.left, r.right);
            const double rTop = std::min(r.top, r.bottom);
            const double rRight = std::max(r.left, r.right);
            const double rBottom = std::max(r.top, r.bottom);

            const double padLeft = std::max(0.0, rLeft - kSelectionPadPt);
            const double padTop = std::max(0.0, rTop - kSelectionPadPt);
            const double padRight = (pl.widthPoints > 0.0) ? std::min(pl.widthPoints, rRight + kSelectionPadPt) : (rRight + kSelectionPadPt);
            const double padBottom = (pl.heightPoints > 0.0) ? std::min(pl.heightPoints, rBottom + kSelectionPadPt) : (rBottom + kSelectionPadPt);

            pdf::ipc::RedactRect rr{padLeft, padTop, padRight, padBottom};
            if (rr.width() > 0.0 && rr.height() > 0.0) {
                m_stagedRedactions[p].push_back(rr);
                addedAny = true;
            }
        }
    }
    clearSelection();
    if (addedAny) {
        viewport()->update();
        Q_EMIT redactionMarksChanged();
    }
    return true;
}

void PdfViewerWidget::drawRedactionOverlays(QPainter& painter, const layout::ViewportRange& visPages,
                                           int64_t scrollDocX, int64_t scrollDocY, double effScale) {
    if (!visPages.isValid() || effScale <= 0.0) {
        return;
    }

    // Draw committed staged redactions
    for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
        auto it = m_stagedRedactions.find(p);
        if (it == m_stagedRedactions.end() || it->second.empty()) {
            continue;
        }
        for (const auto& r : it->second) {
            // Canonical page points -> document pixels under the current view rotation.
            int64_t d0x = 0, d0y = 0, d1x = 0, d1y = 0;
            if (!m_layout.pagePointsToDocument(p, r.left, r.top, d0x, d0y) ||
                !m_layout.pagePointsToDocument(p, r.right, r.bottom, d1x, d1y)) {
                continue;
            }
            const qreal rx = static_cast<qreal>(std::min(d0x, d1x) - scrollDocX);
            const qreal ry = static_cast<qreal>(std::min(d0y, d1y) - scrollDocY);
            const qreal rw = static_cast<qreal>(std::max(d0x, d1x) - std::min(d0x, d1x));
            const qreal rh = static_cast<qreal>(std::max(d0y, d1y) - std::min(d0y, d1y));
            QRectF box(rx, ry, rw, rh);

            // Semi-opaque dark mask
            painter.fillRect(box, QColor(0, 0, 0, 190));
            // Crimson red boundary
            QPen pen(QColor(220, 20, 60), 2);
            painter.setPen(pen);
            painter.drawRect(box);

            // Small badge when box is sufficiently large
            if (rw >= 40.0 && rh >= 16.0) {
                painter.setFont(QFont(QStringLiteral("Arial"), 8, QFont::Bold));
                painter.setPen(QColor(255, 255, 255, 230));
                painter.drawText(box, Qt::AlignCenter, QStringLiteral("REDACT"));
            }
        }
    }

    // Draw active drag rubber-band
    if (m_isRedacting && m_redactStartPage >= 0) {
        const qreal rx = static_cast<qreal>(std::min(m_redactStartDocPos.x(), m_redactCurrentDocPos.x()) - static_cast<double>(scrollDocX));
        const qreal ry = static_cast<qreal>(std::min(m_redactStartDocPos.y(), m_redactCurrentDocPos.y()) - static_cast<double>(scrollDocY));
        const qreal rw = static_cast<qreal>(std::abs(m_redactCurrentDocPos.x() - m_redactStartDocPos.x()));
        const qreal rh = static_cast<qreal>(std::abs(m_redactCurrentDocPos.y() - m_redactStartDocPos.y()));
        QRectF dragBox(rx, ry, rw, rh);

        painter.fillRect(dragBox, QColor(220, 20, 60, 70));
        QPen dragPen(QColor(220, 20, 60), 2, Qt::DashLine);
        painter.setPen(dragPen);
        painter.drawRect(dragBox);
    }
}

bool PdfViewerWidget::markupSelection(pdf::AnnotationSubtype subtype, uint32_t colorArgb) {
    if (!hasSelection() || !m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);

    if (colorArgb == 0) {
        switch (subtype) {
            case pdf::AnnotationSubtype::Highlight: colorArgb = 0x80FFFF00; break;
            case pdf::AnnotationSubtype::Underline: colorArgb = 0xFF00AA00; break;
            case pdf::AnnotationSubtype::StrikeOut: colorArgb = 0xFFFF0000; break;
            case pdf::AnnotationSubtype::Squiggly: colorArgb = 0xFFFF0000; break;
            default: colorArgb = 0x80FFFF00; break;
        }
    }

    bool forward = (m_selectionStart.pageIndex < m_selectionEnd.pageIndex) ||
                   (m_selectionStart.pageIndex == m_selectionEnd.pageIndex && m_selectionStart.charIndex <= m_selectionEnd.charIndex);
    int p0 = forward ? m_selectionStart.pageIndex : m_selectionEnd.pageIndex;
    int p1 = forward ? m_selectionEnd.pageIndex : m_selectionStart.pageIndex;
    int c0 = forward ? m_selectionStart.charIndex : m_selectionEnd.charIndex;
    int c1 = forward ? m_selectionEnd.charIndex : m_selectionStart.charIndex;

    bool anyAdded = false;

    for (int p = p0; p <= p1; ++p) {
        auto it = m_pageTextCache.find(p);
        if (it == m_pageTextCache.end() || !it->second) {
            continue;
        }
        const auto& pt = *it->second;
        int startChar = (p == p0) ? c0 : 0;
        int endChar = (p == p1) ? c1 : static_cast<int>(pt.charCount()) - 1;
        auto rects = pt.getSelectionRects(startChar, endChar);
        if (rects.empty()) {
            continue;
        }

        pdf::AnnotationItem hl;
        // The UI owns the annotation id (RFC section 3.1): undo, redo and crash recovery address it by /NM.
        hl.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        hl.subtype = subtype;
        hl.pageIndex = p;
        hl.colorArgb = colorArgb;
        hl.hasColor = true;
        hl.flags = 4; // FPDF_ANNOT_FLAG_PRINT
        hl.contents = QString::fromStdU16String(pt.getSelectedText(startChar, endChar));

        double minX = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double minY = std::numeric_limits<double>::max();
        double maxY = std::numeric_limits<double>::lowest();
        const int first = std::max(0, startChar);
        const int last = std::min(endChar, static_cast<int>(pt.charCount()) - 1);
        for (int charIndex = first; charIndex <= last; ++charIndex) {
            const auto& b = pt.chars[static_cast<size_t>(charIndex)];
            if (!b.hasUserSpaceBox) {
                continue;
            }
            const double pLeft = b.userSpaceLeft;
            const double pRight = b.userSpaceRight;
            const double pTop = b.userSpaceTop;
            const double pBottom = b.userSpaceBottom;

            minX = std::min(minX, pLeft);
            maxX = std::max(maxX, pRight);
            minY = std::min(minY, pBottom);
            maxY = std::max(maxY, pTop);

            pdf::AnnotationQuad quad;
            quad.x1 = pLeft;  quad.y1 = pTop;
            quad.x2 = pRight; quad.y2 = pTop;
            quad.x3 = pLeft;  quad.y3 = pBottom;
            quad.x4 = pRight; quad.y4 = pBottom;
            hl.quadPoints.push_back(quad);
        }
        if (hl.quadPoints.empty()) {
            Q_EMIT loadError(tr("Could not determine PDF coordinates for the selected text."));
            continue;
        }
        hl.left = minX;
        hl.right = maxX;
        hl.bottom = minY;
        hl.top = maxY;

        const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, p, hl);
        if (!response.success) {
            Q_EMIT loadError(tr("Could not apply markup on page %1: %2").arg(p + 1).arg(response.errorMessage));
            continue;
        }
        m_editLog.recordAddAnnotation(p, hl);
        anyAdded = true;
    }

    if (anyAdded) {
        clearSelection();
        m_cache.clear();
        m_inFlightKeys.clear();
        ++m_currentGeneration;
        m_renderWorker.setGeneration(m_currentGeneration);
        viewport()->update();
    }
    return anyAdded;
}

bool PdfViewerWidget::highlightSelection(uint32_t colorArgb) {
    return markupSelection(pdf::AnnotationSubtype::Highlight, colorArgb);
}

bool PdfViewerWidget::underlineSelection(uint32_t colorArgb) {
    return markupSelection(pdf::AnnotationSubtype::Underline, colorArgb);
}

bool PdfViewerWidget::strikeOutSelection(uint32_t colorArgb) {
    return markupSelection(pdf::AnnotationSubtype::StrikeOut, colorArgb);
}

bool PdfViewerWidget::squigglySelection(uint32_t colorArgb) {
    return markupSelection(pdf::AnnotationSubtype::Squiggly, colorArgb);
}

bool PdfViewerWidget::addTextAnnotation(int pageIndex, double userX, double userY, const QString& contents, uint32_t colorArgb) {
    if (!m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    if (pageIndex < 0 || pageIndex >= m_pageCount) {
        return false;
    }
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);

    if (colorArgb == 0) {
        colorArgb = 0xFFFFD700; // Gold / Yellow
    }

    constexpr double kIconSize = 24.0;
    pdf::AnnotationItem note;
    note.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    note.subtype = pdf::AnnotationSubtype::Text;
    note.pageIndex = pageIndex;
    note.left = userX;
    note.bottom = userY - kIconSize;
    note.right = userX + kIconSize;
    note.top = userY;
    note.contents = contents;
    note.colorArgb = colorArgb;
    note.hasColor = true;
    note.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, pageIndex, note);
    if (!response.success) {
        Q_EMIT loadError(tr("Could not add sticky note: %1").arg(response.errorMessage));
        return false;
    }

    m_editLog.recordAddAnnotation(pageIndex, note);
    refreshAfterEdit();
    return true;
}

bool PdfViewerWidget::addSquareAnnotation(int pageIndex, const QRectF& pagePointsRect, uint32_t colorArgb, double borderWidth) {
    if (!m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    const QRectF normalized = pagePointsRect.normalized();
    if (pageIndex < 0 || pageIndex >= m_pageCount || normalized.isEmpty()) {
        return false;
    }
    (void)borderWidth;
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);

    if (colorArgb == 0) {
        colorArgb = 0xFFFF0000; // Red
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::Square;
    item.pageIndex = pageIndex;
    item.left = normalized.left();
    item.bottom = normalized.top();
    item.right = normalized.right();
    item.top = normalized.bottom();
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, pageIndex, item);
    if (!response.success) {
        Q_EMIT loadError(tr("Could not add square annotation: %1").arg(response.errorMessage));
        return false;
    }

    m_editLog.recordAddAnnotation(pageIndex, item);
    refreshAfterEdit();
    return true;
}

bool PdfViewerWidget::addCircleAnnotation(int pageIndex, const QRectF& pagePointsRect, uint32_t colorArgb, double borderWidth) {
    if (!m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    const QRectF normalized = pagePointsRect.normalized();
    if (pageIndex < 0 || pageIndex >= m_pageCount || normalized.isEmpty()) {
        return false;
    }
    (void)borderWidth;
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);

    if (colorArgb == 0) {
        colorArgb = 0xFF0000FF; // Blue
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::Circle;
    item.pageIndex = pageIndex;
    item.left = normalized.left();
    item.bottom = normalized.top();
    item.right = normalized.right();
    item.top = normalized.bottom();
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, pageIndex, item);
    if (!response.success) {
        Q_EMIT loadError(tr("Could not add circle annotation: %1").arg(response.errorMessage));
        return false;
    }

    m_editLog.recordAddAnnotation(pageIndex, item);
    refreshAfterEdit();
    return true;
}

bool PdfViewerWidget::deleteAnnotation(int pageIndex, const pdf::AnnotationItem& item) {
    if (!m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    if (item.name.isEmpty() || pageIndex < 0 || pageIndex >= m_pageCount) {
        return false;
    }
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);

    const auto response = m_renderWorker.removeAnnotationSync(m_currentDocumentId, pageIndex, item.name);
    if (!response.success || response.removedCount == 0) {
        Q_EMIT loadError(tr("Could not delete annotation: %1").arg(response.errorMessage));
        return false;
    }

    m_editLog.recordDeleteAnnotation(pageIndex, item);
    refreshAfterEdit();
    return true;
}

QString PdfViewerWidget::annotationTooltipHtml(const QString& author, const QString& contents) {
    constexpr qsizetype kMaxTipChars = 1000;
    auto prep = [](QString s) {
        if (s.size() > kMaxTipChars) s = s.left(kMaxTipChars) + QStringLiteral("…");
        return s.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    QString html = QStringLiteral("<p>");
    if (!author.isEmpty()) html += QStringLiteral("<b>[") + prep(author) + QStringLiteral("]</b><br>");
    html += prep(contents) + QStringLiteral("</p>");
    return html;
}

bool PdfViewerWidget::deleteAnnotationByName(int pageIndex, const QString& annotationName) {
    if (!m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    if (annotationName.isEmpty() || pageIndex < 0 || pageIndex >= m_pageCount) {
        return false;
    }

    pdf::AnnotationItem item;
    bool found = false;
    for (const auto& cmd : m_editLog.activeCommands()) {
        if (cmd.pageIndex == pageIndex) {
            if (cmd.type == pdf::EditCommandType::AddAnnotation && cmd.after.name == annotationName) {
                item = cmd.after;
                found = true;
                break;
            } else if (cmd.type == pdf::EditCommandType::ModifyAnnotation && cmd.after.name == annotationName) {
                item = cmd.after;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        const auto list = m_renderWorker.listAnnotationsSync(m_currentDocumentId, pageIndex);
        if (!list.success || list.truncated) {
            Q_EMIT loadError(tr("Could not delete annotation because its complete details could not be loaded."));
            return false;
        }
        for (const auto& a : list.annotations) {
            if (a.name == annotationName) {
                item = a;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return false;
    }

    return deleteAnnotation(pageIndex, item);
}

bool PdfViewerWidget::modifyTextAnnotation(int pageIndex, const pdf::AnnotationItem& before, const QString& newContents) {
    if (!m_hasDocument || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    if (pageIndex < 0 || pageIndex >= m_pageCount || before.name.isEmpty()) {
        return false;
    }
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);

    pdf::AnnotationItem after = before;
    after.contents = newContents;
    after.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);

    const auto remResp = m_renderWorker.removeAnnotationSync(m_currentDocumentId, pageIndex, before.name);
    if (!remResp.success || remResp.removedCount == 0) {
        Q_EMIT loadError(tr("Could not update note: %1").arg(remResp.errorMessage));
        return false;
    }

    const auto addResp = m_renderWorker.addAnnotationSync(m_currentDocumentId, pageIndex, after);
    if (!addResp.success) {
        m_renderWorker.addAnnotationSync(m_currentDocumentId, pageIndex, before);
        Q_EMIT loadError(tr("Could not add updated note: %1").arg(addResp.errorMessage));
        return false;
    }

    m_editLog.recordModifyAnnotation(pageIndex, before, after);
    refreshAfterEdit();
    return true;
}

void PdfViewerWidget::contextMenuEvent(QContextMenuEvent* event) {
    if (!isDocumentOpen()) {
        QAbstractScrollArea::contextMenuEvent(event);
        return;
    }

    const double dpr = effectiveDpr();
    const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->pos().x() * dpr));
    const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->pos().y() * dpr));
    const int clickPage = m_layout.getPageAt(docX, docY);

    QMenu menu(this);
    if (hasSelection()) {
        QAction* copyAct = menu.addAction(tr("&Copy"), this, &PdfViewerWidget::copySelection);
        copyAct->setShortcut(QKeySequence::Copy);
        menu.addSeparator();
        menu.addAction(tr("&Highlight"), this, [this]() { highlightSelection(); });
        menu.addAction(tr("&Underline"), this, [this]() { underlineSelection(); });
        menu.addAction(tr("&Strikeout"), this, [this]() { strikeOutSelection(); });
        menu.addAction(tr("S&quiggly"), this, [this]() { squigglySelection(); });
        menu.addSeparator();
    }

    if (clickPage >= 0) {
        double ax = 0.0, ay = 0.0;
        if (m_layout.documentToPagePoints(clickPage, docX, docY, ax, ay)) {
            const auto& pl = m_layout.getPageLayout(clickPage);
            const double userX = ax;
            const double userY = pl.heightPoints - ay;

            auto hit = m_renderWorker.hitTestAnnotationSync(m_currentDocumentId, clickPage, userX, userY);
            if (hit.hit && !hit.annotation.name.isEmpty()) {
                const auto hitItem = hit.annotation;
                menu.addAction(tr("&Delete Annotation"), this, [this, clickPage, hitItem]() {
                    deleteAnnotation(clickPage, hitItem);
                });
                menu.addSeparator();
            }

            menu.addAction(tr("&Add Sticky Note Here"), this, [this, clickPage, userX, userY]() {
                bool ok = false;
                const QString text = QInputDialog::getMultiLineText(
                    this, tr("Add Sticky Note"), tr("Note text:"), QString(), &ok);
                if (ok && !text.trimmed().isEmpty()) {
                    addTextAnnotation(clickPage, userX, userY, text);
                }
            });
            menu.addAction(tr("Add &Rectangle Here"), this, [this, clickPage, userX, userY]() {
                addSquareAnnotation(clickPage, QRectF(userX, userY - 60.0, 100.0, 60.0));
            });
            menu.addAction(tr("Add &Circle Here"), this, [this, clickPage, userX, userY]() {
                addCircleAnnotation(clickPage, QRectF(userX, userY - 60.0, 100.0, 60.0));
            });
        }
    }

    if (!menu.isEmpty()) {
        menu.exec(event->globalPos());
    }
}

void PdfViewerWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isDocumentOpen()) {
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(event->position().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(event->position().y() * dpr));
        const int clickPage = m_layout.getPageAt(docX, docY);
        if (clickPage >= 0) {
            double ax = 0.0, ay = 0.0;
            if (m_layout.documentToPagePoints(clickPage, docX, docY, ax, ay)) {
                const auto& pl = m_layout.getPageLayout(clickPage);
                const double userX = ax;
                const double userY = pl.heightPoints - ay;

                auto hit = m_renderWorker.hitTestAnnotationSync(m_currentDocumentId, clickPage, userX, userY);
                if (hit.hit && hit.annotation.subtype == pdf::AnnotationSubtype::Text) {
                    bool ok = false;
                    const QString title = hit.annotation.author.isEmpty() ? tr("Sticky Note") : tr("Sticky Note - %1").arg(hit.annotation.author);
                    const QString newText = QInputDialog::getMultiLineText(
                        this, title, tr("Note Content:"), hit.annotation.contents, &ok);
                    if (ok && newText != hit.annotation.contents) {
                        modifyTextAnnotation(clickPage, hit.annotation, newText);
                    }
                    event->accept();
                    return;
                }
            }
        }
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

bool PdfViewerWidget::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::ToolTip && isDocumentOpen()) {
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        const double dpr = effectiveDpr();
        const int64_t docX = m_docScrollX + static_cast<int64_t>(std::round(helpEvent->pos().x() * dpr));
        const int64_t docY = m_docScrollY + static_cast<int64_t>(std::round(helpEvent->pos().y() * dpr));
        const int page = m_layout.getPageAt(docX, docY);
        if (page >= 0) {
            double ax = 0.0, ay = 0.0;
            if (m_layout.documentToPagePoints(page, docX, docY, ax, ay)) {
                const auto& pl = m_layout.getPageLayout(page);
                const double userX = ax;
                const double userY = pl.heightPoints - ay;
                auto hit = m_renderWorker.hitTestAnnotationSync(m_currentDocumentId, page, userX, userY);
                if (hit.hit && !hit.annotation.contents.isEmpty()) {
                    const QString tip = annotationTooltipHtml(hit.annotation.author, hit.annotation.contents);
                    QToolTip::showText(helpEvent->globalPos(), tip, viewport());
                    return true;
                }
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

void PdfViewerWidget::refreshAfterEdit() {
    m_cache.clear();
    m_inFlightKeys.clear();
    ++m_currentGeneration;
    m_renderWorker.setGeneration(m_currentGeneration);
    viewport()->update();
}

bool PdfViewerWidget::undoEdit() {
    if (m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);
    pdf::EditCommand cmd;
    if (!m_hasDocument || !m_editLog.undo(cmd)) {
        return false;
    }
    if (cmd.type == pdf::EditCommandType::AddAnnotation) {
        const auto response = m_renderWorker.removeAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.after.name);
        if (!response.success || response.removedCount == 0) {
            pdf::EditCommand restored;
            m_editLog.redo(restored);
            Q_EMIT loadError(tr("Could not undo the annotation: %1").arg(response.errorMessage));
            return false;
        }
    } else if (cmd.type == pdf::EditCommandType::DeleteAnnotation) {
        const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.before);
        if (!response.success) {
            pdf::EditCommand restored;
            m_editLog.redo(restored);
            Q_EMIT loadError(tr("Could not undo deletion: %1").arg(response.errorMessage));
            return false;
        }
    } else if (cmd.type == pdf::EditCommandType::ModifyAnnotation) {
        m_renderWorker.removeAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.after.name);
        const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.before);
        if (!response.success) {
            pdf::EditCommand restored;
            m_editLog.redo(restored);
            Q_EMIT loadError(tr("Could not undo modification: %1").arg(response.errorMessage));
            return false;
        }
    }
    refreshAfterEdit();
    return true;
}

bool PdfViewerWidget::redoEdit() {
    if (m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    QScopedValueRollback<bool> editGuard(m_editInProgress, true);
    pdf::EditCommand cmd;
    if (!m_hasDocument || !m_editLog.redo(cmd)) {
        return false;
    }
    if (cmd.type == pdf::EditCommandType::AddAnnotation) {
        const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.after);
        if (!response.success) {
            pdf::EditCommand restored;
            m_editLog.undo(restored);
            Q_EMIT loadError(tr("Could not redo the annotation: %1").arg(response.errorMessage));
            return false;
        }
    } else if (cmd.type == pdf::EditCommandType::DeleteAnnotation) {
        const auto response = m_renderWorker.removeAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.before.name);
        if (!response.success || response.removedCount == 0) {
            pdf::EditCommand restored;
            m_editLog.undo(restored);
            Q_EMIT loadError(tr("Could not redo deletion: %1").arg(response.errorMessage));
            return false;
        }
    } else if (cmd.type == pdf::EditCommandType::ModifyAnnotation) {
        m_renderWorker.removeAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.before.name);
        const auto response = m_renderWorker.addAnnotationSync(m_currentDocumentId, cmd.pageIndex, cmd.after);
        if (!response.success) {
            pdf::EditCommand restored;
            m_editLog.undo(restored);
            Q_EMIT loadError(tr("Could not redo modification: %1").arg(response.errorMessage));
            return false;
        }
    }
    refreshAfterEdit();
    return true;
}

void PdfViewerWidget::recoverAfterWorkerFailure() {
    if (m_recovering || m_recoveryPath.isEmpty()) {
        return;
    }
    m_recovering = true;
    const QString path = m_recoveryPath;
    m_recoveryPath.clear();

    m_preserveEditLogOnOpen = true;
    const pdf::DocumentError err = openDocument(path, m_currentPassword);   // restarts the worker; the file on disk is the saved state
    m_preserveEditLogOnOpen = false;
    if (err != pdf::DocumentError::None) {
        m_recovering = false;
        Q_EMIT loadError(tr("The viewer process stopped and the document could not be reopened. "
                            "Unsaved annotations were not restored."));
        return;
    }

    QScopedValueRollback<bool> editGuard(m_editInProgress, true);
    int applied = 0;
    bool ok = true;
    for (const auto& op : m_editLog.opsSinceSave()) {
        bool done = false;
        if (op.add) {
            done = m_renderWorker.addAnnotationSync(m_currentDocumentId, op.pageIndex, op.item).success;
        } else {
            const auto response = m_renderWorker.removeAnnotationSync(m_currentDocumentId, op.pageIndex, op.name);
            done = response.success && response.removedCount > 0;
        }
        if (!done) {
            ok = false;
            break;
        }
        ++applied;
    }
    refreshAfterEdit();
    m_recovering = false;
    if (ok) {
        Q_EMIT editsRecovered(applied);
    } else {
        m_hasDocument = false;
        Q_EMIT loadError(tr("The viewer process stopped; only some unsaved annotations could be restored."));
    }
}

bool PdfViewerWidget::saveDocument(const QString& targetPath) {
    if (!m_hasDocument || m_currentDocumentId == 0 || m_saveInProgress || m_editInProgress || m_loading || m_recovering) {
        return false;
    }
    QScopedValueRollback<bool> saveGuard(m_saveInProgress, true);
    QString destPath = targetPath.isEmpty() ? m_currentFilePath : targetPath;
    if (destPath.isEmpty()) {
        return false;
    }

    QFileInfo destInfo(destPath);
    QTemporaryFile tempFile(destInfo.absoluteDir().filePath(QStringLiteral("save_XXXXXX.tmp")));
    if (!tempFile.open()) {
        return false;
    }
    QString tempPath = tempFile.fileName();

    // Preserve the source PDF's existing objects and signatures for every save based on a file,
    // including Save As. A full rewrite is only appropriate for documents without a source path.
    const bool incremental = !m_currentFilePath.isEmpty();
    QByteArray origBytes;
    if (incremental) {
        const QFileInfo currentSourceInfo(m_currentFilePath);
        if (!currentSourceInfo.exists() || currentSourceInfo.size() != m_sourceFileSize ||
            currentSourceInfo.lastModified() != m_sourceFileModified) {
            tempFile.remove();
            return false;
        }
        QFile origFile(m_currentFilePath);
        if (!origFile.open(QIODevice::ReadOnly) || origFile.size() <= 0 || origFile.size() > MAX_VALIDATED_PDF_BYTES) {
            tempFile.remove();
            return false;
        }
        origBytes = origFile.readAll();
        origFile.close();
        if (origBytes.isEmpty()) {
            tempFile.remove();
            return false;
        }
    }
    int tempFd = -1;
#ifdef Q_OS_LINUX
    tempFd = ::dup(tempFile.handle());
    if (tempFd < 0) {
        tempFile.remove();
        return false;
    }
#else
    tempFile.close();
#endif
    auto saveResp = m_renderWorker.saveDocumentSync(m_currentDocumentId, tempFd, incremental, tempPath);
    if (!saveResp.success || saveResp.bytesWritten == 0) {
        tempFile.remove();
        return false;
    }

#ifdef PDF_WORKER_TEST_HOOKS
    if (m_beforeValidationHook) {
        m_beforeValidationHook(tempPath);
    }
#endif

    QFile checkFile(tempPath);
    if (!checkFile.open(QIODevice::ReadOnly)) {
        tempFile.remove();
        return false;
    }
    const qint64 outputSize = checkFile.size();
    if (outputSize <= 0 || outputSize > MAX_VALIDATED_PDF_BYTES ||
        static_cast<uint64_t>(outputSize) != saveResp.bytesWritten) {
        checkFile.close();
        tempFile.remove();
        return false;
    }
    QByteArray checkBytes = checkFile.readAll();
    const bool completeRead = checkBytes.size() == outputSize && checkFile.error() == QFileDevice::NoError;
    checkFile.close();
    if (!completeRead) {
        tempFile.remove();
        return false;
    }

    if (!checkBytes.startsWith("%PDF-")) {
        tempFile.remove();
        return false;
    }

    if (incremental) {
        if (!checkBytes.startsWith(origBytes)) {
            tempFile.remove();
            return false;
        }
    }

    // Reject malformed envelopes cheaply before spending time starting the independent parser worker.
    if (checkBytes.size() < 16) {
        tempFile.remove();
        return false;
    }
    qsizetype eofIdx = checkBytes.lastIndexOf("%%EOF");
    if (eofIdx == -1 || checkBytes.size() - eofIdx > 1024) {
        tempFile.remove();
        return false;
    }

    // Reopen the exact byte snapshot in a new confined worker before committing. This proves the
    // output parses independently and that persisted annotations survived serialization.
    std::vector<size_t> expectedAnnotationCounts;
    expectedAnnotationCounts.reserve(static_cast<size_t>(m_pageCount));
    for (int page = 0; page < m_pageCount; ++page) {
        const auto response = m_renderWorker.listAnnotationsSync(m_currentDocumentId, page, 15000);
        if (!response.success || response.truncated) {
            tempFile.remove();
            return false;
        }
        expectedAnnotationCounts.push_back(response.annotations.size());
    }

    QTemporaryFile snapshot;
    if (!snapshot.open() || snapshot.write(checkBytes) != checkBytes.size() || !snapshot.flush()) {
        tempFile.remove();
        return false;
    }
    const QString snapshotPath = snapshot.fileName();
    snapshot.close();
    pdf::ipc::WorkerClient verifier;
    const auto reopened = verifier.openDocumentSync(1, snapshotPath.toStdString(), m_currentPassword.toStdString());
    bool verified = reopened.error == pdf::DocumentError::None && reopened.pageCount == m_pageCount;
    if (verified) {
        for (int page = 0; page < m_pageCount; ++page) {
            const auto response = verifier.listAnnotationsSync(1, page, 15000);
            if (!response.success || response.truncated ||
                response.annotations.size() != expectedAnnotationCounts[static_cast<size_t>(page)]) {
                verified = false;
                break;
            }
        }
    }
    verifier.stop();
    if (!verified) {
        tempFile.remove();
        return false;
    }

    // Refuse a commit if another process replaced or changed the source while validation ran.
    if (incremental) {
        const QFileInfo currentSourceInfo(m_currentFilePath);
        if (!currentSourceInfo.exists() || currentSourceInfo.size() != m_sourceFileSize ||
            currentSourceInfo.lastModified() != m_sourceFileModified) {
            tempFile.remove();
            return false;
        }
    }

#ifdef PDF_WORKER_TEST_HOOKS
    if (m_beforeCommitHook) {
        m_beforeCommitHook(tempPath);
    }
#endif

    // Recheck both metadata and bytes as late as possible. This narrows the conflict window and
    // catches source changes made while output validation (or a test hook) was running.
    if (incremental) {
        const QFileInfo currentSourceInfo(m_currentFilePath);
        QFile currentSource(m_currentFilePath);
        if (!currentSourceInfo.exists() || currentSourceInfo.size() != m_sourceFileSize ||
            currentSourceInfo.lastModified() != m_sourceFileModified ||
            !currentSource.open(QIODevice::ReadOnly) || currentSource.readAll() != origBytes) {
            tempFile.remove();
            return false;
        }
    }

    // Commit exactly the bytes that were validated above - never re-read the worker-written temp file, whose
    // descriptor a compromised worker may still hold. QSaveFile writes a sibling temp file and renames it over
    // the target (atomic), follows a symlink to its target, and keeps the existing file's permissions.
    // No ".bak" is created or deleted.
    QSaveFile out(destPath);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) || out.write(checkBytes) != checkBytes.size() || !out.commit()) {
        out.cancelWriting();
        tempFile.remove();
        return false;
    }
    tempFile.remove();

    m_currentFilePath = destPath;
    const QFileInfo savedInfo(destPath);
    m_sourceFileSize = savedInfo.exists() ? savedInfo.size() : -1;
    m_sourceFileModified = savedInfo.exists() ? savedInfo.lastModified() : QDateTime();
    m_editLog.markSaved();

    // The worker must use the committed revision as its base for the next incremental save.
    // Keep the edit history (now clean) so undo remains available after saving.
    const int64_t rebasedDocumentId = m_currentDocumentId + 1;
    const auto rebased = m_renderWorker.openDocumentSync(rebasedDocumentId, destPath.toStdString(),
                                                         m_currentPassword.toStdString());
    if (rebased.error != pdf::DocumentError::None) {
        m_hasDocument = false;
        m_workerErrorMessage = openErrorMessage(rebased.error, destPath);
        Q_EMIT loadError(tr("The PDF was saved, but the viewer could not reload the saved revision. Reopen the file to continue."));
        return true;
    }
    const int savedPage = m_currentPage;
    const double savedZoom = m_scale;
    const int64_t savedScrollX = m_docScrollX;
    const int64_t savedScrollY = m_docScrollY;
    m_preserveEditLogOnOpen = true;
    applyOpenedDocument(rebasedDocumentId, destPath, m_currentPassword, rebased);
    m_preserveEditLogOnOpen = false;
    restoreReadingPosition(savedPage, savedZoom, savedScrollX, savedScrollY);
    return true;
}

} // namespace ui
