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
    void requestListAnnotations(int64_t documentId, int pageIndex);
    void requestHitTestAnnotation(int64_t documentId, int pageIndex, double xPoints, double yPoints);
    ListAnnotationsResponse listAnnotationsSync(int64_t documentId, int pageIndex, int timeoutMs = 10000);
    HitTestAnnotationResponse hitTestAnnotationSync(int64_t documentId, int pageIndex, double xPoints, double yPoints, int timeoutMs = 5000);
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
    void setWatchdogEnabled(bool enabled);
    bool isWatchdogEnabled() const noexcept { return m_watchdogEnabled; }
    bool isWatchdogActive() const noexcept { return m_watchdogTimer.isActive(); }
    void setWatchdogParams(int intervalMs, int maxMissedPings, int engineStallMs = 60000);
    qint64 workerPid() const noexcept { return m_process.processId(); }
#ifdef PDF_WORKER_TEST_HOOKS
    void hangEngineForTesting();
#endif
    void requestThumbnail(int64_t documentId, int pageIndex, int targetWidth = 140, int rotation = 0);
    void requestPrintPage(int64_t documentId, int pageIndex, double scale = 1.0, double dpr = 2.0, int rotation = 0);
    void requestExtractPages(int64_t documentId, int firstPage, int lastPage);
    QByteArray extractPagesSync(int64_t documentId, int firstPage, int lastPage, int timeoutMs = 60000);
    pdf::PixelBuffer renderPageForPrintSync(int64_t documentId, int pageIndex, double scale = 1.0, double dpr = 2.0, int rotation = 0, int timeoutMs = 10000);
    void requestRedactDocument(int64_t documentId,
                               const std::vector<PageRedactionItem>& redactions,
                               double dpi = 150.0,
                               int edgeGrowPx = 1);
    /// Blocks (nested event loop) until the redaction finishes. @p timeoutMs is a SILENCE limit, not a
    /// total budget: it restarts every time the worker's engine heartbeat advances, so a long document
    /// that keeps making progress completes, while a stalled worker still fails after @p timeoutMs (and
    /// the watchdog, when enabled, terminates it).
    RedactDocumentResponse redactDocumentSync(int64_t documentId,
                                             const std::vector<PageRedactionItem>& redactions,
                                             double dpi = 150.0,
                                             int edgeGrowPx = 1,
                                             int timeoutMs = 60000);

    void requestAddAnnotation(int64_t documentId, int pageIndex, const pdf::AnnotationItem& annotation);
    void requestRemoveAnnotation(int64_t documentId, int pageIndex, const QString& annotationName);
    RemoveAnnotationResponse removeAnnotationSync(int64_t documentId, int pageIndex, const QString& annotationName,
                                                  int timeoutMs = 10000);
    AddAnnotationResponse addAnnotationSync(int64_t documentId, int pageIndex,
                                            const pdf::AnnotationItem& annotation,
                                            int timeoutMs = 10000);

    void requestSaveDocument(int64_t documentId, int writeFd, bool incremental, const QString& tempFilePath = "");
    SaveDocumentResponse saveDocumentSync(int64_t documentId, int writeFd, bool incremental,
                                          const QString& tempFilePath = "", int timeoutMs = 60000);

Q_SIGNALS:
    void tileReady(int64_t documentId, uint64_t generation, const pdf::TileKey& key,
                   const pdf::PixelBuffer& buffer);
    void tileFailed(int64_t documentId, uint64_t generation, const pdf::TileKey& key);
    void documentOpened(int64_t documentId, pdf::OpenResult result);
    void documentOutlineReady(int64_t documentId, const std::vector<pdf::OutlineItem>& outline);
    void pageTextReady(int64_t documentId, int pageIndex, std::shared_ptr<pdf::PageText> text);
    void annotationsReady(int64_t documentId, int pageIndex, const std::vector<pdf::AnnotationItem>& annotations, bool truncated);
    void annotationsFailed(int64_t documentId, int pageIndex);
    void annotationHitTestReady(int64_t documentId, int pageIndex, bool hit, const pdf::AnnotationItem& annotation);
    void annotationHitTestFailed(int64_t documentId, int pageIndex);
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
    void redactDocumentReady(int64_t documentId, const pdf::ipc::RedactDocumentResponse& response);
    void redactDocumentFailed(int64_t documentId, const QString& errorMessage);
    void annotationAdded(int64_t documentId, int pageIndex, const QString& annotationName);
    void annotationAddFailed(int64_t documentId, int pageIndex, const QString& errorMessage);
    void annotationRemoved(int64_t documentId, int pageIndex, int removedCount);
    void annotationRemoveFailed(int64_t documentId, int pageIndex, const QString& errorMessage);
    void documentSaved(int64_t documentId, uint64_t bytesWritten);
    void documentSaveFailed(int64_t documentId, const QString& errorMessage);
    /// The worker's engine heartbeat advanced while busy (observed through the watchdog ping).
    void engineProgressed();

private Q_SLOTS:
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketError(QLocalSocket::LocalSocketError error);
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onWatchdogTimeout();

private:
    void startWorker();
    bool ensureConnected();
    void tryConnect();
    void sendMessage(MessageType type, const QByteArray& payload);
    bool sendPacket(MessageType type, const QByteArray& payload);
    bool sendOpenDocumentPacket(const OpenDocumentRequest& request, int docFd);
    bool sendPacketWithFd(MessageType type, const QByteArray& payload, int fd);
    bool sendSaveDocumentPacket(const SaveDocumentRequest& request, int saveFd);
    void flushOutgoing();
    void failWorker(const QString& message);
    WorkerControlResponse requestControl(const WorkerControlRequest& request);
    void dispatchMessage(MessageType type, const QByteArray& payload);

    QProcess m_process;
    QLocalSocket m_socket;
    QTimer m_connectTimer;
    QTimer m_watchdogTimer;
    int m_watchdogIntervalMs{2000};
    int m_maxMissedPings{3};
    int m_engineStallMs{60000};
    int m_missedPings{0};
    int64_t m_lastWorkerProgressEpochMs{0};
    uint32_t m_lastWorkerOp{0};
    std::chrono::steady_clock::time_point m_lastProgressAdvanceTime{};
    bool m_watchdogEnabled{true};
    QString m_serverName;
    QByteArray m_incomingBuffer;
    std::vector<QByteArray> m_outgoingPackets;
    size_t m_outgoingBytes{0};
    bool m_stopping{false};
    bool m_failureReported{false};
    quint64 m_nextControlRequestId{1};
};

} // namespace pdf::ipc
