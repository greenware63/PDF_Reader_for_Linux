#include "WorkerClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QRandomGenerator>
#include "ipc/SharedTile.h"
#include <QTimer>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <limits>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#endif

namespace pdf::ipc {
namespace {

constexpr int CONNECTION_TIMEOUT_MS = 10000;
constexpr int RESPONSE_TIMEOUT_MS = 60000;
constexpr uint64_t MAX_TILE_BYTES = 256ULL * 1024 * 1024;
constexpr int MAX_TILE_DIMENSION = 32768;
constexpr size_t MAX_QUEUED_IPC_BYTES = 64 * 1024 * 1024;

bool isKnownMessageType(uint32_t type) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::Pong:
        case MessageType::OpenDocumentResponse:
        case MessageType::GetOutlineResponse:
        case MessageType::TileRenderedResponse:
        case MessageType::TileFailedResponse:
        case MessageType::PageTextResponse:
        case MessageType::SearchMatchesFoundResponse:
        case MessageType::SearchProgressResponse:
        case MessageType::SearchCompletedResponse:
        case MessageType::ThumbnailResponse:
        case MessageType::ThumbnailFailedResponse:
        case MessageType::RenderPageResponse:
        case MessageType::RenderPageFailedResponse:
        case MessageType::ExtractPagesResponse:
        case MessageType::ExtractPagesFailedResponse:
        case MessageType::RedactDocumentResponse:
        case MessageType::RedactDocumentFailedResponse:
        case MessageType::ListAnnotationsResponse:
        case MessageType::HitTestAnnotationResponse:
        case MessageType::WorkerError:
        case MessageType::WorkerControlResponse:
            return true;
        default:
            return false;
    }
}

pdf::OpenResult failedOpenResult() {
    pdf::OpenResult result;
    result.error = pdf::DocumentError::InvalidFormat;
    return result;
}

} // namespace

WorkerClient::WorkerClient(QObject* parent)
    : QObject(parent),
      m_serverName(QStringLiteral("pdf-reader-%1-%2")
                       .arg(QCoreApplication::applicationPid())
                       .arg(QRandomGenerator::global()->generate64(), 0, 16)) {
    connect(&m_socket, &QLocalSocket::connected, this, &WorkerClient::onSocketConnected);
    connect(&m_socket, &QLocalSocket::readyRead, this, &WorkerClient::onSocketReadyRead);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, &WorkerClient::onSocketError);
    connect(&m_process, &QProcess::errorOccurred, this, &WorkerClient::onProcessError);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &WorkerClient::onProcessFinished);
    connect(&m_connectTimer, &QTimer::timeout, this, &WorkerClient::tryConnect);
    m_connectTimer.setInterval(40);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &WorkerClient::onWatchdogTimeout);
}

WorkerClient::~WorkerClient() {
    stop();
}

void WorkerClient::startWorker() {
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }
    m_failureReported = false;
    const QString executable = QDir(QCoreApplication::applicationDirPath())
#ifdef Q_OS_WIN
                                   .filePath(QStringLiteral("pdf_worker.exe"));
#else
                                   .filePath(QStringLiteral("pdf_worker"));
#endif
    if (!QFileInfo::exists(executable)) {
        failWorker(tr("PDF worker executable was not found: %1").arg(executable));
        return;
    }
    m_process.setProgram(executable);
    m_process.setArguments({QStringLiteral("--socket"), m_serverName});
    m_process.start();
    m_connectTimer.start();
}

void WorkerClient::tryConnect() {
    if (m_socket.state() == QLocalSocket::ConnectedState ||
        m_socket.state() == QLocalSocket::ConnectingState ||
        m_process.state() == QProcess::NotRunning) {
        return;
    }
#ifdef Q_OS_LINUX
    m_socket.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    m_socket.connectToServer(m_serverName);
}

bool WorkerClient::ensureConnected() {
    if (m_socket.state() == QLocalSocket::ConnectedState) {
        return true;
    }
    startWorker();
    if (m_process.state() == QProcess::NotRunning) {
        return false;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    const auto connectedConnection = connect(&m_socket, &QLocalSocket::connected, &loop, &QEventLoop::quit);
    const auto failedConnection = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    tryConnect();
    timeout.start(CONNECTION_TIMEOUT_MS);
    if (m_socket.state() != QLocalSocket::ConnectedState && !m_failureReported) {
        loop.exec();
    }
    disconnect(connectedConnection);
    disconnect(failedConnection);
    const bool connected = m_socket.state() == QLocalSocket::ConnectedState;
    if (!connected && !m_failureReported) {
        failWorker(tr("Timed out connecting to the PDF worker."));
    }
    return connected;
}

void WorkerClient::onSocketConnected() {
    m_connectTimer.stop();
    m_failureReported = false;
    m_missedPings = 0;
    if (m_watchdogEnabled && m_watchdogIntervalMs > 0) {
        m_watchdogTimer.start(m_watchdogIntervalMs);
    }
    flushOutgoing();
}

void WorkerClient::onSocketError(QLocalSocket::LocalSocketError error) {
    Q_UNUSED(error);
    if (m_process.state() == QProcess::Running && !m_stopping) {
        QTimer::singleShot(40, this, &WorkerClient::tryConnect);
    }
}

void WorkerClient::onProcessError(QProcess::ProcessError error) {
    if (m_stopping) {
        return;
    }
    failWorker(error == QProcess::FailedToStart
                   ? tr("Could not start the PDF worker: %1").arg(m_process.errorString())
                   : tr("PDF worker process error: %1").arg(m_process.errorString()));
}

void WorkerClient::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    if (!m_stopping) {
        failWorker(tr("PDF worker exited unexpectedly (code %1, status %2).")
                       .arg(exitCode)
                       .arg(static_cast<int>(status)));
    }
}

