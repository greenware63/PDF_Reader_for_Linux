#include "WorkerServer.h"
#include <QCoreApplication>
#include <QRandomGenerator>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

#ifdef Q_OS_LINUX
#include <QSocketNotifier>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstddef>
#include <cerrno>
#endif

namespace pdf::worker {
namespace {

constexpr uint64_t MAX_OUTSTANDING_TILE_BYTES = 128ULL * 1024 * 1024;

bool isRequestType(uint32_t type) {
    switch (static_cast<ipc::MessageType>(type)) {
        case ipc::MessageType::Ping:
        case ipc::MessageType::OpenDocumentRequest:
        case ipc::MessageType::CloseDocumentRequest:
        case ipc::MessageType::GetOutlineRequest:
        case ipc::MessageType::RenderTileRequest:
        case ipc::MessageType::CancelRenderRequest:
        case ipc::MessageType::SetGenerationRequest:
        case ipc::MessageType::PageTextRequest:
        case ipc::MessageType::StartSearchRequest:
        case ipc::MessageType::CancelSearchRequest:
        case ipc::MessageType::ThumbnailRequest:
        case ipc::MessageType::RenderPageRequest:
        case ipc::MessageType::ExtractPagesRequest:
        case ipc::MessageType::RedactDocumentRequest:
        case ipc::MessageType::ListAnnotationsRequest:
        case ipc::MessageType::HitTestAnnotationRequest:
        case ipc::MessageType::AddAnnotationRequest:
        case ipc::MessageType::RemoveAnnotationRequest:
        case ipc::MessageType::SaveDocumentRequest:
        case ipc::MessageType::SharedMemoryAck:
        case ipc::MessageType::WorkerControlRequest:
            return true;
        default:
            return false;
    }
}

} // namespace

WorkerServer::WorkerServer(const QString& serverName, QObject* parent)
    : QObject(parent)
    , m_serverName(serverName) {

    connect(&m_engineWorker, &pdf::RenderWorker::documentOpened,
            this, &WorkerServer::onDocumentOpened, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::tileReady,
            this, &WorkerServer::onTileReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::tileFailed,
            this, &WorkerServer::onTileFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::documentOutlineReady,
            this, &WorkerServer::onDocumentOutlineReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::pageTextReady,
            this, &WorkerServer::onPageTextReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::searchMatchesFound,
            this, &WorkerServer::onSearchMatchesFound, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::searchProgress,
            this, &WorkerServer::onSearchProgress, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::searchCompleted,
            this, &WorkerServer::onSearchCompleted, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::thumbnailReady,
            this, &WorkerServer::onThumbnailReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::thumbnailFailed,
            this, &WorkerServer::onThumbnailFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::printPageReady,
            this, &WorkerServer::onPrintPageReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::printPageFailed,
            this, &WorkerServer::onPrintPageFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::extractPagesReady,
            this, &WorkerServer::onExtractPagesReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::extractPagesFailed,
            this, &WorkerServer::onExtractPagesFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::redactDocumentReady,
            this, &WorkerServer::onRedactDocumentReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::redactDocumentFailed,
            this, &WorkerServer::onRedactDocumentFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationsReady,
            this, &WorkerServer::onAnnotationsReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationsFailed,
            this, &WorkerServer::onAnnotationsFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationHitTestReady,
            this, &WorkerServer::onAnnotationHitTestReady, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationHitTestFailed,
            this, &WorkerServer::onAnnotationHitTestFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationAdded,
            this, &WorkerServer::onAnnotationAdded, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationRemoved,
            this, &WorkerServer::onAnnotationRemoved, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationRemoveFailed,
            this, &WorkerServer::onAnnotationRemoveFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::annotationAddFailed,
            this, &WorkerServer::onAnnotationAddFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::documentSaved,
            this, &WorkerServer::onDocumentSaved, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::documentSaveFailed,
            this, &WorkerServer::onDocumentSaveFailed, Qt::QueuedConnection);
    connect(&m_engineWorker, &pdf::RenderWorker::idle,
            this, &WorkerServer::onIdle, Qt::QueuedConnection);
}

WorkerServer::~WorkerServer() {
    m_engineWorker.stop();
#ifdef Q_OS_LINUX
    if (m_clientNotifier) {
        m_clientNotifier->setEnabled(false);
        delete m_clientNotifier;
        m_clientNotifier = nullptr;
    }
    if (m_listenNotifier) {
        m_listenNotifier->setEnabled(false);
        delete m_listenNotifier;
        m_listenNotifier = nullptr;
    }
    if (m_clientFd >= 0) {
        ::close(m_clientFd);
        m_clientFd = -1;
    }
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    closePacketFds();
#else
    if (m_clientSocket) {
        m_clientSocket->close();
    }
    if (m_server) {
        m_server->close();
        QLocalServer::removeServer(m_serverName);
    }
#endif
}

bool WorkerServer::start() {
#ifdef Q_OS_LINUX
    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0) {
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    const QByteArray nameBytes = m_serverName.toUtf8();
    if (nameBytes.size() >= static_cast<int>(sizeof(addr.sun_path) - 1)) {
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }
    std::memcpy(addr.sun_path + 1, nameBytes.constData(), static_cast<size_t>(nameBytes.size()));
    socklen_t addrLen = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + nameBytes.size());

