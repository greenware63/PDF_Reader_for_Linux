#include "ui/ViewerLayoutController.h"
#include "cache/TileCache.h"

namespace ui {

void mergeLayoutResults(LayoutChangeResult& target, const LayoutChangeResult& other) {
    target.hasChanges = target.hasChanges || other.hasChanges;
    target.scrollBarsChanged = target.scrollBarsChanged || other.scrollBarsChanged;
    if (other.scrollBarsChanged) {
        if (other.verticalScroll.hasRange) {
            target.verticalScroll.hasRange = true;
            target.verticalScroll.min = other.verticalScroll.min;
            target.verticalScroll.max = other.verticalScroll.max;
            target.verticalScroll.pageStep = other.verticalScroll.pageStep;
            target.verticalScroll.singleStep = other.verticalScroll.singleStep;
        }
        if (other.verticalScroll.hasValue) {
            target.verticalScroll.hasValue = true;
            target.verticalScroll.value = other.verticalScroll.value;
        }
        if (other.horizontalScroll.hasRange) {
            target.horizontalScroll.hasRange = true;
            target.horizontalScroll.min = other.horizontalScroll.min;
            target.horizontalScroll.max = other.horizontalScroll.max;
            target.horizontalScroll.pageStep = other.horizontalScroll.pageStep;
            target.horizontalScroll.singleStep = other.horizontalScroll.singleStep;
        }
        if (other.horizontalScroll.hasValue) {
            target.horizontalScroll.hasValue = true;
            target.horizontalScroll.value = other.horizontalScroll.value;
        }
    }
    target.generationIncremented = target.generationIncremented || other.generationIncremented;
    target.clearTransformedPixmaps = target.clearTransformedPixmaps || other.clearTransformedPixmaps;
    if (other.recordFallbackScale) {
        target.recordFallbackScale = true;
        target.fallbackScale = other.fallbackScale;
    }
    if (other.emitPageChanged) {
        target.emitPageChanged = true;
        target.newCurrentPage = other.newCurrentPage;
        target.totalPages = other.totalPages;
    }
    if (other.emitZoomChanged) {
        target.emitZoomChanged = true;
        target.zoomRatio = other.zoomRatio;
    }
    if (other.emitViewModeChanged) {
        target.emitViewModeChanged = true;
        target.newViewMode = other.newViewMode;
    }
    if (other.emitRotationChanged) {
        target.emitRotationChanged = true;
        target.newRotation = other.newRotation;
    }
    target.updateFormEditorGeometry = target.updateFormEditorGeometry || other.updateFormEditorGeometry;
    target.needsRepaint = target.needsRepaint || other.needsRepaint;
}

ViewerLayoutController::ViewerLayoutController()
    : m_layout()
    , m_scrollMapper()
{
}

void ViewerLayoutController::setDocument(int pageCount, const std::vector<std::pair<double, double>>& pageSizes) {
    m_pageCount = pageCount;
    m_layout.setFocusPage(0);
    m_layout.setPageSizes(pageSizes);
    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;
    m_scale = DESKTOP_100_PERCENT_SCALE;
    m_lastVpWidthDevice = 0;
    m_lastVpHeightDevice = 0;
    m_pageFlipAccumulator = 0;
    m_navigating = false;
}

void ViewerLayoutController::resetDocument() {
    m_pageCount = 0;
    m_rotation = 0;
    m_layout.setRotation(0);
    m_layout.setViewMode(layout::ViewMode::Continuous);
    m_layout.setFocusPage(0);
    m_layout.setPageSizes({});
    m_scale = DESKTOP_100_PERCENT_SCALE;
    m_currentPage = 0;
    m_docScrollX = 0;
    m_docScrollY = 0;
    m_lastVpWidthDevice = 0;
    m_lastVpHeightDevice = 0;
    m_pageFlipAccumulator = 0;
    m_navigating = false;
}

LayoutChangeResult ViewerLayoutController::goToPage(int pageIndex, std::optional<double> targetYFraction, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (m_pageCount <= 0 || pageIndex < 0 || pageIndex >= m_pageCount) {
        return result;
    }

    ensurePageShown(pageIndex, vp, result);
    const auto& pl = m_layout.getPageLayout(pageIndex);

    const int64_t oldScrollX = m_docScrollX;
    const int64_t oldScrollY = m_docScrollY;

    int64_t targetDocY = pl.topPixel;
    if (targetYFraction && std::isfinite(*targetYFraction)) {
        const double topDownFraction = std::clamp(*targetYFraction, 0.0, 1.0);
        int64_t docX = 0, docY = 0;
        if (m_layout.pagePointsToDocument(pageIndex, 0.0, topDownFraction * pl.heightPoints, docX, docY)) {
            targetDocY = docY;
        } else {
            targetDocY += static_cast<int64_t>(std::llround(topDownFraction * static_cast<double>(pl.heightPixel)));
        }
    }

    m_navigating = true;
    setDocScrollPosition(m_docScrollX, targetDocY, vp, result);
    m_navigating = false;

    if (m_docScrollX != oldScrollX || m_docScrollY != oldScrollY) {
        result.generationIncremented = true;
    }

    if (m_currentPage != pageIndex) {
        m_currentPage = pageIndex;
        result.emitPageChanged = true;
        result.newCurrentPage = m_currentPage;
        result.totalPages = m_pageCount;
    }

    result.needsRepaint = true;
    return result;
}

LayoutChangeResult ViewerLayoutController::nextPage(const ViewportGeometry& vp) {
    const int target = m_layout.rowFirstPage(m_currentPage) + m_layout.rowPageCount(m_currentPage);
    if (target < m_pageCount) {
        return goToPage(target, std::nullopt, vp);
    }
    return {};
}

LayoutChangeResult ViewerLayoutController::previousPage(const ViewportGeometry& vp) {
    const int rowFirst = m_layout.rowFirstPage(m_currentPage);
    if (rowFirst > 0) {
        return goToPage(m_layout.rowFirstPage(rowFirst - 1), std::nullopt, vp);
    } else if (m_currentPage > rowFirst) {
        return goToPage(rowFirst, std::nullopt, vp);
    }
    return {};
}

void ViewerLayoutController::ensurePageShown(int pageIndex, const ViewportGeometry& vp, LayoutChangeResult& result) {
    if (m_layout.viewMode() != layout::ViewMode::SinglePage || m_layout.focusPage() == pageIndex) {
        return;
    }
    m_layout.setFocusPage(pageIndex);
    m_layout.updateLayout(pdf::quantizeScale(m_scale), vp.dpr, vp.widthDevice);
    updateScrollBars(vp, result);
    result.generationIncremented = true;
}

LayoutChangeResult ViewerLayoutController::setZoom(double zoom, const QPoint* anchorWidgetPos, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (m_pageCount <= 0) {
        return result;
    }

    // Zoom bounds in scale units (1.0 = 72 dpi, so 100% on screen is 96/72): 0.1 .. 10.0
    zoom = std::clamp(zoom, 0.1, 10.0);
    if (std::abs(zoom - m_scale) < 1e-4) {
        return result;
    }

    result.clearTransformedPixmaps = true;
    result.recordFallbackScale = true;
    result.fallbackScale = pdf::quantizeScale(m_scale);

    const double qOldScale = pdf::quantizeScale(m_scale);
    const double qNewScale = pdf::quantizeScale(zoom);

    // Determine 2D anchor point
    int64_t anchorDocX = 0;
    int64_t anchorDocY = 0;
    int64_t widgetAnchorX = vp.widthDevice / 2;
    int64_t widgetAnchorY = vp.heightDevice / 2;

    if (anchorWidgetPos) {
        widgetAnchorX = static_cast<int64_t>(std::round(anchorWidgetPos->x() * vp.dpr));
        widgetAnchorY = static_cast<int64_t>(std::round(anchorWidgetPos->y() * vp.dpr));
        anchorDocX = m_docScrollX + widgetAnchorX;
        anchorDocY = m_docScrollY + widgetAnchorY;
    } else {
        anchorDocX = m_docScrollX + widgetAnchorX;
        anchorDocY = m_docScrollY + widgetAnchorY;
    }

    auto anchor = m_layout.createAnchor(anchorDocX, anchorDocY);

    m_scale = zoom;

    // Advance generation on zoom changes to prune obsolete tile renders immediately
    if (qNewScale != qOldScale) {
        result.generationIncremented = true;
    }

    m_layout.updateLayout(qNewScale, vp.dpr, vp.widthDevice);
    updateScrollBars(vp, result);

    int64_t newDocX = 0;
    int64_t newDocY = 0;
    m_layout.resolveAnchor(anchor, widgetAnchorX, widgetAnchorY, vp.widthDevice, vp.heightDevice, newDocX, newDocY);

    setDocScrollPosition(newDocX, newDocY, vp, result);

    result.needsRepaint = true;
    result.emitZoomChanged = true;
    result.zoomRatio = m_scale / DESKTOP_100_PERCENT_SCALE;
    return result;
}

LayoutChangeResult ViewerLayoutController::zoomIn(const ViewportGeometry& vp) {
    return setZoom(m_scale * ZOOM_STEP, nullptr, vp);
}

LayoutChangeResult ViewerLayoutController::zoomOut(const ViewportGeometry& vp) {
    return setZoom(m_scale / ZOOM_STEP, nullptr, vp);
}

LayoutChangeResult ViewerLayoutController::zoomReset(const ViewportGeometry& vp) {
    return setZoom(DESKTOP_100_PERCENT_SCALE, nullptr, vp);
}

LayoutChangeResult ViewerLayoutController::zoomFitWidth(const ViewportGeometry& vp) {
    if (m_pageCount <= 0) {
        return {};
    }

    constexpr int64_t HORIZONTAL_MARGIN = 16;
    const bool twoUp = m_layout.viewMode() == layout::ViewMode::TwoPage ||
                       m_layout.viewMode() == layout::ViewMode::TwoPageCover;
    const int64_t availableWidth = vp.widthDevice - (2 * HORIZONTAL_MARGIN) - (twoUp ? m_layout.columnGapPixels() : 0);

    if (availableWidth <= 0) {
        return {};
    }

    // Widest page (continuous), widest spread (two-page modes) or the shown page (single page)
    const double widestPoints = m_layout.widestRowPoints();
    if (widestPoints > 0.0) {
        double fitScale = static_cast<double>(availableWidth) / (widestPoints * vp.dpr);
        return setZoom(fitScale, nullptr, vp);
    }
    return {};
}

LayoutChangeResult ViewerLayoutController::zoomFitPage(const ViewportGeometry& vp) {
    if (m_pageCount <= 0) {
        return {};
    }

    constexpr int64_t MARGIN = 16;
    const bool twoUp = m_layout.viewMode() == layout::ViewMode::TwoPage ||
                       m_layout.viewMode() == layout::ViewMode::TwoPageCover;
    const int64_t availW = vp.widthDevice - (2 * MARGIN) - (twoUp ? m_layout.columnGapPixels() : 0);
    const int64_t availH = vp.heightDevice - (2 * m_layout.columnGapPixels());
    if (availW <= 0 || availH <= 0) {
        return {};
    }

    const int page = m_currentPage;
    const auto [rowW, rowH] = m_layout.rowSizePoints(page);
    if (rowW <= 0.0 || rowH <= 0.0) {
        return {};
    }
    const double fitScale = std::min(static_cast<double>(availW) / (rowW * vp.dpr),
                                     static_cast<double>(availH) / (rowH * vp.dpr));
    auto zResult = setZoom(fitScale, nullptr, vp);
    auto navResult = goToPage(page, std::nullopt, vp);
    mergeLayoutResults(zResult, navResult);
    return zResult;
}

LayoutChangeResult ViewerLayoutController::setViewMode(layout::ViewMode mode, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (mode == m_layout.viewMode()) {
        return result;
    }
    const int page = std::clamp(m_currentPage, 0, std::max(0, m_pageCount - 1));
    m_layout.setViewMode(mode);
    m_layout.setFocusPage(page);
    m_pageFlipAccumulator = 0;
    if (m_pageCount > 0) {
        m_layout.updateLayout(pdf::quantizeScale(m_scale), vp.dpr, vp.widthDevice);
        updateScrollBars(vp, result);
        result.generationIncremented = true;
        auto navResult = goToPage(page, std::nullopt, vp);
        mergeLayoutResults(result, navResult);
    }
    result.emitViewModeChanged = true;
    result.newViewMode = mode;
    result.needsRepaint = true;
    return result;
}

LayoutChangeResult ViewerLayoutController::setRotation(int degrees, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    const int normalized = (degrees % 360 + 360) % 360;
    if (m_rotation == normalized) {
        return result;
    }
    if (m_pageCount <= 0) {
        m_rotation = normalized;
        m_layout.setRotation(m_rotation);
        result.emitRotationChanged = true;
        result.newRotation = m_rotation;
        result.needsRepaint = true;
        return result;
    }

    const int64_t centreDocX = m_docScrollX + vp.widthDevice / 2;
    const int64_t centreDocY = m_docScrollY + vp.heightDevice / 2;

    // Rotation is display-only, so the page-space point under the viewport centre is
    // invariant across it. Remember that point before the relayout and scroll back to
    // it afterwards, instead of jumping to the top of the current page.
    const int anchorPage = m_layout.getPageAt(centreDocX, centreDocY);
    double anchorPageX = 0.0;
    double anchorPageY = 0.0;
    const bool haveAnchor = anchorPage >= 0 &&
        m_layout.documentToPagePoints(anchorPage, centreDocX, centreDocY, anchorPageX, anchorPageY);

    m_rotation = normalized;
    m_layout.setRotation(m_rotation);

    result.generationIncremented = true;
    result.clearTransformedPixmaps = true;

    m_layout.updateLayout(pdf::quantizeScale(m_scale), vp.dpr, vp.widthDevice);
    updateScrollBars(vp, result);

    int64_t newDocX = m_docScrollX;
    int64_t newDocY = m_docScrollY;
    if (haveAnchor) {
        int64_t anchorDocX = 0;
        int64_t anchorDocY = 0;
        if (m_layout.pagePointsToDocument(anchorPage, anchorPageX, anchorPageY, anchorDocX, anchorDocY)) {
            newDocX = anchorDocX - vp.widthDevice / 2;
            newDocY = anchorDocY - vp.heightDevice / 2;
        }
    }
    setDocScrollPosition(newDocX, newDocY, vp, result);

    result.emitRotationChanged = true;
    result.newRotation = m_rotation;
    result.needsRepaint = true;
    return result;
}

LayoutChangeResult ViewerLayoutController::rotateClockwise(const ViewportGeometry& vp) {
    return setRotation(m_rotation + 90, vp);
}

LayoutChangeResult ViewerLayoutController::rotateCounterClockwise(const ViewportGeometry& vp) {
    return setRotation(m_rotation - 90, vp);
}

void ViewerLayoutController::setDocScrollPosition(int64_t docX, int64_t docY, const ViewportGeometry& vp, LayoutChangeResult& result) {
    const int64_t maxX = std::max<int64_t>(0, m_layout.maxDocumentWidth() - vp.widthDevice);
    const int64_t maxY = std::max<int64_t>(0, m_layout.totalDocumentHeight() - vp.heightDevice);

    m_docScrollX = std::clamp<int64_t>(docX, 0, maxX);
    m_docScrollY = std::clamp<int64_t>(docY, 0, maxY);

    m_lastVpWidthDevice = vp.widthDevice;
    m_lastVpHeightDevice = vp.heightDevice;

    result.scrollBarsChanged = true;
    result.verticalScroll.hasValue = true;
    result.verticalScroll.value = m_scrollMapper.docToSlider(m_docScrollY, m_layout.totalDocumentHeight(), vp.heightDevice);
    result.horizontalScroll.hasValue = true;
    result.horizontalScroll.value = m_scrollMapper.docToSlider(m_docScrollX, m_layout.maxDocumentWidth(), vp.widthDevice);

    if (!m_navigating) {
        auto visPages = m_layout.getVisiblePages(m_docScrollY, vp.heightDevice);
        if (visPages.isValid()) {
            int bestPage = -1;
            int64_t maxOverlap = 0;
            for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
                const auto& pl = m_layout.getPageLayout(p);
                int64_t overlapTop = std::max(m_docScrollY, pl.topPixel);
                int64_t overlapBottom = std::min(m_docScrollY + vp.heightDevice, pl.bottomPixel());
                int64_t overlap = overlapBottom - overlapTop;
                if (overlap > maxOverlap) {
                    maxOverlap = overlap;
                    bestPage = p;
                }
            }
            if (bestPage >= 0 && bestPage != m_currentPage) {
                m_currentPage = bestPage;
                result.emitPageChanged = true;
                result.newCurrentPage = m_currentPage;
                result.totalPages = m_pageCount;
            }
        }
    }
}