void WorkerClient::failWorker(const QString& message) {
    if (m_failureReported || m_stopping) {
        return;
    }
    m_failureReported = true;
    m_connectTimer.stop();
    m_watchdogTimer.stop();
    m_missedPings = 0;
    m_outgoingPackets.clear();
    m_outgoingBytes = 0;
    m_socket.abort();
    Q_EMIT workerFailed(message);
}

void WorkerClient::sendMessage(MessageType type, const QByteArray& payload) {
    QByteArray packet = buildPacket(type, payload);
    if (packet.isEmpty()) {
        failWorker(tr("An IPC message exceeded the protocol size limit."));
        return;
    }
    startWorker();
    if (m_socket.state() == QLocalSocket::ConnectedState) {
        sendPacket(type, payload);
        return;
    }
    if (static_cast<size_t>(packet.size()) > MAX_QUEUED_IPC_BYTES - m_outgoingBytes) {
        failWorker(tr("PDF worker request queue exceeded its memory limit."));
        return;
    }
    m_outgoingBytes += static_cast<size_t>(packet.size());
    m_outgoingPackets.push_back(std::move(packet));
}

bool WorkerClient::sendPacket(MessageType type, const QByteArray& payload) {
    const QByteArray packet = buildPacket(type, payload);
    if (packet.isEmpty() || m_socket.write(packet) != packet.size()) {
        failWorker(tr("Could not send a complete request to the PDF worker."));
        return false;
    }
    return true;
}

bool WorkerClient::sendOpenDocumentPacket(const OpenDocumentRequest& request, int docFd) {
#ifdef Q_OS_LINUX
    if (!ensureConnected()) {
        if (docFd >= 0) {
            ::close(docFd);
        }
        return false;
    }

    flushOutgoing();
    while (m_socket.bytesToWrite() > 0) {
        if (!m_socket.waitForBytesWritten(200)) {
            if (docFd >= 0) {
                ::close(docFd);
            }
            failWorker(tr("Timed out waiting to flush socket before sending document descriptor."));
            return false;
        }
    }

    const QByteArray payload = serialize(request);
    const QByteArray packet = buildPacket(MessageType::OpenDocumentRequest, payload);
    if (packet.isEmpty()) {
        if (docFd >= 0) {
            ::close(docFd);
        }
        failWorker(tr("An IPC message exceeded the protocol size limit."));
        return false;
    }

    if (docFd < 0) {
        return sendPacket(MessageType::OpenDocumentRequest, payload);
    }

    int sockfd = static_cast<int>(m_socket.socketDescriptor());
    if (sockfd < 0) {
        ::close(docFd);
        failWorker(tr("Worker socket descriptor is invalid."));
        return false;
    }

    struct msghdr msg{};
    struct iovec iov{};
    iov.iov_base = const_cast<char*>(packet.constData());
    iov.iov_len = static_cast<size_t>(packet.size());
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsgBuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsgBuf;
    msg.msg_controllen = sizeof(cmsgBuf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &docFd, sizeof(int));

    ssize_t sent = 0;
    while (true) {
        sent = ::sendmsg(sockfd, &msg, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(docFd);
            failWorker(tr("Failed to send document descriptor to worker: %1").arg(strerror(errno)));
            return false;
        }
        break;
    }

    ::close(docFd);

    if (sent < packet.size()) {
        const char* remainingData = packet.constData() + sent;
        size_t remainingBytes = static_cast<size_t>(packet.size() - sent);
        while (remainingBytes > 0) {
            ssize_t n = ::write(sockfd, remainingData, remainingBytes);
            if (n < 0) {
                if (errno == EINTR) continue;
                failWorker(tr("Failed to complete sending packet to worker: %1").arg(strerror(errno)));
                return false;
            }
            remainingData += n;
            remainingBytes -= static_cast<size_t>(n);
        }
    }

    return true;
#else
    (void)docFd;
    return sendPacket(MessageType::OpenDocumentRequest, serialize(request));
#endif
}

void WorkerClient::flushOutgoing() {
    for (const QByteArray& packet : m_outgoingPackets) {
        if (m_socket.write(packet) != packet.size()) {
            failWorker(tr("Could not send a request to the PDF worker."));
            return;
        }
    }
    m_outgoingPackets.clear();
    m_outgoingBytes = 0;
}

