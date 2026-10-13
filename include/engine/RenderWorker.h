#pragma once

#include <QObject>
#include <QMetaType>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <memory>
#include <unordered_map>
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

/**
 * @brief Dedicated single-threaded engine worker for serializing all PDFium calls off the UI thread.
 * Queues tile requests by priority, discards stale work using document and request generations,
 * delivers results back via Qt signals, and shuts down cleanly.
 */
class RenderWorker : public QObject {
    Q_OBJECT

public:
    explicit RenderWorker(QObject* parent = nullptr);
    ~RenderWorker() override;

    RenderWorker(const RenderWorker&) = delete;
    RenderWorker& operator=(const RenderWorker&) = delete;

    /**
     * @brief Sets or swaps the active document handle on the worker.
     * All pending requests for previous documents are discarded immediately.
     */
    void setDocument(int64_t documentId, std::shared_ptr<IDocument> document);

    /**
     * @brief Sets current request generation. Stale requests with older generations are dropped.
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
     * @brief Blocks until all queued requests and current in-flight render complete.
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

Q_SIGNALS:
    void tileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void tileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);

private:
    void workerLoop();

    std::atomic<bool> m_running{true};
    std::atomic<int64_t> m_activeDocumentId{0};
    std::atomic<uint64_t> m_currentGeneration{0};

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_idleCv;
    bool m_isRendering{false};

    std::shared_ptr<IDocument> m_document;
    std::vector<TileRenderRequest> m_queue;

    std::unordered_map<int, std::unique_ptr<IPage>> m_workerPageCache;

    std::thread m_thread;
};

} // namespace pdf

Q_DECLARE_METATYPE(pdf::TileKey)
Q_DECLARE_METATYPE(pdf::PixelBuffer)