    if (::bind(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), addrLen) != 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    if (::listen(m_listenFd, 1) != 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
        return false;
    }

    m_listenNotifier = new QSocketNotifier(m_listenFd, QSocketNotifier::Read, this);
    connect(m_listenNotifier, &QSocketNotifier::activated, this, &WorkerServer::onNewConnection);
    return true;
#else
    QLocalServer::removeServer(m_serverName);
    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this, &WorkerServer::onNewConnection);
    return m_server->listen(m_serverName);
#endif
}

void WorkerServer::onNewConnection() {
#ifdef Q_OS_LINUX
    int client = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
        return;
    }

    if (m_clientFd >= 0) {
        ::close(client);
        return;
    }

    m_clientFd = client;
    m_clientNotifier = new QSocketNotifier(m_clientFd, QSocketNotifier::Read, this);
    connect(m_clientNotifier, &QSocketNotifier::activated, this, &WorkerServer::onReadyRead);
#else
    if (m_clientSocket) {
        auto* extra = m_server->nextPendingConnection();
        if (extra) {
            extra->disconnectFromServer();
            extra->deleteLater();
        }
        return;
    }

    m_clientSocket = m_server->nextPendingConnection();
    if (!m_clientSocket) {
        return;
    }

    connect(m_clientSocket, &QLocalSocket::readyRead, this, &WorkerServer::onReadyRead);
    connect(m_clientSocket, &QLocalSocket::disconnected, this, &WorkerServer::onSocketDisconnected);
    m_clientSocket->setReadBufferSize(static_cast<qint64>(ipc::MAX_IPC_PAYLOAD_SIZE + sizeof(ipc::PacketHeader)));
#endif
}

