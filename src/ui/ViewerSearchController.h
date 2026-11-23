#pragma once

#include <cstdint>
#include <vector>

#include <QString>

#include "engine/SearchTypes.h"

namespace ui {

class ViewerSearchController {
public:
    ViewerSearchController() = default;
    ~ViewerSearchController() = default;

    ViewerSearchController(const ViewerSearchController&) = delete;
    ViewerSearchController& operator=(const ViewerSearchController&) = delete;
    ViewerSearchController(ViewerSearchController&&) = delete;
    ViewerSearchController& operator=(ViewerSearchController&&) = delete;

    // Search state accessors
    const std::vector<pdf::SearchMatch>& matches() const noexcept { return m_searchMatches; }
    int currentIndex() const noexcept { return m_currentMatchIndex; }
    bool hasMatches() const noexcept { return !m_searchMatches.empty(); }
    size_t matchCount() const noexcept { return m_searchMatches.size(); }
    int64_t currentSearchId() const noexcept { return m_currentSearchId; }
    const QString& searchQuery() const noexcept { return m_searchQuery; }
    bool matchCase() const noexcept { return m_searchMatchCase; }
    int startPage() const noexcept { return m_searchStartPage; }

    int64_t beginSearch(const QString& query, bool matchCase, int startPage);
    void clear();

    const pdf::SearchMatch* next();
    const pdf::SearchMatch* previous();
    const pdf::SearchMatch* goTo(int index);

    bool acceptsReply(int64_t documentId, int64_t currentDocumentId, int64_t searchId) const noexcept;

    struct BatchResult {
        const pdf::SearchMatch* scrollTarget{nullptr};
    };
    BatchResult addMatches(const std::vector<pdf::SearchMatch>& matches, int pageCount);

    int completedDisplayIndex() const noexcept {
        return (m_currentMatchIndex >= 0) ? (m_currentMatchIndex + 1) : 0;
    }

private:
    int64_t m_currentSearchId{0};
    QString m_searchQuery;
    bool m_searchMatchCase{false};
    int m_searchStartPage{0};
    std::vector<pdf::SearchMatch> m_searchMatches;
    int m_currentMatchIndex{-1};
};

} // namespace ui
