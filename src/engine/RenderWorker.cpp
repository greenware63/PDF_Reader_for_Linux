#include "engine/RenderWorker.h"
#include "engine/SearchText.h"
#include "redact/Redaction.h"
#include "ipc/IpcMessages.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QRandomGenerator>
#include <algorithm>
#include <cerrno>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

namespace pdf {

namespace {

// Best-effort wipe of a secret held in a std::string (volatile writes so the stores are not elided).
void secureWipe(std::string& s) {
    if (!s.empty()) {
        volatile char* p = s.data();
        for (std::size_t i = 0; i < s.size(); ++i) {
            p[i] = 0;
        }
    }
    s.clear();
}

struct LoadedSource {
    std::unique_ptr<IDocument> doc;
    std::shared_ptr<const std::vector<uint8_t>> snapshot; // set when the bytes were pinned in memory
    long long fileSize{-1};      // unpinned only: identity of the file at open time
    long long fileMtimeMs{0};
    int fd{-1};                  // unpinned descriptor for pread streaming / redaction
    DocumentError error{DocumentError::Unknown};
};

LoadedSource loadSource(const std::string& filePath, const std::string& password, unsigned long long maxPinned, int fd = -1) {
    LoadedSource out;
    out.doc = createDocument();

    if (fd >= 0) {
        uint64_t size = 0;
        long long mtimeMs = 0;
#ifdef _WIN32
        struct _stat64 st{};
        if (::_fstat64(fd, &st) != 0 || (st.st_mode & _S_IFREG) == 0 || st.st_size < 4) {
            ::_close(fd);
            out.error = DocumentError::InvalidFormat;
            return out;
        }
        size = static_cast<uint64_t>(st.st_size);
        mtimeMs = static_cast<long long>(st.st_mtime) * 1000LL;
#else
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4) {
            ::close(fd);
            out.error = DocumentError::InvalidFormat;
            return out;
        }
        size = static_cast<uint64_t>(st.st_size);
        mtimeMs = static_cast<long long>(st.st_mtime) * 1000LL;
#endif

        if (size <= maxPinned) {
            auto buffer = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size));
            size_t totalRead = 0;
            while (totalRead < static_cast<size_t>(size)) {
#ifdef _WIN32
                int bytes = ::_read(fd, buffer->data() + totalRead, static_cast<unsigned int>(size - totalRead));
                if (bytes <= 0) break;
                totalRead += static_cast<size_t>(bytes);
#else
                ssize_t bytes = ::pread(fd, buffer->data() + totalRead, static_cast<size_t>(size) - totalRead, static_cast<off_t>(totalRead));
                if (bytes < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (bytes == 0) break;
                totalRead += static_cast<size_t>(bytes);
#endif
            }
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
            if (totalRead != static_cast<size_t>(size)) {
                out.error = DocumentError::InvalidFormat;
                return out;
            }
            out.snapshot = buffer;
            out.error = out.doc->loadFromBuffer(out.snapshot, password);
            return out;
        }

        // > maxPinned: Stream via pread.
        // For redaction to be able to read without opening by path under Landlock, dup the descriptor.
        int redactFd = -1;
#ifdef _WIN32
        redactFd = ::_dup(fd);
#else
        redactFd = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
#endif
        out.fileSize = static_cast<long long>(size);
        out.fileMtimeMs = mtimeMs;
        out.fd = redactFd;
        out.error = out.doc->loadFromDescriptor(fd, password, /*takeOwnership=*/true);
        if (out.error != DocumentError::None && redactFd >= 0) {
#ifdef _WIN32
            ::_close(redactFd);
#else
            ::close(redactFd);
#endif
            out.fd = -1;
        }
        return out;
    }

    const QFileInfo info(QString::fromStdString(filePath));
    const qint64 size = info.isFile() ? info.size() : -1;
    if (size > 0 && static_cast<unsigned long long>(size) <= maxPinned) {
        QFile file(info.filePath());
        if (file.open(QIODevice::ReadOnly)) {
            auto buffer = std::make_shared<std::vector<uint8_t>>(static_cast<std::size_t>(size));
            const qint64 got = file.read(reinterpret_cast<char*>(buffer->data()), size);
            if (got == size && file.size() == size) {
                out.snapshot = buffer;
                out.error = out.doc->loadFromBuffer(out.snapshot, password);
                return out;
            }
        }
    }

    // Not pinnable (too large, unreadable, or changing while read): open by path, and remember the file's
    // identity BEFORE loading so a later redaction can detect that it changed.
    out.fileSize = size;
    out.fileMtimeMs = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
    out.error = out.doc->loadFromFile(filePath, password);
    return out;
}

} // namespace

