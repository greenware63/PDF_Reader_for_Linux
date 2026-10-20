#include "ipc/IpcMessages.h"
#include <algorithm>

namespace pdf {
namespace {

constexpr quint32 MAX_PAGE_COUNT = 100000;
constexpr quint32 MAX_OUTLINE_ITEMS = 10000;
constexpr quint32 MAX_PAGE_CHARACTERS = 4000000;
constexpr quint32 IPC_MAX_SEARCH_MATCHES = 10000;
constexpr quint32 MAX_MATCH_RECTS = 1000000;
constexpr int MAX_OUTLINE_DEPTH = 128;
constexpr quint32 MIN_OUTLINE_ITEM_BYTES = 21;
thread_local quint32 outlineNodeCount = 0;

bool validateCount(QDataStream& stream, quint32 count, quint32 limit) {
    if (count <= limit) {
        return true;
    }
    stream.setStatus(QDataStream::ReadCorruptData);
    return false;
}

bool validateCollection(QDataStream& stream, quint32 count, quint32 limit,
                        quint32 minimumItemBytes) {
    if (!validateCount(stream, count, limit) || stream.status() != QDataStream::Ok ||
        !stream.device() ||
        static_cast<quint64>(count) * minimumItemBytes >
            static_cast<quint64>(std::max<qint64>(0, stream.device()->bytesAvailable()))) {
        stream.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    return true;
}

} // namespace

// DocumentError
QDataStream& operator<<(QDataStream& out, const DocumentError& err) {
    out << static_cast<quint32>(err);
    return out;
}

QDataStream& operator>>(QDataStream& in, DocumentError& err) {
    quint32 val = 0;
    in >> val;
    err = static_cast<DocumentError>(val);
    return in;
}

// OpenResult
QDataStream& operator<<(QDataStream& out, const OpenResult& res) {
    out << res.error;
    out << static_cast<qint32>(res.pageCount);
    out << static_cast<quint32>(res.pageSizes.size());
    for (const auto& size : res.pageSizes) {
        out << size.first << size.second;
    }
    out << static_cast<quint32>(res.pageLabels.size());
    for (const auto& label : res.pageLabels) {
        out << QString::fromStdString(label);
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, OpenResult& res) {
    in >> res.error;
    qint32 count = 0;
    in >> count;
    res.pageCount = count;
    quint32 sizeCount = 0;
    in >> sizeCount;
    res.pageSizes.clear();
    if (!validateCollection(in, sizeCount, MAX_PAGE_COUNT, 16)) {
        return in;
    }
    res.pageSizes.reserve(sizeCount);
    for (quint32 i = 0; i < sizeCount; ++i) {
        double w = 0.0, h = 0.0;
        in >> w >> h;
        res.pageSizes.emplace_back(w, h);
    }
    quint32 labelCount = 0;
    in >> labelCount;
    res.pageLabels.clear();
    if (!validateCollection(in, labelCount, MAX_PAGE_COUNT, 4)) {
        return in;
    }
    res.pageLabels.reserve(labelCount);
    for (quint32 i = 0; i < labelCount; ++i) {
        QString s;
        in >> s;
        res.pageLabels.push_back(s.toStdString());
    }
    return in;
}

// TileKey
QDataStream& operator<<(QDataStream& out, const TileKey& key) {
    out << static_cast<qint64>(key.documentId);
    out << static_cast<qint32>(key.pageIndex);
    out << static_cast<qint32>(key.col);
    out << static_cast<qint32>(key.row);
    out << static_cast<qint32>(key.scaleMilli);
    out << static_cast<qint32>(key.dprMilli);
    out << static_cast<qint32>(key.rotation);
    return out;
}

QDataStream& operator>>(QDataStream& in, TileKey& key) {
    qint64 docId = 0;
    qint32 page = 0, col = 0, row = 0, sm = 0, dm = 0;
    in >> docId >> page >> col >> row >> sm >> dm;
    key.documentId = docId;
    key.pageIndex = page;
    key.col = col;
    key.row = row;
    key.scaleMilli = sm;
    key.dprMilli = dm;
    // rotation is a mandatory field of the v3 wire format. Do not probe atEnd():
    // it reports the end of the enclosing message, not of this nested struct, so a
    // guard here would consume the *next* field of the parent on an older payload.
    qint32 rot = 0;
    in >> rot;
    key.rotation = rot;
    return in;
}

// PixelRect
QDataStream& operator<<(QDataStream& out, const PixelRect& rect) {
    out << static_cast<qint32>(rect.x);
    out << static_cast<qint32>(rect.y);
    out << static_cast<qint32>(rect.width);
    out << static_cast<qint32>(rect.height);
    return out;
}

QDataStream& operator>>(QDataStream& in, PixelRect& rect) {
    qint32 x = 0, y = 0, w = 0, h = 0;
    in >> x >> y >> w >> h;
    rect.x = x;
    rect.y = y;
    rect.width = w;
    rect.height = h;
    return in;
}

// TileRenderRequest
QDataStream& operator<<(QDataStream& out, const TileRenderRequest& req) {
    out << static_cast<qint64>(req.documentId);
    out << static_cast<quint64>(req.generation);
    out << static_cast<qint32>(req.pageIndex);
    out << req.rect;
    out << req.scale;
    out << req.dpr;
    out << req.key;
    out << static_cast<qint32>(req.priority);
    out << static_cast<qint32>(req.rotation);
    return out;
}

QDataStream& operator>>(QDataStream& in, TileRenderRequest& req) {
    qint64 docId = 0;
    quint64 gen = 0;
    qint32 page = 0, prio = 0;
    in >> docId >> gen >> page >> req.rect >> req.scale >> req.dpr >> req.key >> prio;
    req.documentId = docId;
    req.generation = gen;
    req.pageIndex = page;
    req.priority = prio;
    qint32 rot = 0;
    in >> rot;
    req.rotation = (rot != 0) ? rot : req.key.rotation;
    return in;
}

// OutlineItem
QDataStream& operator<<(QDataStream& out, const OutlineItem& item) {
    out << item.title;
    out << static_cast<qint32>(item.pageIndex);
    out << item.targetYFraction;
    out << item.hasTargetY;
    out << static_cast<quint32>(item.children.size());
    for (const auto& child : item.children) {
        out << child;
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, OutlineItem& item) {
    static thread_local int depth = 0;
    if (depth >= MAX_OUTLINE_DEPTH) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    if (outlineNodeCount >= MAX_OUTLINE_ITEMS) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    ++depth;
    ++outlineNodeCount;
    in >> item.title;
    qint32 page = 0;
    in >> page;
    item.pageIndex = page;
    in >> item.targetYFraction;
    in >> item.hasTargetY;
    quint32 childCount = 0;
    in >> childCount;
    item.children.clear();
    if (!validateCollection(in, childCount, MAX_OUTLINE_ITEMS, MIN_OUTLINE_ITEM_BYTES)) {
        --outlineNodeCount;
        --depth;
        return in;
    }
    item.children.reserve(childCount);
    for (quint32 i = 0; i < childCount; ++i) {
        OutlineItem child;
        in >> child;
        if (in.status() != QDataStream::Ok) {
            --outlineNodeCount;
            --depth;
            return in;
        }
        item.children.push_back(std::move(child));
    }
    --depth;
    return in;
}

// PageCharBox
QDataStream& operator<<(QDataStream& out, const PageCharBox& box) {
    out << box.left << box.top << box.right << box.bottom;
    out << static_cast<quint32>(box.unicode);
    return out;
}

QDataStream& operator>>(QDataStream& in, PageCharBox& box) {
    quint32 u = 0;
    in >> box.left >> box.top >> box.right >> box.bottom >> u;
    box.unicode = static_cast<char32_t>(u);
    return in;
}

// PageText
QDataStream& operator<<(QDataStream& out, const PageText& text) {
    out << static_cast<qint32>(text.pageIndex);
    out << text.widthPoints << text.heightPoints;
    QString qstr = QString::fromStdU16String(text.text);
    out << qstr;
    out << static_cast<quint32>(text.chars.size());
    for (const auto& box : text.chars) {
        out << box;
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, PageText& text) {
    qint32 p = 0;
    in >> p >> text.widthPoints >> text.heightPoints;
    text.pageIndex = p;
    QString qstr;
    in >> qstr;
    text.text = qstr.toStdU16String();
    quint32 count = 0;
    in >> count;
    text.chars.clear();
    if (!validateCollection(in, count, MAX_PAGE_CHARACTERS, 36)) {
        return in;
    }
    text.chars.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        PageCharBox box;
        in >> box;
        text.chars.push_back(box);
    }
    return in;
}

// SearchMatch
QDataStream& operator<<(QDataStream& out, const SearchMatch& match) {
    out << static_cast<qint32>(match.pageIndex);
    out << static_cast<qint32>(match.charStartIndex);
    out << static_cast<qint32>(match.charCount);
    out << static_cast<quint32>(match.rects.size());
    for (const auto& r : match.rects) {
        out << r;
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, SearchMatch& match) {
    qint32 p = 0, start = 0, len = 0;
    in >> p >> start >> len;
    match.pageIndex = p;
    match.charStartIndex = start;
    match.charCount = len;
    quint32 count = 0;
    in >> count;
    match.rects.clear();
    if (!validateCollection(in, count, MAX_MATCH_RECTS, 36)) {
        return in;
    }
    match.rects.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        PageCharBox box;
        in >> box;
        match.rects.push_back(box);
    }
    return in;
}

} // namespace pdf

namespace pdf::ipc {

// OpenDocumentRequest
QDataStream& operator<<(QDataStream& out, const OpenDocumentRequest& req) {
    out << static_cast<qint64>(req.documentId) << req.filePath << req.password;
    return out;
}

QDataStream& operator>>(QDataStream& in, OpenDocumentRequest& req) {
    qint64 id = 0;
    in >> id >> req.filePath >> req.password;
    req.documentId = id;
    return in;
}

// OpenDocumentResponse
QDataStream& operator<<(QDataStream& out, const OpenDocumentResponse& res) {
    out << static_cast<qint64>(res.documentId) << res.result;
    return out;
}

QDataStream& operator>>(QDataStream& in, OpenDocumentResponse& res) {
    qint64 id = 0;
    in >> id >> res.result;
    res.documentId = id;
    return in;
}

// CloseDocumentRequest
QDataStream& operator<<(QDataStream& out, const CloseDocumentRequest& req) {
    out << static_cast<qint64>(req.documentId);
    return out;
}

QDataStream& operator>>(QDataStream& in, CloseDocumentRequest& req) {
    qint64 id = 0;
    in >> id;
    req.documentId = id;
    return in;
}

// GetOutlineRequest
QDataStream& operator<<(QDataStream& out, const GetOutlineRequest& req) {
    out << static_cast<qint64>(req.documentId);
    return out;
}

QDataStream& operator>>(QDataStream& in, GetOutlineRequest& req) {
    qint64 id = 0;
    in >> id;
    req.documentId = id;
    return in;
}

// GetOutlineResponse
QDataStream& operator<<(QDataStream& out, const GetOutlineResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<quint32>(res.outline.size());
    for (const auto& item : res.outline) {
        out << item;
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, GetOutlineResponse& res) {
    qint64 id = 0;
    in >> id;
    res.documentId = id;
    quint32 count = 0;
    in >> count;
    res.outline.clear();
    if (!validateCollection(in, count, MAX_OUTLINE_ITEMS, MIN_OUTLINE_ITEM_BYTES)) {
        return in;
    }
    outlineNodeCount = 0;
    res.outline.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        OutlineItem item;
        in >> item;
        if (in.status() != QDataStream::Ok) {
            break;
        }
        res.outline.push_back(std::move(item));
    }
    outlineNodeCount = 0;
    return in;
}

// RenderTileRequest
QDataStream& operator<<(QDataStream& out, const RenderTileRequest& req) {
    out << req.request;
    return out;
}

QDataStream& operator>>(QDataStream& in, RenderTileRequest& req) {
    in >> req.request;
    return in;
}

// TileRenderedResponse
QDataStream& operator<<(QDataStream& out, const TileRenderedResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<quint64>(res.generation);
    out << res.key;
    out << static_cast<qint32>(res.width);
    out << static_cast<qint32>(res.height);
    out << static_cast<qint32>(res.stride);
    out << static_cast<quint32>(res.format);
    out << res.isShm;
    out << res.shmKey;
    out << res.pixelData;
    return out;
}

QDataStream& operator>>(QDataStream& in, TileRenderedResponse& res) {
    qint64 docId = 0;
    quint64 gen = 0;
    qint32 w = 0, h = 0, stride = 0;
    quint32 fmt = 0;
    in >> docId >> gen >> res.key >> w >> h >> stride >> fmt >> res.isShm >> res.shmKey >> res.pixelData;
    res.documentId = docId;
    res.generation = gen;
    res.width = w;
    res.height = h;
    res.stride = stride;
    res.format = fmt;
    return in;
}

// TileFailedResponse
QDataStream& operator<<(QDataStream& out, const TileFailedResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<quint64>(res.generation);
    out << res.key;
    return out;
}

QDataStream& operator>>(QDataStream& in, TileFailedResponse& res) {
    qint64 docId = 0;
    quint64 gen = 0;
    in >> docId >> gen >> res.key;
    res.documentId = docId;
    res.generation = gen;
    return in;
}

// CancelRenderRequest
QDataStream& operator<<(QDataStream& out, const CancelRenderRequest& req) {
    out << static_cast<qint64>(req.documentId);
    return out;
}

QDataStream& operator>>(QDataStream& in, CancelRenderRequest& req) {
    qint64 id = 0;
    in >> id;
    req.documentId = id;
    return in;
}

// SetGenerationRequest
QDataStream& operator<<(QDataStream& out, const SetGenerationRequest& req) {
    out << static_cast<quint64>(req.generation);
    return out;
}

QDataStream& operator>>(QDataStream& in, SetGenerationRequest& req) {
    quint64 gen = 0;
    in >> gen;
    req.generation = gen;
    return in;
}

// PageTextRequest
QDataStream& operator<<(QDataStream& out, const PageTextRequest& req) {
    out << static_cast<qint64>(req.documentId);
    out << static_cast<qint32>(req.pageIndex);
    return out;
}

QDataStream& operator>>(QDataStream& in, PageTextRequest& req) {
    qint64 id = 0;
    qint32 p = 0;
    in >> id >> p;
    req.documentId = id;
    req.pageIndex = p;
    return in;
}

// PageTextResponse
QDataStream& operator<<(QDataStream& out, const PageTextResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<qint32>(res.pageIndex);
    out << res.success;
    out << res.pageText;
    return out;
}

QDataStream& operator>>(QDataStream& in, PageTextResponse& res) {
    qint64 id = 0;
    qint32 p = 0;
    in >> id >> p >> res.success >> res.pageText;
    res.documentId = id;
    res.pageIndex = p;
    return in;
}

// StartSearchRequest
QDataStream& operator<<(QDataStream& out, const StartSearchRequest& req) {
    out << static_cast<qint64>(req.documentId);
    out << static_cast<qint64>(req.searchId);
    out << req.query;
    out << req.matchCase;
    out << static_cast<qint32>(req.totalPages);
    out << static_cast<qint32>(req.startPage);
    return out;
}

QDataStream& operator>>(QDataStream& in, StartSearchRequest& req) {
    qint64 docId = 0, sId = 0;
    qint32 total = 0, start = 0;
    in >> docId >> sId >> req.query >> req.matchCase >> total >> start;
    req.documentId = docId;
    req.searchId = sId;
    req.totalPages = total;
    req.startPage = start;
    return in;
}

// CancelSearchRequest
QDataStream& operator<<(QDataStream& out, const CancelSearchRequest& req) {
    out << static_cast<qint64>(req.searchId);
    return out;
}

QDataStream& operator>>(QDataStream& in, CancelSearchRequest& req) {
    qint64 id = 0;
    in >> id;
    req.searchId = id;
    return in;
}

// SearchMatchesFoundResponse
QDataStream& operator<<(QDataStream& out, const SearchMatchesFoundResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<qint64>(res.searchId);
    out << static_cast<qint32>(res.pageIndex);
    out << static_cast<quint32>(res.matches.size());
    for (const auto& m : res.matches) {
        out << m;
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, SearchMatchesFoundResponse& res) {
    qint64 docId = 0, sId = 0;
    qint32 p = 0;
    in >> docId >> sId >> p;
    res.documentId = docId;
    res.searchId = sId;
    res.pageIndex = p;
    quint32 count = 0;
    in >> count;
    res.matches.clear();
    if (!validateCollection(in, count, IPC_MAX_SEARCH_MATCHES, 16)) {
        return in;
    }
    res.matches.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        SearchMatch m;
        in >> m;
        res.matches.push_back(std::move(m));
    }
    return in;
}

// SearchProgressResponse
QDataStream& operator<<(QDataStream& out, const SearchProgressResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<qint64>(res.searchId);
    out << static_cast<qint32>(res.pagesSearched);
    out << static_cast<qint32>(res.totalPages);
    return out;
}

QDataStream& operator>>(QDataStream& in, SearchProgressResponse& res) {
    qint64 docId = 0, sId = 0;
    qint32 searched = 0, total = 0;
    in >> docId >> sId >> searched >> total;
    res.documentId = docId;
    res.searchId = sId;
    res.pagesSearched = searched;
    res.totalPages = total;
    return in;
}

// SearchCompletedResponse
QDataStream& operator<<(QDataStream& out, const SearchCompletedResponse& res) {
    out << static_cast<qint64>(res.documentId);
    out << static_cast<qint64>(res.searchId);
    out << static_cast<qint32>(res.totalMatches);
    out << res.truncated;
    return out;
}

QDataStream& operator>>(QDataStream& in, SearchCompletedResponse& res) {
    qint64 docId = 0, sId = 0;
    qint32 matches = 0;
    in >> docId >> sId >> matches >> res.truncated;
    res.documentId = docId;
    res.searchId = sId;
    res.totalMatches = matches;
    return in;
}

// PingRequest
QDataStream& operator<<(QDataStream& out, const PingRequest& req) {
    out << static_cast<quint64>(req.seq);
    return out;
}

QDataStream& operator>>(QDataStream& in, PingRequest& req) {
    quint64 s = 0;
    in >> s;
    req.seq = s;
    return in;
}

// PongResponse
QDataStream& operator<<(QDataStream& out, const PongResponse& res) {
    out << static_cast<quint64>(res.seq)
        << static_cast<qint64>(res.lastProgressEpochMs)
        << static_cast<quint32>(res.currentOp);
    return out;
}

QDataStream& operator>>(QDataStream& in, PongResponse& res) {
    quint64 s = 0;
    qint64 prog = 0;
    quint32 op = 0;
    in >> s >> prog >> op;
    res.seq = s;
    res.lastProgressEpochMs = prog;
    res.currentOp = op;
    return in;
}

// WorkerErrorResponse
QDataStream& operator<<(QDataStream& out, const WorkerErrorResponse& res) {
    out << static_cast<qint64>(res.documentId) << res.errorMessage;
    return out;
}

QDataStream& operator>>(QDataStream& in, WorkerErrorResponse& res) {
    qint64 id = 0;
    in >> id >> res.errorMessage;
    res.documentId = id;
    return in;
}

QDataStream& operator<<(QDataStream& out, const SharedMemoryAck& ack) {
    out << ack.shmKey;
    return out;
}

QDataStream& operator>>(QDataStream& in, SharedMemoryAck& ack) {
    in >> ack.shmKey;
    return in;
}

QDataStream& operator<<(QDataStream& out, const WorkerControlRequest& request) {
    out << request.requestId
        << static_cast<qint32>(request.renderDelayMs)
        << static_cast<qint32>(request.openDelayMs)
        << request.waitForIdle
        << request.queryStatus;
#ifdef PDF_WORKER_TEST_HOOKS
    out << request.hangEngine;
#endif
    return out;
}

QDataStream& operator>>(QDataStream& in, WorkerControlRequest& request) {
    in >> request.requestId;
    qint32 renderDelay = -1;
    qint32 openDelay = -1;
    in >> renderDelay >> openDelay >> request.waitForIdle >> request.queryStatus;
    request.renderDelayMs = renderDelay;
    request.openDelayMs = openDelay;
#ifdef PDF_WORKER_TEST_HOOKS
    if (!in.atEnd()) {
        in >> request.hangEngine;
    }
#endif
    return in;
}

QDataStream& operator<<(QDataStream& out, const WorkerControlResponse& response) {
    out << response.requestId << response.pendingCount << response.cachedPageCount;
    return out;
}

QDataStream& operator>>(QDataStream& in, WorkerControlResponse& response) {
    in >> response.requestId >> response.pendingCount >> response.cachedPageCount;
    return in;
}

// ThumbnailRequest
QDataStream& operator<<(QDataStream& out, const ThumbnailRequest& req) {
    out << static_cast<qint64>(req.documentId)
        << static_cast<qint32>(req.pageIndex)
        << static_cast<qint32>(req.targetWidth)
        << static_cast<qint32>(req.rotation);
    return out;
}

QDataStream& operator>>(QDataStream& in, ThumbnailRequest& req) {
    qint64 docId = 0;
    qint32 pIdx = 0;
    qint32 tW = 0;
    qint32 rot = 0;
    in >> docId >> pIdx >> tW >> rot;
    req.documentId = docId;
    req.pageIndex = pIdx;
    req.targetWidth = tW;
    req.rotation = rot;
    return in;
}

// ThumbnailResponse
QDataStream& operator<<(QDataStream& out, const ThumbnailResponse& res) {
    out << static_cast<qint64>(res.documentId)
        << static_cast<qint32>(res.pageIndex)
        << static_cast<qint32>(res.width)
        << static_cast<qint32>(res.height)
        << static_cast<qint32>(res.stride)
        << res.pixelData;
    return out;
}

QDataStream& operator>>(QDataStream& in, ThumbnailResponse& res) {
    qint64 docId = 0;
    qint32 pIdx = 0, w = 0, h = 0, s = 0;
    in >> docId >> pIdx >> w >> h >> s >> res.pixelData;
    res.documentId = docId;
    res.pageIndex = pIdx;
    res.width = w;
    res.height = h;
    res.stride = s;
    return in;
}

// ThumbnailFailedResponse
QDataStream& operator<<(QDataStream& out, const ThumbnailFailedResponse& res) {
    out << static_cast<qint64>(res.documentId)
        << static_cast<qint32>(res.pageIndex);
    return out;
}

QDataStream& operator>>(QDataStream& in, ThumbnailFailedResponse& res) {
    qint64 docId = 0;
    qint32 pIdx = 0;
    in >> docId >> pIdx;
    res.documentId = docId;
    res.pageIndex = pIdx;
    return in;
}

// RenderPageRequest
QDataStream& operator<<(QDataStream& out, const RenderPageRequest& req) {
    out << static_cast<qint64>(req.documentId)
        << static_cast<qint32>(req.pageIndex)
        << req.scale
        << req.dpr
        << static_cast<qint32>(req.rotation);
    return out;
}

QDataStream& operator>>(QDataStream& in, RenderPageRequest& req) {
    qint64 docId = 0;
    qint32 pIdx = 0;
    qint32 rot = 0;
    in >> docId >> pIdx >> req.scale >> req.dpr >> rot;
    req.documentId = docId;
    req.pageIndex = pIdx;
    req.rotation = rot;
    return in;
}

// RenderPageResponse
QDataStream& operator<<(QDataStream& out, const RenderPageResponse& res) {
    out << static_cast<qint64>(res.documentId)
        << static_cast<qint32>(res.pageIndex)
        << static_cast<qint32>(res.width)
        << static_cast<qint32>(res.height)
        << static_cast<qint32>(res.stride)
        << res.pixelData;
    return out;
}

QDataStream& operator>>(QDataStream& in, RenderPageResponse& res) {
    qint64 docId = 0;
    qint32 pIdx = 0, w = 0, h = 0, s = 0;
    in >> docId >> pIdx >> w >> h >> s >> res.pixelData;
    res.documentId = docId;
    res.pageIndex = pIdx;
    res.width = w;
    res.height = h;
    res.stride = s;
    return in;
}

// RenderPageFailedResponse
QDataStream& operator<<(QDataStream& out, const RenderPageFailedResponse& res) {
    out << static_cast<qint64>(res.documentId)
        << static_cast<qint32>(res.pageIndex);
    return out;
}

QDataStream& operator>>(QDataStream& in, RenderPageFailedResponse& res) {
    qint64 docId = 0;
    qint32 pIdx = 0;
    in >> docId >> pIdx;
    res.documentId = docId;
    res.pageIndex = pIdx;
    return in;
}

// ExtractPagesRequest / Response / Failed
QDataStream& operator<<(QDataStream& out, const ExtractPagesRequest& req) {
    out << static_cast<qint64>(req.documentId) << static_cast<qint32>(req.firstPage) << static_cast<qint32>(req.lastPage);
    return out;
}
QDataStream& operator>>(QDataStream& in, ExtractPagesRequest& req) {
    qint64 docId = 0; qint32 a = 0, b = 0;
    in >> docId >> a >> b;
    req.documentId = docId; req.firstPage = a; req.lastPage = b;
    return in;
}
QDataStream& operator<<(QDataStream& out, const ExtractPagesResponse& res) {
    out << static_cast<qint64>(res.documentId) << static_cast<qint32>(res.firstPage)
        << static_cast<qint32>(res.lastPage) << res.pdfBytes;
    return out;
}
QDataStream& operator>>(QDataStream& in, ExtractPagesResponse& res) {
    qint64 docId = 0; qint32 a = 0, b = 0;
    in >> docId >> a >> b >> res.pdfBytes;
    res.documentId = docId; res.firstPage = a; res.lastPage = b;
    return in;
}
QDataStream& operator<<(QDataStream& out, const ExtractPagesFailedResponse& res) {
    out << static_cast<qint64>(res.documentId) << static_cast<qint32>(res.firstPage) << static_cast<qint32>(res.lastPage);
    return out;
}
QDataStream& operator>>(QDataStream& in, ExtractPagesFailedResponse& res) {
    qint64 docId = 0; qint32 a = 0, b = 0;
    in >> docId >> a >> b;
    res.documentId = docId; res.firstPage = a; res.lastPage = b;
    return in;
}

// RedactRect
QDataStream& operator<<(QDataStream& out, const RedactRect& r) {
    out << r.left << r.top << r.right << r.bottom;
    return out;
}
QDataStream& operator>>(QDataStream& in, RedactRect& r) {
    in >> r.left >> r.top >> r.right >> r.bottom;
    return in;
}

// PageRedactionItem
QDataStream& operator<<(QDataStream& out, const PageRedactionItem& item) {
    out << static_cast<qint32>(item.pageIndex) << static_cast<quint32>(item.rects.size());
    for (const auto& r : item.rects) {
        out << r;
    }
    return out;
}
QDataStream& operator>>(QDataStream& in, PageRedactionItem& item) {
    qint32 pageIdx = 0;
    quint32 count = 0;
    in >> pageIdx >> count;
    item.pageIndex = pageIdx;
    if (count > 100000) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    item.rects.clear();
    item.rects.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        RedactRect r;
        in >> r;
        item.rects.push_back(r);
    }
    return in;
}

// RedactDocumentRequest
QDataStream& operator<<(QDataStream& out, const RedactDocumentRequest& req) {
    out << static_cast<qint64>(req.documentId)
        << static_cast<quint32>(req.redactions.size());
    for (const auto& item : req.redactions) {
        out << item;
    }
    out << req.dpi << static_cast<qint32>(req.edgeGrowPx);
    return out;
}
QDataStream& operator>>(QDataStream& in, RedactDocumentRequest& req) {
    qint64 docId = 0;
    quint32 count = 0;
    in >> docId >> count;
    req.documentId = docId;
    if (count > 10000) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    req.redactions.clear();
    req.redactions.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        PageRedactionItem item;
        in >> item;
        req.redactions.push_back(std::move(item));
    }
    qint32 eg = 0;
    in >> req.dpi >> eg;
    req.edgeGrowPx = eg;
    return in;
}

// RedactDocumentResponse
QDataStream& operator<<(QDataStream& out, const RedactDocumentResponse& res) {
    out << static_cast<qint64>(res.documentId)
        << res.success
        << res.errorMessage
        << res.pdfBytes
        << static_cast<qint32>(res.pageCount)
        << static_cast<qint32>(res.sourceSignatureCount)
        << static_cast<qint32>(res.sourceAttachmentCount)
        << res.sourceWasEncrypted;

    out << static_cast<quint32>(res.warnings.size());
    for (const auto& w : res.warnings) {
        out << w;
    }
    out << static_cast<quint32>(res.verification.size());
    for (const auto& v : res.verification) {
        out << v;
    }
    return out;
}
QDataStream& operator>>(QDataStream& in, RedactDocumentResponse& res) {
    qint64 docId = 0;
    qint32 pc = 0, sigs = 0, atts = 0;
    in >> docId >> res.success >> res.errorMessage >> res.pdfBytes >> pc >> sigs >> atts >> res.sourceWasEncrypted;
    res.documentId = docId;
    res.pageCount = pc;
    res.sourceSignatureCount = sigs;
    res.sourceAttachmentCount = atts;

    quint32 warnCount = 0;
    in >> warnCount;
    if (warnCount > 1000) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    res.warnings.clear();
    res.warnings.reserve(warnCount);
    for (quint32 i = 0; i < warnCount; ++i) {
        QString w;
        in >> w;
        res.warnings.push_back(std::move(w));
    }

    quint32 verifCount = 0;
    in >> verifCount;
    if (verifCount > 1000) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    res.verification.clear();
    res.verification.reserve(verifCount);
    for (quint32 i = 0; i < verifCount; ++i) {
        QString v;
        in >> v;
        res.verification.push_back(std::move(v));
    }
    return in;
}

// RedactDocumentFailedResponse
QDataStream& operator<<(QDataStream& out, const RedactDocumentFailedResponse& res) {
    out << static_cast<qint64>(res.documentId) << res.errorMessage;
    return out;
}
QDataStream& operator>>(QDataStream& in, RedactDocumentFailedResponse& res) {
    qint64 docId = 0;
    in >> docId >> res.errorMessage;
    res.documentId = docId;
    return in;
}

} // namespace pdf::ipc