void ViewerLayoutController::updateScrollBars(const ViewportGeometry& vp, LayoutChangeResult& result) {
    int32_t maxSliderV = 0, pageStepV = 0, singleStepV = 0;
    m_scrollMapper.computeRange(m_layout.totalDocumentHeight(), vp.heightDevice, maxSliderV, pageStepV, singleStepV, static_cast<int64_t>(std::round(40 * vp.dpr)));

    int32_t maxSliderH = 0, pageStepH = 0, singleStepH = 0;
    m_scrollMapper.computeRange(m_layout.maxDocumentWidth(), vp.widthDevice, maxSliderH, pageStepH, singleStepH, static_cast<int64_t>(std::round(40 * vp.dpr)));

    result.scrollBarsChanged = true;
    result.verticalScroll.hasRange = true;
    result.verticalScroll.min = 0;
    result.verticalScroll.max = maxSliderV;
    result.verticalScroll.pageStep = pageStepV;
    result.verticalScroll.singleStep = singleStepV;

    result.horizontalScroll.hasRange = true;
    result.horizontalScroll.min = 0;
    result.horizontalScroll.max = maxSliderH;
    result.horizontalScroll.pageStep = pageStepH;
    result.horizontalScroll.singleStep = singleStepH;
}

LayoutChangeResult ViewerLayoutController::updateLayoutGeometry(const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (m_pageCount <= 0) {
        return result;
    }
    const double qScale = pdf::quantizeScale(m_scale);
    const int64_t oldVpW = (m_lastVpWidthDevice > 0) ? m_lastVpWidthDevice : vp.widthDevice;
    const int64_t oldVpH = (m_lastVpHeightDevice > 0) ? m_lastVpHeightDevice : vp.heightDevice;

    int64_t curDocX = m_docScrollX + oldVpW / 2;
    int64_t curDocY = m_docScrollY + oldVpH / 2;
    auto anchor = m_layout.createAnchor(curDocX, curDocY);

    m_layout.updateLayout(qScale, vp.dpr, vp.widthDevice);
    updateScrollBars(vp, result);

    int64_t newDocX = 0, newDocY = 0;
    m_layout.resolveAnchor(anchor, vp.widthDevice / 2, vp.heightDevice / 2, vp.widthDevice, vp.heightDevice, newDocX, newDocY);
    setDocScrollPosition(newDocX, newDocY, vp, result);

    m_lastVpWidthDevice = vp.widthDevice;
    m_lastVpHeightDevice = vp.heightDevice;

    result.needsRepaint = true;
    return result;
}

