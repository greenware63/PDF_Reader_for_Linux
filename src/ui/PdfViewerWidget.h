#pragma once

#include <QAbstractScrollArea>
#include <QString>
#include <memory>
#include <optional>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <QPixmap>
#include "ColorMode.h"
#include "layout/DocumentLayout.h"
#include "layout/ScrollMapper.h"
#include "cache/TileCache.h"
#include "ipc/WorkerClient.h"
#include "engine/PageText.h"
#include "engine/SearchTypes.h"

namespace ui {

/**
 * @brief Professional, high-performance Qt6 PDF Viewer widget.
 * Implements continuous document scrolling, smooth zooming, exact 64-bit coordinate space,
 * compressed scrollbar mapping, sub-pixel rounding-safe fit-width, asynchronous background rendering,
 * background text extraction with cursor hit-testing, selection highlighting, clipboard copy,
 * background asynchronous page-by-page text search with match highlighting and cycling,
 * reversible Dark Mode night inversion shader, and comprehensive keyboard navigation.
 * All PDFium operations run in the separate pdf_worker process; the UI process never loads PDFium.
 */
class PdfViewerWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    static constexpr double DESKTOP_100_PERCENT_SCALE = 96.0 / 72.0; // 1.3333333333333333
    static constexpr double ZOOM_STEP = 1.25;                         // one zoom button click or wheel notch
    static constexpr size_t MAX_CACHED_TEXT_PAGES = 48;               // extracted-text pages kept in memory

    explicit PdfViewerWidget(QWidget* parent = nullptr);
    ~PdfViewerWidget() override;

    /**
     * @brief Opens a document synchronously (blocks the caller until the worker has loaded it).
     * Cancels any pending openDocumentAsync(). Prefer openDocumentAsync() for interactive use.
     */
    pdf::DocumentError openDocument(const QString& filePath);

    /**
     * @brief Opens a document without blocking the UI thread.
     * The currently open document stays visible and interactive while the file loads; on success the
     * viewer switches to the new document and emits documentLoaded(), on failure it emits loadError()
     * and keeps the current document. Calls made while an open is in progress are coalesced:
     * only the most recent pending path/password is opened after the current one finishes.
     * loadingChanged() is emitted when loading starts and when the last queued open finishes.
     */
    void openDocumentAsync(const QString& filePath, const QString& password = QString());

    /// @return true while an asynchronous open is in progress or queued.
    bool isLoading() const noexcept { return m_loading; }

    void closeDocument();

    bool isDocumentOpen() const noexcept { return m_hasDocument; }
    QString currentFilePath() const noexcept { return m_currentFilePath; }
    int pageCount() const noexcept { return m_pageCount; }
    int currentPage() const noexcept { return m_currentPage; }
    double currentZoom() const noexcept { return m_scale; }
    int64_t currentDocumentId() const noexcept { return m_currentDocumentId; }
    uint64_t currentGeneration() const noexcept { return m_currentGeneration; }

    int64_t currentDocScrollX() const noexcept { return m_docScrollX; }
    int64_t currentDocScrollY() const noexcept { return m_docScrollY; }

    void setZoom(double zoom, const QPoint* anchorWidgetPos = nullptr);
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void zoomFitWidth();
    /// Fits the whole current page (or spread, in the two-page modes) into the viewport.
    void zoomFitPage();

    /// Page arrangement: continuous, single page, two-page spreads (optionally with a stand-alone cover).
    void setViewMode(layout::ViewMode mode);
    layout::ViewMode viewMode() const noexcept { return m_layout.viewMode(); }

    /// Page rotation APIs: 0, 90, 180, 270 degrees clockwise
    int rotation() const noexcept { return m_rotation; }
    void setRotation(int degrees);
    void rotateClockwise();
    void rotateCounterClockwise();

    void goToPage(int pageIndex);
    /// Navigates to a normalized vertical position in the displayed page.
    void goToPage(int pageIndex, double targetYFraction);
    void nextPage();
    void previousPage();

    /// Color mode / dark mode / night inversion APIs
    ColorMode colorMode() const noexcept { return m_colorMode; }
    void setColorMode(ColorMode mode);
    bool isDarkMode() const noexcept { return m_colorMode == ColorMode::Inverted || m_colorMode == ColorMode::HighContrast; }
    void setDarkMode(bool enabled);
    void toggleDarkMode();

    /// Reading position restoration & scroll positioning
    void restoreReadingPosition(int pageIndex, double zoom, int64_t scrollX, int64_t scrollY);
    void setDocScrollPosition(int64_t docX, int64_t docY);

    /**
     * @brief Renders a single page synchronously via the isolated worker for printing or export.
     */
    QImage renderPageForPrint(int pageIndex, double scale = 1.0, double dpr = 2.0);

