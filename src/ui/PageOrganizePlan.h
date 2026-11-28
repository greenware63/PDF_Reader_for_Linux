#pragma once

#include <cstddef>
#include "engine/PageOrganizationTypes.h"
#include <vector>

namespace ui {

/**
 * @brief Pure, GUI-free model holding page order, deletions, and rotation deltas
 * for the Organize Pages dialog.
 *
 * Invariants:
 *  - Every entry's sourceIndex is in [0, sourcePageCount).
 *  - No duplicate sourceIndex values.
 *  - Never empty when sourcePageCount > 0 (deletions that would empty the plan are rejected).
 *  - Every rotationDegrees is normalized to {0, 90, 180, 270}.
 */
class PageOrganizePlan {
public:
    explicit PageOrganizePlan(int sourcePageCount = 0);

    int sourcePageCount() const noexcept { return m_sourcePageCount; }
    int count() const noexcept { return static_cast<int>(m_entries.size()); }
    bool empty() const noexcept { return m_entries.empty(); }
    bool isUnchanged() const noexcept;

    const std::vector<pdf::PageSpec>& pageSpecs() const noexcept { return m_entries; }
    bool containsSourcePage(int sourceIndex) const noexcept;

    /// Deletes the entry at @p pos. Returns false if @p pos is out of range or would leave the plan empty.
    bool deleteAt(int pos);

    /// Deletes multiple entries by their current positions. Returns false if all entries would be deleted.
    bool deletePositions(const std::vector<int>& positions);

    /// Restores a deleted @p sourceIndex (with 0° rotation) in its natural source order.
    bool restore(int sourceIndex);

    /// Moves entry at @p fromPos to @p toPos.
    bool move(int fromPos, int toPos);

    /// Reorders entries to match @p newSourceOrder (must be a permutation of current entries' sourceIndex values).
    bool reorderBySourceIndices(const std::vector<int>& newSourceOrder);

    /// Rotates entry at @p pos by +90° (clockwise).
    bool rotateClockwise(int pos);

    /// Rotates entry at @p pos by -90° (+270° mod 360, counter-clockwise).
    bool rotateCounterClockwise(int pos);

    /// Resets the plan to the original unedited page order and 0° rotations.
    void reset();

private:
    bool rotateBy(int pos, int deltaDegrees);

    int m_sourcePageCount{0};
    std::vector<pdf::PageSpec> m_entries;
};

} // namespace ui