void WorkerServer::onSocketDisconnected() {
    m_incomingBuffer.clear();
    m_pendingControlRequests.clear();
#ifdef Q_OS_LINUX
    if (m_clientNotifier) {
        m_clientNotifier->setEnabled(false);
        delete m_clientNotifier;
        m_clientNotifier = nullptr;
    }
    if (m_clientFd >= 0) {
        ::close(m_clientFd);
        m_clientFd = -1;
    }
    closePacketFds();
#else
    if (m_clientSocket) {
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
#endif
    Q_EMIT clientDisconnected();
}

namespace {
constexpr size_t kMaxFdsPerPacket = 4;
}

void WorkerServer::closePacketFds() {
#ifdef Q_OS_LINUX
    for (int fd : m_packetFds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    m_packetFds.clear();
#endif
}

bool WorkerServer::headerIsValid(const ipc::PacketHeader& header) const {
    return header.magic == ipc::IPC_MAGIC &&
           header.version == ipc::IPC_PROTOCOL_VERSION &&
           header.payloadSize <= ipc::MAX_IPC_PAYLOAD_SIZE &&
           isRequestType(header.type);
}

void WorkerServer::onReadyRead() {
#ifdef Q_OS_LINUX
    if (m_clientFd < 0) {
        return;
    }

    // SCM_RIGHTS descriptors travel with the first byte of the sendmsg() that carried them, but a stream
    // read has no packet boundaries: one recvmsg() may span several packets, and the kernel hands the
    // descriptors to whichever read first touches that byte. To keep every descriptor attached to exactly
    // one packet, never read past the end of the packet being assembled: read the header, validate it, then
    // read exactly the remaining payload. A descriptor that arrives during those reads belongs to this
    // packet, and only to this packet.
    while (m_clientFd >= 0) {
        const size_t have = static_cast<size_t>(m_incomingBuffer.size());
        size_t want = 0;
        if (have < sizeof(ipc::PacketHeader)) {
            want = sizeof(ipc::PacketHeader) - have;
        } else {
            ipc::PacketHeader header;
            std::memcpy(&header, m_incomingBuffer.constData(), sizeof(header));
            if (!headerIsValid(header)) {
                std::cerr << "[WorkerServer] Protocol error: Invalid header received. Disconnecting." << std::endl;
                onSocketDisconnected();
                return;
            }
            const size_t total = sizeof(ipc::PacketHeader) + header.payloadSize;
            if (have >= total) {
                QByteArray payload = m_incomingBuffer.mid(static_cast<qsizetype>(sizeof(ipc::PacketHeader)),
                                                          static_cast<qsizetype>(header.payloadSize));
                m_incomingBuffer.clear();
                std::vector<int> fds;
                fds.swap(m_packetFds);
                dispatchMessage(static_cast<ipc::MessageType>(header.type), payload, std::move(fds));
                continue;
            }
            want = total - have;
        }

        char buf[65536];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = std::min(want, sizeof(buf)); // never past the end of the current packet

        char cmsgBuf[CMSG_SPACE(kMaxFdsPerPacket * sizeof(int))];
        struct msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsgBuf;
        msg.msg_controllen = sizeof(cmsgBuf);

        ssize_t n = ::recvmsg(m_clientFd, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            onSocketDisconnected();
            return;
        }

        // Take ownership of every descriptor first so that none can leak on any error path below.
        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
                continue;
            }
            const size_t numFds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
            for (size_t i = 0; i < numFds; ++i) {
                if (fds[i] >= 0) {
                    m_packetFds.push_back(fds[i]);
                }
            }
        }
        if ((msg.msg_flags & MSG_CTRUNC) != 0 || m_packetFds.size() > kMaxFdsPerPacket) {
            // The kernel already closed the descriptors that did not fit; no honest client sends this many.
            std::cerr << "[WorkerServer] Protocol error: too many descriptors on one packet. Disconnecting." << std::endl;
            onSocketDisconnected();
            return;
        }
        if (n == 0) {
            onSocketDisconnected();
            return;
        }
        m_incomingBuffer.append(buf, static_cast<qsizetype>(n));
    }
#else
    if (!m_clientSocket) {
        return;
    }
    m_incomingBuffer.append(m_clientSocket->readAll());

    while (true) {
        if (static_cast<size_t>(m_incomingBuffer.size()) < sizeof(ipc::PacketHeader)) {
            return;
        }
        ipc::PacketHeader header;
        std::memcpy(&header, m_incomingBuffer.constData(), sizeof(ipc::PacketHeader));

        if (!headerIsValid(header)) {
            std::cerr << "[WorkerServer] Protocol error: Invalid header received. Disconnecting." << std::endl;
            m_clientSocket->disconnectFromServer();
            return;
        }

        const size_t totalPacketSize = sizeof(ipc::PacketHeader) + header.payloadSize;
        if (static_cast<size_t>(m_incomingBuffer.size()) < totalPacketSize) {
            return;
        }

        QByteArray payload;
        if (header.payloadSize > 0) {
            payload = m_incomingBuffer.mid(static_cast<qsizetype>(sizeof(ipc::PacketHeader)),
                                           static_cast<qsizetype>(header.payloadSize));
        }
        m_incomingBuffer.remove(0, static_cast<qsizetype>(totalPacketSize));

        dispatchMessage(static_cast<ipc::MessageType>(header.type), payload, {});
    }
#endif
}

