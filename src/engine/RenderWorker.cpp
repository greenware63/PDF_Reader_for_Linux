#include "engine/RenderWorker.h"
#include <algorithm>

namespace pdf {

RenderWorker::RenderWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<pdf::TileKey>("pdf::TileKey");
    qRegisterMetaType<pdf::PixelBuffer>("pdf::PixelBuffer");
    qRegisterMetaType<pdf::OpenResult>("pdf::OpenResult");
    qRegisterMetaType<std::shared_ptr<pdf::PageText>>("std::shared_ptr<pdf::PageText>");
    qRegisterMetaType<pdf::SearchMatch>("pdf::SearchMatch");
    qRegisterMetaType<std::vector<pdf::SearchMatch>>("std::vector<pdf::SearchMatch>");

    m_thread = std::thread(&RenderWorker::workerLoop, this);
}

RenderWorker::~RenderWorker() {
    stop();
}

void RenderWorker::stop() {
    cancelSearch();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return;
        }
        m_running.store(false);
        m_queue.clear();
        m_controlQueue.clear();
        m_activeSearch.searchId = 0;
        m_cv.notify_all();
        m_idleCv.notify_all();
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

OpenResult RenderWorker::openDocumentSync(int64_t documentId, const std::string& filePath, const std::string& password) {
    cancelSearch();
    auto promise = std::make_shared<std::promise<OpenResult>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return {DocumentError::Unknown, 0, {}};
        }

        // Cancel pending render requests from older epochs
        m_queue.clear();

        // Atomically switch target document ID on the control queue before the open completes
        // BUT remember old document ID in case open fails so we can restore it!
        const int64_t oldDocId = m_activeDocumentId.load();
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

void RenderWorker::openDocumentAsync(int64_t documentId, const std::string& filePath, const std::string& password) {
    cancelSearch();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            OpenResult failed;
            failed.error = DocumentError::Unknown;
            Q_EMIT documentOpened(documentId, failed);
            return;
        }

        m_controlQueue.push_back({[this, documentId, filePath, password]() {
            if (const int delayMs = m_testOpenDelayMs.load(std::memory_order_relaxed); delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            OpenResult result;
            auto newDoc = createDocument();
            auto err = newDoc->loadFromFile(filePath, password);
            if (err != DocumentError::None) {
                // Load failed: leave existing document, its page cache, and queued tiles completely untouched
                result.error = err;
                Q_EMIT documentOpened(documentId, result);
                return;
            }

            // Load succeeded: atomically switch active document and flush obsolete render requests
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_activeDocumentId.store(documentId);
                m_queue.clear();
            }

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
            Q_EMIT documentOpened(documentId, result);
        }});

        m_cv.notify_one();
    }
}

void RenderWorker::closeDocumentSync() {
    cancelSearch();
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return;
        }

        // Cancel any pending render requests
        m_queue.clear();
        m_activeDocumentId.store(0);

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

    // Prune requests from previous generations
    m_queue.erase(
        std::remove_if(m_queue.begin(), m_queue.end(), [generation](const TileRenderRequest& req) {
            return req.generation < generation;
        }),
        m_queue.end()
    );

    if (m_queue.empty() && m_controlQueue.empty() && !m_isRendering && !m_isControlRunning &&
        (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages)) {
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

    // Deduplicate against the tile currently rendering on the worker
    if (m_isRendering && m_inFlightKey == request.key) {
        return;
    }

    // Deduplicate against tiles already waiting in queue
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

void RenderWorker::requestPageText(int64_t documentId, int pageIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load()) {
        return;
    }

    m_controlQueue.push_back({[this, documentId, pageIndex]() {
        if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
            return;
        }
        IPage* page = getOrLoadWorkerPage(pageIndex);
        // Always answer, even for a page that fails to load (empty text), so the UI never waits forever
        auto pageText = page ? std::make_shared<PageText>(page->extractText()) : std::make_shared<PageText>();
        pageText->pageIndex = pageIndex;
        Q_EMIT pageTextReady(documentId, pageIndex, pageText);
    }});
    m_cv.notify_one();
}

std::shared_ptr<const PageText> RenderWorker::getPageTextSync(int64_t documentId, int pageIndex) {
    auto promise = std::make_shared<std::promise<std::shared_ptr<const PageText>>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load() || documentId != m_activeDocumentId.load()) {
            return nullptr;
        }

        m_controlQueue.push_back({[this, documentId, pageIndex, promise]() {
            if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
                promise->set_value(nullptr);
                return;
            }
            IPage* page = getOrLoadWorkerPage(pageIndex);
            if (page) {
                promise->set_value(std::make_shared<const PageText>(page->extractText()));
            } else {
                promise->set_value(nullptr);
            }
        }});
        m_cv.notify_one();
    }

    return future.get();
}