void RenderWorker::installActiveSourceLocked(const std::string& filePath, const std::string& password,
                                             std::shared_ptr<const std::vector<uint8_t>> snapshot,
                                             long long fileSize, long long fileMtimeMs, int fd) {
    if (m_activeFd >= 0) {
#ifdef _WIN32
        ::_close(m_activeFd);
#else
        ::close(m_activeFd);
#endif
        m_activeFd = -1;
    }
    secureWipe(m_activePassword);
    m_activeFilePath = filePath;
    m_activePassword = password;
    m_activeSnapshot = std::move(snapshot);
    m_activeFileSize = fileSize;
    m_activeFileMtimeMs = fileMtimeMs;
    m_activeFd = fd;
}

void RenderWorker::clearActiveSourceLocked() {
    if (m_activeFd >= 0) {
#ifdef _WIN32
        ::_close(m_activeFd);
#else
        ::close(m_activeFd);
#endif
        m_activeFd = -1;
    }
    secureWipe(m_activePassword);
    m_activeFilePath.clear();
    m_activeSnapshot.reset();
    m_activeFileSize = -1;
    m_activeFileMtimeMs = 0;
}

RenderWorker::RenderWorker(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<pdf::TileKey>("pdf::TileKey");
    qRegisterMetaType<pdf::PixelBuffer>("pdf::PixelBuffer");
    qRegisterMetaType<pdf::OpenResult>("pdf::OpenResult");
    qRegisterMetaType<std::shared_ptr<pdf::PageText>>("std::shared_ptr<pdf::PageText>");
    qRegisterMetaType<pdf::SearchMatch>("pdf::SearchMatch");
    qRegisterMetaType<std::vector<pdf::SearchMatch>>("std::vector<pdf::SearchMatch>");
    qRegisterMetaType<pdf::OutlineItem>("pdf::OutlineItem");
    qRegisterMetaType<std::vector<pdf::OutlineItem>>("std::vector<pdf::OutlineItem>");
    qRegisterMetaType<pdf::ipc::RedactDocumentResponse>("pdf::ipc::RedactDocumentResponse");

    m_thread = std::thread(&RenderWorker::workerLoop, this);
}

RenderWorker::~RenderWorker() {
    stop();
}

void RenderWorker::stop() {
    cancelSearch();
    bool shouldEmitIdle = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            return;
        }
        m_running.store(false);
        m_queue.clear();
        m_controlQueue.clear();
        clearActiveSourceLocked();
        m_activeSearch.searchId = 0;
        m_cv.notify_all();
        shouldEmitIdle = checkAndNotifyIdleLocked();
    }
    if (shouldEmitIdle) {
        Q_EMIT idle();
    }
    Q_EMIT idle();

    if (m_thread.joinable()) {
        m_thread.join();
    }
}

OpenResult RenderWorker::openDocumentSync(int64_t documentId, const std::string& filePath, const std::string& password, int fd) {
    cancelSearch();
    auto promise = std::make_shared<std::promise<OpenResult>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            if (fd >= 0) {
#ifdef _WIN32
                ::_close(fd);
#else
                ::close(fd);
#endif
            }
            return {DocumentError::Unknown, 0, {}, {}};
        }

        // Cancel pending render requests from older epochs
        m_queue.clear();

        // Atomically switch target document ID on the control queue before the open completes
        // BUT remember old document ID in case open fails so we can restore it!
        const int64_t oldDocId = m_activeDocumentId.load();
        auto savedQueue = std::move(m_queue);
        m_activeDocumentId.store(documentId);

        m_controlQueue.push_back({[this, documentId, oldDocId, savedQueue = std::move(savedQueue), filePath, password, fd, promise]() mutable {
            OpenResult result;
            auto loaded = loadSource(filePath, password, m_maxPinnedBytes.load(), fd);
            auto newDoc = std::move(loaded.doc);
            auto err = loaded.error;
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
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_activeDocumentId.store(documentId);
                installActiveSourceLocked(filePath, password, loaded.snapshot, loaded.fileSize, loaded.fileMtimeMs, loaded.fd);
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
            result.pageLabels = m_workerDocument->getPageLabels();
            promise->set_value(std::move(result));
        }});

        m_cv.notify_one();
    }

    return future.get();
}

void RenderWorker::openDocumentAsync(int64_t documentId, const std::string& filePath, const std::string& password, int fd) {
    cancelSearch();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load()) {
            if (fd >= 0) {
#ifdef _WIN32
                ::_close(fd);
#else
                ::close(fd);
#endif
            }
            OpenResult failed;
            failed.error = DocumentError::Unknown;
            Q_EMIT documentOpened(documentId, failed);
            return;
        }

        m_controlQueue.push_back({[this, documentId, filePath, password, fd]() {
            noteProgress(ipc::WorkerOp::Open);
            struct OpenGuard {
                RenderWorker* w;
                ~OpenGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
            } guard{this};
            if (const int delayMs = m_testOpenDelayMs.load(std::memory_order_relaxed); delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            OpenResult result;
            auto loaded = loadSource(filePath, password, m_maxPinnedBytes.load(), fd);
            auto newDoc = std::move(loaded.doc);
            auto err = loaded.error;
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
                installActiveSourceLocked(filePath, password, loaded.snapshot, loaded.fileSize, loaded.fileMtimeMs, loaded.fd);
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
            result.pageLabels = m_workerDocument->getPageLabels();
            Q_EMIT documentOpened(documentId, result);
        }});

        m_cv.notify_one();
    }
}

