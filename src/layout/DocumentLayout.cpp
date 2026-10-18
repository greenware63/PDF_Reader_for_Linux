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
    const bool swapDims = (m_rotation == 90 || m_rotation == 270);
    for (auto& page : m_pages) {
        int w = 0, h = 0;
        const double pw = swapDims ? page.heightPoints : page.widthPoints;
        const double ph = swapDims ? page.widthPoints : page.heightPoints;
        computePagePixelDimensions(pw, ph, m_scale, m_dpr, w, h);
        page.widthPixel = w;
        page.heightPixel = h;
        if (page.widthPixel > widestPage) {
            widestPage = page.widthPixel;
        }
    }

    const int n = static_cast<int>(m_pages.size());
    const bool twoUp = (m_viewMode == ViewMode::TwoPage || m_viewMode == ViewMode::TwoPageCover);

    // Rows: one page each, or (first, second) pairs in the two-page modes.
    struct Row { int first; int count; };
    std::vector<Row> rows;
    if (twoUp) {
        int i = 0;
        if (m_viewMode == ViewMode::TwoPageCover && n > 0) {
            rows.push_back({0, 1});
            i = 1;
        }
        for (; i < n; i += 2) {
            rows.push_back({i, (i + 1 < n) ? 2 : 1});
        }
    } else {
        for (int i = 0; i < n; ++i) {
            rows.push_back({i, 1});
        }
    }

    auto rowWidth = [&](const Row& r) {
        int64_t w = m_pages[r.first].widthPixel;
        if (r.count == 2) {
            w += m_pageGap + m_pages[r.first + 1].widthPixel;
        } else if (twoUp) {
            // A lone page keeps the width of a half spread so it sits where a spread page would
            w = w * 2 + m_pageGap;
        }
        return w;
    };

    // Content width: the viewport, or wide enough for the widest row (SinglePage: only the focus page counts)
    const int focus = std::clamp(m_focusPage, 0, n - 1);
    int64_t widestRow = 0;
    if (m_viewMode == ViewMode::SinglePage) {
        widestRow = m_pages[focus].widthPixel;
    } else {
        for (const auto& r : rows) {
            widestRow = std::max(widestRow, rowWidth(r));
        }
    }
    const int64_t contentWidth = std::max(m_viewportWidth, widestRow + (2 * m_horizontalMargin));
    m_maxWidth = contentWidth;

    // Position rows top to bottom, each row centered within contentWidth
    int64_t currentTop = m_pageGap;
    for (const auto& r : rows) {
        int64_t rowHeight = 0;
        for (int k = 0; k < r.count; ++k) {
            rowHeight = std::max(rowHeight, m_pages[r.first + k].heightPixel);
        }
        const int64_t center = contentWidth / 2;
        if (!twoUp) {
            auto& p = m_pages[r.first];
            p.topPixel = currentTop;
            p.leftPixel = (contentWidth - p.widthPixel) / 2;
        } else if (r.count == 2) {
            auto& l = m_pages[r.first];
            auto& rt = m_pages[r.first + 1];
            const int64_t total = l.widthPixel + m_pageGap + rt.widthPixel;
            const int64_t left = (contentWidth - total) / 2;
            l.topPixel = rt.topPixel = currentTop;
            l.leftPixel = left;
            rt.leftPixel = left + l.widthPixel + m_pageGap;
        } else {
            auto& p = m_pages[r.first];
            p.topPixel = currentTop;
            // Cover sits on the right half (book style); a trailing odd page on the left half
            const bool isCover = (m_viewMode == ViewMode::TwoPageCover && r.first == 0);
            p.leftPixel = isCover ? center + m_pageGap / 2 : center - m_pageGap / 2 - p.widthPixel;
        }
        for (int k = 0; k < r.count; ++k) {
            m_pages[r.first + k].rowBottomPixel = currentTop + rowHeight;
        }
        currentTop += rowHeight + m_pageGap;
    }
    m_totalHeight = currentTop;

    if (m_viewMode == ViewMode::SinglePage) {
        // Shift so the focus page sits at the top of a document containing only that page.
        // Neighbouring pages end up at negative / past-the-end coordinates and are never visible.
        const int64_t shift = m_pages[focus].topPixel - m_pageGap;
        for (auto& p : m_pages) {
            p.topPixel -= shift;
            p.rowBottomPixel -= shift;
        }
        m_totalHeight = m_pages[focus].heightPixel + 2 * m_pageGap;
    }
}

