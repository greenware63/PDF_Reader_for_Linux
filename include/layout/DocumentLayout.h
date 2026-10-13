#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include <algorithm>
#include <cmath>

namespace layout {

struct PageLayout {
    int pageIndex{0};
    int64_t topPixel{0};     // Y offset from top of document in device pixels
    int64_t heightPixel{0};  // Page height in device pixels
    int64_t widthPixel{0};   // Page width in device pixels
    double widthPoints{0.0};
    double heightPoints{0.0};
};

struct ViewportRange {
    int firstVisiblePage{-1};
    int lastVisiblePage{-1};

    bool isValid() const { return firstVisiblePage >= 0 && lastVisiblePage >= firstVisiblePage; }
};

struct ScrollAnchor {
    int pageIndex{0};
    double normalizedY{0.0}; // Fraction [0.0, 1.0] down the page
};

/**
 * @brief Pure C++20 document layout engine operating in 64-bit device pixel space.
 * Free of any Qt or GUI dependencies.
 * Handles continuous page scrolling, binary-search visible page queries, and zoom anchoring.
 */
class DocumentLayout {
public:
    DocumentLayout() = default;

    /**
     * @brief Initializes the document layout with page dimensions in PDF points.
     */
    void setPageSizes(const std::vector<std::pair<double, double>>& pageSizePoints);

    /**
     * @brief Recomputes 64-bit pixel offsets when scale or device pixel ratio changes.
     */
    void updateLayout(double scale, double dpr, int64_t pageGapPixels = 16);

    int pageCount() const { return static_cast<int>(m_pages.size()); }
    int64_t totalDocumentHeight() const { return m_totalHeight; }
    int64_t maxDocumentWidth() const { return m_maxWidth; }
    double currentScale() const { return m_scale; }
    double currentDpr() const { return m_dpr; }

    const PageLayout& getPageLayout(int pageIndex) const;

    /**
     * @brief Finds the range of pages overlapping the visible viewport in O(log N) time.
     */
    ViewportRange getVisiblePages(int64_t viewportTopPixel, int64_t viewportHeightPixel) const;

    /**
     * @brief Creates a zoom anchor pinned to the point currently at the viewport center.
     */
    ScrollAnchor createAnchor(int64_t viewportCenterPixel) const;

    /**
     * @brief Resolves an anchor back into a viewport top scroll offset after a zoom update.
     */
    int64_t resolveAnchor(const ScrollAnchor& anchor, int64_t viewportHeightPixel) const;

private:
    std::vector<PageLayout> m_pages;
    int64_t m_totalHeight{0};
    int64_t m_maxWidth{0};
    int64_t m_pageGap{16};
    double m_scale{1.0};
    double m_dpr{1.0};
};

} // namespace layout
