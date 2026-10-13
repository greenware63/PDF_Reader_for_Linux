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

    updateLayout(m_scale, m_dpr, m_pageGap);
}

void DocumentLayout::updateLayout(double scale, double dpr, int64_t pageGapPixels) {
    m_scale = (scale > 0.0) ? scale : 1.0;
    m_dpr = (dpr > 0.0) ? dpr : 1.0;
    m_pageGap = (pageGapPixels >= 0) ? pageGapPixels : 16;

    m_totalHeight = 0;
    m_maxWidth = 0;

    int64_t currentTop = m_pageGap;

    for (auto& page : m_pages) {
        page.topPixel = currentTop;
        page.widthPixel = std::max<int64_t>(1, static_cast<int64_t>(std::round(page.widthPoints * m_scale * m_dpr)));
        page.heightPixel = std::max<int64_t>(1, static_cast<int64_t>(std::round(page.heightPoints * m_scale * m_dpr)));

        if (page.widthPixel > m_maxWidth) {
            m_maxWidth = page.widthPixel;
        }

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

    // Binary search for first visible page: the first page whose bottom edge is > viewportTop
    auto itFirst = std::upper_bound(
        m_pages.begin(),
        m_pages.end(),
        viewportTopPixel,
        [](int64_t top, const PageLayout& page) {
            return top < (page.topPixel + page.heightPixel);
        }
    );

    if (itFirst == m_pages.end()) {
        return {};
    }

    int firstIdx = itFirst->pageIndex;

    // Binary search for last visible page: first page whose top edge is >= viewportBottom
    auto itLast = std::lower_bound(
        itFirst,
        m_pages.end(),
        viewportBottom,
        [](const PageLayout& page, int64_t bottom) {
            return page.topPixel < bottom;
        }
    );

    int lastIdx = (itLast == m_pages.end()) ? (static_cast<int>(m_pages.size()) - 1) : itLast->pageIndex;
    if (itLast != m_pages.end() && itLast->topPixel >= viewportBottom && lastIdx > firstIdx) {
        --lastIdx;
    }

    return {firstIdx, std::min<int>(lastIdx, static_cast<int>(m_pages.size()) - 1)};
}

ScrollAnchor DocumentLayout::createAnchor(int64_t viewportCenterPixel) const {
    if (m_pages.empty()) {
        return {0, 0.0};
    }

    auto it = std::upper_bound(
        m_pages.begin(),
        m_pages.end(),
        viewportCenterPixel,
        [](int64_t center, const PageLayout& page) {
            return center < (page.topPixel + page.heightPixel);
        }
    );

    if (it == m_pages.end()) {
        it = m_pages.end() - 1;
    }

    double normalizedY = 0.0;
    if (it->heightPixel > 0) {
        int64_t offsetIntoPage = viewportCenterPixel - it->topPixel;
        normalizedY = std::clamp(static_cast<double>(offsetIntoPage) / it->heightPixel, 0.0, 1.0);
    }

    return {it->pageIndex, normalizedY};
}

int64_t DocumentLayout::resolveAnchor(const ScrollAnchor& anchor, int64_t viewportHeightPixel) const {
    if (m_pages.empty()) {
        return 0;
    }

    int idx = std::clamp(anchor.pageIndex, 0, static_cast<int>(m_pages.size()) - 1);
    const auto& page = m_pages[idx];

    int64_t targetCenterPixel = page.topPixel + static_cast<int64_t>(std::round(anchor.normalizedY * page.heightPixel));
    int64_t newViewportTop = targetCenterPixel - (viewportHeightPixel / 2);

    return std::max<int64_t>(0, std::min<int64_t>(newViewportTop, m_totalHeight - viewportHeightPixel));
}

} // namespace layout