LayoutChangeResult ViewerLayoutController::onScrollValueChanged(int hSliderVal, int vSliderVal, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (m_navigating || m_pageCount <= 0) {
        return result;
    }

    int64_t newScrollX = m_scrollMapper.sliderToDoc(hSliderVal, m_layout.maxDocumentWidth(), vp.widthDevice);
    int64_t newScrollY = m_scrollMapper.sliderToDoc(vSliderVal, m_layout.totalDocumentHeight(), vp.heightDevice);

    bool scrollChanged = (newScrollX != m_docScrollX || newScrollY != m_docScrollY);
    if (scrollChanged) {
        m_docScrollX = newScrollX;
        m_docScrollY = newScrollY;
        result.generationIncremented = true;
    }

    m_lastVpWidthDevice = vp.widthDevice;
    m_lastVpHeightDevice = vp.heightDevice;

    auto visPages = m_layout.getVisiblePages(m_docScrollY, vp.heightDevice);
    if (visPages.isValid()) {
        int bestPage = -1;
        int64_t maxOverlap = 0;
        for (int p = visPages.firstVisiblePage; p <= visPages.lastVisiblePage; ++p) {
            const auto& pl = m_layout.getPageLayout(p);
            int64_t overlapTop = std::max(m_docScrollY, pl.topPixel);
            int64_t overlapBottom = std::min(m_docScrollY + vp.heightDevice, pl.bottomPixel());
            int64_t overlap = overlapBottom - overlapTop;
            if (overlap > maxOverlap) {
                maxOverlap = overlap;
                bestPage = p;
            }
        }
        if (bestPage >= 0 && bestPage != m_currentPage) {
            m_currentPage = bestPage;
            result.emitPageChanged = true;
            result.newCurrentPage = m_currentPage;
            result.totalPages = m_pageCount;
        }
    }
    result.updateFormEditorGeometry = true;
    result.needsRepaint = true;
    return result;
}