void RenderWorker::requestDocumentOutline(int64_t documentId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load() || m_activeDocumentId.load() != documentId) {
            return;
        }
        m_outlineQueue.push_back(documentId);
        m_cv.notify_one();
    }
}

void RenderWorker::requestThumbnail(int64_t documentId, int pageIndex, int targetWidth, int rotation) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load()) {
        Q_EMIT thumbnailFailed(documentId, pageIndex);
        return;
    }
    m_controlQueue.push_back({[this, documentId, pageIndex, targetWidth, rotation]() {
        noteProgress(ipc::WorkerOp::Thumbnail);
        struct ThumbGuard {
            RenderWorker* w;
            ~ThumbGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};
        if (documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT thumbnailFailed(documentId, pageIndex);
            return;
        }
        IPage* page = getOrLoadWorkerPage(pageIndex);
        if (!page) {
            Q_EMIT thumbnailFailed(documentId, pageIndex);
            return;
        }
        PixelBuffer buf = page->renderThumbnail(targetWidth, rotation);
        if (buf.isValid()) {
            Q_EMIT thumbnailReady(documentId, pageIndex, buf);
        } else {
            Q_EMIT thumbnailFailed(documentId, pageIndex);
        }
    }});
    m_cv.notify_one();
}

void RenderWorker::requestPrintPage(int64_t documentId, int pageIndex, double scale, double dpr, int rotation) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load()) {
        Q_EMIT printPageFailed(documentId, pageIndex);
        return;
    }
    m_controlQueue.push_back({[this, documentId, pageIndex, scale, dpr, rotation]() {
        noteProgress(ipc::WorkerOp::Print);
        struct PrintGuard {
            RenderWorker* w;
            ~PrintGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};
        if (documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT printPageFailed(documentId, pageIndex);
            return;
        }
        IPage* page = getOrLoadWorkerPage(pageIndex);
        if (!page) {
            Q_EMIT printPageFailed(documentId, pageIndex);
            return;
        }
        PixelBuffer buf = page->renderWholePage(scale, dpr, rotation);
        if (buf.isValid()) {
            Q_EMIT printPageReady(documentId, pageIndex, buf);
        } else {
            Q_EMIT printPageFailed(documentId, pageIndex);
        }
    }});
    m_cv.notify_one();
}

void RenderWorker::requestExtractPages(int64_t documentId, int firstPage, int lastPage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load()) {
        Q_EMIT extractPagesFailed(documentId, firstPage, lastPage);
        return;
    }
    m_controlQueue.push_back({[this, documentId, firstPage, lastPage]() {
        noteProgress(ipc::WorkerOp::Extract);
        struct ExtractGuard {
            RenderWorker* w;
            ~ExtractGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};
        if (documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT extractPagesFailed(documentId, firstPage, lastPage);
            return;
        }
        std::vector<uint8_t> bytes;
        if (!m_workerDocument->extractPages(firstPage, lastPage, bytes)) {
            Q_EMIT extractPagesFailed(documentId, firstPage, lastPage);
            return;
        }
        Q_EMIT extractPagesReady(documentId, firstPage, lastPage,
                                 QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                            static_cast<qsizetype>(bytes.size())));
    }});
    m_cv.notify_one();
}

