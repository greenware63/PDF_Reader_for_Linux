#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <limits>
#include <cstdint>
#include "layout/DocumentLayout.h"
#include "layout/ScrollMapper.h"

// Reference linear scan to verify binary search correctness across 1,000 random queries
static layout::ViewportRange bruteForceVisiblePages(const layout::DocumentLayout& layout,
                                                    int64_t vpTop, int64_t vpH) {
    if (vpH <= 0 || layout.pageCount() == 0) {
        return {};
    }
    const int64_t vpBottom = vpTop + vpH;
    int first = -1;
    int last = -1;
    for (int i = 0; i < layout.pageCount(); ++i) {
        const auto& p = layout.getPageLayout(i);
        if (p.topPixel < vpBottom && p.bottomPixel() > vpTop) {
            if (first == -1) first = i;
            last = i;
        }
    }
    return {first, last};
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running DocumentLayout Unit Tests      " << std::endl;
    std::cout << "========================================" << std::endl;

    // Test 1: Empty Document Behavior
    std::cout << "[TEST 1] Testing empty document layout..." << std::endl;
    layout::DocumentLayout emptyLayout;
    emptyLayout.setPageSizes({});
    if (emptyLayout.pageCount() != 0 || emptyLayout.totalDocumentHeight() != 0) {
        std::cerr << "[FAIL] Empty layout reported non-zero page count or height" << std::endl;
        return 1;
    }
    auto emptyRange = emptyLayout.getVisiblePages(0, 500);
    if (emptyRange.isValid()) {
        std::cerr << "[FAIL] Empty layout reported valid visible range" << std::endl;
        return 1;
    }
    auto emptyTiles = emptyLayout.getVisibleTiles({0, 0, 500, 500}, 512);
    if (!emptyTiles.empty()) {
        std::cerr << "[FAIL] Empty layout reported visible tiles" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Empty document returns valid zero-state." << std::endl;

    // Test 2: Untrusted Dimension Hardening & Placeholder Substitution
    std::cout << "[TEST 2] Testing dimension hardening and fallback placeholder substitution..." << std::endl;
    int w = -1, h = -1;
    // 2a. NaN -> must substitute US Letter placeholder (612x792 pt)
    layout::computePagePixelDimensions(std::numeric_limits<double>::quiet_NaN(), 842.0, 1.0, 1.0, w, h);
    if (w != 612 || h != 792) {
        std::cerr << "[FAIL] computePagePixelDimensions failed to substitute placeholder for NaN: got " << w << "x" << h << std::endl;
        return 1;
    }
    // 2b. Infinity -> must substitute US Letter placeholder
    layout::computePagePixelDimensions(595.0, std::numeric_limits<double>::infinity(), 1.0, 1.0, w, h);
    if (w != 612 || h != 792) {
        std::cerr << "[FAIL] computePagePixelDimensions failed to substitute placeholder for Infinity: got " << w << "x" << h << std::endl;
        return 1;
    }
    // 2c. Negative and Zero -> must substitute US Letter placeholder
    layout::computePagePixelDimensions(-100.0, 500.0, 1.0, 1.0, w, h);
    if (w != 612 || h != 792) {
        std::cerr << "[FAIL] computePagePixelDimensions failed to substitute placeholder for negative width: got " << w << "x" << h << std::endl;
        return 1;
    }
    // 2d. Astronomical MediaBox (1e9 pt -> must clamp to MAX_SAFE_PIXEL_DIM without overflow)
    layout::computePagePixelDimensions(1e9, 1e9, 2.0, 2.0, w, h);
    if (w != 500000 || h != 500000) {
        std::cerr << "[FAIL] computePagePixelDimensions failed to safely clamp 1e9 pt: got " << w << "x" << h << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Robust against NaN, Inf, negative (substitutes 612x792 placeholder), and astronomical MediaBoxes." << std::endl;

    // Test 3: Horizontal Centering against Scrollable Content Width
    std::cout << "[TEST 3] Testing horizontal centering in wide and narrow viewports..." << std::endl;
    layout::DocumentLayout layout;
    std::vector<std::pair<double, double>> mixedPages = {
        {595.0, 842.0},  // Portrait A4
        {842.0, 595.0},  // Landscape A4 (wider!)
        {612.0, 792.0},  // US Letter
        {792.0, 612.0},  // US Letter Landscape
        {595.0, 842.0}
    };
    layout.setPageSizes(mixedPages);

    // 3a. Wide viewport: 2000px width. Content width should expand to viewportWidth, centering pages.
    layout.updateLayout(1.0, 1.0, 2000, 20, 20);
    if (layout.maxDocumentWidth() != 2000) {
        std::cerr << "[FAIL] Content width should match viewport width when viewport is wider than pages!" << std::endl;
        return 1;
    }
    for (int i = 0; i < layout.pageCount(); ++i) {
        const auto& p = layout.getPageLayout(i);
        int64_t expectedLeft = (2000 - p.widthPixel) / 2;
        if (p.leftPixel != expectedLeft) {
            std::cerr << "[FAIL] Page " << i << " is not centered in wide viewport! Got " 
                      << p.leftPixel << ", expected " << expectedLeft << std::endl;
            return 1;
        }
    }

    // 3b. Narrow viewport: 600px width. Widest page is 842px + 2*20 margin = 882px.
    // Content width must stay 882px, ensuring wide pages can scroll horizontally rather than clip.
    layout.updateLayout(1.0, 1.0, 600, 20, 20);
    if (layout.maxDocumentWidth() != 882) {
        std::cerr << "[FAIL] Content width should be widestPage + 2*margin (882px) in narrow viewport! Got "
                  << layout.maxDocumentWidth() << std::endl;
        return 1;
    }
    for (int i = 0; i < layout.pageCount(); ++i) {
        const auto& p = layout.getPageLayout(i);
        int64_t expectedLeft = (882 - p.widthPixel) / 2;
        if (p.leftPixel != expectedLeft) {
            std::cerr << "[FAIL] Page " << i << " not centered relative to content width in narrow viewport!" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: Content width expands to viewport in wide view, and expands to widest page in narrow view." << std::endl;

    // Test 4: Visible pages binary search stress test
    std::cout << "[TEST 4] Testing visible pages binary search against linear brute-force scan..." << std::endl;
    std::vector<std::pair<double, double>> stressPages;
    for (int i = 0; i < 500; ++i) {
        stressPages.push_back({595.0 + (i % 5) * 50.0, 842.0 - (i % 3) * 40.0});
    }
    layout.setPageSizes(stressPages);
    layout.updateLayout(1.0, 1.0, 1200, 15, 15);

    std::mt19937 rng(42);
    const int64_t totalHeight = layout.totalDocumentHeight();
    for (int q = 0; q < 1000; ++q) {
        int64_t vpTop = static_cast<int64_t>(rng() % (totalHeight + 1000)) - 500;
        int64_t vpH = 100 + (rng() % 2000);

        auto fastRange = layout.getVisiblePages(vpTop, vpH);
        auto refRange = bruteForceVisiblePages(layout, vpTop, vpH);

        if (fastRange.firstVisiblePage != refRange.firstVisiblePage ||
            fastRange.lastVisiblePage != refRange.lastVisiblePage) {
            std::cerr << "[FAIL] Query " << q << " mismatch: vpTop=" << vpTop << ", vpH=" << vpH
                      << " -> fast: [" << fastRange.firstVisiblePage << ", " << fastRange.lastVisiblePage << "]"
                      << " vs ref: [" << refRange.firstVisiblePage << ", " << refRange.lastVisiblePage << "]" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: 1,000 random viewport queries 100% matched brute-force linear scan." << std::endl;

    // Test 5: Exact 512x512 tile grid coordinate calculation
    std::cout << "[TEST 5] Testing exact 512x512 tile grid coordinate calculation..." << std::endl;
    layout.setPageSizes({{1200.0, 1400.0}});
    layout.updateLayout(1.0, 1.0, 1500, 20, 20);
    const auto& p0 = layout.getPageLayout(0);

    layout::Rect64 viewportWholeDoc{0, 0, layout.maxDocumentWidth(), layout.totalDocumentHeight()};
    auto tiles = layout.getVisibleTiles(viewportWholeDoc, 512);

    int expectedCols = (1200 + 511) / 512; // 3 columns: 512, 512, 176
    int expectedRows = (1400 + 511) / 512; // 3 rows: 512, 512, 376
    if (static_cast<int>(tiles.size()) != expectedCols * expectedRows) {
        std::cerr << "[FAIL] Expected " << (expectedCols * expectedRows) << " tiles, got " << tiles.size() << std::endl;
        return 1;
    }

    // Verify rightmost and bottommost clipped dimensions
    for (const auto& t : tiles) {
        if (t.col == 2) {
            if (t.tileWidth != 176) {
                std::cerr << "[FAIL] Col 2 clipped width expected 176, got " << t.tileWidth << std::endl;
                return 1;
            }
        } else {
            if (t.tileWidth != 512) {
                std::cerr << "[FAIL] Full tile width expected 512, got " << t.tileWidth << std::endl;
                return 1;
            }
        }

        if (t.row == 2) {
            if (t.tileHeight != 376) {
                std::cerr << "[FAIL] Row 2 clipped height expected 376, got " << t.tileHeight << std::endl;
                return 1;
            }
        } else {
            if (t.tileHeight != 512) {
                std::cerr << "[FAIL] Full tile height expected 512, got " << t.tileHeight << std::endl;
                return 1;
            }
        }

        if (t.docX != p0.leftPixel + t.tileXOnPage || t.docY != p0.topPixel + t.tileYOnPage) {
            std::cerr << "[FAIL] Document tile coordinates misaligned with page layout!" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: Exact 512x512 tile grid coordinate calculation and right/bottom edge clipping verified." << std::endl;

    // Test 6: Visible tile partition completeness and non-overlap
    std::cout << "[TEST 6] Testing visible tile partition completeness and non-overlap..." << std::endl;
    {
        std::vector<std::pair<double, double>> multiTestPages = {
            {595.0, 842.0},
            {842.0, 595.0},
            {1200.0, 1600.0}
        };
        layout.setPageSizes(multiTestPages);
        layout.updateLayout(1.333, 1.5, 1400, 20, 20);

        std::mt19937 tileRng(999);
        const int64_t maxW = layout.maxDocumentWidth();
        const int64_t maxH = layout.totalDocumentHeight();

        for (int q = 0; q < 100; ++q) {
            int64_t vpX = static_cast<int64_t>(tileRng() % (maxW + 200)) - 100;
            int64_t vpY = static_cast<int64_t>(tileRng() % (maxH + 200)) - 100;
            int64_t vpW = 200 + (tileRng() % 1200);
            int64_t vpH = 200 + (tileRng() % 1200);
            layout::Rect64 queryVp{vpX, vpY, vpW, vpH};

            auto queryTiles = layout.getVisibleTiles(queryVp, 512);

            // Invariant 1: No two tiles from the same page overlap
            for (size_t i = 0; i < queryTiles.size(); ++i) {
                for (size_t j = i + 1; j < queryTiles.size(); ++j) {
                    if (queryTiles[i].pageIndex == queryTiles[j].pageIndex) {
                        bool xOverlap = (queryTiles[i].docX < queryTiles[j].docX + queryTiles[j].tileWidth) &&
                                        (queryTiles[i].docX + queryTiles[i].tileWidth > queryTiles[j].docX);
                        bool yOverlap = (queryTiles[i].docY < queryTiles[j].docY + queryTiles[j].tileHeight) &&
                                        (queryTiles[i].docY + queryTiles[i].tileHeight > queryTiles[j].docY);
                        if (xOverlap && yOverlap) {
                            std::cerr << "[FAIL] Overlapping tiles detected in partition!" << std::endl;
                            return 1;
                        }
                    }
                }
            }

            // Invariant 2: Total area of clipped tiles across visible pages must exactly equal
            // the intersection area of queryVp with all pages.
            int64_t expectedIntersectionArea = 0;
            for (int p = 0; p < layout.pageCount(); ++p) {
                const auto& pl = layout.getPageLayout(p);
                int64_t interLeft = std::max(queryVp.x, pl.leftPixel);
                int64_t interRight = std::min(queryVp.right(), pl.rightPixel());
                int64_t interTop = std::max(queryVp.y, pl.topPixel);
                int64_t interBottom = std::min(queryVp.bottom(), pl.bottomPixel());
                if (interRight > interLeft && interBottom > interTop) {
                    expectedIntersectionArea += (interRight - interLeft) * (interBottom - interTop);
                }
            }

            int64_t summedTileClippedArea = 0;
            for (const auto& t : queryTiles) {
                int64_t cLeft = std::max(queryVp.x, t.docX);
                int64_t cRight = std::min(queryVp.right(), t.docX + t.tileWidth);
                int64_t cTop = std::max(queryVp.y, t.docY);
                int64_t cBottom = std::min(queryVp.bottom(), t.docY + t.tileHeight);
                if (cRight > cLeft && cBottom > cTop) {
                    summedTileClippedArea += (cRight - cLeft) * (cBottom - cTop);
                }
            }

            if (summedTileClippedArea != expectedIntersectionArea) {
                std::cerr << "[FAIL] Area conservation violated on page " 
                          << ": Summed tile area=" << summedTileClippedArea 
                          << ", Expected viewport∩page=" << expectedIntersectionArea << std::endl;
                return 1;
            }
        }
    }
    std::cout << "  -> Passed: Exact non-overlapping partition & 100% area conservation verified across 100 random viewports." << std::endl;

    // Test 7: Tight Physical Anchor Invariant & Far Document Edge Behavior
    std::cout << "[TEST 7] Testing physical page coordinate pinning (tolerance <= 1.5 device pixels) & far edge clamping..." << std::endl;
    layout.updateLayout(1.0, 1.0, 1024, 20, 20);
    const int testPageIdx = 2;
    const double testPtX = 150.0;
    const double testPtY = 300.0;

    const auto& pBefore = layout.getPageLayout(testPageIdx);
    int64_t docX1 = pBefore.leftPixel + static_cast<int64_t>(std::round(testPtX * 1.0 * 1.0));
    int64_t docY1 = pBefore.topPixel + static_cast<int64_t>(std::round(testPtY * 1.0 * 1.0));

    const int64_t cursorVpX = 250;
    const int64_t cursorVpY = 350;
    auto anchor = layout.createAnchor(docX1, docY1);

    // Zoom in to scale 2.75, DPR 1.5 (total resolution factor = 4.125 device pixels per PDF point)
    const double newScale = 2.75;
    const double newDpr = 1.5;
    const double totalFactor = newScale * newDpr;
    layout.updateLayout(newScale, newDpr, 1024, 20, 20);

    int64_t scrollX2 = 0, scrollY2 = 0;
    layout.resolveAnchor(anchor, cursorVpX, cursorVpY, 1024, 768, scrollX2, scrollY2);

    int64_t docX2 = scrollX2 + cursorVpX;
    int64_t docY2 = scrollY2 + cursorVpY;

    const auto& pAfter = layout.getPageLayout(testPageIdx);
    double reconstructedPtX = static_cast<double>(docX2 - pAfter.leftPixel) / totalFactor;
    double reconstructedPtY = static_cast<double>(docY2 - pAfter.topPixel) / totalFactor;

    // Strict tolerance: <= 1.5 device pixels expressed in PDF points
    const double maxAllowedPointDrift = 1.5 / totalFactor; // ~0.36 pt
    if (std::abs(reconstructedPtX - testPtX) > maxAllowedPointDrift || 
        std::abs(reconstructedPtY - testPtY) > maxAllowedPointDrift) {
        std::cerr << "[FAIL] Physical point drifted beyond 1.5 device pixels! Drift: (" 
                  << std::abs(reconstructedPtX - testPtX) << ", " << std::abs(reconstructedPtY - testPtY) 
                  << ") pt, Allowed: " << maxAllowedPointDrift << " pt" << std::endl;
        return 1;
    }

    // Far document edge clamping: anchor at bottom-right corner of entire document
    int64_t farDocX = layout.maxDocumentWidth() + 500;
    int64_t farDocY = layout.totalDocumentHeight() + 500;
    auto farAnchor = layout.createAnchor(farDocX, farDocY);

    int64_t farScrollX = 0, farScrollY = 0;
    int64_t vpW = 1024, vpH = 768;
    layout.resolveAnchor(farAnchor, 100, 100, vpW, vpH, farScrollX, farScrollY);

    int64_t maxExpectedScrollX = std::max<int64_t>(0, layout.maxDocumentWidth() - vpW);
    int64_t maxExpectedScrollY = std::max<int64_t>(0, layout.totalDocumentHeight() - vpH);

    if (farScrollX > maxExpectedScrollX || farScrollY > maxExpectedScrollY ||
        farScrollX < 0 || farScrollY < 0) {
        std::cerr << "[FAIL] Far edge scroll exceeded maximum bounds! Got: (" 
                  << farScrollX << ", " << farScrollY << "), Max: (" 
                  << maxExpectedScrollX << ", " << maxExpectedScrollY << ")" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Sub-pixel cursor pinning <= 1.5 device pixels and far edge clamping strictly bounded." << std::endl;

    // Test 8: 64-bit coordinates exceeding 2^31 (> 4.2 billion pixels)
    std::cout << "[TEST 8] Testing 64-bit coordinates exceeding 2^31..." << std::endl;
    std::vector<std::pair<double, double>> largeDoc(50000, {595.0, 842.0});
    layout::DocumentLayout hugeLayout;
    hugeLayout.setPageSizes(largeDoc);
    hugeLayout.updateLayout(100.0, 1.0, 0, 50, 0);

    int64_t totalH = hugeLayout.totalDocumentHeight();
    if (totalH <= 2147483647LL) {
        std::cerr << "[FAIL] Expected total height > 2^31, got: " << totalH << std::endl;
        return 1;
    }
    std::cout << "  -> Total height: " << totalH << " pixels (2^31 is 2,147,483,648)." << std::endl;

    int64_t deepOffset = 3500000000LL;
    auto deepRange = hugeLayout.getVisiblePages(deepOffset, 1200);
    if (!deepRange.isValid() || deepRange.firstVisiblePage < 0) {
        std::cerr << "[FAIL] Deep 64-bit query failed!" << std::endl;
        return 1;
    }
    std::cout << "  -> Deep query at Y = 3.5B pixels returned valid pages [" 
              << deepRange.firstVisiblePage << ", " << deepRange.lastVisiblePage << "]." << std::endl;
    std::cout << "  -> Passed: Handled 4.21 billion pixels with zero overflow." << std::endl;

    // Test 9: ScrollMapper Qt-free bidirectional mapping at 10^9 and 2^31
    std::cout << "[TEST 9] Testing ScrollMapper bidirectional mapping at 10^9, 2^31, and randomized property tests..." << std::endl;
    {
        layout::ScrollMapper mapper(1000000000); // 10^9

        // Case A: Small document (50,000 px, viewport 1,000 px) -> 1:1 exact mapping
        int32_t maxSlider = 0, pageStep = 0, singleStep = 0;
        mapper.computeRange(50000, 1000, maxSlider, pageStep, singleStep, 40);
        if (maxSlider != 49000 || pageStep != 1000 || singleStep != 40) {
            std::cerr << "[FAIL] Small document range compute failed: maxSlider=" << maxSlider << std::endl;
            return 1;
        }
        if (mapper.docToSlider(12345, 50000, 1000) != 12345 ||
            mapper.sliderToDoc(12345, 50000, 1000) != 12345) {
            std::cerr << "[FAIL] 1:1 exact mapping failed" << std::endl;
            return 1;
        }

        // Case B: Boundary at 10^9 px
        int64_t doc10e9 = 1000000000LL + 1000LL;
        mapper.computeRange(doc10e9, 1000, maxSlider, pageStep, singleStep, 40);
        if (maxSlider != 1000000000) {
            std::cerr << "[FAIL] Boundary 10^9 maxSlider expected 10^9, got " << maxSlider << std::endl;
            return 1;
        }

        // Case C: Enormous document exceeding 2^31 (5,000,000,000 px, viewport 1,000 px)
        int64_t hugeDoc = 5000000000LL;
        int64_t hugeVp = 1000;
        mapper.computeRange(hugeDoc, hugeVp, maxSlider, pageStep, singleStep, 40);
        if (maxSlider != 1000000000) {
            std::cerr << "[FAIL] Compressed mapping must cap at 10^9, got " << maxSlider << std::endl;
            return 1;
        }

        // Check midpoint round trip
        int64_t maxScroll = hugeDoc - hugeVp;
        int64_t midDoc = maxScroll / 2;
        int32_t sliderVal = mapper.docToSlider(midDoc, hugeDoc, hugeVp);
        if (sliderVal != 500000000) {
            std::cerr << "[FAIL] Expected midpoint slider 500,000,000, got " << sliderVal << std::endl;
            return 1;
        }
        int64_t roundTripDoc = mapper.sliderToDoc(sliderVal, hugeDoc, hugeVp);
        if (std::abs(roundTripDoc - midDoc) > 10) {
            std::cerr << "[FAIL] Round trip drift too large: " << std::abs(roundTripDoc - midDoc) << std::endl;
            return 1;
        }

        // Test extreme coordinates (10^12 px, zero overflow)
        int64_t terraDoc = 1000000000000LL; // 1 trillion pixels
        int64_t terraScroll = terraDoc - 1000;
        int32_t terraSlider = mapper.docToSlider(terraScroll / 2, terraDoc, 1000);
        if (terraSlider != 500000000) {
            std::cerr << "[FAIL] Trillion-pixel coordinate scaling failed: " << terraSlider << std::endl;
            return 1;
        }

        // Case D: Out-of-range slider value regression test
        // Negative slider must clamp to 0
        if (mapper.sliderToDoc(-500, hugeDoc, hugeVp) != 0) {
            std::cerr << "[FAIL] Negative slider did not clamp to 0" << std::endl;
            return 1;
        }
        // Slider exceeding m_maxSliderRange (e.g. INT32_MAX = 2147483647) on massive trillion-pixel document span
        // Must clamp slider to m_maxSliderRange prior to fraction multiplication, avoiding float overflow past INT64_MAX
        int64_t clampedDocOutOfRange = mapper.sliderToDoc(std::numeric_limits<int32_t>::max(), terraDoc, 1000);
        if (clampedDocOutOfRange != terraScroll) {
            std::cerr << "[FAIL] Out-of-range positive slider value did not clamp safely to maxDocScroll! Got: " 
                      << clampedDocOutOfRange << ", expected: " << terraScroll << std::endl;
            return 1;
        }
        if (mapper.sliderToDoc(999999, 50000, 1000) != 49000) {
            std::cerr << "[FAIL] Out-of-range slider on 1:1 range did not clamp to maxDocScroll" << std::endl;
            return 1;
        }

        // Property-based test over 1,000 randomized configurations
        std::mt19937 propRng(777);
        for (int iter = 0; iter < 1000; ++iter) {
            int64_t totalD = 0;
            if (iter % 3 == 0) totalD = 100 + (propRng() % 500000); // Small 1:1 range
            else if (iter % 3 == 1) totalD = 2000000000LL + (static_cast<int64_t>(propRng()) % 5000000000LL); // > 2^31
            else totalD = 1000000000000LL + (static_cast<int64_t>(propRng()) % 1000000000LL); // ~1 trillion
            int64_t vp = 100 + (propRng() % 2000);
            int64_t pMaxScroll = std::max<int64_t>(0, totalD - vp);

            int32_t sMax = 0, pStep = 0, sStep = 0;
            mapper.computeRange(totalD, vp, sMax, pStep, sStep);

            // Property 1: Endpoint consistency
            if (mapper.docToSlider(0, totalD, vp) != 0 || mapper.sliderToDoc(0, totalD, vp) != 0) {
                std::cerr << "[FAIL] Zero endpoint invariant violated" << std::endl;
                return 1;
            }
            if (mapper.docToSlider(pMaxScroll, totalD, vp) != sMax) {
                std::cerr << "[FAIL] docToSlider(pMaxScroll) != sMax" << std::endl;
                return 1;
            }
            if (mapper.sliderToDoc(sMax, totalD, vp) != pMaxScroll) {
                std::cerr << "[FAIL] sliderToDoc(sMax) != pMaxScroll" << std::endl;
                return 1;
            }

            // Property 2: Monotonicity
            int64_t docA = (pMaxScroll > 0) ? (static_cast<int64_t>(propRng()) % pMaxScroll) : 0;
            int64_t docB = (pMaxScroll > 0) ? (static_cast<int64_t>(propRng()) % pMaxScroll) : 0;
            if (docA > docB) std::swap(docA, docB);
            int32_t sA = mapper.docToSlider(docA, totalD, vp);
            int32_t sB = mapper.docToSlider(docB, totalD, vp);
            if (sA > sB) {
                std::cerr << "[FAIL] Monotonicity violated: docA < docB but sA > sB" << std::endl;
                return 1;
            }

            // Property 3: 1:1 round-trip exactness below 10^9
            if (pMaxScroll <= 1000000000LL) {
                if (mapper.sliderToDoc(sA, totalD, vp) != docA) {
                    std::cerr << "[FAIL] 1:1 round trip violated below 10^9!" << std::endl;
                    return 1;
                }
            } else {
                int64_t roundTrip = mapper.sliderToDoc(sA, totalD, vp);
                int64_t maxDrift = static_cast<int64_t>(std::ceil(static_cast<double>(pMaxScroll) / 1000000000.0)) + 1;
                if (std::abs(roundTrip - docA) > maxDrift) {
                    std::cerr << "[FAIL] Compressed round trip drift exceeded bound!" << std::endl;
                    return 1;
                }
            }
        }
        std::cout << "  -> Passed: ScrollMapper verified across 1:1, 10^9, 2^31, trillion-pixel spans, and 1,000 property-based iterations." << std::endl;
    }

    // Test 10: Compressed-scroll resize remapping edge case (> 10^9 slider range)
    std::cout << "[TEST 10] Testing compressed-scroll resize remapping edge case (> 10^9)..." << std::endl;
    {
        layout::ScrollMapper mapper(1000000000); // 10^9 slider range
        const int64_t totalDoc = 2500000000LL; // 2.5 billion pixels
        const int64_t oldVp = 800;
        const int64_t newVp = 1200;

        // Arbitrary document position
        const int64_t originalDocY = 1250000000LL;
        int32_t oldSlider = mapper.docToSlider(originalDocY, totalDoc, oldVp);

        // Naive broken approach: combining oldSlider with newVp directly drifts
        int64_t naiveDocY = mapper.sliderToDoc(oldSlider, totalDoc, newVp);
        // Demonstrating that oldSlider combined with newVp creates drift
        int64_t drift = std::abs(naiveDocY - originalDocY);
        if (drift < 50) {
            std::cerr << "[FAIL] Expected naive approach to exhibit noticeable drift" << std::endl;
            return 1;
        }
        // Correct remapped approach:
        int32_t newSlider = mapper.remapSliderOnResize(oldSlider, totalDoc, oldVp, newVp);
        int64_t correctedDocY = mapper.sliderToDoc(newSlider, totalDoc, newVp);

        int64_t correctedDrift = std::abs(correctedDocY - originalDocY);
        if (correctedDrift > 3) {
            std::cerr << "[FAIL] remapSliderOnResize drift exceeded bound! drift=" << correctedDrift << std::endl;
            return 1;
        }

        // Endpoint invariants on resize
        int32_t oldMaxSlider, oldPageStep, oldSingleStep;
        mapper.computeRange(totalDoc, oldVp, oldMaxSlider, oldPageStep, oldSingleStep);
        int32_t newMaxSlider, newPageStep, newSingleStep;
        mapper.computeRange(totalDoc, newVp, newMaxSlider, newPageStep, newSingleStep);

        if (mapper.remapSliderOnResize(0, totalDoc, oldVp, newVp) != 0) {
            std::cerr << "[FAIL] Resize remapping top endpoint != 0" << std::endl;
            return 1;
        }
        if (mapper.remapSliderOnResize(oldMaxSlider, totalDoc, oldVp, newVp) != newMaxSlider) {
            std::cerr << "[FAIL] Resize remapping bottom endpoint != newMaxSlider" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Compressed-scroll resize remapping edge case accurately verified." << std::endl;
    }

    // Test 11: View modes (single page, two-page, two-page with cover)
    std::cout << "[TEST 11] Testing view modes: row structure, no overlap, visible-page search vs brute force..." << std::endl;
    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<double> wd(200.0, 900.0), hd(200.0, 1200.0);
        for (int count : {1, 2, 3, 4, 5, 8, 9, 40}) {
            std::vector<std::pair<double, double>> sizes;
            for (int i = 0; i < count; ++i) sizes.push_back({wd(rng), hd(rng)});

            for (auto mode : {layout::ViewMode::Continuous, layout::ViewMode::SinglePage,
                              layout::ViewMode::TwoPage, layout::ViewMode::TwoPageCover}) {
                layout::DocumentLayout l;
                l.setViewMode(mode);
                l.setFocusPage(count / 2);
                l.setPageSizes(sizes);
                l.updateLayout(1.0, 1.0, 1000);

                // Row structure
                for (int i = 0; i < count; ++i) {
                    const int first = l.rowFirstPage(i);
                    const int rc = l.rowPageCount(i);
                    bool ok = first <= i && i < first + rc && first >= 0 && first + rc <= count;
                    if (mode == layout::ViewMode::TwoPage) {
                        ok = ok && (first % 2 == 0) && rc == ((first + 1 < count) ? 2 : 1);
                    } else if (mode == layout::ViewMode::TwoPageCover) {
                        ok = ok && ((first == 0 && rc == 1) || (first % 2 == 1 && rc == ((first + 1 < count) ? 2 : 1)));
                    } else {
                        ok = ok && rc == 1 && first == i;
                    }
                    if (!ok) {
                        std::cerr << "[FAIL] Row structure wrong: mode=" << static_cast<int>(mode)
                                  << " count=" << count << " page=" << i << std::endl;
                        return 1;
                    }
                }

                // Pages in the same row share a top and sit side by side without overlapping;
                // different rows never overlap vertically; everything stays inside the content width.
                for (int i = 0; i < count; ++i) {
                    const auto& p = l.getPageLayout(i);
                    if (mode != layout::ViewMode::SinglePage &&
                        (p.leftPixel < 0 || p.rightPixel() > l.maxDocumentWidth())) {
                        std::cerr << "[FAIL] Page outside content width in mode " << static_cast<int>(mode) << std::endl;
                        return 1;
                    }
                    for (int j = i + 1; j < count; ++j) {
                        const auto& q = l.getPageLayout(j);
                        const bool sameRow = l.rowFirstPage(i) == l.rowFirstPage(j);
                        const bool hOverlap = p.leftPixel < q.rightPixel() && q.leftPixel < p.rightPixel();
                        const bool vOverlap = p.topPixel < q.bottomPixel() && q.topPixel < p.bottomPixel();
                        if (sameRow && (p.topPixel != q.topPixel || hOverlap)) {
                            std::cerr << "[FAIL] Pages in a spread overlap or are misaligned" << std::endl;
                            return 1;
                        }
                        if (!sameRow && hOverlap && vOverlap) {
                            std::cerr << "[FAIL] Pages of different rows overlap (mode " << static_cast<int>(mode) << ")" << std::endl;
                            return 1;
                        }
                    }
                }

                // Binary-search visibility matches brute force over random viewports
                const int64_t total = l.totalDocumentHeight();
                for (int q = 0; q < 300; ++q) {
                    const int64_t vpH = 50 + static_cast<int64_t>(rng() % 1500);
                    const int64_t maxTop = std::max<int64_t>(0, total - vpH);
                    const int64_t vpTop = static_cast<int64_t>(rng() % static_cast<uint64_t>(maxTop + 1));
                    const auto fast = l.getVisiblePages(vpTop, vpH);
                    const auto slow = bruteForceVisiblePages(l, vpTop, vpH);
                    if (fast.firstVisiblePage != slow.firstVisiblePage || fast.lastVisiblePage != slow.lastVisiblePage) {
                        std::cerr << "[FAIL] getVisiblePages mismatch: mode=" << static_cast<int>(mode)
                                  << " count=" << count << " top=" << vpTop << " h=" << vpH
                                  << " fast=[" << fast.firstVisiblePage << "," << fast.lastVisiblePage << "]"
                                  << " slow=[" << slow.firstVisiblePage << "," << slow.lastVisiblePage << "]" << std::endl;
                        return 1;
                    }
                }

                // Single page: exactly the focus page is reachable
                if (mode == layout::ViewMode::SinglePage) {
                    const int f = l.focusPage();
                    const auto vis = l.getVisiblePages(0, total);
                    if (vis.firstVisiblePage != f || vis.lastVisiblePage != f) {
                        std::cerr << "[FAIL] SinglePage shows pages [" << vis.firstVisiblePage << ","
                                  << vis.lastVisiblePage << "] instead of " << f << std::endl;
                        return 1;
                    }
                    if (l.getPageLayout(f).topPixel != 16) {
                        std::cerr << "[FAIL] SinglePage focus page not at top gap" << std::endl;
                        return 1;
                    }
                }

                // Anchors round-trip onto the page they were taken from
                for (int i = 0; i < count; ++i) {
                    const auto& p = l.getPageLayout(i);
                    if (!p.widthPixel || !p.heightPixel) continue;
                    const auto a = l.createAnchor(p.leftPixel + p.widthPixel / 2, p.topPixel + p.heightPixel / 2);
                    if (mode != layout::ViewMode::SinglePage && a.pageIndex != i) {
                        std::cerr << "[FAIL] createAnchor picked page " << a.pageIndex << " instead of " << i
                                  << " in mode " << static_cast<int>(mode) << std::endl;
                        return 1;
                    }
                }
            }
        }

        // Spread geometry and fit helpers
        layout::DocumentLayout l;
        l.setViewMode(layout::ViewMode::TwoPageCover);
        l.setPageSizes({{500, 700}, {400, 800}, {600, 600}, {300, 500}});
        l.updateLayout(1.0, 1.0, 2000);
        const auto size = l.rowSizePoints(1);
        if (size.first != 1000.0 || size.second != 800.0) {
            std::cerr << "[FAIL] rowSizePoints for spread wrong: " << size.first << "x" << size.second << std::endl;
            return 1;
        }
        const auto cover = l.rowSizePoints(0);
        if (cover.first != 1000.0 || cover.second != 700.0) {
            std::cerr << "[FAIL] cover row should occupy a half-spread slot: " << cover.first << std::endl;
            return 1;
        }
        if (l.widestRowPoints() != 1000.0) {
            std::cerr << "[FAIL] widestRowPoints wrong: " << l.widestRowPoints() << std::endl;
            return 1;
        }
        // Cover sits on the right half, the trailing single page on the left half
        const auto& c0 = l.getPageLayout(0);
        if (c0.leftPixel < l.maxDocumentWidth() / 2) {
            std::cerr << "[FAIL] Cover page should sit right of the spine" << std::endl;
            return 1;
        }
        l.setViewMode(layout::ViewMode::TwoPage);
        l.setPageSizes({{500, 700}, {400, 800}, {600, 600}});
        l.updateLayout(1.0, 1.0, 2000);
        const auto& last = l.getPageLayout(2);
        if (last.rightPixel() > l.maxDocumentWidth() / 2) {
            std::cerr << "[FAIL] Trailing odd page should sit left of the spine" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: View-mode layouts verified against brute force across page counts and modes." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 11 DOCUMENT LAYOUT TESTS PASSED!   " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
