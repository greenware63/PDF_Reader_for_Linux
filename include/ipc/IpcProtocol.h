#pragma once

#include <cstdint>

namespace pdf::ipc {

constexpr uint32_t IPC_MAGIC = 0x50444657; // 'PDFW'
// v3 added TileKey::rotation and TileRenderRequest::rotation. Bump this whenever
// a serialised layout changes: the frame-level version check is the only thing
// that turns a wire mismatch into a clean rejection instead of silent garbage.
constexpr uint32_t IPC_PROTOCOL_VERSION = 3;
constexpr uint32_t MAX_IPC_PAYLOAD_SIZE = 64 * 1024 * 1024;

enum class WorkerOp : uint32_t {
    Idle = 0,
    Open,
    Tile,
    Thumbnail,
    Print,
    Extract,
    Search
};

enum class MessageType : uint32_t {
    // Control / Lifecycle
    Ping = 1,
    Pong = 2,

    // Document loading / closing
    OpenDocumentRequest = 10,
    OpenDocumentResponse = 11,
    CloseDocumentRequest = 12,

    // Outline / Bookmarks
    GetOutlineRequest = 20,
    GetOutlineResponse = 21,

    // Tile rendering
    RenderTileRequest = 30,
    TileRenderedResponse = 31,
    TileFailedResponse = 32,
    CancelRenderRequest = 33,
    SetGenerationRequest = 34,

    // Text extraction
    PageTextRequest = 40,
    PageTextResponse = 41,

    // Search
    StartSearchRequest = 50,
    CancelSearchRequest = 51,
    SearchMatchesFoundResponse = 52,
    SearchProgressResponse = 53,
    SearchCompletedResponse = 54,
    SharedMemoryAck = 55,
    WorkerControlRequest = 56,
    WorkerControlResponse = 57,

    // Thumbnails
    ThumbnailRequest = 60,
    ThumbnailResponse = 61,
    ThumbnailFailedResponse = 62,

    // Printing / Page Export
    RenderPageRequest = 70,
    RenderPageResponse = 71,
    RenderPageFailedResponse = 72,
    ExtractPagesRequest = 73,
    ExtractPagesResponse = 74,
    ExtractPagesFailedResponse = 75,

    // Error / Diagnostic
    WorkerError = 99
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic{IPC_MAGIC};
    uint32_t version{IPC_PROTOCOL_VERSION};
    uint32_t type{0};
    uint32_t payloadSize{0};
};
#pragma pack(pop)

} // namespace pdf::ipc
