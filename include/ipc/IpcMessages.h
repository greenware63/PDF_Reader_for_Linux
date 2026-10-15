#pragma once

#include "ipc/IpcProtocol.h"
#include "engine/IDocument.h"
#include "engine/PixelBuffer.h"
#include "engine/PageText.h"
#include "engine/SearchTypes.h"
#include "engine/OutlineTypes.h"
#include "engine/RenderWorker.h"
#include "cache/TileCache.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QString>
#include <vector>
#include <cstring>
#include <utility>

namespace pdf {

// Engine serialization declarations in namespace pdf for proper ADL
QDataStream& operator<<(QDataStream& out, const DocumentError& err);
QDataStream& operator>>(QDataStream& in, DocumentError& err);

QDataStream& operator<<(QDataStream& out, const OpenResult& res);
QDataStream& operator>>(QDataStream& in, OpenResult& res);

QDataStream& operator<<(QDataStream& out, const TileKey& key);
QDataStream& operator>>(QDataStream& in, TileKey& key);

QDataStream& operator<<(QDataStream& out, const PixelRect& rect);
QDataStream& operator>>(QDataStream& in, PixelRect& rect);

QDataStream& operator<<(QDataStream& out, const TileRenderRequest& req);
QDataStream& operator>>(QDataStream& in, TileRenderRequest& req);

QDataStream& operator<<(QDataStream& out, const OutlineItem& item);
QDataStream& operator>>(QDataStream& in, OutlineItem& item);

QDataStream& operator<<(QDataStream& out, const PageCharBox& box);
QDataStream& operator>>(QDataStream& in, PageCharBox& box);

QDataStream& operator<<(QDataStream& out, const PageText& text);
QDataStream& operator>>(QDataStream& in, PageText& text);

QDataStream& operator<<(QDataStream& out, const SearchMatch& match);
QDataStream& operator>>(QDataStream& in, SearchMatch& match);

} // namespace pdf