void RenderWorker::requestRedactDocument(int64_t documentId,
                                         const std::vector<ipc::PageRedactionItem>& redactions,
                                         double dpi,
                                         int edgeGrowPx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load()) {
        Q_EMIT redactDocumentFailed(documentId, QStringLiteral("Worker is shutting down."));
        return;
    }
    m_controlQueue.push_back({[this, documentId, redactions, dpi, edgeGrowPx]() {
        noteProgress(ipc::WorkerOp::Redact);
        struct RedactGuard {
            RenderWorker* w;
            ~RedactGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};

        // Take a consistent copy of the active source under the lock (closeDocumentSync clears it from
        // another thread). Redaction must operate on exactly what was opened and is on screen.
        std::string sourcePath;
        std::string password;
        std::shared_ptr<const std::vector<uint8_t>> snapshot;
        long long openedSize = -1;
        long long openedMtimeMs = 0;
        int openedFd = -1;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (documentId != m_activeDocumentId.load() || (!m_activeSnapshot && m_activeFilePath.empty() && m_activeFd < 0)) {
                Q_EMIT redactDocumentFailed(documentId, QStringLiteral("Document is not open or active."));
                return;
            }
            sourcePath = m_activeFilePath;
            password = m_activePassword;
            snapshot = m_activeSnapshot;
            openedSize = m_activeFileSize;
            openedMtimeMs = m_activeFileMtimeMs;
            openedFd = m_activeFd;
        }
        struct PasswordWipe {
            std::string& s;
            ~PasswordWipe() { secureWipe(s); }
        } passwordWipe{password};

        QByteArray unpinnedBytes;
        const uint8_t* sourceData = nullptr;
        std::size_t sourceSize = 0;
        if (snapshot) {
            sourceData = snapshot->data();
            sourceSize = snapshot->size();
        } else if (openedFd >= 0) {
            // Read from dup'd file descriptor via pread loops without opening by path!
            unpinnedBytes.resize(static_cast<qsizetype>(openedSize));
            size_t totalRead = 0;
            while (totalRead < static_cast<size_t>(openedSize)) {
#ifdef _WIN32
                static std::mutex s_redactSeekMutex;
                std::lock_guard<std::mutex> lk(s_redactSeekMutex);
                if (::_lseeki64(openedFd, static_cast<__int64>(totalRead), SEEK_SET) == -1) break;
                int bytes = ::_read(openedFd, unpinnedBytes.data() + totalRead, static_cast<unsigned int>(openedSize - totalRead));
                if (bytes <= 0) break;
                totalRead += static_cast<size_t>(bytes);
#else
                ssize_t bytes = ::pread(openedFd, unpinnedBytes.data() + totalRead, static_cast<size_t>(openedSize) - totalRead, static_cast<off_t>(totalRead));
                if (bytes < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (bytes == 0) break;
                totalRead += static_cast<size_t>(bytes);
#endif
            }
            if (totalRead != static_cast<size_t>(openedSize)) {
                Q_EMIT redactDocumentFailed(documentId, QStringLiteral(
                    "The file changed on disk while it was being read. Reopen it before redacting."));
                return;
            }
            sourceData = reinterpret_cast<const uint8_t*>(unpinnedBytes.constData());
            sourceSize = static_cast<std::size_t>(unpinnedBytes.size());
        } else {
            // Large document opened by path: the bytes on disk are only trustworthy if the file is the
            // same one that was opened. Refuse rather than redact something other than what is displayed.
            const QFileInfo info(QString::fromStdString(sourcePath));
            if (!info.isFile() || info.size() != openedSize ||
                info.lastModified().toMSecsSinceEpoch() != openedMtimeMs) {
                Q_EMIT redactDocumentFailed(documentId, QStringLiteral(
                    "The file changed on disk after it was opened. Reopen it before redacting."));
                return;
            }
            QFile file(info.filePath());
            if (!file.open(QIODevice::ReadOnly)) {
                Q_EMIT redactDocumentFailed(documentId, QStringLiteral("Cannot read source PDF file: %1").arg(file.errorString()));
                return;
            }
            unpinnedBytes = file.readAll();
            file.close();
            if (unpinnedBytes.size() != openedSize) {
                Q_EMIT redactDocumentFailed(documentId, QStringLiteral(
                    "The file changed on disk while it was being read. Reopen it before redacting."));
                return;
            }
            sourceData = reinterpret_cast<const uint8_t*>(unpinnedBytes.constData());
            sourceSize = static_cast<std::size_t>(unpinnedBytes.size());
        }

        std::vector<pdf::redact::PageRedactions> engineRedactions;
        engineRedactions.reserve(redactions.size());
        for (const auto& pr : redactions) {
            pdf::redact::PageRedactions item;
            item.pageIndex = pr.pageIndex;
            item.rects.reserve(pr.rects.size());
            for (const auto& r : pr.rects) {
                item.rects.push_back(pdf::redact::Rect{r.left, r.top, r.right, r.bottom});
            }
            engineRedactions.push_back(std::move(item));
        }

        pdf::redact::Options opt;
        opt.dpi = std::clamp(dpi, 72.0, 300.0);
        opt.edgeGrowPx = std::clamp(edgeGrowPx, 0, 16);
        // Liveness heartbeat: the engine runs for as long as the document is big, and the watchdog
        // judges a worker by whether this stamp advances. Without it a long redaction would be killed
        // after the stall limit although it is making steady progress.
#ifdef PDF_WORKER_TEST_HOOKS
        // Test hook: stretch each completed redaction step by the configured render delay, so a test can
        // guarantee a redaction outlives the watchdog limits however fast the host is (0 disables).
        opt.onProgress = [this, lastDone = -1](int done, int) mutable {
            noteProgress(ipc::WorkerOp::Redact);
            if (done != lastDone) {
                lastDone = done;
                if (const int delayMs = m_testRenderDelayMs.load(std::memory_order_relaxed); delayMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
            }
        };
#else
        opt.onProgress = [this](int, int) { noteProgress(ipc::WorkerOp::Redact); };
#endif

        pdf::redact::Result result = pdf::redact::redactDocument(
            sourceData,
            sourceSize,
            password,
            engineRedactions,
            opt
        );

        if (!result.ok()) {
            Q_EMIT redactDocumentFailed(documentId, QString::fromStdString(result.message));
            return;
        }

        ipc::RedactDocumentResponse response;
        response.documentId = documentId;
        response.success = true;
        response.errorMessage = QString::fromStdString(result.message);
        response.pdfBytes = QByteArray(reinterpret_cast<const char*>(result.pdf.data()),
                                       static_cast<qsizetype>(result.pdf.size()));
        response.pageCount = result.report.pageCount;
        response.sourceSignatureCount = result.report.sourceSignatureCount;
        response.sourceAttachmentCount = result.report.sourceAttachmentCount;
        response.sourceWasEncrypted = result.report.sourceWasEncrypted;
        for (const auto& w : result.report.warnings) {
            response.warnings.push_back(QString::fromStdString(w));
        }
        for (const auto& v : result.report.verification) {
            response.verification.push_back(QString::fromStdString(v));
        }

        Q_EMIT redactDocumentReady(documentId, response);
    }});
    m_cv.notify_one();
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
        m_outlineQueue.clear();
        m_activeDocumentId.store(0);
        clearActiveSourceLocked();

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
    bool shouldEmit = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_currentGeneration.store(generation);

        // Prune requests from previous generations
        m_queue.erase(
            std::remove_if(m_queue.begin(), m_queue.end(), [generation](const TileRenderRequest& req) {
                return req.generation < generation;
            }),
            m_queue.end()
        );

        shouldEmit = checkAndNotifyIdleLocked();
    }
    if (shouldEmit) {
        Q_EMIT idle();
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


void RenderWorker::requestListAnnotations(int64_t documentId, int pageIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load()) {
        Q_EMIT annotationsFailed(documentId, pageIndex);
        return;
    }

    m_controlQueue.push_back({[this, documentId, pageIndex]() {
        if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT annotationsFailed(documentId, pageIndex);
            return;
        }
        noteProgress(ipc::WorkerOp::Annotate);
        struct AnnotGuard {
            RenderWorker* w;
            ~AnnotGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};

        IPage* page = getOrLoadWorkerPage(pageIndex);
        if (!page) {
            Q_EMIT annotationsFailed(documentId, pageIndex);
            return;
        }
        bool truncated = false;
        auto annots = page->listAnnotations(&truncated);
        Q_EMIT annotationsReady(documentId, pageIndex, annots, truncated);
    }});
    m_cv.notify_one();
}

