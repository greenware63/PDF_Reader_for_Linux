#include "layout/DocumentLayout.h"

namespace layout {

void DocumentLayout::setPageSizes(const std::vector<std::pair<double, double>>& pageSizePoints) {
    m_pages.clear();
    m_pages.reserve(pageSizePoints.size());

    for (size_t i = 0; i < pageSizePoints.size(); ++i) {
        PageLayout page;
        page.pageIndex = static_cast<int>(i);
        page.widthPoints = pageSizePoints[i].first;
        page.heightPoints = pageSizePoints[i].second;
        m_pages.push_back(page);
    }

    updateLayout(m_scale, m_dpr, m_viewportWidth, m_pageGap, m_horizontalMargin);
}

void DocumentLayout::updateLayout(double scale, double dpr, int64_t viewportWidth,
                                  int64_t pageGapPixels, int64_t horizontalMarginPixels) {
    m_scale = (scale > 0.0) ? scale : 1.0;
    m_dpr = (dpr > 0.0) ? dpr : 1.0;
    m_pageGap = (pageGapPixels >= 0) ? pageGapPixels : 16;
    m_horizontalMargin = (horizontalMarginPixels >= 0) ? horizontalMarginPixels : 16;
    m_viewportWidth = viewportWidth;

    m_totalHeight = 0;
    m_maxWidth = 0;

    if (m_pages.empty()) {
        return;
    }

    // First pass: compute device pixel dimensions for each page and find the widest page
    int64_t widestPage = 0;
    for (auto& page : m_pages) {
        int w = 0, h = 0;
        computePagePixelDimensions(page.widthPoints, page.heightPoints, m_scale, m_dpr, w, h);
        page.widthPixel = w;
        page.heightPixel = h;
        if (page.widthPixel > widestPage) {
            widestPage = page.widthPixel;
        }
    }

    // Scrollable content width is at least the viewport, or wide enough to fit the widest page with margins
    int64_t contentWidth = std::max(m_viewportWidth, widestPage + (2 * m_horizontalMargin));
    m_maxWidth = contentWidth;

    // Second pass: position all pages centered within contentWidth
    int64_t currentTop = m_pageGap;
    for (auto& page : m_pages) {
        page.topPixel = currentTop;
        page.leftPixel = (contentWidth - page.widthPixel) / 2;
        currentTop += page.heightPixel + m_pageGap;
    }

    m_totalHeight = currentTop;
}

const PageLayout& DocumentLayout::getPageLayout(int pageIndex) const {
    static const PageLayout s_empty{};
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return s_empty;
    }
    return m_pages[pageIndex];
}

ViewportRange DocumentLayout::getVisiblePages(int64_t viewportTopPixel, int64_t viewportHeightPixel) const {
    if (m_pages.empty() || viewportHeightPixel <= 0) {
        return {};
    }

    const int64_t viewportBottom = viewportTopPixel + viewportHeightPixel;

    // Viewport entirely above first page or entirely below last page
    if (viewportBottom <= m_pages.front().topPixel || viewportTopPixel >= m_pages.back().bottomPixel()) {
        return {};
    }

    // Binary search for first candidate page whose bottom edge is > viewportTop
    auto itFirst = std::upper_bound(
        m_pages.begin(),
        m_pages.end(),
        viewportTopPixel,
        [](int64_t top, const PageLayout& page) {
            return top < page.bottomPixel();
        }
    );

    if (itFirst == m_pages.end()) {
        return {};
    }

    // Binary search for candidate page whose top edge is >= viewportBottom
    auto itLast = std::lower_bound(
        itFirst,
        m_pages.end(),
        viewportBottom,
        [](const PageLayout& page, int64_t bottom) {
            return page.topPixel < bottom;
        }
    );

    int firstIdx = itFirst->pageIndex;
    int lastIdx = (itLast == m_pages.end()) ? (static_cast<int>(m_pages.size()) - 1) : itLast->pageIndex;

    // Filter to find actual overlapping pages (guards against sitting entirely in a gap)
    int actualFirst = -1;
    int actualLast = -1;

    for (int i = firstIdx; i <= lastIdx && i < static_cast<int>(m_pages.size()); ++i) {
        const auto& p = m_pages[i];
        if (p.topPixel < viewportBottom && p.bottomPixel() > viewportTopPixel) {
            if (actualFirst == -1) actualFirst = i;
            actualLast = i;
        }
    }

    if (actualFirst == -1) {
        // Viewport sits entirely within the gap between pages!
        return {};
    }

    return {actualFirst, actualLast};
}