void WorkerServer::dispatchMessage(ipc::MessageType type, const QByteArray& payload, std::vector<int> fds) {
    // Descriptor policy: only OpenDocumentRequest may carry a descriptor, and exactly one. Anything else is
    // closed here, before it can be paired with a later request. (Linux only; the vector is always empty
    // on other platforms.)
    auto closeAll = [&fds]() {
#ifdef Q_OS_LINUX
        for (int fd : fds) {
            ::close(fd);
        }
#endif
        fds.clear();
    };
    if (!fds.empty() && type != ipc::MessageType::OpenDocumentRequest && type != ipc::MessageType::SaveDocumentRequest) {
        std::cerr << "[WorkerServer] Rejected " << fds.size()
                  << " unsolicited descriptor(s) on a message type that does not accept them; message dropped."
                  << std::endl;
        closeAll();
        return;
    }

    switch (type) {
        case ipc::MessageType::Ping: {
            ipc::PingRequest req;
            if (ipc::deserialize(payload, req)) {
                const auto [lastProgress, curOp] = m_engineWorker.progressSnapshot();
                ipc::PongResponse resp;
                resp.seq = req.seq;
                resp.lastProgressEpochMs = lastProgress;
                resp.currentOp = curOp;
                sendPacket(ipc::MessageType::Pong, ipc::serialize(resp));
            }
            break;
        }

        case ipc::MessageType::OpenDocumentRequest: {
            ipc::OpenDocumentRequest req;
            const bool parsed = ipc::deserialize(payload, req);
            if (fds.size() > 1) {
                // Malformed: an Open carries exactly one descriptor. Close all of them and fail closed.
                std::cerr << "[WorkerServer] Rejected OpenDocumentRequest with " << fds.size()
                          << " descriptors." << std::endl;
                closeAll();
                if (parsed) {
                    pdf::OpenResult failed;
                    failed.error = pdf::DocumentError::InvalidFormat;
                    onDocumentOpened(req.documentId, std::move(failed));
                }
                break;
            }
            const int docFd = fds.empty() ? -1 : fds.front();
            fds.clear(); // ownership moves to the engine (or is closed below)
            if (parsed) {
                m_engineWorker.openDocumentAsync(req.documentId, req.filePath.toStdString(), req.password.toStdString(), docFd);
            } else if (docFd >= 0) {
#ifdef Q_OS_LINUX
                ::close(docFd);
#endif
            }
            break;
        }

        case ipc::MessageType::CloseDocumentRequest: {
            ipc::CloseDocumentRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.closeDocumentSync();
            }
            break;
        }

        case ipc::MessageType::GetOutlineRequest: {
            ipc::GetOutlineRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestDocumentOutline(req.documentId);
            }
            break;
        }

        case ipc::MessageType::RenderTileRequest: {
            ipc::RenderTileRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.enqueueRequest(req.request);
            }
            break;
        }

        case ipc::MessageType::CancelRenderRequest: {
            ipc::CancelRenderRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.cancelAll();
            }
            break;
        }

        case ipc::MessageType::SetGenerationRequest: {
            ipc::SetGenerationRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.setGeneration(req.generation);
            }
            break;
        }

        case ipc::MessageType::PageTextRequest: {
            ipc::PageTextRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestPageText(req.documentId, req.pageIndex);
            }
            break;
        }

        case ipc::MessageType::StartSearchRequest: {
            ipc::StartSearchRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.startSearch(req.documentId, req.searchId, req.query,
                                           req.matchCase, req.totalPages, req.startPage);
            }
            break;
        }

        case ipc::MessageType::CancelSearchRequest: {
            ipc::CancelSearchRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.cancelSearch();
            }
            break;
        }

        case ipc::MessageType::ThumbnailRequest: {
            ipc::ThumbnailRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestThumbnail(req.documentId, req.pageIndex, req.targetWidth, req.rotation);
            }
            break;
        }

        case ipc::MessageType::RenderPageRequest: {
            ipc::RenderPageRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestPrintPage(req.documentId, req.pageIndex, req.scale, req.dpr, req.rotation);
            }
            break;
        }

        case ipc::MessageType::ExtractPagesRequest: {
            ipc::ExtractPagesRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestExtractPages(req.documentId, req.firstPage, req.lastPage);
            }
            break;
        }

        case ipc::MessageType::ListAnnotationsRequest: {
            ipc::ListAnnotationsRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestListAnnotations(req.documentId, req.pageIndex);
            } else {
                onAnnotationsFailed(req.documentId, req.pageIndex);
            }
            break;
        }

        case ipc::MessageType::HitTestAnnotationRequest: {
            ipc::HitTestAnnotationRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestHitTestAnnotation(req.documentId, req.pageIndex, req.xPoints, req.yPoints);
            } else {
                onAnnotationHitTestFailed(req.documentId, req.pageIndex);
            }
            break;
        }

        case ipc::MessageType::AddAnnotationRequest: {
            ipc::AddAnnotationRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestAddAnnotation(req.documentId, req.pageIndex, req.annotation);
            } else {
                onAnnotationAddFailed(req.documentId, req.pageIndex, QStringLiteral("Malformed AddAnnotationRequest payload"));
            }
            break;
        }

        case ipc::MessageType::RemoveAnnotationRequest: {
            ipc::RemoveAnnotationRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestRemoveAnnotation(req.documentId, req.pageIndex, req.annotationName);
            } else {
                onAnnotationRemoveFailed(req.documentId, req.pageIndex, QStringLiteral("Malformed RemoveAnnotationRequest payload"));
            }
            break;
        }

        case ipc::MessageType::SaveDocumentRequest: {
            ipc::SaveDocumentRequest req;
            const bool parsed = ipc::deserialize(payload, req);
#ifdef Q_OS_LINUX
            if (fds.size() != 1) {
                std::cerr << "[WorkerServer] Rejected SaveDocumentRequest with " << fds.size()
                          << " descriptors (expected exactly 1)." << std::endl;
                closeAll();
                if (parsed) {
                    onDocumentSaveFailed(req.documentId, QStringLiteral("SaveDocumentRequest requires exactly 1 descriptor"));
                }
                break;
            }
            const int saveFd = fds.front();
            fds.clear();

            int fl = ::fcntl(saveFd, F_GETFL);
            if (fl == -1 || ((fl & O_ACCMODE) != O_WRONLY && (fl & O_ACCMODE) != O_RDWR)) {
                std::cerr << "[WorkerServer] Rejected SaveDocumentRequest: descriptor is not open for writing." << std::endl;
                ::close(saveFd);
                if (parsed) {
                    onDocumentSaveFailed(req.documentId, QStringLiteral("Descriptor is not open for writing"));
                }
                break;
            }
#else
            const int saveFd = fds.empty() ? -1 : fds.front();
            fds.clear();
#endif
            if (parsed) {
                m_engineWorker.requestSaveDocument(req.documentId, saveFd, req.incremental, req.tempFilePath);
            } else if (saveFd >= 0) {
#ifdef Q_OS_LINUX
                ::close(saveFd);
#endif
            }
            break;
        }

        case ipc::MessageType::RedactDocumentRequest: {
            ipc::RedactDocumentRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.requestRedactDocument(req.documentId, req.redactions, req.dpi, req.edgeGrowPx);
            } else {
                // Never drop a malformed request silently: the client would wait out its whole timeout.
                // documentId is 0 when it could not even be parsed; the client treats 0 as "any".
                onRedactDocumentFailed(req.documentId, QStringLiteral("Malformed redaction request rejected by worker."));
            }
            break;
        }

        case ipc::MessageType::SharedMemoryAck: {
            ipc::SharedMemoryAck ack;
            if (ipc::deserialize(payload, ack)) {
                const auto it = m_sharedTiles.find(ack.shmKey);
                if (it != m_sharedTiles.end()) {
                    m_sharedTiles.erase(it); // unmaps and unlinks (harmless if the UI already unlinked it)
                }
            }
            break;
        }

        case ipc::MessageType::WorkerControlRequest: {
            ipc::WorkerControlRequest request;
            if (!ipc::deserialize(payload, request)) {
                break;
            }

#ifdef PDF_WORKER_TEST_HOOKS
            // Fault injection is test-only. In a shipping build these fields are
            // ignored, so a peer cannot park the render with an arbitrary delay.
            if (request.renderDelayMs >= 0) {
                m_engineWorker.setRenderDelayForTesting(std::chrono::milliseconds(request.renderDelayMs));
            }
            if (request.openDelayMs >= 0) {
                m_engineWorker.setOpenDelayForTesting(std::chrono::milliseconds(request.openDelayMs));
            }
            if (request.hangEngine) {
                m_engineWorker.setHangEngineForTesting(true);
            }
#endif
            if (request.waitForIdle && !m_engineWorker.isIdle()) {
                m_pendingControlRequests.push_back(request);
                if (m_engineWorker.isIdle()) {
                    onIdle();
                }
                break;
            }

            ipc::WorkerControlResponse response;
            response.requestId = request.requestId;
            if (request.queryStatus) {
                response.pendingCount = m_engineWorker.pendingCount();
                response.cachedPageCount = m_engineWorker.cachedPageCount();
            }
            sendPacket(ipc::MessageType::WorkerControlResponse, ipc::serialize(response));
            break;
        }

        default:
            std::cerr << "[WorkerServer] Warning: Unrecognized message type: "
                      << static_cast<uint32_t>(type) << std::endl;
            break;
    }
}

