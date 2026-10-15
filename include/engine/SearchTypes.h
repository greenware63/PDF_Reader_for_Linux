#pragma once

#include "engine/PageText.h"
#include <vector>
#include <QMetaType>

namespace pdf {

inline constexpr int MAX_SEARCH_MATCHES = 10000;

struct SearchMatch {
    int pageIndex{0};
    int charStartIndex{0};
    int charCount{0};
    std::vector<PageCharBox> rects;

    bool operator==(const SearchMatch& o) const noexcept {
        return pageIndex == o.pageIndex &&
               charStartIndex == o.charStartIndex &&
               charCount == o.charCount;
    }
    bool operator!=(const SearchMatch& o) const noexcept {
        return !(*this == o);
    }
};

} // namespace pdf

Q_DECLARE_METATYPE(pdf::SearchMatch)
Q_DECLARE_METATYPE(std::vector<pdf::SearchMatch>)
