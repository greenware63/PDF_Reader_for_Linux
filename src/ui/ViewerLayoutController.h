#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <cmath>
#include <algorithm>
#include <QPoint>
#include <QSize>
#include <QSizeF>
#include "layout/DocumentLayout.h"
#include "layout/ScrollMapper.h"

namespace ui {

struct ViewportGeometry {
    int64_t widthDevice{0};
    int64_t heightDevice{0};
    double dpr{1.0};
};

struct ScrollBarConfig {
    bool hasRange{false};
    int32_t min{0};
    int32_t max{0};
    int32_t pageStep{0};
    int32_t singleStep{0};

    bool hasValue{false};
    int32_t value{0};
};

struct LayoutChangeResult {
    bool hasChanges{false};
    bool scrollBarsChanged{false};
    ScrollBarConfig verticalScroll;
    ScrollBarConfig horizontalScroll;

    bool generationIncremented{false};
    bool clearTransformedPixmaps{false};
    bool recordFallbackScale{false};
    double fallbackScale{0.0};

    bool emitPageChanged{false};
    int newCurrentPage{0};
    int totalPages{0};

    bool emitZoomChanged{false};
    double zoomRatio{0.0};

    bool emitViewModeChanged{false};
    layout::ViewMode newViewMode{layout::ViewMode::Continuous};

    bool emitRotationChanged{false};
    int newRotation{0};

    bool updateFormEditorGeometry{false};
    bool needsRepaint{false};
};

enum class WheelFlipAction {
    None,
    PassToDefault,
    FlipForward,
    FlipBackward
};

struct WheelFlipResult {
    WheelFlipAction action{WheelFlipAction::None};
    int targetPage{0};
    bool arriveAtBottom{false};
};

void mergeLayoutResults(LayoutChangeResult& target, const LayoutChangeResult& other);

class ViewerLayoutController {
public:
    static constexpr double DESKTOP_100_PERCENT_SCALE = 96.0 / 72.0;
    static constexpr double ZOOM_STEP = 1.25;
    static constexpr int PAGE_FLIP_THRESHOLD = 240;
    static constexpr int WHEEL_ZOOM_THRESHOLD = 120;

    ViewerLayoutController();
    ~ViewerLayoutController() = default;

    void setDocument(int pageCount, const std::vector<std::pair<double, double>>& pageSizes);
    void resetDocument();

    bool isDocumentOpen() const noexcept { return m_pageCount > 0; }
    int pageCount() const noexcept { return m_pageCount; }
    int currentPage() const noexcept { return m_currentPage; }
    void setCurrentPage(int page) noexcept { m_currentPage = page; }
    bool isNavigating() const noexcept { return m_navigating; }
    void setNavigating(bool navigating) noexcept { m_navigating = navigating; }
    double currentZoom() const noexcept { return m_scale; }
    int rotation() const noexcept { return m_rotation; }
    layout::ViewMode viewMode() const noexcept { return m_layout.viewMode(); }

    int64_t currentDocScrollX() const noexcept { return m_docScrollX; }
    int64_t currentDocScrollY() const noexcept { return m_docScrollY; }

    const layout::DocumentLayout& layout() const noexcept { return m_layout; }
    layout::DocumentLayout& layout() noexcept { return m_layout; }
    const layout::ScrollMapper& scrollMapper() const noexcept { return m_scrollMapper; }

    // Navigation
    LayoutChangeResult goToPage(int pageIndex, std::optional<double> targetYFraction, const ViewportGeometry& vp);
    LayoutChangeResult nextPage(const ViewportGeometry& vp);
    LayoutChangeResult previousPage(const ViewportGeometry& vp);
    void ensurePageShown(int pageIndex, const ViewportGeometry& vp, LayoutChangeResult& result);

    // Zoom & fit
    LayoutChangeResult setZoom(double zoom, const QPoint* anchorWidgetPos, const ViewportGeometry& vp);
    LayoutChangeResult zoomIn(const ViewportGeometry& vp);
    LayoutChangeResult zoomOut(const ViewportGeometry& vp);
    LayoutChangeResult zoomReset(const ViewportGeometry& vp);
    LayoutChangeResult zoomFitWidth(const ViewportGeometry& vp);
    LayoutChangeResult zoomFitPage(const ViewportGeometry& vp);

    // View mode & rotation
    LayoutChangeResult setViewMode(layout::ViewMode mode, const ViewportGeometry& vp);
    LayoutChangeResult setRotation(int degrees, const ViewportGeometry& vp);
    LayoutChangeResult rotateClockwise(const ViewportGeometry& vp);
    LayoutChangeResult rotateCounterClockwise(const ViewportGeometry& vp);

    // Scroll & layout geometry
    void setDocScrollPosition(int64_t docX, int64_t docY, const ViewportGeometry& vp, LayoutChangeResult& result);
    void updateScrollBars(const ViewportGeometry& vp, LayoutChangeResult& result);
    LayoutChangeResult updateLayoutGeometry(const ViewportGeometry& vp);
    LayoutChangeResult onScrollValueChanged(int hSliderVal, int vSliderVal, const ViewportGeometry& vp);

    // Reading position
    LayoutChangeResult restoreReadingPosition(int pageIndex, double zoom, int64_t scrollX, int64_t scrollY, const ViewportGeometry& vp);
    LayoutChangeResult restoreReadingPosition(int pageIndex, double zoom, double fractionX, double fractionY,
                                            int64_t fallbackScrollX, int64_t fallbackScrollY, const ViewportGeometry& vp);
    bool getReadingPosition(int& outPage, double& outFractionX, double& outFractionY) const;

    // Wheel helpers
    WheelFlipResult handleWheelPageFlip(int angleDeltaY, bool atTop, bool atBottom);
    int pageFlipAccumulator() const noexcept { return m_pageFlipAccumulator; }
    void resetPageFlipAccumulator() noexcept { m_pageFlipAccumulator = 0; }

private:
    layout::DocumentLayout m_layout;
    layout::ScrollMapper m_scrollMapper;
    int m_pageCount{0};
    int m_currentPage{0};
    double m_scale{DESKTOP_100_PERCENT_SCALE};
    int m_rotation{0};
    int m_pageFlipAccumulator{0};
    bool m_navigating{false};

    int64_t m_docScrollX{0};
    int64_t m_docScrollY{0};
    int64_t m_lastVpWidthDevice{0};
    int64_t m_lastVpHeightDevice{0};
};

} // namespace ui
