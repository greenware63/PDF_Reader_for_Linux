#include "WorkerServer.h"
#include <QCoreApplication>
#include <QRandomGenerator>
#include <algorithm>
#include <iostream>
#include <cstring>

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
}

WorkerServer::~WorkerServer() {
    m_engineWorker.stop();
    if (m_clientSocket) {
        m_clientSocket->close();
    }
    if (m_server) {
        m_server->close();
#ifndef Q_OS_LINUX
        QLocalServer::removeServer(m_serverName);
#endif
    }
}

bool WorkerServer::start() {
#ifndef Q_OS_LINUX
    QLocalServer::removeServer(m_serverName);
#endif
    m_server = new QLocalServer(this);
#ifdef Q_OS_LINUX
    m_server->setSocketOptions(QLocalServer::AbstractNamespaceOption);
#endif
    connect(m_server, &QLocalServer::newConnection, this, &WorkerServer::onNewConnection);
    return m_server->listen(m_serverName);
}

void WorkerServer::onNewConnection() {
    if (m_clientSocket) {
        // Only one active UI client per worker instance
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
}

void WorkerServer::onSocketDisconnected() {
    m_incomingBuffer.clear();
    if (m_clientSocket) {
        m_clientSocket->deleteLater();
        m_clientSocket = nullptr;
    }
    Q_EMIT clientDisconnected();
}

void WorkerServer::onReadyRead() {
    if (!m_clientSocket) {
        return;
    }

    while (true) {
        if (static_cast<size_t>(m_incomingBuffer.size()) < sizeof(ipc::PacketHeader)) {
            const qint64 needed = static_cast<qint64>(sizeof(ipc::PacketHeader)) - m_incomingBuffer.size();
            m_incomingBuffer.append(m_clientSocket->read(std::min(needed, m_clientSocket->bytesAvailable())));
            if (static_cast<size_t>(m_incomingBuffer.size()) < sizeof(ipc::PacketHeader)) {
                return;
            }
        }
        ipc::PacketHeader header;
        std::memcpy(&header, m_incomingBuffer.constData(), sizeof(ipc::PacketHeader));

        if (header.magic != ipc::IPC_MAGIC ||
            header.version != ipc::IPC_PROTOCOL_VERSION ||
            header.payloadSize > ipc::MAX_IPC_PAYLOAD_SIZE ||
            !isRequestType(header.type)) {
            std::cerr << "[WorkerServer] Protocol error: Invalid header received. Disconnecting." << std::endl;
            m_clientSocket->disconnectFromServer();
            return;
        }

        const size_t totalPacketSize = sizeof(ipc::PacketHeader) + header.payloadSize;
        if (static_cast<size_t>(m_incomingBuffer.size()) < totalPacketSize) {
            const qint64 needed = static_cast<qint64>(totalPacketSize - static_cast<size_t>(m_incomingBuffer.size()));
            m_incomingBuffer.append(m_clientSocket->read(std::min(needed, m_clientSocket->bytesAvailable())));
            if (static_cast<size_t>(m_incomingBuffer.size()) < totalPacketSize) {
                return;
            }
        }

        QByteArray payload;
        if (header.payloadSize > 0) {
            payload = m_incomingBuffer.mid(static_cast<qsizetype>(sizeof(ipc::PacketHeader)),
                                           static_cast<qsizetype>(header.payloadSize));
        }
        m_incomingBuffer.remove(0, static_cast<qsizetype>(totalPacketSize));

        dispatchMessage(static_cast<ipc::MessageType>(header.type), payload);
    }
}

void WorkerServer::dispatchMessage(ipc::MessageType type, const QByteArray& payload) {
    switch (type) {
        case ipc::MessageType::Ping: {
            ipc::PingRequest req;
            if (ipc::deserialize(payload, req)) {
                ipc::PongResponse resp{req.seq};
                sendPacket(ipc::MessageType::Pong, ipc::serialize(resp));
            }
            break;
        }

        case ipc::MessageType::OpenDocumentRequest: {
            ipc::OpenDocumentRequest req;
            if (ipc::deserialize(payload, req)) {
                m_engineWorker.openDocumentAsync(req.documentId, req.filePath.toStdString(), req.password.toStdString());
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
            if (request.renderDelayMs >= 0) {
                m_engineWorker.setRenderDelayForTesting(std::chrono::milliseconds(request.renderDelayMs));
            }
            if (request.openDelayMs >= 0) {
                m_engineWorker.setOpenDelayForTesting(std::chrono::milliseconds(request.openDelayMs));
            }
            if (request.waitForIdle) {
                m_engineWorker.waitForIdle();
                QCoreApplication::processEvents();
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

void WorkerServer::sendPacket(ipc::MessageType type, const QByteArray& payload) {
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
            if (m_clientSocket) {
                m_clientSocket->disconnectFromServer();
            }
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

} // namespace pdf::worker