void RenderWorker::startSearch(int64_t documentId, int64_t searchId, const QString& query, bool matchCase, int totalPages) {
    m_currentSearchId.store(searchId);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load() || query.isEmpty() || totalPages <= 0) {
        m_activeSearch.searchId = 0;
        return;
    }

    m_activeSearch.documentId = documentId;
    m_activeSearch.searchId = searchId;
    m_activeSearch.query = query;
    m_activeSearch.matchCase = matchCase;
    m_activeSearch.totalPages = totalPages;
    m_activeSearch.nextPageToSearch = 0;
    m_activeSearch.totalMatchesFound = 0;

    m_cv.notify_one();
}

void RenderWorker::cancelSearch() {
    m_currentSearchId.fetch_add(1);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_activeSearch.searchId = 0;
    if (m_queue.empty() && m_controlQueue.empty() && !m_isRendering && !m_isControlRunning) {
        m_idleCv.notify_all();
    }
}

void RenderWorker::cancelAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    if (m_controlQueue.empty() && !m_isRendering && !m_isControlRunning &&
        (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages)) {
        m_idleCv.notify_all();
    }
}

void RenderWorker::waitForIdle() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idleCv.wait(lock, [&]() {
        return m_queue.empty() && m_controlQueue.empty() && !m_isRendering && !m_isControlRunning &&
               (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages);
    });
}

size_t RenderWorker::pendingCount() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

IPage* RenderWorker::getOrLoadWorkerPage(int pageIndex) {
    if (!m_workerDocument) {
        return nullptr;
    }
    auto it = m_workerPageCache.find(pageIndex);
    if (it != m_workerPageCache.end()) {
        m_workerPageLru.splice(m_workerPageLru.begin(), m_workerPageLru, it->second.second);
        return it->second.first.get();
    }
    auto newPage = m_workerDocument->getPage(pageIndex);
    if (!newPage) {
        return nullptr;
    }
    if (m_workerPageCache.size() >= MAX_WORKER_PAGE_CACHE && !m_workerPageLru.empty()) {
        int lruPage = m_workerPageLru.back();
        m_workerPageLru.pop_back();
        m_workerPageCache.erase(lruPage);
    }
    m_workerPageLru.push_front(pageIndex);
    auto [insIt, _] = m_workerPageCache.emplace(pageIndex, std::make_pair(std::move(newPage), m_workerPageLru.begin()));
    m_cachedPageCount.store(m_workerPageCache.size(), std::memory_order_relaxed);
    return insIt->second.first.get();
}

std::vector<SearchMatch> RenderWorker::findTextInPage(const PageText& pt, const QString& query, bool matchCase) {
    std::vector<SearchMatch> matches;
    if (query.isEmpty() || pt.chars.empty()) {
        return matches;
    }

    QString pageTextStr;
    pageTextStr.reserve(static_cast<qsizetype>(pt.chars.size() * 2));
    std::vector<int> qCharToBoxIndex;
    qCharToBoxIndex.reserve(pt.chars.size() * 2);

    for (size_t i = 0; i < pt.chars.size(); ++i) {
        char32_t cp = pt.chars[i].unicode;
        if (cp == 0) {
            continue;
        }
        if (cp <= 0xFFFF) {
            pageTextStr.append(QChar(static_cast<ushort>(cp)));
            qCharToBoxIndex.push_back(static_cast<int>(i));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            pageTextStr.append(QChar(static_cast<ushort>(0xD800 + ((cp >> 10) & 0x3FF))));
            qCharToBoxIndex.push_back(static_cast<int>(i));
            pageTextStr.append(QChar(static_cast<ushort>(0xDC00 + (cp & 0x3FF))));
            qCharToBoxIndex.push_back(static_cast<int>(i));
        }
    }

    if (pageTextStr.isEmpty() || qCharToBoxIndex.empty()) {
        return matches;
    }

    const Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    qsizetype pos = 0;

    while (pos < pageTextStr.size()) {
        qsizetype found = pageTextStr.indexOf(query, pos, cs);
        if (found < 0) {
            break;
        }

        qsizetype matchEndQ = found + query.size() - 1;
        if (matchEndQ < static_cast<qsizetype>(qCharToBoxIndex.size())) {
            int startBox = qCharToBoxIndex[static_cast<size_t>(found)];
            int endBox = qCharToBoxIndex[static_cast<size_t>(matchEndQ)];

            SearchMatch m;
            m.pageIndex = pt.pageIndex;
            m.charStartIndex = startBox;
            m.charCount = endBox - startBox + 1;
            m.rects = pt.getSelectionRects(startBox, endBox);
            matches.push_back(std::move(m));
        }

        pos = found + std::max<qsizetype>(1, query.size());
    }

    return matches;
}

