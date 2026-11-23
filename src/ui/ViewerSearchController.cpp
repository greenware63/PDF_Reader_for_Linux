#include "ui/ViewerSearchController.h"

#include <algorithm>
#include <cmath>

namespace ui {

int64_t ViewerSearchController::beginSearch(const QString& query, bool matchCase, int startPage) {
    const int64_t searchId = ++m_currentSearchId;
    m_searchQuery = query;
    m_searchMatchCase = matchCase;
    m_searchStartPage = startPage;
    m_searchMatches.clear();
    m_currentMatchIndex = -1;
    return searchId;
}

void ViewerSearchController::clear() {
    ++m_currentSearchId;
    m_searchQuery.clear();
    m_searchMatches.clear();
    m_currentMatchIndex = -1;
}

const pdf::SearchMatch* ViewerSearchController::next() {
    if (m_searchMatches.empty()) {
        return nullptr;
    }
    m_currentMatchIndex = (m_currentMatchIndex + 1) % static_cast<int>(m_searchMatches.size());
    return &m_searchMatches[static_cast<size_t>(m_currentMatchIndex)];
}

const pdf::SearchMatch* ViewerSearchController::previous() {
    if (m_searchMatches.empty()) {
        return nullptr;
    }
    m_currentMatchIndex = (m_currentMatchIndex - 1 + static_cast<int>(m_searchMatches.size())) % static_cast<int>(m_searchMatches.size());
    return &m_searchMatches[static_cast<size_t>(m_currentMatchIndex)];
}

const pdf::SearchMatch* ViewerSearchController::goTo(int index) {
    if (index < 0 || index >= static_cast<int>(m_searchMatches.size())) {
        return nullptr;
    }
    m_currentMatchIndex = index;
    return &m_searchMatches[static_cast<size_t>(index)];
}

bool ViewerSearchController::acceptsReply(int64_t documentId, int64_t currentDocumentId, int64_t searchId) const noexcept {
    return (documentId == currentDocumentId && searchId == m_currentSearchId);
}

ViewerSearchController::BatchResult ViewerSearchController::addMatches(const std::vector<pdf::SearchMatch>& matches, int pageCount) {
    const bool hadSelection = m_currentMatchIndex >= 0 &&
        m_currentMatchIndex < static_cast<int>(m_searchMatches.size());
    const pdf::SearchMatch selectedMatch = hadSelection
        ? m_searchMatches[static_cast<size_t>(m_currentMatchIndex)]
        : pdf::SearchMatch{};
    const size_t remaining = static_cast<size_t>(std::max(0, pdf::MAX_SEARCH_MATCHES - static_cast<int>(m_searchMatches.size())));
    const size_t count = std::min(remaining, matches.size());
    m_searchMatches.insert(m_searchMatches.end(), matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(count));
    std::sort(m_searchMatches.begin(), m_searchMatches.end(), [](const pdf::SearchMatch& lhs, const pdf::SearchMatch& rhs) {
        if (lhs.pageIndex != rhs.pageIndex) {
            return lhs.pageIndex < rhs.pageIndex;
        }
        return lhs.charStartIndex < rhs.charStartIndex;
    });

    BatchResult result;
    if (hadSelection) {
        const auto selectedIt = std::find(m_searchMatches.begin(), m_searchMatches.end(), selectedMatch);
        m_currentMatchIndex = selectedIt == m_searchMatches.end()
            ? -1
            : static_cast<int>(std::distance(m_searchMatches.begin(), selectedIt));
    } else if (!m_searchMatches.empty()) {
        const auto circularDistance = [this, pageCount](int pageIndex) {
            return (pageIndex - m_searchStartPage + pageCount) % pageCount;
        };
        const auto nearestIt = std::min_element(m_searchMatches.begin(), m_searchMatches.end(),
            [&circularDistance](const pdf::SearchMatch& lhs, const pdf::SearchMatch& rhs) {
                const int lhsDistance = circularDistance(lhs.pageIndex);
                const int rhsDistance = circularDistance(rhs.pageIndex);
                if (lhsDistance != rhsDistance) {
                    return lhsDistance < rhsDistance;
                }
                return lhs.charStartIndex < rhs.charStartIndex;
            });
        m_currentMatchIndex = static_cast<int>(std::distance(m_searchMatches.begin(), nearestIt));
        result.scrollTarget = &(*nearestIt);
    }
    return result;
}

} // namespace ui
