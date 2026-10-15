#include "WorkerServer.h"
#include <QCoreApplication>
#include <iostream>

namespace pdf::worker {

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
        QLocalServer::removeServer(m_serverName);
    }
}

bool WorkerServer::start() {
    QLocalServer::removeServer(m_serverName);
    m_server = new QLocalServer(this);
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

    m_incomingBuffer.append(m_clientSocket->readAll());

    while (static_cast<size_t>(m_incomingBuffer.size()) >= sizeof(ipc::PacketHeader)) {
        ipc::PacketHeader header;
        std::memcpy(&header, m_incomingBuffer.constData(), sizeof(ipc::PacketHeader));

        if (header.magic != ipc::IPC_MAGIC) {
            std::cerr << "[WorkerServer] Protocol error: Invalid magic header received. Disconnecting." << std::endl;
            m_clientSocket->disconnectFromServer();
            return;
        }

        const size_t totalPacketSize = sizeof(ipc::PacketHeader) + header.payloadSize;
        if (static_cast<size_t>(m_incomingBuffer.size()) < totalPacketSize) {
            // Incomplete packet; wait for further chunks
            break;
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
            m_engineWorker.closeDocumentSync();
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
            m_engineWorker.cancelAll();
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
            m_engineWorker.cancelSearch();
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
        resp.isShm = false;
        resp.pixelData = QByteArray(reinterpret_cast<const char*>(buffer.data()),
                                    static_cast<qsizetype>(buffer.sizeBytes()));
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
