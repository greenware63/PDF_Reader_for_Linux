#include "engine/RenderWorker.h"
#include <algorithm>

namespace pdf {

RenderWorker::RenderWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<pdf::TileKey>("pdf::TileKey");
    qRegisterMetaType<pdf::PixelBuffer>("pdf::PixelBuffer");

    m_thread = std::thread(&RenderWorker::workerLoop, this);
}

RenderWorker::~RenderWorker() {
    stop();
}

void RenderWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return;
        }
        m_running.store(false);
        m_queue.clear();
        m_controlQueue.clear();
        m_cv.notify_all();
        m_idleCv.notify_all();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

OpenResult RenderWorker::openDocumentSync(int64_t documentId, const std::string& filePath, const std::string& password) {
    auto promise = std::make_shared<std::promise<OpenResult>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return {DocumentError::Unknown, 0, {}};
        }

        int64_t oldDocId = m_activeDocumentId.load();
        auto savedQueue = std::move(m_queue);
        m_activeDocumentId.store(documentId);

        m_controlQueue.push_back({[this, documentId, oldDocId, savedQueue = std::move(savedQueue), filePath, password, promise]() mutable {
            OpenResult result;
            auto newDoc = createDocument();
            auto err = newDoc->loadFromFile(filePath, password);
            if (err != DocumentError::None) {
                // If opening failed, restore previous document ID and queued requests
                m_activeDocumentId.store(oldDocId);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_queue.insert(m_queue.begin(),
                                   std::make_move_iterator(savedQueue.begin()),
                                   std::make_move_iterator(savedQueue.end()));
                }
                result.error = err;
                promise->set_value(std::move(result));
                return;
            }

            // Success: safely replace active document and active ID, and purge worker page cache on worker thread
            m_activeDocumentId.store(documentId);
            m_workerPageCache.clear();
            m_workerPageLru.clear();
            m_cachedPageCount.store(0, std::memory_order_relaxed);
            if (m_workerDocument) {
                m_workerDocument->close();
            }
            m_workerDocument = std::move(newDoc);

            result.error = DocumentError::None;
            result.pageCount = m_workerDocument->pageCount();
            result.pageSizes.reserve(result.pageCount);
            for (int i = 0; i < result.pageCount; ++i) {
                result.pageSizes.push_back(m_workerDocument->pageSizePoints(i));
            }
            promise->set_value(std::move(result));
        }});

        m_cv.notify_one();
    }

    return future.get();
}

void RenderWorker::closeDocumentSync() {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return;
        }

        m_activeDocumentId.store(0);
        m_queue.clear();

        m_controlQueue.push_back({[this, promise]() {
            m_workerPageCache.clear();
            m_workerPageLru.clear();
            m_cachedPageCount.store(0, std::memory_order_relaxed);
            if (m_workerDocument) {
                m_workerDocument->close();
                m_workerDocument.reset();
            }
            promise->set_value();
        }});

        m_cv.notify_one();
    }

    future.get();
}

void RenderWorker::setGeneration(uint64_t generation) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentGeneration.store(generation);
    m_queue.erase(
        std::remove_if(m_queue.begin(), m_queue.end(), [generation](const TileRenderRequest& req) {
            return req.generation < generation;
        }),
        m_queue.end()
    );
    if (m_queue.empty() && m_controlQueue.empty() && !m_isRendering) {
        m_idleCv.notify_all();
    }
}

void RenderWorker::enqueueRequest(TileRenderRequest request) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load()) {
        return;
    }
    if (request.documentId != m_activeDocumentId.load() || request.generation < m_currentGeneration.load()) {
        return;
    }

    // Deduplicate against the tile being rendered right now (its result is delivered regardless of generation)
    if (m_isRendering && m_inFlightKey == request.key) {
        return;
    }

    // Deduplicate against already pending identical requests
    for (const auto& item : m_queue) {
        if (item.key == request.key && item.generation == request.generation) {
            return;
        }
    }

    m_queue.push_back(std::move(request));
    std::stable_sort(m_queue.begin(), m_queue.end(), [](const TileRenderRequest& a, const TileRenderRequest& b) {
        return a.priority > b.priority;
    });

    m_cv.notify_one();
}

void RenderWorker::enqueueRequests(std::vector<TileRenderRequest> requests) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || requests.empty()) {
        return;
    }
    const int64_t curDoc = m_activeDocumentId.load();
    const uint64_t curGen = m_currentGeneration.load();

    for (auto& req : requests) {
        if (req.documentId != curDoc || req.generation < curGen) {
            continue;
        }
        bool dup = (m_isRendering && m_inFlightKey == req.key);
        for (const auto& item : m_queue) {
            if (item.key == req.key && item.generation == req.generation) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            m_queue.push_back(std::move(req));
        }
    }

    std::stable_sort(m_queue.begin(), m_queue.end(), [](const TileRenderRequest& a, const TileRenderRequest& b) {
        return a.priority > b.priority;
    });

    m_cv.notify_one();
}