void RenderWorker::workerLoop() {
    int tilesRenderedSinceSearch = 0;

    while (m_running.load()) {
        ControlTask ctrlTask;
        bool hasControlTask = false;
        TileRenderRequest req;
        bool hasRenderRequest = false;

        bool hasSearchTask = false;
        int searchPageIndex = -1;
        int64_t searchDocId = 0;
        int64_t searchId = 0;
        QString searchQuery;
        bool searchMatchCase = false;
        int searchTotalPages = 0;
        bool isLastSearchPage = false;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&]() {
                if (!m_running.load()) return true;
                if (!m_controlQueue.empty()) return true;
                if (!m_queue.empty()) return true;
                if (m_activeSearch.searchId != 0 && m_activeSearch.nextPageToSearch < m_activeSearch.totalPages) return true;
                return false;
            });

            if (!m_running.load()) {
                break;
            }

            const bool hasSearch = (m_activeSearch.searchId != 0 &&
                                    m_activeSearch.nextPageToSearch < m_activeSearch.totalPages &&
                                    m_activeSearch.searchId == m_currentSearchId.load());

            if (!m_controlQueue.empty()) {
                ctrlTask = std::move(m_controlQueue.front());
                m_controlQueue.erase(m_controlQueue.begin());
                m_isControlRunning = true;
                hasControlTask = true;
            } else if (!m_queue.empty() && (!hasSearch || tilesRenderedSinceSearch < 4)) {
                req = std::move(m_queue.front());
                m_queue.erase(m_queue.begin());

                if (req.documentId != m_activeDocumentId.load() || req.generation < m_currentGeneration.load()) {
                    if (m_queue.empty() && m_controlQueue.empty() && !hasSearch) {
                        m_idleCv.notify_all();
                    }
                    continue;
                }

                m_isRendering = true;
                m_inFlightKey = req.key;
                hasRenderRequest = true;
                tilesRenderedSinceSearch++;
            } else if (hasSearch) {
                hasSearchTask = true;
                searchPageIndex = m_activeSearch.nextPageToSearch++;
                searchDocId = m_activeSearch.documentId;
                searchId = m_activeSearch.searchId;
                searchQuery = m_activeSearch.query;
                searchMatchCase = m_activeSearch.matchCase;
                searchTotalPages = m_activeSearch.totalPages;
                isLastSearchPage = (m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages);
                tilesRenderedSinceSearch = 0;
            }
        }

        if (hasControlTask) {
            ctrlTask.execute();
            std::lock_guard<std::mutex> lock(m_mutex);
            m_isControlRunning = false;
            if (m_queue.empty() && m_controlQueue.empty() && !m_isRendering &&
                (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages)) {
                m_idleCv.notify_all();
            }
            continue;
        }

        if (hasRenderRequest) {
            PixelBuffer buffer;
            bool renderedSuccessfully = false;

            if (m_workerDocument && m_running.load()) {
                IPage* page = getOrLoadWorkerPage(req.pageIndex);
                if (page) {
                    buffer = page->renderTile(req.rect, req.scale, req.dpr);
                    renderedSuccessfully = buffer.isValid() && (buffer.data() != nullptr);
                }
            }

            // Test hook: keep the tile in flight for a while to make timing-dependent behaviour observable
            if (const int delayMs = m_testRenderDelayMs.load(std::memory_order_relaxed); delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            // Deliver completed render whenever documentId matches active document.
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
                if (m_queue.empty() && m_controlQueue.empty() &&
                    (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages)) {
                    m_idleCv.notify_all();
                }
            }
            continue;
        }

        if (hasSearchTask) {
            if (!m_running.load() || searchId != m_currentSearchId.load() || searchDocId != m_activeDocumentId.load()) {
                continue;
            }

            std::vector<SearchMatch> pageMatches;
            if (m_workerDocument && m_running.load()) {
                IPage* page = getOrLoadWorkerPage(searchPageIndex);
                if (page) {
                    PageText pt = page->extractText();
                    pt.pageIndex = searchPageIndex;
                    pageMatches = findTextInPage(pt, searchQuery, searchMatchCase);
                }
            }

            // Deliver matches if search is still current
            if (m_running.load() && searchId == m_currentSearchId.load() && searchDocId == m_activeDocumentId.load()) {
                int totalSoFar = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_activeSearch.searchId == searchId) {
                        m_activeSearch.totalMatchesFound += static_cast<int>(pageMatches.size());
                        totalSoFar = m_activeSearch.totalMatchesFound;
                    }
                }

                if (!pageMatches.empty()) {
                    Q_EMIT searchMatchesFound(searchDocId, searchId, searchPageIndex, pageMatches);
                }
                Q_EMIT searchProgress(searchDocId, searchId, searchPageIndex + 1, searchTotalPages);

                if (isLastSearchPage) {
                    Q_EMIT searchCompleted(searchDocId, searchId, totalSoFar);
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_activeSearch.searchId == searchId) {
                        m_activeSearch.searchId = 0;
                    }
                    if (m_queue.empty() && m_controlQueue.empty() && !m_isRendering) {
                        m_idleCv.notify_all();
                    }
                }
            }
            continue;
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