void WorkerServer::onIdle() {
    if (m_pendingControlRequests.empty()) {
        return;
    }
    if (!m_engineWorker.isIdle()) {
        return;
    }
    auto pending = std::move(m_pendingControlRequests);
    m_pendingControlRequests.clear();
    for (const auto& request : pending) {
        ipc::WorkerControlResponse response;
        response.requestId = request.requestId;
        if (request.queryStatus) {
            response.pendingCount = m_engineWorker.pendingCount();
            response.cachedPageCount = m_engineWorker.cachedPageCount();
        }
        sendPacket(ipc::MessageType::WorkerControlResponse, ipc::serialize(response));
    }
}

void WorkerServer::sendPacket(ipc::MessageType type, const QByteArray& payload) {
#ifdef Q_OS_LINUX
    if (m_clientFd < 0) {
        return;
    }

    QByteArray packet = ipc::buildPacket(type, payload);
    if (packet.isEmpty()) {
        std::cerr << "[WorkerServer] Refusing oversized IPC payload." << std::endl;
        onSocketDisconnected();
        return;
    }

    const char* ptr = packet.constData();
    size_t remaining = static_cast<size_t>(packet.size());
    while (remaining > 0) {
        ssize_t written = ::write(m_clientFd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) continue;
            onSocketDisconnected();
            return;
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
#else
    if (!m_clientSocket || m_clientSocket->state() != QLocalSocket::ConnectedState) {
        return;
    }

    QByteArray packet = ipc::buildPacket(type, payload);
    if (packet.isEmpty()) {
        std::cerr << "[WorkerServer] Refusing oversized IPC payload." << std::endl;
        m_clientSocket->disconnectFromServer();
        return;
    }
    m_clientSocket->write(packet);
    m_clientSocket->flush();
#endif
}

void WorkerServer::onDocumentOpened(int64_t documentId, pdf::OpenResult result) {
    ipc::OpenDocumentResponse resp{documentId, std::move(result)};
    sendPacket(ipc::MessageType::OpenDocumentResponse, ipc::serialize(resp));
}

void WorkerServer::onTileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer) {
    ipc::TileRenderedResponse resp;
    resp.documentId = documentId;
    resp.generation = generation;
    resp.key = key;
    if (buffer.isValid()) {
        resp.width = buffer.width();
        resp.height = buffer.height();
        resp.stride = buffer.stride();
        resp.format = static_cast<uint32_t>(buffer.format());
        uint64_t outstandingBytes = 0;
        for (const auto& entry : m_sharedTiles) {
            outstandingBytes += static_cast<uint64_t>(entry.second->size());
        }
        if (buffer.sizeBytes() > MAX_OUTSTANDING_TILE_BYTES ||
            outstandingBytes > MAX_OUTSTANDING_TILE_BYTES - buffer.sizeBytes()) {
#ifdef Q_OS_LINUX
            onSocketDisconnected();
#else
            if (m_clientSocket) {
                m_clientSocket->disconnectFromServer();
            }
#endif
            return;
        }
        const QString shmKey = QStringLiteral("pdf-tile-%1-%2")
                                   .arg(QCoreApplication::applicationPid())
                                   .arg(QRandomGenerator::global()->generate64(), 0, 16);
        auto memory = std::make_unique<ipc::SharedTileSegment>();
        if (!memory->create(shmKey.toStdString(), buffer.sizeBytes())) {
            sendPacket(ipc::MessageType::TileFailedResponse,
                       ipc::serialize(ipc::TileFailedResponse{documentId, generation, key}));
            return;
        }
        std::memcpy(memory->data(), buffer.data(), buffer.sizeBytes());
        resp.isShm = true;
        resp.shmKey = shmKey;
        m_sharedTiles.emplace(shmKey, std::move(memory));
    }
    sendPacket(ipc::MessageType::TileRenderedResponse, ipc::serialize(resp));
}

