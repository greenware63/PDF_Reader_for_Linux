#pragma once

#include <cstdint>

namespace pdf::ipc {

constexpr uint32_t IPC_MAGIC = 0x50444657; // 'PDFW'
constexpr uint32_t IPC_PROTOCOL_VERSION = 1;

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

static_assert(sizeof(PacketHeader) == 16, "PacketHeader size must be 16 bytes");

} // namespace pdf::ipc