void RenderWorker::cancelAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    if (m_controlQueue.empty() && !m_isRendering) {
        m_idleCv.notify_all();
    }
}

void RenderWorker::waitForIdle() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idleCv.wait(lock, [&]() {
        return m_queue.empty() && m_controlQueue.empty() && !m_isRendering;
    });
}

size_t RenderWorker::pendingCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void RenderWorker::workerLoop() {
    while (m_running.load()) {
        ControlTask ctrlTask;
        bool hasControlTask = false;
        TileRenderRequest req;
        bool hasRenderRequest = false;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&]() {
                return !m_running.load() || !m_controlQueue.empty() || !m_queue.empty();
            });

            if (!m_running.load()) {
                break;
            }

            // Prioritize control tasks (open/close) over tile rendering
            if (!m_controlQueue.empty()) {
                ctrlTask = std::move(m_controlQueue.front());
                m_controlQueue.erase(m_controlQueue.begin());
                hasControlTask = true;
            } else if (!m_queue.empty()) {
                req = std::move(m_queue.front());
                m_queue.erase(m_queue.begin());

                // Check if tile request is stale (e.g. cancelled by generation or obsolete docId)
                if (req.documentId != m_activeDocumentId.load() || req.generation < m_currentGeneration.load()) {
                    if (m_queue.empty() && m_controlQueue.empty()) {
                        m_idleCv.notify_all();
                    }
                    continue;
                }

                m_isRendering = true;
                m_inFlightKey = req.key;
                hasRenderRequest = true;
            }
        }

        if (hasControlTask) {
            ctrlTask.execute();
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.empty() && m_controlQueue.empty() && !m_isRendering) {
                m_idleCv.notify_all();
            }
            continue;
        }

        if (hasRenderRequest) {
            PixelBuffer buffer;
            bool renderedSuccessfully = false;

            if (m_workerDocument && m_running.load()) {
                IPage* page = nullptr;
                auto it = m_workerPageCache.find(req.pageIndex);
                if (it != m_workerPageCache.end()) {
                    // Cache hit: promote to Most Recently Used
                    m_workerPageLru.splice(m_workerPageLru.begin(), m_workerPageLru, it->second.second);
                    page = it->second.first.get();
                } else {
                    // Cache miss: load page and enforce LRU bound
                    auto newPage = m_workerDocument->getPage(req.pageIndex);
                    if (newPage) {
                        if (m_workerPageCache.size() >= MAX_WORKER_PAGE_CACHE && !m_workerPageLru.empty()) {
                            int lruPage = m_workerPageLru.back();
                            m_workerPageLru.pop_back();
                            m_workerPageCache.erase(lruPage);
                        }
                        m_workerPageLru.push_front(req.pageIndex);
                        page = newPage.get();
                        m_workerPageCache.emplace(req.pageIndex, std::make_pair(std::move(newPage), m_workerPageLru.begin()));
                        m_cachedPageCount.store(m_workerPageCache.size(), std::memory_order_relaxed);
                    }
                }

                if (page) {
                    buffer = page->renderTile(req.rect, req.scale, req.dpr);
                    renderedSuccessfully = buffer.isValid() && (buffer.data() != nullptr);
                }
            }

            // Test hook: keep the tile in flight for a while to make timing-dependent behaviour observable
            if (const int delayMs = m_testRenderDelayMs.load(std::memory_order_relaxed); delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            // Fix 1: Deliver completed render whenever documentId matches active document.
            // Generation is strictly for pruning queued work, not for discarding completed tiles!
            if (m_running.load() && req.documentId == m_activeDocumentId.load()) {
                if (renderedSuccessfully) {
                    Q_EMIT tileReady(req.documentId, req.generation, req.key, buffer);
                } else {
                    Q_EMIT tileFailed(req.documentId, req.generation, req.key);
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_isRendering = false;
                if (m_queue.empty() && m_controlQueue.empty()) {
                    m_idleCv.notify_all();
                }
            }
        }
    }

    // Clean up worker cache and document on the worker thread before exit
    m_workerPageCache.clear();
    m_workerPageLru.clear();
    m_cachedPageCount.store(0, std::memory_order_relaxed);
    if (m_workerDocument) {
        m_workerDocument->close();
        m_workerDocument.reset();
    }
}

} // namespace pdf
