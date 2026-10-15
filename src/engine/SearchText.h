#pragma once

#include "engine/SearchTypes.h"
#include <QString>
#include <vector>

namespace pdf {

inline constexpr int MAX_SEARCH_MATCHES = 10000;

std::vector<SearchMatch> findTextInPage(
    const PageText& pageText,
    const QString& query,
    bool matchCase,
    int maxMatches = MAX_SEARCH_MATCHES,
    bool* truncated = nullptr);

} // namespace pdf