LayoutChangeResult ViewerLayoutController::restoreReadingPosition(int pageIndex, double zoom, int64_t scrollX, int64_t scrollY, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (m_pageCount <= 0) {
        return result;
    }
    if (zoom >= 0.1 && zoom <= 10.0) {
        auto zResult = setZoom(zoom, nullptr, vp);
        mergeLayoutResults(result, zResult);
    }
    // Saved scroll offsets are in continuous-layout coordinates; other modes restore by page.
    if ((scrollY > 0 || scrollX > 0) && m_layout.viewMode() == layout::ViewMode::Continuous) {
        setDocScrollPosition(scrollX, scrollY, vp, result);
        result.needsRepaint = true;
    } else if (pageIndex >= 0 && pageIndex < m_pageCount) {
        auto navResult = goToPage(pageIndex, std::nullopt, vp);
        mergeLayoutResults(result, navResult);
    }
    return result;
}

LayoutChangeResult ViewerLayoutController::restoreReadingPosition(int pageIndex, double zoom, double fractionX, double fractionY,
                                                                int64_t fallbackScrollX, int64_t fallbackScrollY, const ViewportGeometry& vp) {
    LayoutChangeResult result;
    if (m_pageCount <= 0) {
        return result;
    }
    if (zoom >= 0.1 && zoom <= 10.0) {
        auto zResult = setZoom(zoom, nullptr, vp);
        mergeLayoutResults(result, zResult);
    }
    // Continuous layout: restore exact fractional offset on page
    if (m_layout.viewMode() == layout::ViewMode::Continuous) {
        if (pageIndex >= 0 && pageIndex < m_pageCount && std::isfinite(fractionY) && fractionY >= 0.0) {
            const auto& pl = m_layout.getPageLayout(pageIndex);
            int64_t targetY = pl.topPixel + static_cast<int64_t>(std::llround(fractionY * static_cast<double>(pl.heightPixel)));
            int64_t targetX = 0;
            if (std::isfinite(fractionX) && fractionX > 0.0) {
                targetX = pl.leftPixel + static_cast<int64_t>(std::llround(fractionX * static_cast<double>(pl.widthPixel)));
            } else if (fallbackScrollX > 0) {
                targetX = fallbackScrollX;
            }
            setDocScrollPosition(targetX, targetY, vp, result);
            result.needsRepaint = true;
            return result;
        } else if (fallbackScrollY > 0 || fallbackScrollX > 0) {
            setDocScrollPosition(fallbackScrollX, fallbackScrollY, vp, result);
            result.needsRepaint = true;
            return result;
        }
    }
    if (pageIndex >= 0 && pageIndex < m_pageCount) {
        auto navResult = goToPage(pageIndex, std::nullopt, vp);
        mergeLayoutResults(result, navResult);
    }
    return result;
}

