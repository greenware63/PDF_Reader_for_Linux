#pragma once

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QByteArray>
#include "ipc/SharedTile.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include "ipc/IpcProtocol.h"
#include "ipc/IpcMessages.h"
#include "engine/RenderWorker.h"

namespace pdf::worker {

class WorkerServer : public QObject {
    Q_OBJECT

public:
    explicit WorkerServer(const QString& serverName, QObject* parent = nullptr);
    ~WorkerServer() override;

    bool start();
#ifdef PDF_WORKER_TEST_HOOKS
    void setHangEngineForTesting(bool hang) { m_engineWorker.setHangEngineForTesting(hang); }
#endif

Q_SIGNALS:
    void clientDisconnected();

private Q_SLOTS:
    void onNewConnection();
    void onReadyRead();
    void onSocketDisconnected();
    void onIdle();

    // RenderWorker forwarders
    void onDocumentOpened(int64_t documentId, pdf::OpenResult result);
    void onTileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key, const pdf::PixelBuffer& buffer);
    void onTileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);
    void onDocumentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline);
    void onPageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text);
    void onSearchMatchesFound(int64_t documentId, int64_t searchId, int pageIndex, const std::vector<pdf::SearchMatch>& matches);
    void onSearchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages);
    void onSearchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated);
    void onThumbnailReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer);
    void onThumbnailFailed(int64_t documentId, int pageIndex);
    void onPrintPageReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer);
    void onPrintPageFailed(int64_t documentId, int pageIndex);
    void onExtractPagesReady(int64_t documentId, int firstPage, int lastPage, const QByteArray& pdfBytes);
    void onExtractPagesFailed(int64_t documentId, int firstPage, int lastPage);
    void onRedactDocumentReady(int64_t documentId, const pdf::ipc::RedactDocumentResponse& response);
    void onRedactDocumentFailed(int64_t documentId, const QString& errorMessage);

private:
    void dispatchMessage(ipc::MessageType type, const QByteArray& payload);
    void sendPacket(ipc::MessageType type, const QByteArray& payload);

    QString m_serverName;
    QLocalServer* m_server{nullptr};
    QLocalSocket* m_clientSocket{nullptr};
    QByteArray m_incomingBuffer;
    std::unordered_map<QString, std::unique_ptr<ipc::SharedTileSegment>> m_sharedTiles;
    std::vector<ipc::WorkerControlRequest> m_pendingControlRequests;

    pdf::RenderWorker m_engineWorker;
};

} // namespace pdf::worker
