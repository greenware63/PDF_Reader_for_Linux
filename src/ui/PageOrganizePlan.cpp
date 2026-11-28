#include "PageOrganizePlan.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace ui {

PageOrganizePlan::PageOrganizePlan(int sourcePageCount)
    : m_sourcePageCount(std::max(0, sourcePageCount)) {
    reset();
}

void PageOrganizePlan::reset() {
    m_entries.clear();
    m_entries.reserve(static_cast<size_t>(m_sourcePageCount));
    for (int i = 0; i < m_sourcePageCount; ++i) {
        m_entries.push_back(pdf::PageSpec{i, 0});
    }
}

bool PageOrganizePlan::isUnchanged() const noexcept {
    if (m_entries.size() != static_cast<size_t>(m_sourcePageCount)) {
        return false;
    }
    for (int i = 0; i < m_sourcePageCount; ++i) {
        const auto& e = m_entries[static_cast<size_t>(i)];
        if (e.sourceIndex != i || e.rotationDegrees != 0) {
            return false;
        }
    }
    return true;
}

bool PageOrganizePlan::containsSourcePage(int sourceIndex) const noexcept {
    for (const auto& e : m_entries) {
        if (e.sourceIndex == sourceIndex) {
            return true;
        }
    }
    return false;
}

bool PageOrganizePlan::deleteAt(int pos) {
    if (pos < 0 || pos >= count() || count() <= 1) {
        return false;
    }
    m_entries.erase(m_entries.begin() + pos);
    return true;
}

bool PageOrganizePlan::deletePositions(const std::vector<int>& positions) {
    if (positions.empty()) {
        return false;
    }
    std::vector<bool> toRemove(m_entries.size(), false);
    int removeCount = 0;
    for (int pos : positions) {
        if (pos >= 0 && pos < count() && !toRemove[static_cast<size_t>(pos)]) {
            toRemove[static_cast<size_t>(pos)] = true;
            ++removeCount;
        }
    }
    if (removeCount == 0 || removeCount >= count()) {
        return false;
    }
    std::vector<pdf::PageSpec> kept;
    kept.reserve(m_entries.size() - static_cast<size_t>(removeCount));
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (!toRemove[i]) {
            kept.push_back(m_entries[i]);
        }
    }
    m_entries = std::move(kept);
    return true;
}

bool PageOrganizePlan::restore(int sourceIndex) {
    if (sourceIndex < 0 || sourceIndex >= m_sourcePageCount || containsSourcePage(sourceIndex)) {
        return false;
    }
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [sourceIndex](const pdf::PageSpec& e) {
        return e.sourceIndex > sourceIndex;
    });
    m_entries.insert(it, pdf::PageSpec{sourceIndex, 0});
    return true;
}

bool PageOrganizePlan::move(int fromPos, int toPos) {
    const int n = count();
    if (fromPos < 0 || fromPos >= n || toPos < 0 || toPos >= n) {
        return false;
    }
    if (fromPos == toPos) {
        return true;
    }
    const pdf::PageSpec item = m_entries[static_cast<size_t>(fromPos)];
    m_entries.erase(m_entries.begin() + fromPos);
    m_entries.insert(m_entries.begin() + toPos, item);
    return true;
}

bool PageOrganizePlan::reorderBySourceIndices(const std::vector<int>& newSourceOrder) {
    if (newSourceOrder.size() != m_entries.size()) {
        return false;
    }
    std::unordered_map<int, pdf::PageSpec> bySource;
    bySource.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        bySource.emplace(e.sourceIndex, e);
    }

    std::unordered_set<int> seen;
    seen.reserve(newSourceOrder.size());
    std::vector<pdf::PageSpec> reordered;
    reordered.reserve(newSourceOrder.size());

    for (int srcIdx : newSourceOrder) {
        auto it = bySource.find(srcIdx);
        if (it == bySource.end() || !seen.insert(srcIdx).second) {
            return false;
        }
        reordered.push_back(it->second);
    }
    m_entries = std::move(reordered);
    return true;
}

bool PageOrganizePlan::rotateBy(int pos, int deltaDegrees) {
    if (pos < 0 || pos >= count()) {
        return false;
    }
    int& rot = m_entries[static_cast<size_t>(pos)].rotationDegrees;
    rot = ((rot + deltaDegrees) % 360 + 360) % 360;
    return true;
}

bool PageOrganizePlan::rotateClockwise(int pos) {
    return rotateBy(pos, 90);
}

bool PageOrganizePlan::rotateCounterClockwise(int pos) {
    return rotateBy(pos, -90);
}

} // namespace ui