bool ViewerLayoutController::getReadingPosition(int& outPage, double& outFractionX, double& outFractionY) const {
    if (m_pageCount <= 0) {
        return false;
    }
    if (m_layout.viewMode() == layout::ViewMode::Continuous) {
        auto anchor = m_layout.createAnchor(m_docScrollX, m_docScrollY);
        if (anchor.isValid() && anchor.pageIndex >= 0 && anchor.pageIndex < m_pageCount) {
            outPage = anchor.pageIndex;
            const auto& pl = m_layout.getPageLayout(anchor.pageIndex);
            if (m_docScrollX > pl.leftPixel && pl.widthPixel > 0) {
                outFractionX = std::clamp(static_cast<double>(m_docScrollX - pl.leftPixel) / static_cast<double>(pl.widthPixel), 0.0, 1.0);
            } else {
                outFractionX = 0.0;
            }
            if (pl.heightPixel > 0) {
                if (m_docScrollY >= pl.topPixel) {
                    outFractionY = std::clamp(static_cast<double>(m_docScrollY - pl.topPixel) / static_cast<double>(pl.heightPixel), 0.0, 1.0);
                } else {
                    outFractionY = 0.0;
                }
            } else {
                outFractionY = 0.0;
            }
            return true;
        }
    }
    outPage = std::clamp(m_currentPage, 0, m_pageCount - 1);
    outFractionX = 0.0;
    outFractionY = 0.0;
    return true;
}