void RenderWorker::requestHitTestAnnotation(int64_t documentId, int pageIndex, double xPoints, double yPoints) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load()) {
        Q_EMIT annotationHitTestFailed(documentId, pageIndex);
        return;
    }

    m_controlQueue.push_back({[this, documentId, pageIndex, xPoints, yPoints]() {
        if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT annotationHitTestFailed(documentId, pageIndex);
            return;
        }
        noteProgress(ipc::WorkerOp::Annotate);
        struct AnnotGuard {
            RenderWorker* w;
            ~AnnotGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};

        IPage* page = getOrLoadWorkerPage(pageIndex);
        if (!page) {
            Q_EMIT annotationHitTestFailed(documentId, pageIndex);
            return;
        }
        AnnotationItem item;
        bool hit = page->hitTestAnnotation(xPoints, yPoints, item);
        Q_EMIT annotationHitTestReady(documentId, pageIndex, hit, item);
    }});
    m_cv.notify_one();
}

void RenderWorker::requestAddAnnotation(int64_t documentId, int pageIndex, const AnnotationItem& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load()) {
        Q_EMIT annotationAddFailed(documentId, pageIndex, QStringLiteral("Worker not running or document mismatch"));
        return;
    }

    m_controlQueue.push_back({[this, documentId, pageIndex, item]() {
        if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT annotationAddFailed(documentId, pageIndex, QStringLiteral("Document is not open"));
            return;
        }
        noteProgress(ipc::WorkerOp::Annotate);
        struct AnnotGuard {
            RenderWorker* w;
            ~AnnotGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};

        IPage* page = getOrLoadWorkerPage(pageIndex);
        if (!page) {
            Q_EMIT annotationAddFailed(documentId, pageIndex, QStringLiteral("Failed to load page"));
            return;
        }

        AnnotationItem annotItem = item;
        if (annotItem.name.isEmpty()) {
            annotItem.name = QStringLiteral("annot-%1-%2")
                .arg(QRandomGenerator::global()->generate64(), 0, 16)
                .arg(pageIndex);
        }

        bool ok = false;
        if (annotItem.subtype == AnnotationSubtype::Highlight) {
            ok = page->addHighlightAnnotation(annotItem);
        } else {
            Q_EMIT annotationAddFailed(documentId, pageIndex, QStringLiteral("Unsupported annotation subtype"));
            return;
        }

        if (ok) {
            Q_EMIT annotationAdded(documentId, pageIndex, annotItem.name);
        } else {
            Q_EMIT annotationAddFailed(documentId, pageIndex, QStringLiteral("Failed to add annotation"));
        }
    }});
    m_cv.notify_one();
}

