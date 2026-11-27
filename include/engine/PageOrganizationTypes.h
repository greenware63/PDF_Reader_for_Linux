#pragma once

#include <cstdint>

namespace pdf {

constexpr size_t kMaxOrganizedPages = 10000;

struct PageSpec {
    int sourceIndex{0};
    int rotationDegrees{0}; // 0, 90, 180, or 270 (added to existing page rotation)
};

struct OrganizeOptions {
    bool allowInvalidateSignatures{false};
    bool allowDecrypt{false};
};

enum class OrganizeStatus {
    Ok,
    InvalidSpec,
    PermissionDenied,
    SignedNotAcknowledged,
    EncryptedNotAcknowledged,
    StreamedSourceUnsupported,
    SaveFailed,
    InternalError
};

struct OrganizeResult {
    OrganizeStatus status{OrganizeStatus::InternalError};
    uint64_t bytesWritten{0};
    int danglingOutlineCount{0};
    int danglingLinkCount{0};
};

} // namespace pdf