WheelFlipResult ViewerLayoutController::handleWheelPageFlip(int dy, bool atTop, bool atBottom) {
    if (m_layout.viewMode() != layout::ViewMode::SinglePage || m_pageCount <= 0 || dy == 0) {
        return {WheelFlipAction::PassToDefault, 0, false};
    }
    if (dy < 0 && atBottom) {
        m_pageFlipAccumulator = std::max(0, m_pageFlipAccumulator) - dy;
    } else if (dy > 0 && atTop) {
        m_pageFlipAccumulator = std::min(0, m_pageFlipAccumulator) - dy;
    } else {
        m_pageFlipAccumulator = 0;
    }
    constexpr int FLIP_THRESHOLD = 240;
    if (m_pageFlipAccumulator >= FLIP_THRESHOLD && m_currentPage + 1 < m_pageCount) {
        m_pageFlipAccumulator = 0;
        return {WheelFlipAction::FlipForward, m_currentPage + 1, false};
    } else if (m_pageFlipAccumulator <= -FLIP_THRESHOLD && m_currentPage > 0) {
        m_pageFlipAccumulator = 0;
        return {WheelFlipAction::FlipBackward, m_currentPage - 1, true};
    }
    return {WheelFlipAction::PassToDefault, 0, false};
}

} // namespace ui
