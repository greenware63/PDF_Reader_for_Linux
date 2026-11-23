#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <QFont>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRectF>

#include "engine/PageText.h"
#include "ipc/IpcMessages.h"
#include "layout/DocumentLayout.h"

namespace ui {

class ViewerRedactionController {
public:
    ViewerRedactionController() = default;
    ~ViewerRedactionController() = default;

    ViewerRedactionController(const ViewerRedactionController&) = delete;
    ViewerRedactionController& operator=(const ViewerRedactionController&) = delete;
    ViewerRedactionController(ViewerRedactionController&&) = delete;
    ViewerRedactionController& operator=(ViewerRedactionController&&) = delete;

    // Mode
    bool isMode() const noexcept { return m_mode; }
    bool mode() const noexcept { return m_mode; }
    bool setMode(bool enabled) noexcept;

    // Drag state
    bool isDragging() const noexcept { return m_isDragging; }
    int dragStartPage() const noexcept { return m_dragStartPage; }
    const QPointF& dragStartDocPos() const noexcept { return m_dragStartDocPos; }
    const QPointF& dragCurrentDocPos() const noexcept { return m_dragCurrentDocPos; }

    void beginDrag(int page, const QPointF& docPos) noexcept;
    void updateDrag(const QPointF& docPos) noexcept;
    void resetDrag() noexcept;
    bool finishDrag(const layout::DocumentLayout& layout, bool documentOpen,
                    int& outPage, pdf::ipc::RedactRect& outRect) noexcept;

    // Staged marks
    bool addStaged(int pageIndex, const pdf::ipc::RedactRect& rect, int pageCount);
    bool stageSelectionRect(int page, const pdf::PageCharBox& box,
                            double widthPoints, double heightPoints);
    bool clear() noexcept;
    int count() const noexcept;
    bool empty() const noexcept { return m_stagedRedactions.empty(); }
    const std::map<int, std::vector<pdf::ipc::RedactRect>>& marks() const noexcept {
        return m_stagedRedactions;
    }

    // Overlays
    void drawOverlays(QPainter& painter, const layout::DocumentLayout& layout,
                      const layout::ViewportRange& visPages,
                      int64_t scrollDocX, int64_t scrollDocY, double effScale) const;

private:
    bool m_mode{false};
    std::map<int, std::vector<pdf::ipc::RedactRect>> m_stagedRedactions;

    bool m_isDragging{false};
    int m_dragStartPage{-1};
    QPointF m_dragStartDocPos;
    QPointF m_dragCurrentDocPos;
};

} // namespace ui
