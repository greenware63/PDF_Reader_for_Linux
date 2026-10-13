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
        m_cv.notify_all();
        m_idleCv.notify_all();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void RenderWorker::setDocument(int64_t documentId, std::shared_ptr<IDocument> document) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeDocumentId.store(documentId);
    m_document = std::move(document);
    m_workerPageCache.clear();
    m_queue.clear();
    m_cv.notify_all();
    m_idleCv.notify_all();
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
    if (m_queue.empty() && !m_isRendering) {
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
        bool dup = false;
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
    m_idleCv.notify_all();
}

void RenderWorker::waitForIdle() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idleCv.wait(lock, [&]() {
        return m_queue.empty() && !m_isRendering;
    });
}

size_t RenderWorker::pendingCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void RenderWorker::workerLoop() {
    while (m_running.load()) {
        TileRenderRequest req;
        std::shared_ptr<IDocument> doc;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&]() {
                return !m_running.load() || !m_queue.empty();
            });

            if (!m_running.load()) {
                break;
            }

            req = std::move(m_queue.front());
            m_queue.erase(m_queue.begin());

            // Check if stale before starting render
            if (req.documentId != m_activeDocumentId.load() || req.generation < m_currentGeneration.load()) {
                if (m_queue.empty()) {
                    m_idleCv.notify_all();
                }
                continue;
            }

            m_isRendering = true;
            doc = m_document;
        }

        PixelBuffer buffer;
        bool renderedSuccessfully = false;

        if (doc && m_running.load()) {
            IPage* page = nullptr;
            auto it = m_workerPageCache.find(req.pageIndex);
            if (it != m_workerPageCache.end()) {
                page = it->second.get();
            } else {
                auto newPage = doc->getPage(req.pageIndex);
                if (newPage) {
                    page = newPage.get();
                    m_workerPageCache.emplace(req.pageIndex, std::move(newPage));
                }
            }

            if (page) {
                buffer = page->renderTile(req.rect, req.scale, req.dpr);
                renderedSuccessfully = buffer.isValid() && (buffer.data() != nullptr);
            }
        }

        // Before emitting result, check if request became stale during rendering
        if (m_running.load() &&
            req.documentId == m_activeDocumentId.load() &&
            req.generation >= m_currentGeneration.load())
        {
            if (renderedSuccessfully) {
                Q_EMIT tileReady(req.documentId, req.generation, req.key, buffer);
            } else {
                Q_EMIT tileFailed(req.documentId, req.generation, req.key);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_isRendering = false;
            if (m_queue.empty()) {
                m_idleCv.notify_all();
            }
        }
    }

    // Clean up worker cache before thread termination
    m_workerPageCache.clear();
    m_document.reset();
}

} // namespace pdf