void WorkerServer::onTileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key) {
    ipc::TileFailedResponse resp{documentId, generation, key};
    sendPacket(ipc::MessageType::TileFailedResponse, ipc::serialize(resp));
}

void WorkerServer::onDocumentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline) {
    ipc::GetOutlineResponse resp{documentId, outline};
    sendPacket(ipc::MessageType::GetOutlineResponse, ipc::serialize(resp));
}

void WorkerServer::onPageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> pageText) {
    ipc::PageTextResponse resp;
    resp.documentId = documentId;
    resp.pageIndex = pageIndex;
    resp.success = (pageText != nullptr);
    if (pageText) {
        resp.pageText = *pageText;
    }
    sendPacket(ipc::MessageType::PageTextResponse, ipc::serialize(resp));
}

void WorkerServer::onSearchMatchesFound(int64_t documentId, int64_t searchId, int pageIndex, const std::vector<pdf::SearchMatch>& matches) {
    ipc::SearchMatchesFoundResponse resp{documentId, searchId, pageIndex, matches};
    sendPacket(ipc::MessageType::SearchMatchesFoundResponse, ipc::serialize(resp));
}

void WorkerServer::onSearchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages) {
    ipc::SearchProgressResponse resp{documentId, searchId, pagesSearched, totalPages};
    sendPacket(ipc::MessageType::SearchProgressResponse, ipc::serialize(resp));
}