OpenResult WorkerClient::openDocumentSync(int64_t documentId, const std::string& filePath,
                                          const std::string& password) {
#ifdef Q_OS_LINUX
    const QString qPath = QString::fromStdString(filePath);
    const QFileInfo fileInfo(qPath);
    const std::string absPath = fileInfo.exists() ? fileInfo.absoluteFilePath().toStdString() : filePath;
    int docFd = ::open(absPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (docFd < 0) {
        OpenResult res;
        res.error = DocumentError::FileNotFound;
        return res;
    }
    if (!ensureConnected()) {
        ::close(docFd);
        return failedOpenResult();
    }
#else
    if (!ensureConnected()) {
        return failedOpenResult();
    }
#endif
    if (m_watchdogEnabled && m_socket.state() == QLocalSocket::ConnectedState && m_watchdogIntervalMs > 0) {
        m_missedPings = 0;
        if (!m_watchdogTimer.isActive()) {
            m_watchdogTimer.start(m_watchdogIntervalMs);
        }
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    OpenResult result = failedOpenResult();
    bool received = false;
    const auto responseConnection = connect(this, &WorkerClient::documentOpened, &loop,
        [&](int64_t id, OpenResult response) {
            if (id == documentId) {
                result = std::move(response);
                received = true;
                loop.quit();
            }
        });
    const auto failedConnection = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
#ifdef Q_OS_LINUX
    OpenDocumentRequest request{documentId, QString::fromStdString(absPath),
                                QString::fromStdString(password)};
    sendOpenDocumentPacket(request, docFd);
#else
    OpenDocumentRequest request{documentId, QString::fromStdString(filePath),
                                QString::fromStdString(password)};
    sendMessage(MessageType::OpenDocumentRequest, serialize(request));
#endif
    timeout.start(RESPONSE_TIMEOUT_MS);
    loop.exec();
    disconnect(responseConnection);
    disconnect(failedConnection);
    if (!received && !m_failureReported) {
        failWorker(tr("Timed out waiting for the PDF worker to open the document."));
    }
    return result;
}

void WorkerClient::openDocumentAsync(int64_t documentId, const std::string& filePath,
                                     const std::string& password) {
    if (m_watchdogEnabled && m_socket.state() == QLocalSocket::ConnectedState && m_watchdogIntervalMs > 0) {
        m_missedPings = 0;
        if (!m_watchdogTimer.isActive()) {
            m_watchdogTimer.start(m_watchdogIntervalMs);
        }
    }
#ifdef Q_OS_LINUX
    const QString qPath = QString::fromStdString(filePath);
    const QFileInfo fileInfo(qPath);
    const std::string absPath = fileInfo.exists() ? fileInfo.absoluteFilePath().toStdString() : filePath;
    int docFd = ::open(absPath.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (docFd < 0) {
        QTimer::singleShot(0, this, [this, documentId]() {
            OpenResult res;
            res.error = DocumentError::FileNotFound;
            Q_EMIT documentOpened(documentId, res);
        });
        return;
    }
    OpenDocumentRequest request{documentId, QString::fromStdString(absPath),
                                QString::fromStdString(password)};
    sendOpenDocumentPacket(request, docFd);
#else
    OpenDocumentRequest request{documentId, QString::fromStdString(filePath),
                                QString::fromStdString(password)};
    sendMessage(MessageType::OpenDocumentRequest, serialize(request));
#endif
}

void WorkerClient::closeDocumentSync() {
    m_watchdogTimer.stop();
    m_missedPings = 0;
    if (!ensureConnected()) {
        return;
    }
    sendMessage(MessageType::CloseDocumentRequest, serialize(CloseDocumentRequest{}));
    waitForIdle();
}

void WorkerClient::requestDocumentOutline(int64_t documentId) {
    sendMessage(MessageType::GetOutlineRequest, serialize(GetOutlineRequest{documentId}));
}

void WorkerClient::setGeneration(uint64_t generation) {
    sendMessage(MessageType::SetGenerationRequest, serialize(SetGenerationRequest{generation}));
}

void WorkerClient::enqueueRequest(TileRenderRequest request) {
    sendMessage(MessageType::RenderTileRequest, serialize(RenderTileRequest{std::move(request)}));
}

void WorkerClient::enqueueRequests(std::vector<TileRenderRequest> requests) {
    for (auto& request : requests) {
        enqueueRequest(std::move(request));
    }
}

void WorkerClient::requestListAnnotations(int64_t documentId, int pageIndex) {
    if (m_stopping || m_failureReported) {
        Q_EMIT annotationsFailed(documentId, pageIndex);
        return;
    }
    sendMessage(MessageType::ListAnnotationsRequest,
                serialize(ListAnnotationsRequest{documentId, pageIndex}));
}

void WorkerClient::requestHitTestAnnotation(int64_t documentId, int pageIndex, double xPoints, double yPoints) {
    if (m_stopping || m_failureReported) {
        Q_EMIT annotationHitTestFailed(documentId, pageIndex);
        return;
    }
    sendMessage(MessageType::HitTestAnnotationRequest,
                serialize(HitTestAnnotationRequest{documentId, pageIndex, xPoints, yPoints}));
}

ListAnnotationsResponse WorkerClient::listAnnotationsSync(int64_t documentId, int pageIndex, int timeoutMs) {
    if (!ensureConnected()) {
        ListAnnotationsResponse failed;
        failed.documentId = documentId;
        failed.pageIndex = pageIndex;
        failed.success = false;
        return failed;
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    ListAnnotationsResponse result;
    result.documentId = documentId;
    result.pageIndex = pageIndex;
    result.success = false;

    const auto readyConn = connect(this, &WorkerClient::annotationsReady, &loop,
        [&](int64_t docId, int pIdx, const std::vector<pdf::AnnotationItem>& annots, bool trunc) {
            if (docId == documentId && pIdx == pageIndex) {
                result.success = true;
                result.truncated = trunc;
                result.annotations = annots;
                loop.quit();
            }
        });
    const auto failConn = connect(this, &WorkerClient::annotationsFailed, &loop,
        [&](int64_t docId, int pIdx) {
            if (docId == documentId && pIdx == pageIndex) {
                result.success = false;
                loop.quit();
            }
        });
    const auto workerFailConn = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestListAnnotations(documentId, pageIndex);
    timeout.start(timeoutMs);
    loop.exec();

    disconnect(readyConn);
    disconnect(failConn);
    disconnect(workerFailConn);
    return result;
}

HitTestAnnotationResponse WorkerClient::hitTestAnnotationSync(int64_t documentId, int pageIndex, double xPoints, double yPoints, int timeoutMs) {
    if (!ensureConnected()) {
        HitTestAnnotationResponse failed;
        failed.documentId = documentId;
        failed.pageIndex = pageIndex;
        failed.hit = false;
        return failed;
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    HitTestAnnotationResponse result;
    result.documentId = documentId;
    result.pageIndex = pageIndex;
    result.hit = false;

    const auto readyConn = connect(this, &WorkerClient::annotationHitTestReady, &loop,
        [&](int64_t docId, int pIdx, bool hit, const pdf::AnnotationItem& annot) {
            if (docId == documentId && pIdx == pageIndex) {
                result.hit = hit;
                result.annotation = annot;
                loop.quit();
            }
        });
    const auto failConn = connect(this, &WorkerClient::annotationHitTestFailed, &loop,
        [&](int64_t docId, int pIdx) {
            if (docId == documentId && pIdx == pageIndex) {
                result.hit = false;
                loop.quit();
            }
        });
    const auto workerFailConn = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestHitTestAnnotation(documentId, pageIndex, xPoints, yPoints);
    timeout.start(timeoutMs);
    loop.exec();

    disconnect(readyConn);
    disconnect(failConn);
    disconnect(workerFailConn);
    return result;
}

void WorkerClient::requestPageText(int64_t documentId, int pageIndex) {
    sendMessage(MessageType::PageTextRequest, serialize(PageTextRequest{documentId, pageIndex}));
}

std::shared_ptr<const PageText> WorkerClient::getPageTextSync(int64_t documentId, int pageIndex) {
    if (!ensureConnected()) {
        return {};
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    std::shared_ptr<const PageText> result;
    bool received = false;
    const auto responseConnection = connect(this, &WorkerClient::pageTextReady, &loop,
        [&](int64_t id, int page, std::shared_ptr<PageText> text) {
            if (id == documentId && page == pageIndex) {
                result = std::move(text);
                received = true;
                loop.quit();
            }
        });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    requestPageText(documentId, pageIndex);
    timeout.start(RESPONSE_TIMEOUT_MS);
    loop.exec();
    disconnect(responseConnection);
    if (!received && !m_failureReported) {
        failWorker(tr("Timed out waiting for page text from the PDF worker."));
    }
    return result;
}

void WorkerClient::startSearch(int64_t documentId, int64_t searchId, const QString& query,
                               bool matchCase, int totalPages, int startPage) {
    sendMessage(MessageType::StartSearchRequest,
                serialize(StartSearchRequest{documentId, searchId, query, matchCase, totalPages, startPage}));
}

void WorkerClient::cancelSearch() {
    sendMessage(MessageType::CancelSearchRequest, serialize(CancelSearchRequest{}));
}

void WorkerClient::cancelAll() {
    sendMessage(MessageType::CancelRenderRequest, serialize(CancelRenderRequest{}));
}

WorkerControlResponse WorkerClient::requestControl(const WorkerControlRequest& request) {
    if (!ensureConnected()) {
        return {};
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    WorkerControlResponse response;
    bool received = false;
    WorkerControlRequest trackedRequest = request;
    trackedRequest.requestId = m_nextControlRequestId++;
    const auto responseConnection = connect(this, &WorkerClient::workerControlReceived, &loop,
        [&](quint64 id, const WorkerControlResponse& incoming) {
            if (id == trackedRequest.requestId) {
                response = incoming;
                received = true;
                loop.quit();
            }
        });
    const auto failedConnection = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    sendMessage(MessageType::WorkerControlRequest, serialize(trackedRequest));
    timeout.start(RESPONSE_TIMEOUT_MS);
    if (!m_failureReported) {
        loop.exec();
    }
    disconnect(responseConnection);
    disconnect(failedConnection);
    if (!received && !m_failureReported) {
        failWorker(tr("Timed out waiting for the PDF worker response."));
    }
    return response;
}

void WorkerClient::waitForIdle() {
    requestControl(WorkerControlRequest{0, -1, -1, true, false});
}

size_t WorkerClient::pendingCount() {
    return static_cast<size_t>(requestControl(WorkerControlRequest{0, -1, -1, false, true}).pendingCount);
}

size_t WorkerClient::cachedPageCount() {
    return static_cast<size_t>(requestControl(WorkerControlRequest{0, -1, -1, false, true}).cachedPageCount);
}

void WorkerClient::setRenderDelayForTesting(std::chrono::milliseconds delay) {
    sendMessage(MessageType::WorkerControlRequest,
                serialize(WorkerControlRequest{0, static_cast<int>(delay.count()), -1, false, false}));
}

void WorkerClient::setOpenDelayForTesting(std::chrono::milliseconds delay) {
    sendMessage(MessageType::WorkerControlRequest,
                serialize(WorkerControlRequest{0, -1, static_cast<int>(delay.count()), false, false}));
}

void WorkerClient::killWorkerForTesting() {
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
    }
}

void WorkerClient::requestThumbnail(int64_t documentId, int pageIndex, int targetWidth, int rotation) {
    if (m_stopping || m_failureReported) {
        Q_EMIT thumbnailFailed(documentId, pageIndex);
        return;
    }
    sendMessage(MessageType::ThumbnailRequest,
                serialize(ThumbnailRequest{documentId, pageIndex, targetWidth, rotation}));
}

void WorkerClient::requestPrintPage(int64_t documentId, int pageIndex, double scale, double dpr, int rotation) {
    if (m_stopping || m_failureReported) {
        Q_EMIT printPageFailed(documentId, pageIndex);
        return;
    }
    sendMessage(MessageType::RenderPageRequest,
                serialize(RenderPageRequest{documentId, pageIndex, scale, dpr, rotation}));
}

pdf::PixelBuffer WorkerClient::renderPageForPrintSync(int64_t documentId, int pageIndex, double scale, double dpr, int rotation, int timeoutMs) {
    if (!ensureConnected()) {
        return {};
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    pdf::PixelBuffer result;

    const auto readyConn = connect(this, &WorkerClient::printPageReady, &loop,
        [&](int64_t docId, int pIdx, const pdf::PixelBuffer& buf) {
            if (docId == documentId && pIdx == pageIndex) {
                result = buf;
                loop.quit();
            }
        });
    const auto failConn = connect(this, &WorkerClient::printPageFailed, &loop,
        [&](int64_t docId, int pIdx) {
            if (docId == documentId && pIdx == pageIndex) {
                loop.quit();
            }
        });
    const auto workerFailConn = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestPrintPage(documentId, pageIndex, scale, dpr, rotation);
    timeout.start(timeoutMs);
    loop.exec();

    disconnect(readyConn);
    disconnect(failConn);
    disconnect(workerFailConn);

    return result;
}

void WorkerClient::requestExtractPages(int64_t documentId, int firstPage, int lastPage) {
    if (m_stopping || m_failureReported) {
        Q_EMIT extractPagesFailed(documentId, firstPage, lastPage);
        return;
    }
    sendMessage(MessageType::ExtractPagesRequest,
                serialize(ExtractPagesRequest{documentId, firstPage, lastPage}));
}

QByteArray WorkerClient::extractPagesSync(int64_t documentId, int firstPage, int lastPage, int timeoutMs) {
    if (!ensureConnected()) {
        return {};
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QByteArray result;

    const auto readyConn = connect(this, &WorkerClient::extractPagesReady, &loop,
        [&](int64_t docId, int a, int b, const QByteArray& bytes) {
            if (docId == documentId && a == firstPage && b == lastPage) {
                result = bytes;
                loop.quit();
            }
        });
    const auto failConn = connect(this, &WorkerClient::extractPagesFailed, &loop,
        [&](int64_t docId, int a, int b) {
            if (docId == documentId && a == firstPage && b == lastPage) {
                loop.quit();
            }
        });
    const auto workerFailConn = connect(this, &WorkerClient::workerFailed, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    requestExtractPages(documentId, firstPage, lastPage);
    timeout.start(timeoutMs);
    loop.exec();

    disconnect(readyConn);
    disconnect(failConn);
    disconnect(workerFailConn);
    return result;
}

void WorkerClient::requestRedactDocument(int64_t documentId,
                                         const std::vector<PageRedactionItem>& redactions,
                                         double dpi,
                                         int edgeGrowPx) {
    if (m_stopping || m_failureReported) {
        Q_EMIT redactDocumentFailed(documentId, tr("Worker is stopped or failed."));
        return;
    }
    sendMessage(MessageType::RedactDocumentRequest,
                serialize(RedactDocumentRequest{documentId, redactions, dpi, edgeGrowPx}));
}

RedactDocumentResponse WorkerClient::redactDocumentSync(int64_t documentId,
                                                       const std::vector<PageRedactionItem>& redactions,
                                                       double dpi,
                                                       int edgeGrowPx,
                                                       int timeoutMs) {
    if (!ensureConnected()) {
        RedactDocumentResponse fail;
        fail.documentId = documentId;
        fail.success = false;
        fail.errorMessage = tr("Worker connection unavailable.");
        return fail;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    RedactDocumentResponse result;
    result.documentId = documentId;
    result.success = false;

    const auto readyConn = connect(this, &WorkerClient::redactDocumentReady, &loop,
        [&](int64_t docId, const RedactDocumentResponse& response) {
            if (docId == documentId) {
                result = response;
                loop.quit();
            }
        });

    const auto failConn = connect(this, &WorkerClient::redactDocumentFailed, &loop,
        [&](int64_t docId, const QString& error) {
            // docId 0: the worker could not parse the request far enough to know which document.
            if (docId == documentId || docId == 0) {
                result.documentId = docId;
                result.success = false;
                result.errorMessage = error;
                loop.quit();
            }
        });

    const auto workerFailConn = connect(this, &WorkerClient::workerFailed, &loop,
        [&](const QString& error) {
            result.documentId = documentId;
            result.success = false;
            result.errorMessage = error;
            loop.quit();
        });

    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    // The limit is a silence limit: every observed engine heartbeat restarts it.
    const auto progressConn = connect(this, &WorkerClient::engineProgressed, &loop,
        [&]() { timeout.start(timeoutMs); });

    requestRedactDocument(documentId, redactions, dpi, edgeGrowPx);
    timeout.start(timeoutMs);
    loop.exec();

    disconnect(readyConn);
    disconnect(failConn);
    disconnect(workerFailConn);
    disconnect(progressConn);

    if (!result.success && result.errorMessage.isEmpty()) {
        result.errorMessage = tr("The worker made no progress on the redaction within the time limit.");
    }
    return result;
}

void WorkerClient::stop() {
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    m_connectTimer.stop();
    m_watchdogTimer.stop();
    m_missedPings = 0;
    m_socket.abort();
    m_outgoingPackets.clear();
    m_outgoingBytes = 0;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.waitForFinished(2500);
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
            m_process.waitForFinished(1000);
        }
    }
    m_stopping = false;
}

void WorkerClient::onSocketReadyRead() {
    while (true) {
        if (static_cast<size_t>(m_incomingBuffer.size()) < sizeof(PacketHeader)) {
            const qint64 needed = static_cast<qint64>(sizeof(PacketHeader)) - m_incomingBuffer.size();
            m_incomingBuffer.append(m_socket.read(std::min(needed, m_socket.bytesAvailable())));
            if (static_cast<size_t>(m_incomingBuffer.size()) < sizeof(PacketHeader)) {
                return;
            }
        }
        PacketHeader header;
        std::memcpy(&header, m_incomingBuffer.constData(), sizeof(header));
        if (header.magic != IPC_MAGIC || header.version != IPC_PROTOCOL_VERSION ||
            header.payloadSize > MAX_IPC_PAYLOAD_SIZE || !isKnownMessageType(header.type)) {
            failWorker(tr("PDF worker sent an invalid IPC header."));
            return;
        }
        const size_t packetSize = sizeof(PacketHeader) + static_cast<size_t>(header.payloadSize);
        if (static_cast<size_t>(m_incomingBuffer.size()) < packetSize) {
            const qint64 needed = static_cast<qint64>(packetSize - static_cast<size_t>(m_incomingBuffer.size()));
            m_incomingBuffer.append(m_socket.read(std::min(needed, m_socket.bytesAvailable())));
            if (static_cast<size_t>(m_incomingBuffer.size()) < packetSize) {
                return;
            }
        }
        QByteArray payload = m_incomingBuffer.mid(static_cast<qsizetype>(sizeof(PacketHeader)),
                                                  static_cast<qsizetype>(header.payloadSize));
        m_incomingBuffer.remove(0, static_cast<qsizetype>(packetSize));
        dispatchMessage(static_cast<MessageType>(header.type), payload);
    }
}

void WorkerClient::dispatchMessage(MessageType type, const QByteArray& payload) {
    switch (type) {
        case MessageType::Pong: {
            PongResponse response;
            if (deserialize(payload, response)) {
                m_missedPings = 0;
                const auto now = std::chrono::steady_clock::now();
                if (response.currentOp == static_cast<uint32_t>(WorkerOp::Idle)) {
                    m_lastWorkerProgressEpochMs = response.lastProgressEpochMs;
                    m_lastWorkerOp = response.currentOp;
                    m_lastProgressAdvanceTime = now;
                } else {
                    if (response.lastProgressEpochMs != m_lastWorkerProgressEpochMs || m_lastWorkerOp != response.currentOp) {
                        m_lastWorkerProgressEpochMs = response.lastProgressEpochMs;
                        m_lastWorkerOp = response.currentOp;
                        m_lastProgressAdvanceTime = now;
                        Q_EMIT engineProgressed();
                    } else {
                        const auto elapsedStallMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastProgressAdvanceTime).count();
                        if (elapsedStallMs >= m_engineStallMs) {
                            std::cerr << "[WorkerClient] Worker engine stalled on op " << response.currentOp
                                      << " for " << elapsedStallMs << " ms (limit " << m_engineStallMs
                                      << " ms). Terminating hung worker." << std::endl;
                            m_watchdogTimer.stop();
                            m_process.kill();
                            static const char* const kOpNames[] = {"Idle", "Open", "Tile", "Thumbnail", "Print", "Extract", "Search", "Redact"};
                            const char* opName = response.currentOp < sizeof(kOpNames) / sizeof(kOpNames[0])
                                                     ? kOpNames[response.currentOp] : "Unknown";
                            failWorker(tr("PDF worker engine stalled (operation heartbeat frozen): %1 made no progress for %2 ms (limit %3 ms).")
                                           .arg(QLatin1String(opName)).arg(static_cast<qint64>(elapsedStallMs)).arg(m_engineStallMs));
                            return;
                        }
                    }
                }
            }
            break;
        }
        case MessageType::OpenDocumentResponse: {
            OpenDocumentResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT documentOpened(response.documentId, std::move(response.result));
            }
            break;
        }
        case MessageType::GetOutlineResponse: {
            GetOutlineResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT documentOutlineReady(response.documentId, response.outline);
            }
            break;
        }
        case MessageType::TileRenderedResponse: {
            TileRenderedResponse response;
            if (!deserialize(payload, response)) {
                break;
            }
            PixelBuffer buffer;
            uint64_t bytes = 0;
            if (response.width > 0 && response.height > 0 &&
                response.width <= MAX_TILE_DIMENSION && response.height <= MAX_TILE_DIMENSION &&
                response.stride >= response.width * 4 &&
                static_cast<uint64_t>(response.stride) * static_cast<uint64_t>(response.height) <= MAX_TILE_BYTES &&
                response.format <= static_cast<uint32_t>(PixelFormat::BGRA8888)) {
                bytes = static_cast<uint64_t>(response.stride) * static_cast<uint64_t>(response.height);
                buffer = PixelBuffer::allocate(response.width, response.height,
                                               static_cast<PixelFormat>(response.format));
                if (buffer.isValid()) {
                    if (response.isShm) {
                        // The worker is untrusted: validated name, size-checked, copied with pread (no mmap).
                        if (!readAndUnlinkSharedTile(response.shmKey.toStdString(),
                                                     static_cast<size_t>(response.height),
                                                     static_cast<size_t>(response.stride),
                                                     static_cast<size_t>(response.width) * 4,
                                                     buffer.data(), static_cast<size_t>(buffer.stride()))) {
                            buffer = {};
                        }
                        sendMessage(MessageType::SharedMemoryAck, serialize(SharedMemoryAck{response.shmKey}));
                    } else if (response.pixelData.size() == static_cast<qsizetype>(bytes)) {
                        for (int row = 0; row < response.height; ++row) {
                            std::memcpy(buffer.data() + static_cast<size_t>(row) * buffer.stride(),
                                        response.pixelData.constData() +
                                            static_cast<qsizetype>(row) * response.stride,
                                        static_cast<size_t>(response.width) * 4);
                        }
                    } else {
                        buffer = {};
                    }
                }
            }
            if (buffer.isValid()) {
                Q_EMIT tileReady(response.documentId, response.generation, response.key, buffer);
            } else {
                Q_EMIT tileFailed(response.documentId, response.generation, response.key);
            }
            break;
        }
        case MessageType::TileFailedResponse: {
            TileFailedResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT tileFailed(response.documentId, response.generation, response.key);
            }
            break;
        }
        case MessageType::PageTextResponse: {
            PageTextResponse response;
            if (deserialize(payload, response)) {
                auto text = response.success ? std::make_shared<PageText>(std::move(response.pageText)) : nullptr;
                Q_EMIT pageTextReady(response.documentId, response.pageIndex, std::move(text));
            }
            break;
        }
        case MessageType::SearchMatchesFoundResponse: {
            SearchMatchesFoundResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT searchMatchesFound(response.documentId, response.searchId, response.pageIndex, response.matches);
            }
            break;
        }
        case MessageType::SearchProgressResponse: {
            SearchProgressResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT searchProgress(response.documentId, response.searchId,
                                      response.pagesSearched, response.totalPages);
            }
            break;
        }
        case MessageType::SearchCompletedResponse: {
            SearchCompletedResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT searchCompleted(response.documentId, response.searchId,
                                        response.totalMatches, response.truncated);
            }
            break;
        }
        case MessageType::WorkerError: {
            WorkerErrorResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT workerFailed(response.errorMessage);
            }
            break;
        }
        case MessageType::WorkerControlResponse: {
            WorkerControlResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT workerControlReceived(response.requestId, response);
            }
            break;
        }
        case MessageType::ThumbnailResponse: {
            ThumbnailResponse response;
            if (deserialize(payload, response)) {
                constexpr size_t MAX_THUMBNAIL_BYTES = 16 * 1024 * 1024;
                if (response.width > 0 && response.height > 0 &&
                    response.width <= 2048 && response.height <= 2048 &&
                    response.stride > 0) {
                    const size_t width = static_cast<size_t>(response.width);
                    const size_t height = static_cast<size_t>(response.height);
                    const size_t stride = static_cast<size_t>(response.stride);
                    const size_t rowBytes = width * 4;
                    const bool validSize = stride >= rowBytes &&
                        height <= MAX_THUMBNAIL_BYTES / stride &&
                        stride * height <= static_cast<size_t>(response.pixelData.size());
                    if (!validSize) {
                        Q_EMIT thumbnailFailed(response.documentId, response.pageIndex);
                        break;
                    }

                    PixelBuffer buf = PixelBuffer::allocate(response.width, response.height, PixelFormat::BGRX8888);
                    if (buf.isValid()) {
                        for (int row = 0; row < response.height; ++row) {
                            std::memcpy(buf.data() + static_cast<size_t>(row) * buf.stride(),
                                        response.pixelData.constData() + static_cast<size_t>(row) * stride,
                                        rowBytes);
                        }
                        Q_EMIT thumbnailReady(response.documentId, response.pageIndex, buf);
                    } else {
                        Q_EMIT thumbnailFailed(response.documentId, response.pageIndex);
                    }
                } else {
                    Q_EMIT thumbnailFailed(response.documentId, response.pageIndex);
                }
            }
            break;
        }
        case MessageType::ThumbnailFailedResponse: {
            ThumbnailFailedResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT thumbnailFailed(response.documentId, response.pageIndex);
            }
            break;
        }
        case MessageType::RenderPageResponse: {
            RenderPageResponse response;
            if (deserialize(payload, response)) {
                if (response.width > 0 && response.height > 0 &&
                    response.width <= 16384 && response.height <= 16384 &&
                    response.stride >= response.width * 4 &&
                    response.pixelData.size() >= response.stride * response.height) {
                    PixelBuffer buf = PixelBuffer::allocate(response.width, response.height, PixelFormat::BGRX8888);
                    if (buf.isValid()) {
                        std::memcpy(buf.data(), response.pixelData.constData(), buf.sizeBytes());
                        Q_EMIT printPageReady(response.documentId, response.pageIndex, buf);
                    } else {
                        Q_EMIT printPageFailed(response.documentId, response.pageIndex);
                    }
                } else {
                    Q_EMIT printPageFailed(response.documentId, response.pageIndex);
                }
            }
            break;
        }
        case MessageType::RenderPageFailedResponse: {
            RenderPageFailedResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT printPageFailed(response.documentId, response.pageIndex);
            }
            break;
        }
        case MessageType::ExtractPagesResponse: {
            ExtractPagesResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT extractPagesReady(response.documentId, response.firstPage, response.lastPage, response.pdfBytes);
            }
            break;
        }
        case MessageType::ExtractPagesFailedResponse: {
            ExtractPagesFailedResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT extractPagesFailed(response.documentId, response.firstPage, response.lastPage);
            }
            break;
        }
        case MessageType::RedactDocumentResponse: {
            RedactDocumentResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT redactDocumentReady(response.documentId, response);
            }
            break;
        }
        case MessageType::ListAnnotationsResponse: {
            ListAnnotationsResponse response;
            if (deserialize(payload, response)) {
                if (response.success) {
                    Q_EMIT annotationsReady(response.documentId, response.pageIndex, response.annotations, response.truncated);
                } else {
                    Q_EMIT annotationsFailed(response.documentId, response.pageIndex);
                }
            } else {
                Q_EMIT annotationsFailed(0, 0);
            }
            break;
        }

        case MessageType::HitTestAnnotationResponse: {
            HitTestAnnotationResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT annotationHitTestReady(response.documentId, response.pageIndex, response.hit, response.annotation);
            } else {
                Q_EMIT annotationHitTestFailed(0, 0);
            }
            break;
        }

        case MessageType::RedactDocumentFailedResponse: {
            RedactDocumentFailedResponse response;
            if (deserialize(payload, response)) {
                Q_EMIT redactDocumentFailed(response.documentId, response.errorMessage);
            }
            break;
        }

        default:
            failWorker(tr("PDF worker sent an unexpected message."));
            break;
    }
}