namespace pdf::ipc {

// --- Message Structures ---\n
struct OpenDocumentRequest {
    int64_t documentId{0};
    QString filePath;
    QString password;
};

struct OpenDocumentResponse {
    int64_t documentId{0};
    pdf::OpenResult result;
};

struct CloseDocumentRequest {
    int64_t documentId{0};
};

struct GetOutlineRequest {
    int64_t documentId{0};
};

struct GetOutlineResponse {
    int64_t documentId{0};
    std::vector<pdf::OutlineItem> outline;
};

struct RenderTileRequest {
    pdf::TileRenderRequest request;
};

struct TileRenderedResponse {
    int64_t documentId{0};
    uint64_t generation{0};
    pdf::TileKey key;
    int width{0};
    int height{0};
    int stride{0};
    uint32_t format{0}; // PixelFormat
    bool isShm{false};
    QString shmKey;
    QByteArray pixelData;
};

struct TileFailedResponse {
    int64_t documentId{0};
    uint64_t generation{0};
    pdf::TileKey key;
};

struct CancelRenderRequest {
    int64_t documentId{0};
};

struct SetGenerationRequest {
    uint64_t generation{0};
};

struct PageTextRequest {
    int64_t documentId{0};
    int pageIndex{0};
};

struct PageTextResponse {
    int64_t documentId{0};
    int pageIndex{0};
    bool success{false};
    pdf::PageText pageText;
};

struct StartSearchRequest {
    int64_t documentId{0};
    int64_t searchId{0};
    QString query;
    bool matchCase{false};
    int totalPages{0};
    int startPage{0};
};

struct CancelSearchRequest {
    int64_t searchId{0};
};

struct SearchMatchesFoundResponse {
    int64_t documentId{0};
    int64_t searchId{0};
    int pageIndex{0};
    std::vector<pdf::SearchMatch> matches;
};

struct SearchProgressResponse {
    int64_t documentId{0};
    int64_t searchId{0};
    int pagesSearched{0};
    int totalPages{0};
};

struct SearchCompletedResponse {
    int64_t documentId{0};
    int64_t searchId{0};
    int totalMatches{0};
    bool truncated{false};
};

struct PingRequest {
    uint64_t seq{0};
};

struct PongResponse {
    uint64_t seq{0};
};

struct WorkerErrorResponse {
    int64_t documentId{0};
    QString errorMessage;
};

struct SharedMemoryAck {
    QString shmKey;
};

struct WorkerControlRequest {
    quint64 requestId{0};
    int renderDelayMs{-1};
    int openDelayMs{-1};
    bool waitForIdle{false};
    bool queryStatus{false};
};

struct WorkerControlResponse {
    quint64 requestId{0};
    quint64 pendingCount{0};
    quint64 cachedPageCount{0};
};

struct ThumbnailRequest {
    int64_t documentId{0};
    int pageIndex{0};
    int targetWidth{140};
};

struct ThumbnailResponse {
    int64_t documentId{0};
    int pageIndex{0};
    int width{0};
    int height{0};
    int stride{0};
    QByteArray pixelData;
};

struct ThumbnailFailedResponse {
    int64_t documentId{0};
    int pageIndex{0};
};

// Message stream operators
QDataStream& operator<<(QDataStream& out, const OpenDocumentRequest& req);
QDataStream& operator>>(QDataStream& in, OpenDocumentRequest& req);

QDataStream& operator<<(QDataStream& out, const OpenDocumentResponse& res);
QDataStream& operator>>(QDataStream& in, OpenDocumentResponse& res);

QDataStream& operator<<(QDataStream& out, const CloseDocumentRequest& req);
QDataStream& operator>>(QDataStream& in, CloseDocumentRequest& req);

QDataStream& operator<<(QDataStream& out, const GetOutlineRequest& req);
QDataStream& operator>>(QDataStream& in, GetOutlineRequest& req);

QDataStream& operator<<(QDataStream& out, const GetOutlineResponse& res);
QDataStream& operator>>(QDataStream& in, GetOutlineResponse& res);

QDataStream& operator<<(QDataStream& out, const RenderTileRequest& req);
QDataStream& operator>>(QDataStream& in, RenderTileRequest& req);

QDataStream& operator<<(QDataStream& out, const TileRenderedResponse& res);
QDataStream& operator>>(QDataStream& in, TileRenderedResponse& res);

QDataStream& operator<<(QDataStream& out, const TileFailedResponse& res);
QDataStream& operator>>(QDataStream& in, TileFailedResponse& res);

QDataStream& operator<<(QDataStream& out, const CancelRenderRequest& req);
QDataStream& operator>>(QDataStream& in, CancelRenderRequest& req);

QDataStream& operator<<(QDataStream& out, const SetGenerationRequest& req);
QDataStream& operator>>(QDataStream& in, SetGenerationRequest& req);

QDataStream& operator<<(QDataStream& out, const PageTextRequest& req);
QDataStream& operator>>(QDataStream& in, PageTextRequest& req);

QDataStream& operator<<(QDataStream& out, const PageTextResponse& res);
QDataStream& operator>>(QDataStream& in, PageTextResponse& res);

QDataStream& operator<<(QDataStream& out, const StartSearchRequest& req);
QDataStream& operator>>(QDataStream& in, StartSearchRequest& req);

QDataStream& operator<<(QDataStream& out, const CancelSearchRequest& req);
QDataStream& operator>>(QDataStream& in, CancelSearchRequest& req);

QDataStream& operator<<(QDataStream& out, const SearchMatchesFoundResponse& res);
QDataStream& operator>>(QDataStream& in, SearchMatchesFoundResponse& res);

QDataStream& operator<<(QDataStream& out, const SearchProgressResponse& res);
QDataStream& operator>>(QDataStream& in, SearchProgressResponse& res);

QDataStream& operator<<(QDataStream& out, const SearchCompletedResponse& res);
QDataStream& operator>>(QDataStream& in, SearchCompletedResponse& res);

QDataStream& operator<<(QDataStream& out, const PingRequest& req);
QDataStream& operator>>(QDataStream& in, PingRequest& req);

QDataStream& operator<<(QDataStream& out, const PongResponse& res);
QDataStream& operator>>(QDataStream& in, PongResponse& res);

QDataStream& operator<<(QDataStream& out, const WorkerErrorResponse& res);
QDataStream& operator>>(QDataStream& in, WorkerErrorResponse& res);
QDataStream& operator<<(QDataStream& out, const SharedMemoryAck& ack);
QDataStream& operator>>(QDataStream& in, SharedMemoryAck& ack);
QDataStream& operator<<(QDataStream& out, const WorkerControlRequest& request);
QDataStream& operator>>(QDataStream& in, WorkerControlRequest& request);
QDataStream& operator<<(QDataStream& out, const WorkerControlResponse& response);
QDataStream& operator>>(QDataStream& in, WorkerControlResponse& response);

QDataStream& operator<<(QDataStream& out, const ThumbnailRequest& req);
QDataStream& operator>>(QDataStream& in, ThumbnailRequest& req);

QDataStream& operator<<(QDataStream& out, const ThumbnailResponse& res);
QDataStream& operator>>(QDataStream& in, ThumbnailResponse& res);

QDataStream& operator<<(QDataStream& out, const ThumbnailFailedResponse& res);
QDataStream& operator>>(QDataStream& in, ThumbnailFailedResponse& res);

// --- Serialization Templates ---

template <typename T>
QByteArray serialize(const T& msg) {
    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_4);
    stream << msg;
    return buffer;
}

template <typename T>
bool deserialize(const QByteArray& buffer, T& msg) {
    QDataStream stream(buffer);
    stream.setVersion(QDataStream::Qt_6_4);
    stream >> msg;
    return stream.status() == QDataStream::Ok && stream.atEnd();
}

inline QByteArray buildPacket(MessageType type, const QByteArray& payload) {
    if (payload.size() < 0 || static_cast<uint64_t>(payload.size()) > MAX_IPC_PAYLOAD_SIZE) {
        return {};
    }
    PacketHeader header;
    header.magic = IPC_MAGIC;
    header.version = IPC_PROTOCOL_VERSION;
    header.type = static_cast<uint32_t>(type);
    header.payloadSize = static_cast<uint32_t>(payload.size());

    QByteArray packet;
    packet.resize(static_cast<qsizetype>(sizeof(PacketHeader) + payload.size()));
    std::memcpy(packet.data(), &header, sizeof(PacketHeader));
    if (!payload.isEmpty()) {
        std::memcpy(packet.data() + sizeof(PacketHeader), payload.constData(), static_cast<size_t>(payload.size()));
    }
    return packet;
}

} // namespace pdf::ipc