void WorkerServer::onSearchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated) {
    ipc::SearchCompletedResponse resp{documentId, searchId, totalMatches, truncated};
    sendPacket(ipc::MessageType::SearchCompletedResponse, ipc::serialize(resp));
}

void WorkerServer::onThumbnailReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer) {
    ipc::ThumbnailResponse resp;
    resp.documentId = documentId;
    resp.pageIndex = pageIndex;
    resp.width = buffer.width();
    resp.height = buffer.height();
    resp.stride = static_cast<int>(buffer.stride());
    resp.pixelData = QByteArray(reinterpret_cast<const char*>(buffer.data()), static_cast<qsizetype>(buffer.sizeBytes()));
    sendPacket(ipc::MessageType::ThumbnailResponse, ipc::serialize(resp));
}

void WorkerServer::onThumbnailFailed(int64_t documentId, int pageIndex) {
    ipc::ThumbnailFailedResponse resp{documentId, pageIndex};
    sendPacket(ipc::MessageType::ThumbnailFailedResponse, ipc::serialize(resp));
}

void WorkerServer::onPrintPageReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer) {
    ipc::RenderPageResponse resp;
    resp.documentId = documentId;
    resp.pageIndex = pageIndex;
    resp.width = buffer.width();
    resp.height = buffer.height();
    resp.stride = static_cast<int>(buffer.stride());
    resp.pixelData = QByteArray(reinterpret_cast<const char*>(buffer.data()), static_cast<qsizetype>(buffer.sizeBytes()));
    sendPacket(ipc::MessageType::RenderPageResponse, ipc::serialize(resp));
}

void WorkerServer::onPrintPageFailed(int64_t documentId, int pageIndex) {
    ipc::RenderPageFailedResponse resp{documentId, pageIndex};
    sendPacket(ipc::MessageType::RenderPageFailedResponse, ipc::serialize(resp));
}

void WorkerServer::onExtractPagesReady(int64_t documentId, int firstPage, int lastPage, const QByteArray& pdfBytes) {
    // Leave headroom for the response header; oversized results are reported as failures
    // instead of tripping the protocol payload cap.
    if (pdfBytes.size() > static_cast<qsizetype>(ipc::MAX_IPC_PAYLOAD_SIZE - 1024)) {
        onExtractPagesFailed(documentId, firstPage, lastPage);
        return;
    }
    sendPacket(ipc::MessageType::ExtractPagesResponse,
               ipc::serialize(ipc::ExtractPagesResponse{documentId, firstPage, lastPage, pdfBytes}));
}

