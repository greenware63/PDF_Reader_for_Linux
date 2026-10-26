#pragma once

#include "engine/PageText.h"
#include <vector>
#include <QString>
#include <QMetaType>

namespace pdf {

inline constexpr int MAX_SEARCH_MATCHES = 10000;
/// Upper bound (UTF-16 units) of a result snippet; enforced when building and when deserializing.
inline constexpr int MAX_SEARCH_SNIPPET_CHARS = 256;

struct SearchMatch {
    int pageIndex{0};
    int charStartIndex{0};
    int charCount{0};
    std::vector<PageCharBox> rects;
    /// Short, whitespace-collapsed text around the hit for result lists ("...context HIT context...").
    QString snippet;
    /// Where the hit lies inside `snippet` (UTF-16 offsets); both 0 when there is no snippet.
    int snippetMatchStart{0};
    int snippetMatchLength{0};

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
