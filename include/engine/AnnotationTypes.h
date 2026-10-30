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

    /// Determines whether the point (x, y) lies inside the quad quadrilateral.
    /// Accurately handles oriented, rotated, or skewed quadrilaterals via ray casting
    /// rather than relying solely on axis-aligned bounding boxes.
    bool contains(double x, double y) const noexcept {
        if (!isValid() || std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y)) {
            return false;
        }
        const double qMinX = std::min({x1, x2, x3, x4});
        const double qMaxX = std::max({x1, x2, x3, x4});
        const double qMinY = std::min({y1, y2, y3, y4});
        const double qMaxY = std::max({y1, y2, y3, y4});
        if (x < qMinX || x > qMaxX || y < qMinY || y > qMaxY) {
            return false;
        }

        // Determine vertex order: ISO 32000-1 §12.5.6.10 uses (p1, p2, p4, p3).
        // Compare signed area of (p1, p2, p4, p3) vs (p1, p2, p3, p4) to pick the non-self-intersecting quad.
        const double area1 = (x1 * y2 - x2 * y1) + (x2 * y4 - x4 * y2) + (x4 * y3 - x3 * y4) + (x3 * y1 - x1 * y3);
        const double area2 = (x1 * y2 - x2 * y1) + (x2 * y3 - x3 * y2) + (x3 * y4 - x4 * y3) + (x4 * y1 - x1 * y4);

        if (std::abs(area1) < 1e-6 && std::abs(area2) < 1e-6) {
            // Degenerate collinear or zero-height quad: fallback to bounding box with 1pt margin
            return x >= qMinX - 1.0 && x <= qMaxX + 1.0 && y >= qMinY - 1.0 && y <= qMaxY + 1.0;
        }

        double px[4];
        double py[4];
        if (std::abs(area1) >= std::abs(area2)) {
            px[0] = x1; py[0] = y1;
            px[1] = x2; py[1] = y2;
            px[2] = x4; py[2] = y4;
            px[3] = x3; py[3] = y3;
        } else {
            px[0] = x1; py[0] = y1;
            px[1] = x2; py[1] = y2;
            px[2] = x3; py[2] = y3;
            px[3] = x4; py[3] = y4;
        }

        // Ray casting point-in-polygon test
        bool inside = false;
        for (int i = 0, j = 3; i < 4; j = i++) {
            const double xi = px[i], yi = py[i];
            const double xj = px[j], yj = py[j];
            if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
                inside = !inside;
            }
        }
        return inside;
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
                if (q.contains(x, y)) {
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
