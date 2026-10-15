#include "WorkerClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSharedMemory>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <limits>

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
    if (!ensureConnected()) {
        return failedOpenResult();
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
    OpenDocumentRequest request{documentId, QString::fromStdString(filePath),
                                QString::fromStdString(password)};
    sendMessage(MessageType::OpenDocumentRequest, serialize(request));
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
    sendMessage(MessageType::OpenDocumentRequest,
                serialize(OpenDocumentRequest{documentId, QString::fromStdString(filePath),
                                              QString::fromStdString(password)}));
}

void WorkerClient::closeDocumentSync() {
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

void WorkerClient::stop() {
    if (m_stopping) {
        return;
    }
    m_stopping = true;
    m_connectTimer.stop();
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
                        QSharedMemory memory(response.shmKey);
                        if (memory.attach(QSharedMemory::ReadOnly) && memory.size() >= static_cast<int>(bytes)) {
                            const size_t rowBytes = static_cast<size_t>(response.width) * 4;
                            for (int row = 0; row < response.height; ++row) {
                                std::memcpy(buffer.data() + static_cast<size_t>(row) * buffer.stride(),
                                            static_cast<const uint8_t*>(memory.constData()) +
                                                static_cast<size_t>(row) * response.stride,
                                            rowBytes);
                            }
                            memory.detach();
                        } else {
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
        case MessageType::Pong:
            break;
        default:
            failWorker(tr("PDF worker sent an unexpected message."));
            break;
    }
}

} // namespace pdf::ipc
