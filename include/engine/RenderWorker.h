#pragma once

#include <QObject>
#include <QMetaType>
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
 * Document loading, metadata queries, page creation, tile rendering, and resource teardown
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
     * @brief Closes the active document and clears all open page handles serially on the worker thread.
     */
    void closeDocumentSync();

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
     * @brief Cancels all pending queued requests.
     */
    void cancelAll();

    /**
     * @brief Blocks until all queued requests, control tasks, and in-flight render complete.
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
     * @return Number of open IPage handles currently cached on the worker thread.
     */
    size_t cachedPageCount() const noexcept {
        return m_cachedPageCount.load(std::memory_order_relaxed);
    }

Q_SIGNALS:
    void tileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void tileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);

private:
    void workerLoop();

    struct ControlTask {
        std::function<void()> execute;
    };

    std::atomic<bool> m_running{true};
    std::atomic<int64_t> m_activeDocumentId{0};
    std::atomic<uint64_t> m_currentGeneration{0};
    std::atomic<size_t> m_cachedPageCount{0};
    std::atomic<int> m_testRenderDelayMs{0};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    bool m_isRendering{false};
    // Key of the tile currently being rendered (valid while m_isRendering); guarded by m_mutex.
    // enqueueRequest(s) deduplicates against it so a scroll that re-requests the in-flight tile is a no-op.
    TileKey m_inFlightKey;

    std::vector<ControlTask> m_controlQueue;
    std::vector<TileRenderRequest> m_queue;

    // Document and open page handles are accessed EXCLUSIVELY on the worker thread inside workerLoop()
    std::unique_ptr<IDocument> m_workerDocument;

    // Bounded LRU page cache on worker thread
    static constexpr size_t MAX_WORKER_PAGE_CACHE = 64;
    std::list<int> m_workerPageLru; // front = MRU, back = LRU
    std::unordered_map<int, std::pair<std::unique_ptr<IPage>, std::list<int>::iterator>> m_workerPageCache;

    std::thread m_thread;
};

} // namespace pdf

Q_DECLARE_METATYPE(pdf::TileKey)
Q_DECLARE_METATYPE(pdf::PixelBuffer)
