#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <algorithm>
#include <cmath>

namespace layout {

/**
 * @brief Canonical page pixel dimension calculation shared between layout and engine.
 * Ensures the layout grid and tile renderer agree on pixel sizes to the exact pixel.
 * Hardened against non-positive dimensions, NaNs, infinities, and massive MediaBoxes.
 */
inline void computePagePixelDimensions(double widthPoints, double heightPoints,
                                       double scale, double dpr,
                                       int& outPixelW, int& outPixelH) {
    // Sane upper limit per dimension (~13 meters wide at 96 DPI) to prevent integer overflow
    constexpr int MAX_SAFE_PIXEL_DIM = 500000;

    if (!(widthPoints > 0.0) || !(heightPoints > 0.0) ||
        std::isnan(widthPoints) || std::isnan(heightPoints) ||
        std::isinf(widthPoints) || std::isinf(heightPoints)) {
        outPixelW = 0;
        outPixelH = 0;
        return;
    }

    const double effDpr = (dpr > 0.0 && !std::isnan(dpr) && !std::isinf(dpr)) ? dpr : 1.0;
    const double s = (scale > 0.0 && !std::isnan(scale) && !std::isinf(scale)) ? scale : 1.0;

    double rawW = std::round(widthPoints * s * effDpr);
    double rawH = std::round(heightPoints * s * effDpr);

    if (std::isnan(rawW) || !(rawW > 0.0)) {
        outPixelW = 1;
    } else if (rawW > MAX_SAFE_PIXEL_DIM) {
        outPixelW = MAX_SAFE_PIXEL_DIM;
    } else {
        outPixelW = static_cast<int>(rawW);
    }

    if (std::isnan(rawH) || !(rawH > 0.0)) {
        outPixelH = 1;
    } else if (rawH > MAX_SAFE_PIXEL_DIM) {
        outPixelH = MAX_SAFE_PIXEL_DIM;
    } else {
        outPixelH = static_cast<int>(rawH);
    }
}

struct Rect64 {
    int64_t x{0};
    int64_t y{0};
    int64_t width{0};
    int64_t height{0};

    int64_t right() const { return x + width; }
    int64_t bottom() const { return y + height; }
    bool isValid() const { return width > 0 && height > 0; }
};

struct PageLayout {
    int pageIndex{0};
    int64_t leftPixel{0};    // X offset from left in document coordinates
    int64_t topPixel{0};     // Y offset from top in document coordinates
    int64_t widthPixel{0};   // Page width in device pixels
    int64_t heightPixel{0};  // Page height in device pixels
    double widthPoints{0.0};
    double heightPoints{0.0};

    int64_t rightPixel() const { return leftPixel + widthPixel; }
    int64_t bottomPixel() const { return topPixel + heightPixel; }
};

struct ViewportRange {
    int firstVisiblePage{-1};
    int lastVisiblePage{-1};

    bool isValid() const { return firstVisiblePage >= 0 && lastVisiblePage >= firstVisiblePage; }
};

/**
 * @brief Scroll anchor representing normalized coordinates inside a page.
 * Note: Normalized coordinates clamp to [0.0, 1.0], meaning cursors hovering in
 * horizontal or vertical page margins snap to the nearest page border.
 */
struct ScrollAnchor {
    int pageIndex{-1};
    double normalizedX{0.0}; // Fraction [0.0, 1.0] across page width
    double normalizedY{0.0}; // Fraction [0.0, 1.0] down page height

    bool isValid() const { return pageIndex >= 0; }
};

struct VisibleTile {
    int pageIndex{0};
    int col{0};
    int row{0};
    int tileXOnPage{0};     // X offset in page raster grid (pixels)
    int tileYOnPage{0};     // Y offset in page raster grid (pixels)
    int tileWidth{0};       // Width in pixels (usually 512, or clipped at edge)
    int tileHeight{0};      // Height in pixels
    int64_t docX{0};        // Document X in 64-bit device pixels
    int64_t docY{0};        // Document Y in 64-bit device pixels
};

/**
 * @brief Pure C++20 document layout engine operating in 64-bit device pixel space.
 * Free of any Qt or GUI dependencies.
 * Handles continuous page scrolling, binary-search visible page queries, 2D zoom anchoring,
 * and visible tile enumeration.
 */
class DocumentLayout {
public:
    DocumentLayout() = default;

    /**
     * @brief Initializes the document layout with page dimensions in PDF points.
     */
    void setPageSizes(const std::vector<std::pair<double, double>>& pageSizePoints);

    /**
     * @brief Recomputes 64-bit pixel layout when scale, device pixel ratio, or viewport width changes.
     */
    void updateLayout(double scale, double dpr, int64_t viewportWidth = 0,
                      int64_t pageGapPixels = 16, int64_t horizontalMarginPixels = 16);

    int pageCount() const { return static_cast<int>(m_pages.size()); }
    int64_t totalDocumentHeight() const { return m_totalHeight; }
    int64_t maxDocumentWidth() const { return m_maxWidth; }
    double currentScale() const { return m_scale; }
    double currentDpr() const { return m_dpr; }

    const PageLayout& getPageLayout(int pageIndex) const;

    /**
     * @brief Finds the range of pages overlapping the visible viewport in O(log N) time.
     * Returns invalid range if viewport sits entirely inside a gap between pages.
     */
    ViewportRange getVisiblePages(int64_t viewportTopPixel, int64_t viewportHeightPixel) const;

    /**
     * @brief Enumerates visible tiles for a viewport rectangle without any GUI dependency.
     */
    std::vector<VisibleTile> getVisibleTiles(const Rect64& viewport, int tileSize = 512) const;

    /**
     * @brief Creates a 2D zoom anchor pinned to the point under cursor/viewport offset.
     */
    ScrollAnchor createAnchor(int64_t docX, int64_t docY) const;

    /**
     * @brief Resolves a 2D anchor back into document scroll offsets after zoom update.
     */
    void resolveAnchor(const ScrollAnchor& anchor, int64_t viewportOffsetX, int64_t viewportOffsetY,
                       int64_t viewportW, int64_t viewportH,
                       int64_t& outScrollX, int64_t& outScrollY) const;

private:
    std::vector<PageLayout> m_pages;
    int64_t m_totalHeight{0};
    int64_t m_maxWidth{0};
    int64_t m_pageGap{16};
    int64_t m_horizontalMargin{16};
    int64_t m_viewportWidth{0};
    double m_scale{1.0};
    double m_dpr{1.0};
};

} // namespace layout