int DocumentLayout::rowFirstPage(int pageIndex) const {
    const int n = static_cast<int>(m_pages.size());
    if (n == 0) return 0;
    pageIndex = std::clamp(pageIndex, 0, n - 1);
    switch (m_viewMode) {
        case ViewMode::TwoPage:
            return pageIndex & ~1;
        case ViewMode::TwoPageCover:
            return pageIndex == 0 ? 0 : ((pageIndex - 1) & ~1) + 1;
        default:
            return pageIndex;
    }
}

int DocumentLayout::rowPageCount(int pageIndex) const {
    const int n = static_cast<int>(m_pages.size());
    if (n == 0) return 0;
    const int first = rowFirstPage(pageIndex);
    if (m_viewMode == ViewMode::TwoPage || m_viewMode == ViewMode::TwoPageCover) {
        if (m_viewMode == ViewMode::TwoPageCover && first == 0) return 1;
        return (first + 1 < n) ? 2 : 1;
    }
    return 1;
}

std::pair<double, double> DocumentLayout::rowSizePoints(int pageIndex) const {
    const int n = static_cast<int>(m_pages.size());
    if (n == 0) return {0.0, 0.0};
    const int first = rowFirstPage(pageIndex);
    const int count = rowPageCount(pageIndex);
    double w = 0.0, h = 0.0;
    const bool swapDims = (m_rotation == 90 || m_rotation == 270);
    for (int k = 0; k < count; ++k) {
        const auto& p = m_pages[first + k];
        const double rawW = (p.widthPoints > 0.0) ? p.widthPoints : DEFAULT_PLACEHOLDER_WIDTH_POINTS;
        const double rawH = (p.heightPoints > 0.0) ? p.heightPoints : DEFAULT_PLACEHOLDER_HEIGHT_POINTS;
        const double pw = swapDims ? rawH : rawW;
        const double ph = swapDims ? rawW : rawH;
        w += pw;
        h = std::max(h, ph);
    }
    if (count == 1 && (m_viewMode == ViewMode::TwoPage || m_viewMode == ViewMode::TwoPageCover)) {
        w *= 2.0; // a lone page occupies a half-spread slot
    }
    return {w, h};
}