    /// Returns a standalone vector PDF containing pages [firstPage, lastPage] (0-based), or empty on failure.
    QByteArray extractPagesPdf(int firstPage, int lastPage);

    /**
     * @brief Manually triggers retry of all currently failed tiles in the viewport.
     */
    void retryFailedTiles();

    /**
     * @brief Injects an explicit Device Pixel Ratio override (for high-DPI testing and simulation).
     * Passing 0.0 or negative resets back to system devicePixelRatioF().
     */
    void setDevicePixelRatioOverride(double dpr);
    double effectiveDpr() const;

    /**
     * @brief Blocks until the background render worker has finished all pending requests and in-flight renders.
     * Useful for deterministic testing and flush operations.
     */
    void waitForRendering();

    pdf::ipc::WorkerClient& renderWorker() noexcept { return m_renderWorker; }
    pdf::TileCache& cache() noexcept { return m_cache; }
    const layout::DocumentLayout& layout() const noexcept { return m_layout; }
    size_t inFlightCount() const noexcept { return m_inFlightKeys.size(); }

    /// Tiles drawn from a previous zoom level (scaled) because the current-zoom tile was not ready yet.
    size_t fallbackTilesDrawn() const noexcept { return m_fallbackTilesDrawn; }
    /// Number of pages whose extracted text is currently held in memory (bounded by MAX_CACHED_TEXT_PAGES
    /// plus any pages protected by the current selection).
    size_t cachedTextPageCount() const noexcept { return m_pageTextCache.size(); }
    /// Prefetch (off-screen) tile requests handed to the worker so far.
    size_t prefetchRequestsIssued() const noexcept { return m_prefetchRequestsIssued; }
    /// Enables/disables the off-screen prefetch ring (enabled by default).
    void setPrefetchEnabled(bool enabled) noexcept { m_prefetchEnabled = enabled; }

    // Text selection & copy APIs
    bool hasSelection() const noexcept;
    QString selectedText() const;
    void clearSelection();
    void copySelection();
    std::shared_ptr<const pdf::PageText> getPageText(int pageIndex) const;

    // Text search APIs
    void findText(const QString& query, bool matchCase = false);
    void findNext();
    void findPrevious();
    void clearSearch();
    int searchMatchCount() const noexcept { return static_cast<int>(m_searchMatches.size()); }
    int currentSearchMatchIndex() const noexcept { return m_currentMatchIndex; }
    const std::vector<pdf::SearchMatch>& searchMatches() const noexcept { return m_searchMatches; }
    const std::vector<pdf::OutlineItem>& outline() const noexcept { return m_outline; }
    bool hasWorkerError() const noexcept { return !m_workerErrorMessage.isEmpty(); }
    QString workerErrorMessage() const noexcept { return m_workerErrorMessage; }
    QString pageLabel(int pageIndex) const;
    const std::vector<QString>& pageLabels() const noexcept { return m_pageLabels; }
    void requestThumbnail(int pageIndex, int targetWidth = 140);

Q_SIGNALS:
    void documentLoaded(const QString& title, int pages);
    void pageLabelsReady(const std::vector<QString>& labels);
    void thumbnailReady(int pageIndex, const QImage& image);
    void thumbnailFailed(int pageIndex);
    void loadError(const QString& errorMessage);
    void documentOpenFailed(const QString& filePath, pdf::DocumentError error);
    void loadingChanged(bool loading, const QString& fileName);
    void pageChanged(int currentPage, int totalPages);
    void zoomChanged(double zoomRatio);
    void selectionChanged();
    void searchResultsChanged(int currentMatch, int totalMatches);
    void searchProgressChanged(int pagesSearched, int totalPages);
    void searchFinished(bool truncated);
    void outlineReady(const std::vector<pdf::OutlineItem>& outline);
    void darkModeChanged(bool isDark);
    void colorModeChanged(ui::ColorMode mode);
    void viewModeChanged(layout::ViewMode mode);
    void rotationChanged(int degrees);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private Q_SLOTS:
    void onScrollValueChanged();
    void onTileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void onTileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);
    void onDocumentOpened(int64_t documentId, pdf::OpenResult result);
    void onPageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text);
    void onSearchMatchesFound(int64_t documentId, int64_t searchId, int pageIndex, const std::vector<pdf::SearchMatch>& matches);
    void onSearchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages);
    void onSearchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated);
    void onDocumentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline);