void WorkerServer::onExtractPagesFailed(int64_t documentId, int firstPage, int lastPage) {
    sendPacket(ipc::MessageType::ExtractPagesFailedResponse,
               ipc::serialize(ipc::ExtractPagesFailedResponse{documentId, firstPage, lastPage}));
}

void WorkerServer::onRedactDocumentReady(int64_t documentId, const ipc::RedactDocumentResponse& response) {
    if (response.pdfBytes.size() > static_cast<qsizetype>(ipc::MAX_IPC_PAYLOAD_SIZE - 65536)) {
        onRedactDocumentFailed(documentId, QStringLiteral("Redacted PDF exceeds maximum IPC payload limit (64MB)."));
        return;
    }
    sendPacket(ipc::MessageType::RedactDocumentResponse, ipc::serialize(response));
}

void WorkerServer::onRedactDocumentFailed(int64_t documentId, const QString& errorMessage) {
    sendPacket(ipc::MessageType::RedactDocumentFailedResponse,
               ipc::serialize(ipc::RedactDocumentFailedResponse{documentId, errorMessage}));
}


void WorkerServer::onAnnotationsReady(int64_t documentId, int pageIndex, const std::vector<pdf::AnnotationItem>& annotations, bool truncated) {
    ipc::ListAnnotationsResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.success = true;
    res.annotations = annotations;
    res.truncated = truncated;
    sendPacket(ipc::MessageType::ListAnnotationsResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationsFailed(int64_t documentId, int pageIndex) {
    ipc::ListAnnotationsResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.success = false;
    sendPacket(ipc::MessageType::ListAnnotationsResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationHitTestReady(int64_t documentId, int pageIndex, bool hit, const pdf::AnnotationItem& annotation) {
    ipc::HitTestAnnotationResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.hit = hit;
    res.annotation = annotation;
    sendPacket(ipc::MessageType::HitTestAnnotationResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationHitTestFailed(int64_t documentId, int pageIndex) {
    ipc::HitTestAnnotationResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.hit = false;
    sendPacket(ipc::MessageType::HitTestAnnotationResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationAdded(int64_t documentId, int pageIndex, const QString& annotationName) {
    ipc::AddAnnotationResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.success = true;
    res.annotationName = annotationName;
    sendPacket(ipc::MessageType::AddAnnotationResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationRemoved(int64_t documentId, int pageIndex, int removedCount) {
    ipc::RemoveAnnotationResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.success = true;
    res.removedCount = removedCount;
    sendPacket(ipc::MessageType::RemoveAnnotationResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationRemoveFailed(int64_t documentId, int pageIndex, const QString& errorMessage) {
    ipc::RemoveAnnotationResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.success = false;
    res.errorMessage = errorMessage;
    sendPacket(ipc::MessageType::RemoveAnnotationResponse, ipc::serialize(res));
}

void WorkerServer::onAnnotationAddFailed(int64_t documentId, int pageIndex, const QString& errorMessage) {
    ipc::AddAnnotationResponse res;
    res.documentId = documentId;
    res.pageIndex = pageIndex;
    res.success = false;
    res.errorMessage = errorMessage;
    sendPacket(ipc::MessageType::AddAnnotationResponse, ipc::serialize(res));
}

void WorkerServer::onDocumentSaved(int64_t documentId, uint64_t bytesWritten) {
    ipc::SaveDocumentResponse res;
    res.documentId = documentId;
    res.success = true;
    res.bytesWritten = bytesWritten;
    sendPacket(ipc::MessageType::SaveDocumentResponse, ipc::serialize(res));
}

void WorkerServer::onDocumentSaveFailed(int64_t documentId, const QString& errorMessage) {
    ipc::SaveDocumentResponse res;
    res.documentId = documentId;
    res.success = false;
    res.errorMessage = errorMessage;
    sendPacket(ipc::MessageType::SaveDocumentResponse, ipc::serialize(res));
}

} // namespace pdf::worker