void RenderWorker::requestRemoveAnnotation(int64_t documentId, int pageIndex, const QString& annotationName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load()) {
        Q_EMIT annotationRemoveFailed(documentId, pageIndex, QStringLiteral("Worker not running or document mismatch"));
        return;
    }

    m_controlQueue.push_back({[this, documentId, pageIndex, annotationName]() {
        if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT annotationRemoveFailed(documentId, pageIndex, QStringLiteral("Document is not open"));
            return;
        }
        noteProgress(ipc::WorkerOp::Annotate);
        struct AnnotGuard {
            RenderWorker* w;
            ~AnnotGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};

        IPage* page = getOrLoadWorkerPage(pageIndex);
        if (!page) {
            Q_EMIT annotationRemoveFailed(documentId, pageIndex, QStringLiteral("Failed to load page"));
            return;
        }
        const int removed = page->removeAnnotationByName(annotationName);
        if (removed > 0) {
            Q_EMIT annotationRemoved(documentId, pageIndex, removed);
        } else {
            Q_EMIT annotationRemoveFailed(documentId, pageIndex, QStringLiteral("No annotation with that name"));
        }
    }});
    m_cv.notify_one();
}

void RenderWorker::requestSaveDocument(int64_t documentId, int writeFd, bool incremental, const QString& tempFilePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load() || documentId != m_activeDocumentId.load()) {
        if (writeFd >= 0) {
#ifdef _WIN32
            ::_close(writeFd);
#else
            ::close(writeFd);
#endif
        }
        Q_EMIT documentSaveFailed(documentId, QStringLiteral("Worker not running or document mismatch"));
        return;
    }

    m_controlQueue.push_back({[this, documentId, writeFd, incremental, tempFilePath]() {
        struct FdCleanup {
            int fd;
            ~FdCleanup() {
                if (fd >= 0) {
#ifdef _WIN32
                    ::_close(fd);
#else
                    ::close(fd);
#endif
                }
            }
        } fdCleaner{writeFd};

        if (!m_running.load() || documentId != m_activeDocumentId.load() || !m_workerDocument) {
            Q_EMIT documentSaveFailed(documentId, QStringLiteral("Document is not open"));
            return;
        }

        noteProgress(ipc::WorkerOp::Save);
        struct SaveGuard {
            RenderWorker* w;
            ~SaveGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
        } guard{this};

        int targetFd = writeFd;
#ifdef _WIN32
        if (targetFd < 0 && !tempFilePath.isEmpty()) {
            std::wstring wpath = tempFilePath.toStdWString();
            targetFd = ::_wopen(wpath.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
            fdCleaner.fd = targetFd;
        }
#endif
        if (targetFd < 0) {
            Q_EMIT documentSaveFailed(documentId, QStringLiteral("Invalid save file descriptor"));
            return;
        }

        uint64_t bytesWritten = 0;
        auto heartbeat = [this](uint64_t) {
            noteProgress(ipc::WorkerOp::Save);
        };

        bool ok = m_workerDocument->saveAsCopy(targetFd, incremental, bytesWritten, heartbeat);
        if (ok && bytesWritten > 0) {
            Q_EMIT documentSaved(documentId, bytesWritten);
        } else {
            QString err = (m_workerDocument->signatureCount() > 0 && !incremental)
                ? QStringLiteral("Full rewrite is forbidden on signed documents")
                : QStringLiteral("FPDF_SaveAsCopy failed");
            Q_EMIT documentSaveFailed(documentId, err);
        }
    }});
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

void RenderWorker::startSearch(int64_t documentId, int64_t searchId, const QString& query, bool matchCase, int totalPages, int startPage) {
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
    m_activeSearch.startPage = std::clamp(startPage, 0, totalPages - 1);
    m_activeSearch.nextPageToSearch = 0;
    m_activeSearch.totalMatchesFound = 0;

    m_cv.notify_one();
}

void RenderWorker::cancelSearch() {
    m_currentSearchId.fetch_add(1);

    bool shouldEmit = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeSearch.searchId = 0;
        shouldEmit = checkAndNotifyIdleLocked();
    }
    if (shouldEmit) {
        Q_EMIT idle();
    }
}

void RenderWorker::cancelAll() {
    bool shouldEmit = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        shouldEmit = checkAndNotifyIdleLocked();
    }
    if (shouldEmit) {
        Q_EMIT idle();
    }
}

bool RenderWorker::checkAndNotifyIdleLocked() {
    const bool idleState = m_queue.empty() && m_controlQueue.empty() && m_outlineQueue.empty() &&
                           !m_isRendering && !m_isControlRunning && !m_isOutlineRunning && !m_isSearchRunning &&
                           (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages);
    if (idleState) {
        m_idleCv.notify_all();
    }
    return idleState;
}

bool RenderWorker::isIdle() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty() && m_controlQueue.empty() && m_outlineQueue.empty() &&
           !m_isRendering && !m_isControlRunning && !m_isOutlineRunning && !m_isSearchRunning &&
           (m_activeSearch.searchId == 0 || m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages);
}