std::vector<VisibleTile> DocumentLayout::getVisibleTiles(const Rect64& viewport, int tileSize) const {
    std::vector<VisibleTile> result;
    if (!viewport.isValid() || tileSize <= 0) {
        return result;
    }

    auto range = getVisiblePages(viewport.y, viewport.height);
    if (!range.isValid()) {
        return result;
    }

    const int64_t vpRight = viewport.right();
    const int64_t vpBottom = viewport.bottom();

    for (int pageIdx = range.firstVisiblePage; pageIdx <= range.lastVisiblePage; ++pageIdx) {
        const auto& page = m_pages[pageIdx];

        // Intersect viewport with page bounding box
        int64_t interX0 = std::max(viewport.x, page.leftPixel);
        int64_t interY0 = std::max(viewport.y, page.topPixel);
        int64_t interX1 = std::min(vpRight, page.rightPixel());
        int64_t interY1 = std::min(vpBottom, page.bottomPixel());

        if (interX1 <= interX0 || interY1 <= interY0) {
            continue;
        }

        // Relative coordinates within the page raster grid
        int relX0 = static_cast<int>(interX0 - page.leftPixel);
        int relY0 = static_cast<int>(interY0 - page.topPixel);
        int relX1 = static_cast<int>(interX1 - page.leftPixel);
        int relY1 = static_cast<int>(interY1 - page.topPixel);

        int col0 = relX0 / tileSize;
        int row0 = relY0 / tileSize;
        int col1 = (relX1 - 1) / tileSize;
        int row1 = (relY1 - 1) / tileSize;

        for (int r = row0; r <= row1; ++r) {
            for (int c = col0; c <= col1; ++c) {
                int tileX = c * tileSize;
                int tileY = r * tileSize;
                int tw = std::min<int>(tileSize, static_cast<int>(page.widthPixel - tileX));
                int th = std::min<int>(tileSize, static_cast<int>(page.heightPixel - tileY));

                if (tw > 0 && th > 0) {
                    VisibleTile tile;
                    tile.pageIndex = pageIdx;
                    tile.col = c;
                    tile.row = r;
                    tile.tileXOnPage = tileX;
                    tile.tileYOnPage = tileY;
                    tile.tileWidth = tw;
                    tile.tileHeight = th;
                    tile.docX = page.leftPixel + tileX;
                    tile.docY = page.topPixel + tileY;
                    result.push_back(tile);
                }
            }
        }
    }

    return result;
}

ScrollAnchor DocumentLayout::createAnchor(int64_t docX, int64_t docY) const {
    if (m_pages.empty()) {
        return {};
    }

    // Find page containing docY or nearest page
    auto it = std::upper_bound(
        m_pages.begin(),
        m_pages.end(),
        docY,
        [](int64_t y, const PageLayout& page) {
            return y < page.bottomPixel();
        }
    );

    if (it == m_pages.end()) {
        it = m_pages.end() - 1;
    }

    double normX = 0.5;
    double normY = 0.5;

    if (it->widthPixel > 0) {
        int64_t offX = docX - it->leftPixel;
        normX = std::clamp(static_cast<double>(offX) / it->widthPixel, 0.0, 1.0);
    }
    if (it->heightPixel > 0) {
        int64_t offY = docY - it->topPixel;
        normY = std::clamp(static_cast<double>(offY) / it->heightPixel, 0.0, 1.0);
    }

    return {it->pageIndex, normX, normY};
}

void DocumentLayout::resolveAnchor(const ScrollAnchor& anchor, int64_t viewportOffsetX, int64_t viewportOffsetY,
                                   int64_t viewportW, int64_t viewportH,
                                   int64_t& outScrollX, int64_t& outScrollY) const {
    if (!anchor.isValid() || m_pages.empty()) {
        outScrollX = 0;
        outScrollY = 0;
        return;
    }

    int idx = std::clamp(anchor.pageIndex, 0, static_cast<int>(m_pages.size()) - 1);
    const auto& page = m_pages[idx];

    int64_t targetX = page.leftPixel + static_cast<int64_t>(std::round(anchor.normalizedX * page.widthPixel));
    int64_t targetY = page.topPixel + static_cast<int64_t>(std::round(anchor.normalizedY * page.heightPixel));

    outScrollX = targetX - viewportOffsetX;
    outScrollY = targetY - viewportOffsetY;

    // Clamp within document bounds
    int64_t maxScrollX = std::max<int64_t>(0, m_maxWidth - viewportW);
    int64_t maxScrollY = std::max<int64_t>(0, m_totalHeight - viewportH);

    outScrollX = std::clamp<int64_t>(outScrollX, 0, maxScrollX);
    outScrollY = std::clamp<int64_t>(outScrollY, 0, maxScrollY);
}

} // namespace layout
