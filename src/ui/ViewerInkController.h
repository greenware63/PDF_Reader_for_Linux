#pragma once

#include <cstdint>
#include <vector>

#include <QPainter>

#include "engine/AnnotationTypes.h"
#include "layout/DocumentLayout.h"

namespace ui {

class ViewerInkController {
public:
    ViewerInkController() = default;
    ~ViewerInkController() = default;

    ViewerInkController(const ViewerInkController&) = delete;
    ViewerInkController& operator=(const ViewerInkController&) = delete;
    ViewerInkController(ViewerInkController&&) = delete;
    ViewerInkController& operator=(ViewerInkController&&) = delete;

    // Mode & attributes
    bool isMode() const noexcept { return m_inkDrawingMode; }
    bool setMode(bool enabled) noexcept;

    void setColor(uint32_t colorArgb) noexcept { m_inkColorArgb = colorArgb; }
    uint32_t color() const noexcept { return m_inkColorArgb; }
    void setBorderWidth(double width) noexcept { m_inkBorderWidth = width; }
    double borderWidth() const noexcept { return m_inkBorderWidth; }

    // Stroke state
    bool isDrawing() const noexcept { return m_isDrawingInk; }
    int currentPage() const noexcept { return m_inkCurrentPage; }

    bool beginStroke(const layout::DocumentLayout& layout, int64_t docX, int64_t docY);
    bool extendStroke(const layout::DocumentLayout& layout, int64_t docX, int64_t docY);
    bool finishStroke(int& outPage, std::vector<pdf::AnnotationPoint>& outPoints);

    // Painting
    void drawActiveStroke(QPainter& painter, const layout::DocumentLayout& layout,
                          int pageCount, int64_t scrollDocX, int64_t scrollDocY) const;

private:
    bool m_inkDrawingMode{false};
    uint32_t m_inkColorArgb{0xFF000000};
    double m_inkBorderWidth{2.0};
    bool m_isDrawingInk{false};
    int m_inkCurrentPage{-1};
    std::vector<pdf::AnnotationPoint> m_currentInkStroke;
};

} // namespace ui