double DocumentLayout::widestRowPoints() const {
    const int n = static_cast<int>(m_pages.size());
    if (n == 0) return 0.0;
    if (m_viewMode == ViewMode::SinglePage) {
        return rowSizePoints(std::clamp(m_focusPage, 0, n - 1)).first;
    }
    double widest = 0.0;
    for (int i = 0; i < n; i += rowPageCount(i)) {
        widest = std::max(widest, rowSizePoints(i).first);
    }
    return widest;
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
    if (viewportBottom <= m_pages.front().topPixel || viewportTopPixel >= m_pages.back().rowBottomPixel) {
        return {};
    }

    // Binary search for first candidate page whose bottom edge is > viewportTop
    auto itFirst = std::upper_bound(
        m_pages.begin(),
        m_pages.end(),
        viewportTopPixel,
        [](int64_t top, const PageLayout& page) {
            return top < page.rowBottomPixel;
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
            return y < page.rowBottomPixel;
        }
    );

    if (it == m_pages.end()) {
        it = m_pages.end() - 1;
    }
    // Two pages share a row: pick the one whose horizontal extent is closest to docX
    if (it + 1 != m_pages.end() && (it + 1)->topPixel == it->topPixel && docX >= (it + 1)->leftPixel) {
        ++it;
    }

    double normX = 0.5;
    double normY = 0.5;

    if (it->widthPixel > 0) {
        int64_t offX = docX - it->leftPixel;
        normX = std::clamp(static_cast<double>(offX) / static_cast<double>(it->widthPixel), 0.0, 1.0);
    }
    if (it->heightPixel > 0) {
        int64_t offY = docY - it->topPixel;
        normY = std::clamp(static_cast<double>(offY) / static_cast<double>(it->heightPixel), 0.0, 1.0);
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

    int64_t targetX = page.leftPixel + static_cast<int64_t>(std::round(anchor.normalizedX * static_cast<double>(page.widthPixel)));
    int64_t targetY = page.topPixel + static_cast<int64_t>(std::round(anchor.normalizedY * static_cast<double>(page.heightPixel)));

    outScrollX = targetX - viewportOffsetX;
    outScrollY = targetY - viewportOffsetY;

    // Clamp within document bounds
    int64_t maxScrollX = std::max<int64_t>(0, m_maxWidth - viewportW);
    int64_t maxScrollY = std::max<int64_t>(0, m_totalHeight - viewportH);

    outScrollX = std::clamp<int64_t>(outScrollX, 0, maxScrollX);
    outScrollY = std::clamp<int64_t>(outScrollY, 0, maxScrollY);
}

int DocumentLayout::getPageAt(int64_t docX, int64_t docY) const {
    for (size_t i = 0; i < m_pages.size(); ++i) {
        const auto& p = m_pages[i];
        if (docX >= p.leftPixel && docX <= p.rightPixel() &&
            docY >= p.topPixel && docY <= p.bottomPixel()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool DocumentLayout::documentToPagePoints(int pageIndex, int64_t docX, int64_t docY, double& outPageX, double& outPageY) const {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return false;
    }
    const auto& p = m_pages[pageIndex];
    const double effScale = m_scale * m_dpr;
    if (effScale <= 0.0) {
        return false;
    }
    const double dispX = static_cast<double>(docX - p.leftPixel) / effScale;
    const double dispY = static_cast<double>(docY - p.topPixel) / effScale;

    const double W = (p.widthPoints > 0.0) ? p.widthPoints : DEFAULT_PLACEHOLDER_WIDTH_POINTS;
    const double H = (p.heightPoints > 0.0) ? p.heightPoints : DEFAULT_PLACEHOLDER_HEIGHT_POINTS;

    switch (m_rotation) {
        case 90:
            outPageX = dispY;
            outPageY = H - dispX;
            break;
        case 180:
            outPageX = W - dispX;
            outPageY = H - dispY;
            break;
        case 270:
            outPageX = W - dispY;
            outPageY = dispX;
            break;
        default:
            outPageX = dispX;
            outPageY = dispY;
            break;
    }
    return true;
}

bool DocumentLayout::pagePointsToDocument(int pageIndex, double pageX, double pageY, int64_t& outDocX, int64_t& outDocY) const {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_pages.size())) {
        return false;
    }
    const auto& p = m_pages[pageIndex];
    const double effScale = m_scale * m_dpr;

    const double W = (p.widthPoints > 0.0) ? p.widthPoints : DEFAULT_PLACEHOLDER_WIDTH_POINTS;
    const double H = (p.heightPoints > 0.0) ? p.heightPoints : DEFAULT_PLACEHOLDER_HEIGHT_POINTS;

    double dispX = pageX;
    double dispY = pageY;

    switch (m_rotation) {
        case 90:
            dispX = H - pageY;
            dispY = pageX;
            break;
        case 180:
            dispX = W - pageX;
            dispY = H - pageY;
            break;
        case 270:
            dispX = pageY;
            dispY = W - pageX;
            break;
        default:
            break;
    }

    outDocX = p.leftPixel + static_cast<int64_t>(std::round(dispX * effScale));
    outDocY = p.topPixel + static_cast<int64_t>(std::round(dispY * effScale));
    return true;
}

} // namespace layout