private:
    struct TextPosition {
        int pageIndex{-1};
        int charIndex{-1};
        bool isValid() const noexcept { return pageIndex >= 0 && charIndex >= 0; }
        bool operator==(const TextPosition& o) const noexcept {
            return pageIndex == o.pageIndex && charIndex == o.charIndex;
        }
        bool operator!=(const TextPosition& o) const noexcept {
            return !(*this == o);
        }
    };

    void updateLayoutGeometry();
    void updateScrollBars();

    void goToPageImpl(int pageIndex, std::optional<double> targetYFraction);
    /// SinglePage mode: makes `pageIndex` the focus page (relayout) so it is reachable. No-op otherwise.
    void ensurePageShown(int pageIndex);
    /// Page-flip accumulator for wheel scrolling past the edge in SinglePage mode.
    int m_pageFlipAccumulator{0};

    void drawErrorPlaceholder(QPainter& painter, const QRectF& rect);
    void drawPendingPlaceholder(QPainter& painter, const QRectF& rect);
    void drawLoadingOverlay(QPainter& painter);
    void drawWorkerErrorBanner(QPainter& painter);
    bool drawFallbackTile(QPainter& painter, const layout::VisibleTile& tile, const QRectF& tileRect, double qScale, double qDpr);
    void rememberFallbackScale(double previousScale);
    void ensurePageTextRequested(int pageIndex);
    void trimPageTextCache();
    void drawSelection(QPainter& painter, int pageIndex, const layout::PageLayout& pl, int64_t scrollDocX, int64_t scrollDocY, double effScale);
    void drawSearchHighlights(QPainter& painter, int pageIndex, const layout::PageLayout& pl, int64_t scrollDocX, int64_t scrollDocY, double effScale);
    void scrollToMatch(const pdf::SearchMatch& match);

    QString openErrorMessage(pdf::DocumentError error, const QString& filePath) const;
    void applyOpenedDocument(int64_t newDocumentId, const QString& filePath, const pdf::OpenResult& result);
    void startAsyncOpen(const QString& filePath, const QString& password);
    void cancelPendingOpen();

    layout::DocumentLayout m_layout;
    layout::ScrollMapper m_scrollMapper;
    pdf::TileCache m_cache;
    pdf::ipc::WorkerClient m_renderWorker;

    // Asynchronous open state (UI thread only)
    bool m_loading{false};
    int64_t m_pendingOpenId{0};
    QString m_pendingOpenPath;
    bool m_hasQueuedOpen{false};
    QString m_queuedOpenPath;
    QString m_queuedOpenPassword;

    bool m_hasDocument{false};
    bool m_outlineRequestPending{false};
    int m_pageCount{0};
    int64_t m_currentDocumentId{1};
    uint64_t m_currentGeneration{1};
    int m_currentPage{0};
    double m_scale{DESKTOP_100_PERCENT_SCALE};
    double m_dprOverride{0.0};
    bool m_navigating{false};
    int m_wheelDeltaAccumulator{0};

    // Tracks tile requests currently in-flight on the worker to prevent duplicate enqueues
    std::unordered_set<pdf::TileKey, pdf::TileKeyHash> m_inFlightKeys;

    // Primary 64-bit document coordinates, immune to 32-bit slider resize quantization
    int64_t m_docScrollX{0};
    int64_t m_docScrollY{0};

    // Recently used zoom scales (newest first, quantized); their cached tiles serve as scaled stand-ins
    // while tiles for the current zoom are still rendering.
    static constexpr size_t MAX_FALLBACK_SCALES = 3;
    std::vector<double> m_fallbackScales;
    size_t m_fallbackTilesDrawn{0};

    // Off-screen prefetch ring (one viewport height above and below the visible area)
    bool m_prefetchEnabled{true};
    size_t m_prefetchRequestsIssued{0};

    // Cached previous viewport dimensions to anchor precisely during resizes
    int64_t m_lastVpWidthDevice{0};
    int64_t m_lastVpHeightDevice{0};

    // Text layer and selection
    std::unordered_map<int, std::shared_ptr<pdf::PageText>> m_pageTextCache;
    TextPosition m_selectionStart;
    TextPosition m_selectionEnd;
    bool m_isSelecting{false};

    // Text search state
    int64_t m_currentSearchId{0};
    QString m_searchQuery;
    bool m_searchMatchCase{false};
    int m_searchStartPage{0};
    std::vector<pdf::SearchMatch> m_searchMatches;
    int m_currentMatchIndex{-1};
    std::vector<pdf::OutlineItem> m_outline;
    QString m_workerErrorMessage;
    std::vector<QString> m_pageLabels;
    QString m_currentFilePath;

    // Color mode / dark mode / night inversion state
    ColorMode m_colorMode{ColorMode::Normal};
    bool m_darkMode{false};
    int m_rotation{0};
    mutable std::unordered_map<pdf::TileKey, QPixmap, pdf::TileKeyHash> m_transformedPixmapCache;
};

} // namespace ui