void WorkerClient::setWatchdogEnabled(bool enabled) {
    m_watchdogEnabled = enabled;
    if (!enabled) {
        m_watchdogTimer.stop();
        m_missedPings = 0;
    } else if (m_socket.state() == QLocalSocket::ConnectedState && m_watchdogIntervalMs > 0) {
        m_missedPings = 0;
        m_watchdogTimer.start(m_watchdogIntervalMs);
    }
}

void WorkerClient::setWatchdogParams(int intervalMs, int maxMissedPings, int engineStallMs) {
    m_watchdogIntervalMs = intervalMs;
    m_maxMissedPings = maxMissedPings;
    m_engineStallMs = engineStallMs;
    m_lastProgressAdvanceTime = std::chrono::steady_clock::now();
    if (m_watchdogEnabled && m_socket.state() == QLocalSocket::ConnectedState && intervalMs > 0) {
        m_missedPings = 0;
        m_watchdogTimer.start(intervalMs);
    }
}

void WorkerClient::onWatchdogTimeout() {
    if (m_process.state() == QProcess::NotRunning || m_socket.state() != QLocalSocket::ConnectedState) {
        m_watchdogTimer.stop();
        return;
    }
    if (m_missedPings >= m_maxMissedPings) {
        std::cerr << "[WorkerClient] Worker heartbeat timed out (" << m_missedPings
                  << " missed pings). Terminating unresponsive worker." << std::endl;
        m_watchdogTimer.stop();
        m_process.kill();
        failWorker(tr("PDF worker became unresponsive (heartbeat timeout)."));
        return;
    }
    m_missedPings++;
    sendMessage(MessageType::Ping, serialize(PingRequest{m_nextControlRequestId++}));
}

#ifdef PDF_WORKER_TEST_HOOKS
void WorkerClient::hangEngineForTesting() {
    WorkerControlRequest req;
    req.requestId = m_nextControlRequestId++;
    req.hangEngine = true;
    sendMessage(MessageType::WorkerControlRequest, serialize(req));
}
#endif

} // namespace pdf::ipc
