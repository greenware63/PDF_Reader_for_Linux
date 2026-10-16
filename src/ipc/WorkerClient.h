#pragma once

#include "ipc/IpcMessages.h"

#include <QObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTimer>
#include <chrono>
#include <memory>
#include <vector>

namespace pdf::ipc {

class WorkerClient final : public QObject {
    Q_OBJECT

public:
    explicit WorkerClient(QObject* parent = nullptr);
    ~WorkerClient() override;

    OpenResult openDocumentSync(int64_t documentId, const std::string& filePath,
                                const std::string& password = {});
    void openDocumentAsync(int64_t documentId, const std::string& filePath,
                           const std::string& password = {});
    void closeDocumentSync();
    void requestDocumentOutline(int64_t documentId);
    void setGeneration(uint64_t generation);
    void enqueueRequest(TileRenderRequest request);
    void enqueueRequests(std::vector<TileRenderRequest> requests);
    void requestPageText(int64_t documentId, int pageIndex);
    std::shared_ptr<const PageText> getPageTextSync(int64_t documentId, int pageIndex);
    void startSearch(int64_t documentId, int64_t searchId, const QString& query,
                     bool matchCase, int totalPages, int startPage = 0);
    void cancelSearch();
    void cancelAll();
    void waitForIdle();
    void stop();
    size_t pendingCount();
    size_t cachedPageCount();
    void setRenderDelayForTesting(std::chrono::milliseconds delay);
    void setOpenDelayForTesting(std::chrono::milliseconds delay);
    void killWorkerForTesting();
    void requestThumbnail(int64_t documentId, int pageIndex, int targetWidth = 140);
    void requestPrintPage(int64_t documentId, int pageIndex, double scale = 1.0, double dpr = 2.0);
    void requestExtractPages(int64_t documentId, int firstPage, int lastPage);
    QByteArray extractPagesSync(int64_t documentId, int firstPage, int lastPage, int timeoutMs = 60000);
    pdf::PixelBuffer renderPageForPrintSync(int64_t documentId, int pageIndex, double scale = 1.0, double dpr = 2.0, int timeoutMs = 10000);

Q_SIGNALS:
    void tileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key,
                   const pdf::PixelBuffer& buffer);
    void tileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);
    void documentOpened(int64_t documentId, pdf::OpenResult result);
    void documentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline);
    void pageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text);
    void searchMatchesFound(int64_t documentId, int64_t searchId, int pageIndex,
                            const std::vector<pdf::SearchMatch>& matches);
    void searchProgress(int64_t documentId, int64_t searchId, int pagesSearched, int totalPages);
    void searchCompleted(int64_t documentId, int64_t searchId, int totalMatches, bool truncated);
    void workerFailed(const QString& message);
    void workerControlReceived(quint64 requestId, const pdf::ipc::WorkerControlResponse& response);
    void thumbnailReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer);
    void thumbnailFailed(int64_t documentId, int pageIndex);
    void printPageReady(int64_t documentId, int pageIndex, const pdf::PixelBuffer& buffer);
    void printPageFailed(int64_t documentId, int pageIndex);
    void extractPagesReady(int64_t documentId, int firstPage, int lastPage, const QByteArray& pdfBytes);
    void extractPagesFailed(int64_t documentId, int firstPage, int lastPage);

private Q_SLOTS:
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketError(QLocalSocket::LocalSocketError error);
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    void startWorker();
    bool ensureConnected();
    void tryConnect();
    void sendMessage(MessageType type, const QByteArray& payload);
    bool sendPacket(MessageType type, const QByteArray& payload);
    void flushOutgoing();
    void failWorker(const QString& message);
    WorkerControlResponse requestControl(const WorkerControlRequest& request);
    void dispatchMessage(MessageType type, const QByteArray& payload);

    QProcess m_process;
    QLocalSocket m_socket;
    QTimer m_connectTimer;
    QString m_serverName;
    QByteArray m_incomingBuffer;
    std::vector<QByteArray> m_outgoingPackets;
    size_t m_outgoingBytes{0};
    bool m_stopping{false};
    bool m_failureReported{false};
    quint64 m_nextControlRequestId{1};
};

} // namespace pdf::ipc
