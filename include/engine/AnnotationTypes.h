#pragma once

#include <QString>
#include <QDataStream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace pdf {

enum class AnnotationSubtype : uint32_t {
    Unknown = 0,
    Text = 1,
    Link = 2,
    FreeText = 3,
    Line = 4,
    Square = 5,
    Circle = 6,
    Polygon = 7,
    Polyline = 8,
    Highlight = 9,
    Underline = 10,
    Squiggly = 11,
    StrikeOut = 12,
    Stamp = 13,
    Caret = 14,
    Ink = 15,
    Popup = 16,
    FileAttachment = 17,
    Sound = 18,
    Movie = 19,
    Widget = 20,
    Screen = 21,
    PrinterMark = 22,
    TrapNet = 23,
    Watermark = 24,
    ThreeD = 25,
    RichMedia = 26,
    XfaWidget = 27,
    Redact = 28
};

// Represents 4 vertices (8 coordinates) for text markup or link attachment points in PDF points:
// (x1, y1) top-left, (x2, y2) top-right, (x3, y3) bottom-left, (x4, y4) bottom-right
struct AnnotationQuad {
    double x1{0.0};
    double y1{0.0};
    double x2{0.0};
    double y2{0.0};
    double x3{0.0};
    double y3{0.0};
    double x4{0.0};
    double y4{0.0};

    bool isValid() const noexcept {
        return !std::isnan(x1) && !std::isnan(y1) &&
               !std::isnan(x2) && !std::isnan(y2) &&
               !std::isnan(x3) && !std::isnan(y3) &&
               !std::isnan(x4) && !std::isnan(y4) &&
               !std::isinf(x1) && !std::isinf(y1) &&
               !std::isinf(x2) && !std::isinf(y2) &&
               !std::isinf(x3) && !std::isinf(y3) &&
               !std::isinf(x4) && !std::isinf(y4);
    }
};

struct AnnotationItem {
    int pageIndex{0};
    int annotIndex{0};
    AnnotationSubtype subtype{AnnotationSubtype::Unknown};

    // Bounding box in PDF points (standard user space: left, top, right, bottom)
    double left{0.0};
    double top{0.0};
    double right{0.0};
    double bottom{0.0};

    // Standard metadata entries
    QString contents;         // /Contents string (max 64 KiB UTF-16)
    QString author;           // /T author string (max 256 chars)
    QString name;             // /NM unique annotation identifier string
    QString modificationDate; // /M date string

    uint32_t flags{0};        // /F annotation flags bitfield
    uint32_t colorArgb{0};    // 0xAARRGGBB format
    bool hasColor{false};

    // Subtype-specific payloads
    std::vector<AnnotationQuad> quadPoints; // For text markup or link attachment points (max 4096)
    QString uri;                            // For Link subtype (action URI)

    bool isValid() const noexcept {
        return !std::isnan(left) && !std::isnan(top) &&
               !std::isnan(right) && !std::isnan(bottom) &&
               !std::isinf(left) && !std::isinf(top) &&
               !std::isinf(right) && !std::isinf(bottom);
    }

    bool contains(double x, double y) const noexcept {
        if (!isValid() || std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y)) {
            return false;
        }
        const double minX = std::min(left, right);
        const double maxX = std::max(left, right);
        const double minY = std::min(bottom, top);
        const double maxY = std::max(bottom, top);

        if (x < minX || x > maxX || y < minY || y > maxY) {
            return false;
        }

        // If specific quad points are defined (e.g. text markup across lines), hit-test quads
        if (!quadPoints.empty()) {
            for (const auto& q : quadPoints) {
                if (!q.isValid()) continue;
                const double qMinX = std::min({q.x1, q.x2, q.x3, q.x4});
                const double qMaxX = std::max({q.x1, q.x2, q.x3, q.x4});
                const double qMinY = std::min({q.y1, q.y2, q.y3, q.y4});
                const double qMaxY = std::max({q.y1, q.y2, q.y3, q.y4});
                if (x >= qMinX && x <= qMaxX && y >= qMinY && y <= qMaxY) {
                    return true;
                }
            }
            return false;
        }
        return true;
    }
};

QDataStream& operator<<(QDataStream& out, const AnnotationQuad& q);
QDataStream& operator>>(QDataStream& in, AnnotationQuad& q);

QDataStream& operator<<(QDataStream& out, const AnnotationItem& item);
QDataStream& operator>>(QDataStream& in, AnnotationItem& item);

} // namespace pdf
