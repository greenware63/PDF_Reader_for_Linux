#pragma once

#include <QObject>
#include <QMetaType>
#include <QString>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <list>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <future>
#include <functional>
#include <string>
#include <chrono>
#include <utility>
#include "engine/IDocument.h"
#include "engine/IPage.h"
#include "engine/PixelBuffer.h"
#include "engine/PageText.h"
#include "engine/SearchTypes.h"
#include "cache/TileCache.h"
#include "ipc/IpcProtocol.h"

namespace pdf {

namespace ipc {
struct PageRedactionItem;
struct RedactDocumentResponse;
} // namespace ipc

struct TileRenderRequest {
    int64_t documentId{0};
    uint64_t generation{0};
    int pageIndex{0};
    PixelRect rect;
    double scale{1.0};
    double dpr{1.0};
    TileKey key;
    int priority{0}; // Higher value = rendered first
    int rotation{0};
};

struct OpenResult {
    DocumentError error{DocumentError::None};
    int pageCount{0};
    std::vector<std::pair<double, double>> pageSizes;
    std::vector<std::string> pageLabels;
};

/**
 * @brief Dedicated single-threaded engine worker for serializing ALL PDFium calls off the UI thread.
 * Document loading, metadata queries, page creation, tile rendering, background search, and resource teardown
 * run strictly on this worker thread.
 */
class RenderWorker : public QObject {
    Q_OBJECT

public:
    explicit RenderWorker(QObject* parent = nullptr);
    ~RenderWorker() override;

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    /**
     * @brief Opens a PDF file serially on the worker thread.
     * Extracts pageCount and page sizes on the worker thread without touching PDFium from the UI thread.
     * @param fd Optional open file descriptor (e.g. transferred via SCM_RIGHTS under Landlock).
     */
    OpenResult openDocumentSync(int64_t documentId, const std::string& filePath, const std::string& password = "", int fd = -1);

    /**
     * @brief Opens a PDF file on the worker thread WITHOUT blocking the caller.
     * The result (including page count and all page sizes) is delivered via the documentOpened() signal,
     * which fires exactly once per call. On failure the previously active document and its queued
     * tile requests are left completely untouched. On success the active document id switches to
     * `documentId` and queued requests for the old document are discarded.
     * Connect with Qt::QueuedConnection from the UI thread.
     * @param fd Optional open file descriptor (e.g. transferred via SCM_RIGHTS under Landlock).
     */
    void openDocumentAsync(int64_t documentId, const std::string& filePath, const std::string& password = "", int fd = -1);

    /**
     * @brief Closes the active document and clears all open page handles serially on the worker thread.
     */
    void closeDocumentSync();

    /**
     * @brief Requests low-priority extraction of the active document outline.
     * The result is delivered through documentOutlineReady().
     */
    void requestDocumentOutline(int64_t documentId);

    /**
     * @brief Requests rendering a miniature thumbnail for the given page.
     * Delivered via thumbnailReady() or thumbnailFailed().
     */
    void requestThumbnail(int64_t documentId, int pageIndex, int targetWidth = 140, int rotation = 0);

    /**
     * @brief Requests high-resolution rendering of a full page for printing or export.
     * Delivered via printPageReady() or printPageFailed().
     */
    void requestPrintPage(int64_t documentId, int pageIndex, double scale = 1.0, double dpr = 2.0, int rotation = 0);

    /**
     * @brief Extracts pages [firstPage, lastPage] into a standalone vector PDF on the worker thread.
     * Delivered via extractPagesReady() or extractPagesFailed().
     */
    void requestExtractPages(int64_t documentId, int firstPage, int lastPage);

    /**
     * @brief Applies burn-in redactions to the active document and generates a sanitized raster PDF.
     * Delivered via redactDocumentReady() or redactDocumentFailed().
     */
    void requestRedactDocument(int64_t documentId,
                               const std::vector<ipc::PageRedactionItem>& redactions,
                               double dpi = 150.0,
                               int edgeGrowPx = 1);

    /**
     * @brief Sets current request generation. Discards queued requests with older generations.
     *
     * Only QUEUED requests are discarded. A render that is already running is not cancelled and is still
     * delivered when it finishes, even though its generation is now stale: a newer-generation request for
     * the same tile may have been deduplicated onto it (see enqueueRequest), and tile keys, not
     * generations, identify content, so a finished tile is valid for any consumer that still wants it.
     */
    void setGeneration(uint64_t generation);

    /**
     * @brief Enqueues a single tile render request.
     */
    void enqueueRequest(TileRenderRequest request);

    /**
     * @brief Enqueues a batch of tile render requests.
     */
    void enqueueRequests(std::vector<TileRenderRequest> requests);

    /**
     * @brief Cancels all pending render requests on the queue.
     */
    void cancelAll();

    /**
     * @brief Requests text extraction for the given page.
     */
    void requestPageText(int64_t documentId, int pageIndex);

    /**
     * @brief Synchronously extracts text for the given page on the worker thread.
     */
    std::shared_ptr<const PageText> getPageTextSync(int64_t documentId, int pageIndex);

    /**
     * @brief Initiates asynchronous document search across all pages.
     */
    void startSearch(int64_t documentId, int64_t searchId, const QString& query,
                     bool matchCase, int totalPages, int startPage = 0);

    /**
     * @brief Cancels any ongoing background search.
     */
    void cancelSearch();

    /**
     * @brief Blocks until the worker queue is empty and the worker thread is idle.
     */
    void waitForIdle();

    /**
     * @brief Checks if the worker queue is empty and the worker thread is idle.
     */
    bool isIdle() const;

    /**
     * @brief Shuts down the background worker thread cleanly.
     */
    void stop();

    /**
     * @return Number of pending render requests in the queue.
     */
    size_t pendingCount();

