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

namespace pdf {

struct TileRenderRequest {
    int64_t documentId{0};
    uint64_t generation{0};
    int pageIndex{0};
    PixelRect rect;
    double scale{1.0};
    double dpr{1.0};
    TileKey key;
    int priority{0}; // Higher value = rendered first
};

struct OpenResult {
    DocumentError error{DocumentError::None};
    int pageCount{0};
    std::vector<std::pair<double, double>> pageSizes;
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
     */
    OpenResult openDocumentSync(int64_t documentId, const std::string& filePath, const std::string& password = "");

    /**
     * @brief Opens a PDF file on the worker thread WITHOUT blocking the caller.
     * The result (including page count and all page sizes) is delivered via the documentOpened() signal,
     * which fires exactly once per call. On failure the previously active document and its queued
     * tile requests are left completely untouched. On success the active document id switches to
     * `documentId` and queued requests for the old document are discarded.
     * Connect with Qt::QueuedConnection from the UI thread.
     */
    void openDocumentAsync(int64_t documentId, const std::string& filePath, const std::string& password = "");

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
     * @brief Sets current request generation. Discards queued requests with older generations.
     */
    void setGeneration(uint64_t generation);

    /**
     * @brief Enqueues a single tile render request.
     */
    void enqueueRequest(TileRenderRequest request);

    /**
     * @brief Enqueues multiple tile render requests, maintaining priority order.
     */
    void enqueueRequests(std::vector<TileRenderRequest> requests);

    /**
     * @brief Requests extraction of text and character bounding boxes for a page asynchronously.
     * Result is emitted via pageTextReady().
     */
    void requestPageText(int64_t documentId, int pageIndex);

    /**
     * @brief Synchronously extracts text for a page on the worker thread (blocks caller).
     */
    std::shared_ptr<const PageText> getPageTextSync(int64_t documentId, int pageIndex);

    /**
     * @brief Starts an asynchronous page-by-page background text search.
     * Cancels any ongoing search. Results arrive incrementally via searchMatchesFound() and searchProgress().
     */
    void startSearch(int64_t documentId, int64_t searchId, const QString& query, bool matchCase, int totalPages, int startPage = 0);

    /**
     * @brief Immediately cancels any running or pending background search.
     */
    void cancelSearch();

    /**
     * @brief Cancels all pending queued requests.
     */
    void cancelAll();

    /**
     * @brief Blocks until all queued requests, control tasks, in-flight render, and active searches complete.
     */
    void waitForIdle();

    /**
     * @brief Shuts down the background worker thread cleanly.
     */
    void stop();

    /**
     * @return Number of pending requests currently in queue.
     */
    size_t pendingCount();

    /**
     * @brief Test hook: artificially delays every completed render by this many milliseconds
     * (the worker keeps the tile "in flight" during the delay). 0 disables. Never use in production.
     */
    void setRenderDelayForTesting(std::chrono::milliseconds delay) noexcept {
        m_testRenderDelayMs.store(static_cast<int>(delay.count()), std::memory_order_relaxed);
    }

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
    /// Emitted exactly once per openDocumentAsync() call, from the worker thread.
    void documentOpened(int64_t documentId, pdf::OpenResult result);
    /// Emitted when the document outline (bookmarks) has been extracted.
    void documentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline);
    /// Emitted when text extraction for a page completes on the worker thread.
    void pageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text);
    /// Emitted when search finds matches on a page.
    void searchMatchesFound(int64_t documentId, int64_t searchId, int pageIndex, const std::vector<pdf::SearchMatch>& matches);
    /// Emitted as search progresses page-by-page.
    void searchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages);
    /// Emitted when a search across all pages completes.
    void searchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated);

private:
    void workerLoop();
    IPage* getOrLoadWorkerPage(int pageIndex);

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
    std::atomic<uint64_t> m_currentGeneration{0};
    std::atomic<size_t> m_cachedPageCount{0};
    std::atomic<int> m_testRenderDelayMs{0};
    std::atomic<int> m_testOpenDelayMs{0};
    std::atomic<int64_t> m_currentSearchId{0};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    bool m_isRendering{false};
    // True while a control task (open/close) executes on the worker; included in the idle condition
    // so waitForIdle() also waits for asynchronous document opens. Guarded by m_mutex.
    bool m_isControlRunning{false};
    bool m_isOutlineRunning{false};
    // Key of the tile currently being rendered (valid while m_isRendering); guarded by m_mutex.
    // enqueueRequest(s) deduplicates against it so a scroll that re-requests the in-flight tile is a no-op.
    TileKey m_inFlightKey;

    std::vector<ControlTask> m_controlQueue;
    std::vector<TileRenderRequest> m_queue;
    std::vector<int64_t> m_outlineQueue;
    ActiveSearch m_activeSearch;

    // Document and open page handles are accessed EXCLUSIVELY on the worker thread inside workerLoop()
    std::unique_ptr<IDocument> m_workerDocument;

    // Bounded LRU page cache on worker thread
    static constexpr size_t MAX_WORKER_PAGE_CACHE = 64;
    std::list<int> m_workerPageLru; // front = MRU, back = LRU
    std::unordered_map<int, std::pair<std::unique_ptr<IPage>, std::list<int>::iterator>> m_workerPageCache;

    std::thread m_thread;
};

} // namespace pdf
