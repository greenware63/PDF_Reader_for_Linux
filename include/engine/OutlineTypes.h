#pragma once

#include <QString>
#include <vector>
#include <QMetaType>

namespace pdf {

/**
 * @brief Represents a single node in the document outline / bookmarks hierarchy.
 */
struct OutlineItem {
    QString title;
    int pageIndex{-1};          // 0-based page destination, or -1 if no destination
    double targetYPoints{0.0};  // Target vertical position in PDF points, measured from the page bottom
    bool hasTargetY{false};
    std::vector<OutlineItem> children;

    bool isValid() const noexcept {
        return pageIndex >= 0;
    }

    bool operator==(const OutlineItem& o) const noexcept {
        return pageIndex == o.pageIndex &&
               title == o.title &&
               targetYPoints == o.targetYPoints &&
               hasTargetY == o.hasTargetY &&
               children == o.children;
    }

    bool operator!=(const OutlineItem& o) const noexcept {
        return !(*this == o);
    }
};

} // namespace pdf

Q_DECLARE_METATYPE(pdf::OutlineItem)
Q_DECLARE_METATYPE(std::vector<pdf::OutlineItem>)