    /**
     * @brief Test-only delay injected per tile render to simulate slow rendering.
     */
    void setRenderDelayForTesting(std::chrono::milliseconds delay) noexcept {
        m_testRenderDelayMs.store(static_cast<int>(delay.count()), std::memory_order_relaxed);
    }

    /**
     * @brief Updates the engine's heartbeat progress timestamp and current operation.
     */
    void noteProgress(ipc::WorkerOp op = ipc::WorkerOp::Idle);
    std::pair<int64_t, uint32_t> progressSnapshot() const noexcept;
    void postHealthCheck(std::function<void()> onHealthy);

    /**
     * @brief Checks if the engine worker is alive or actively progressing on an operation
     * within the specified silent duration window.
     */
    bool isEngineProgressing(int64_t maxSilentDurationMs = 5000) const noexcept;

#ifdef PDF_WORKER_TEST_HOOKS
    /// Test hook: documents larger than this are opened by path instead of being pinned in memory.
    void setMaxPinnedBytesForTesting(unsigned long long bytes) noexcept { m_maxPinnedBytes.store(bytes); }

    /**
     * @brief Test hook: artificially blocks the engine thread to simulate an unresponsive engine.
     */
    void setHangEngineForTesting(bool hang) noexcept {
        if (!hang) {
            m_testHangEngine.store(false, std::memory_order_relaxed);
            return;
        }
        m_testHangParked.store(false, std::memory_order_release);
        m_testHangEngine.store(true, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lock(m_mutex); } // order the store against the engine's wait predicate
        m_cv.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (!m_testHangParked.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
#endif

    /**
     * @brief Test hook: artificially delays every openDocumentAsync() by this many milliseconds
     * before the file is loaded, to make non-blocking behaviour observable. 0 disables.
     */
    void setOpenDelayForTesting(std::chrono::milliseconds delay) noexcept {
        m_testOpenDelayMs.store(static_cast<int>(delay.count()), std::memory_order_relaxed);
    }

    /**
     * @return Number of open IPage handles currently cached on the worker thread.
     */
    size_t cachedPageCount() const noexcept {
        return m_cachedPageCount.load(std::memory_order_relaxed);
    }

Q_SIGNALS:
    void tileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void tileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);
    void documentOpened(int64_t documentId, pdf::OpenResult result);
    void documentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline);
    void thumbnailReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer);
    void thumbnailFailed(int64_t documentId, int pageIndex);
    void printPageReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer);
    void printPageFailed(int64_t documentId, int pageIndex);
    void extractPagesReady(int64_t documentId, int firstPage, int lastPage, const QByteArray& pdfBytes);
    void extractPagesFailed(int64_t documentId, int firstPage, int lastPage);
    void redactDocumentReady(int64_t documentId, const pdf::ipc::RedactDocumentResponse& response);
    void redactDocumentFailed(int64_t documentId, const QString& errorMessage);
    void pageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text);
    void searchMatchesFound(int64_t documentId, int64_t searchId, int pageIndex, const std::vector<pdf::SearchMatch>& matches);
    void searchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages);
    void searchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated);
    /// Emitted when all queued requests, in-flight render, control tasks, and active searches have completed.
    void idle();

private:
    void workerLoop();
    IPage* getOrLoadWorkerPage(int pageIndex);
    bool checkAndNotifyIdleLocked();

    struct ControlTask {
        std::function<void()> execute;
    };

    struct ActiveSearch {
        int64_t documentId{0};
        int64_t searchId{0};
        QString query;
        bool matchCase{false};
        int totalPages{0};
        int startPage{0};
        int nextPageToSearch{0};
        int totalMatchesFound{0};
    };

    std::atomic<bool> m_running{true};
    std::atomic<int64_t> m_activeDocumentId{0};
    std::string m_activeFilePath;
    std::string m_activePassword;
    std::shared_ptr<const std::vector<uint8_t>> m_activeSnapshot;
    long long m_activeFileSize{-1};
    long long m_activeFileMtimeMs{0};
    int m_activeFd{-1};
    std::atomic<unsigned long long> m_maxPinnedBytes{512ull * 1024 * 1024};
    void installActiveSourceLocked(const std::string& filePath, const std::string& password,
                                   std::shared_ptr<const std::vector<uint8_t>> snapshot,
                                   long long fileSize, long long fileMtimeMs, int fd = -1);
    void clearActiveSourceLocked();
    std::atomic<uint64_t> m_currentGeneration{0};
    std::atomic<size_t> m_cachedPageCount{0};
    std::atomic<int> m_testRenderDelayMs{0};
    std::atomic<uint32_t> m_currentOp{0};
    std::atomic<int64_t> m_lastProgressEpochMs{0};
#ifdef PDF_WORKER_TEST_HOOKS
    std::atomic<bool> m_testHangEngine{false};
    std::atomic<bool> m_testHangParked{false};
#endif
    std::atomic<int> m_testOpenDelayMs{0};
    std::atomic<int64_t> m_currentSearchId{0};

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    bool m_isRendering{false};
    bool m_isControlRunning{false};
    bool m_isOutlineRunning{false};
    bool m_isSearchRunning{false};
    TileKey m_inFlightKey;

    std::vector<ControlTask> m_controlQueue;
    std::vector<TileRenderRequest> m_queue;
    std::vector<int64_t> m_outlineQueue;
    ActiveSearch m_activeSearch;

    std::unique_ptr<IDocument> m_workerDocument;

    static constexpr size_t MAX_WORKER_PAGE_CACHE = 64;
    std::list<int> m_workerPageLru;
    std::unordered_map<int, std::pair<std::unique_ptr<IPage>, std::list<int>::iterator>> m_workerPageCache;

    std::thread m_thread;
};

} // namespace pdf