void RenderWorker::waitForIdle() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idleCv.wait(lock, [&]() {
        return m_queue.empty() && m_controlQueue.empty() && m_outlineQueue.empty() &&
               !m_isRendering && !m_isControlRunning && !m_isOutlineRunning && !m_isSearchRunning &&
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

void RenderWorker::noteProgress(ipc::WorkerOp op) {
    m_currentOp.store(static_cast<uint32_t>(op), std::memory_order_relaxed);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_lastProgressEpochMs.store(now, std::memory_order_relaxed);
}

std::pair<int64_t, uint32_t> RenderWorker::progressSnapshot() const noexcept {
    return {m_lastProgressEpochMs.load(std::memory_order_relaxed),
            m_currentOp.load(std::memory_order_relaxed)};
}

bool RenderWorker::isEngineProgressing(int64_t maxSilentDurationMs) const noexcept {
    const int64_t last = m_lastProgressEpochMs.load(std::memory_order_relaxed);
    if (last == 0) return true;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return (now - last) <= maxSilentDurationMs;
}

void RenderWorker::postHealthCheck(std::function<void()> onHealthy) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running.load()) {
        return;
    }
    m_controlQueue.push_back({std::move(onHealthy)});
    m_cv.notify_one();
}

void RenderWorker::workerLoop() {
    int tilesRenderedSinceSearch = 0;

    while (m_running.load()) {
#ifdef PDF_WORKER_TEST_HOOKS
        if (m_testHangEngine.load(std::memory_order_relaxed)) {
            // The engine thread itself records the hang (op=Tile, one heartbeat stamp) and then acknowledges
            // it. The setter waits for that acknowledgement, so no progress write can land after it returns:
            // previously this thread's own noteProgress(Idle) below could overwrite the setter's Tile, which
            // made the hang invisible to the watchdog (it only flags non-Idle ops with a frozen heartbeat).
            noteProgress(ipc::WorkerOp::Tile);
            m_testHangParked.store(true, std::memory_order_release);
            while (m_testHangEngine.load(std::memory_order_relaxed) && m_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            m_testHangParked.store(false, std::memory_order_release);
        }
#endif
        ControlTask ctrlTask;
        bool hasControlTask = false;
        TileRenderRequest req;
        bool hasRenderRequest = false;

        bool hasSearchTask = false;
        bool hasOutlineTask = false;
        int64_t outlineDocId = 0;
        int searchPageIndex = -1;
        int64_t searchDocId = 0;
        int64_t searchId = 0;
        QString searchQuery;
        bool searchMatchCase = false;
        int searchTotalPages = 0;
        int searchPagesSearched = 0;
        int searchMatchBudget = 0;
        bool isLastSearchPage = false;

        {
            noteProgress(ipc::WorkerOp::Idle);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&]() {
                if (!m_running.load()) return true;
#ifdef PDF_WORKER_TEST_HOOKS
                if (m_testHangEngine.load(std::memory_order_relaxed)) return true;
#endif
                if (!m_controlQueue.empty()) return true;
                if (!m_queue.empty()) return true;
                if (!m_outlineQueue.empty()) return true;
                if (m_activeSearch.searchId != 0 && m_activeSearch.nextPageToSearch < m_activeSearch.totalPages) return true;
                return false;
            });

            if (!m_running.load()) {
                break;
            }
#ifdef PDF_WORKER_TEST_HOOKS
            if (m_testHangEngine.load(std::memory_order_relaxed)) {
                continue;
            }
#endif

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
                    const bool shouldEmit = checkAndNotifyIdleLocked();
                    lock.unlock();
                    if (shouldEmit) {
                        Q_EMIT idle();
                    }
                    continue;
                }

                m_isRendering = true;
                m_inFlightKey = req.key;
                hasRenderRequest = true;
                tilesRenderedSinceSearch++;
            } else if (!m_outlineQueue.empty()) {
                outlineDocId = m_outlineQueue.front();
                m_outlineQueue.erase(m_outlineQueue.begin());
                m_isOutlineRunning = true;
                hasOutlineTask = true;
            } else if (hasSearch) {
                hasSearchTask = true;
                m_isSearchRunning = true;
                searchPageIndex = (m_activeSearch.startPage + m_activeSearch.nextPageToSearch) % m_activeSearch.totalPages;
                ++m_activeSearch.nextPageToSearch;
                searchDocId = m_activeSearch.documentId;
                searchId = m_activeSearch.searchId;
                searchQuery = m_activeSearch.query;
                searchMatchCase = m_activeSearch.matchCase;
                searchTotalPages = m_activeSearch.totalPages;
                searchPagesSearched = m_activeSearch.nextPageToSearch;
                searchMatchBudget = std::max(0, MAX_SEARCH_MATCHES - m_activeSearch.totalMatchesFound);
                isLastSearchPage = (m_activeSearch.nextPageToSearch >= m_activeSearch.totalPages);
                tilesRenderedSinceSearch = 0;
            }
        }

        if (hasControlTask) {
            ctrlTask.execute();
            bool shouldEmit = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_isControlRunning = false;
                shouldEmit = checkAndNotifyIdleLocked();
            }
            if (shouldEmit) {
                Q_EMIT idle();
            }
            continue;
        }

        if (hasOutlineTask) {
            if (m_workerDocument && outlineDocId == m_activeDocumentId.load()) {
                const auto outline = m_workerDocument->getOutline();
                if (m_running.load() && outlineDocId == m_activeDocumentId.load()) {
                    Q_EMIT documentOutlineReady(outlineDocId, outline);
                }
            }
            bool shouldEmit = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_isOutlineRunning = false;
                shouldEmit = checkAndNotifyIdleLocked();
            }
            if (shouldEmit) {
                Q_EMIT idle();
            }
            continue;
        }

        if (hasRenderRequest) {
            noteProgress(ipc::WorkerOp::Tile);
            struct TileGuard {
                RenderWorker* w;
                ~TileGuard() { w->noteProgress(ipc::WorkerOp::Idle); }
            } guard{this};
            PixelBuffer buffer;
            bool renderedSuccessfully = false;

            if (m_workerDocument && m_running.load()) {
                IPage* page = getOrLoadWorkerPage(req.pageIndex);
                if (page) {
                    buffer = page->renderTile(req.rect, req.scale, req.dpr, req.rotation != 0 ? req.rotation : req.key.rotation);
                    renderedSuccessfully = buffer.isValid() && (buffer.data() != nullptr);
                }
            }

            // Test hook: keep the tile in flight for a while when artificially configured (0 disables)
            if (const int delayMs = m_testRenderDelayMs.load(std::memory_order_relaxed); delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }

            // Deliver completed render whenever documentId matches active document. Deliberately NOT
            // filtered by generation: this tile may be the one a newer-generation request for the same key
            // was deduplicated onto (enqueueRequest), so dropping it would leave that request unserved
            // and the viewer waiting on a tile that is never re-rendered. A stale-generation delivery is
            // harmless: the key identifies the content.
            if (m_running.load() && req.documentId == m_activeDocumentId.load()) {
                if (renderedSuccessfully) {
                    Q_EMIT tileReady(req.documentId, req.generation, req.key, buffer);
                } else {
                    Q_EMIT tileFailed(req.documentId, req.generation, req.key);
                }
            }

            bool shouldEmit = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_isRendering = false;
                shouldEmit = checkAndNotifyIdleLocked();
            }
            if (shouldEmit) {
                Q_EMIT idle();
            }
            continue;
        }

        if (hasSearchTask) {
            // The page was accounted for (nextPageToSearch) when picked, so the idle predicate alone
            // would call the worker idle while this page is still being extracted and delivered.
            // m_isSearchRunning closes that window; the guard clears it on every path out of here
            // (including the early `continue`s) and wakes waitForIdle().
            struct SearchRunGuard {
                RenderWorker* w;
                ~SearchRunGuard() {
                    bool shouldEmit = false;
                    {
                        std::lock_guard<std::mutex> lock(w->m_mutex);
                        w->m_isSearchRunning = false;
                        shouldEmit = w->checkAndNotifyIdleLocked();
                    }
                    if (shouldEmit) {
                        Q_EMIT w->idle();
                    }
                }
            } searchRunGuard{this};
            noteProgress(ipc::WorkerOp::Search);
            if (!m_running.load() || searchId != m_currentSearchId.load() || searchDocId != m_activeDocumentId.load()) {
                continue;
            }

            std::vector<SearchMatch> pageMatches;
            bool limitReached = false;
            if (m_workerDocument && m_running.load()) {
                auto page = m_workerDocument->getPage(searchPageIndex);
                if (page) {
                    PageText pt = page->extractText();
                    pt.pageIndex = searchPageIndex;
                    pageMatches = pdf::findTextInPage(pt, searchQuery, searchMatchCase, searchMatchBudget, &limitReached);
                }
            }

            // Deliver matches if search is still current
            if (m_running.load() && searchId == m_currentSearchId.load() && searchDocId == m_activeDocumentId.load()) {
                int totalSoFar = 0;
                bool searchTruncated = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_activeSearch.searchId == searchId) {
                        m_activeSearch.totalMatchesFound += static_cast<int>(pageMatches.size());
                        totalSoFar = m_activeSearch.totalMatchesFound;
                        searchTruncated = limitReached;
                    }
                }

                if (!pageMatches.empty()) {
                    Q_EMIT searchMatchesFound(searchDocId, searchId, searchPageIndex, pageMatches);
                }
                Q_EMIT searchProgress(searchDocId, searchId, searchPagesSearched, searchTotalPages);

                if (isLastSearchPage || limitReached) {
                    Q_EMIT searchCompleted(searchDocId, searchId, totalSoFar, searchTruncated);
                    noteProgress(ipc::WorkerOp::Idle);
                    bool shouldEmit = false;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_activeSearch.searchId == searchId) {
                            m_activeSearch.searchId = 0;
                        }
                        shouldEmit = checkAndNotifyIdleLocked();
                    }
                    if (shouldEmit) {
                        Q_EMIT idle();
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
