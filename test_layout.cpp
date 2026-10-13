#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <limits>
#include "layout/DocumentLayout.h"

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

    // Case 3a: Narrow viewport (viewport narrower than the landscape page)
    const int64_t narrowVpW = 800;
    const int64_t margin = 20;
    layout.updateLayout(1.0, 1.0, narrowVpW, 30, margin);

    // Widest page is page 1 (842 px). Content width must be 842 + 2 * 20 = 882 px
    int64_t expectedContentW = 842 + 2 * margin;
    if (layout.maxDocumentWidth() != expectedContentW) {
        std::cerr << "[FAIL] maxDocumentWidth was " << layout.maxDocumentWidth() 
                  << ", expected " << expectedContentW << std::endl;
        return 1;
    }

    // Every page must be centered relative to expectedContentW:
    for (int i = 0; i < layout.pageCount(); ++i) {
        const auto& p = layout.getPageLayout(i);
        int64_t centerX = p.leftPixel + (p.widthPixel / 2);
        int64_t expectedCenter = expectedContentW / 2;
        if (std::abs(centerX - expectedCenter) > 1) {
            std::cerr << "[FAIL] Page " << i << " center X drifted: " << centerX 
                      << " vs expected " << expectedCenter << std::endl;
            return 1;
        }
        if (p.leftPixel < margin) {
            std::cerr << "[FAIL] Page " << i << " leftPixel violated margin in narrow viewport: " 
                      << p.leftPixel << std::endl;
            return 1;
        }
    }

    // Case 3b: Wide viewport (viewport wider than all pages)
    const int64_t wideVpW = 2000;
    layout.updateLayout(1.0, 1.0, wideVpW, 30, margin);
    if (layout.maxDocumentWidth() != wideVpW) {
        std::cerr << "[FAIL] maxDocumentWidth in wide viewport was " << layout.maxDocumentWidth()
                  << ", expected " << wideVpW << std::endl;
        return 1;
    }
    for (int i = 0; i < layout.pageCount(); ++i) {
        const auto& p = layout.getPageLayout(i);
        int64_t centerX = p.leftPixel + (p.widthPixel / 2);
        if (std::abs(centerX - (wideVpW / 2)) > 1) {
            std::cerr << "[FAIL] Page " << i << " not centered in wide viewport" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: All pages vertically aligned and centered in both narrow and wide viewports." << std::endl;

    // Test 4: Boundary Edge Cases (Above, Below, Gap, and Exact Edges)
    std::cout << "[TEST 4] Testing boundary edge cases (gaps, above, below, exact edges)..." << std::endl;
    // 4a. Viewport entirely above first page
    auto rangeAbove = layout.getVisiblePages(-500, 400);
    if (rangeAbove.isValid()) {
        std::cerr << "[FAIL] Expected invalid range above first page" << std::endl;
        return 1;
    }

    // 4b. Viewport entirely in gap between page 0 and page 1
    int64_t gapTop = layout.getPageLayout(0).bottomPixel() + 5;
    int64_t gapH = 15; // 30px gap total, sits in middle 15px
    auto rangeGap = layout.getVisiblePages(gapTop, gapH);
    if (rangeGap.isValid()) {
        std::cerr << "[FAIL] Expected invalid range in gap, got [" 
                  << rangeGap.firstVisiblePage << ", " << rangeGap.lastVisiblePage << "]" << std::endl;
        return 1;
    }

    // 4c. Viewport entirely below last page
    int64_t belowTop = layout.getPageLayout(4).bottomPixel() + 100;
    auto rangeBelow = layout.getVisiblePages(belowTop, 500);
    if (rangeBelow.isValid()) {
        std::cerr << "[FAIL] Expected invalid range below last page" << std::endl;
        return 1;
    }

    // 4d. Viewport exactly touching page edge (starting exactly at page top)
    const auto& p2 = layout.getPageLayout(2);
    auto rangeExactTop = layout.getVisiblePages(p2.topPixel, 100);
    if (!rangeExactTop.isValid() || rangeExactTop.firstVisiblePage != 2) {
        std::cerr << "[FAIL] Viewport exactly at page 2 top failed to detect page 2" << std::endl;
        return 1;
    }

    // 4e. Viewport ending exactly at page top (viewportBottom == p2.topPixel -> should NOT include p2)
    auto rangeEndingAtTop = layout.getVisiblePages(p2.topPixel - 100, 100);
    if (rangeEndingAtTop.isValid() && rangeEndingAtTop.lastVisiblePage >= 2) {
        std::cerr << "[FAIL] Viewport ending at page top improperly included page 2" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Exact edge and gap detection passed all boundary assertions." << std::endl;

    // Test 5: Brute-Force Property Test (1,000 random viewport queries cross-checked against linear scan)
    std::cout << "[TEST 5] Running 1,000 random viewport queries cross-checked against linear scan..." << std::endl;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int64_t> distTop(-200, layout.totalDocumentHeight() + 200);
    std::uniform_int_distribution<int64_t> distH(1, 2500);

    for (int q = 0; q < 1000; ++q) {
        int64_t vpTop = distTop(rng);
        int64_t vpH = distH(rng);

        auto fastResult = layout.getVisiblePages(vpTop, vpH);
        auto bruteResult = bruteForceVisiblePages(layout, vpTop, vpH);

        if (fastResult.firstVisiblePage != bruteResult.firstVisiblePage ||
            fastResult.lastVisiblePage != bruteResult.lastVisiblePage) {
            std::cerr << "[FAIL] Query mismatch at vpTop=" << vpTop << ", vpH=" << vpH
                      << " -> BinarySearch: [" << fastResult.firstVisiblePage << ", " << fastResult.lastVisiblePage << "]"
                      << " vs BruteForce: [" << bruteResult.firstVisiblePage << ", " << bruteResult.lastVisiblePage << "]" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: 1,000 / 1,000 random queries matched brute force exactly." << std::endl;

    // Test 6: Visible Tile Exact Partition & Area Conservation Invariant
    std::cout << "[TEST 6] Testing visible tile exact non-overlapping partition & area conservation..." << std::endl;
    std::uniform_int_distribution<int64_t> distVpX(-50, layout.maxDocumentWidth() + 50);
    std::uniform_int_distribution<int64_t> distVpY(-50, layout.totalDocumentHeight() + 50);
    std::uniform_int_distribution<int64_t> distVpW(200, 1500);
    std::uniform_int_distribution<int64_t> distVpH(200, 1500);

    for (int trial = 0; trial < 100; ++trial) {
        layout::Rect64 vp{distVpX(rng), distVpY(rng), distVpW(rng), distVpH(rng)};
        auto tiles = layout.getVisibleTiles(vp, 512);

        // Invariant A: No tiles for the same page may overlap
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& tA = tiles[i];
            for (size_t j = i + 1; j < tiles.size(); ++j) {
                const auto& tB = tiles[j];
                if (tA.pageIndex != tB.pageIndex) continue;

                bool xOverlap = (tA.tileXOnPage < tB.tileXOnPage + tB.tileWidth) &&
                                (tB.tileXOnPage < tA.tileXOnPage + tA.tileWidth);
                bool yOverlap = (tA.tileYOnPage < tB.tileYOnPage + tB.tileHeight) &&
                                (tB.tileYOnPage < tA.tileYOnPage + tA.tileHeight);
                if (xOverlap && yOverlap) {
                    std::cerr << "[FAIL] Overlapping tiles detected on page " << tA.pageIndex << std::endl;
                    return 1;
                }
            }
        }

        // Invariant B: Area conservation - Sum of clipped tile areas must EQUAL exact viewport ∩ page area
        auto range = layout.getVisiblePages(vp.y, vp.height);
        if (range.isValid()) {
            for (int pIdx = range.firstVisiblePage; pIdx <= range.lastVisiblePage; ++pIdx) {
                const auto& page = layout.getPageLayout(pIdx);
                int64_t interX0 = std::max(vp.x, page.leftPixel);
                int64_t interY0 = std::max(vp.y, page.topPixel);
                int64_t interX1 = std::min(vp.right(), page.rightPixel());
                int64_t interY1 = std::min(vp.bottom(), page.bottomPixel());

                int64_t expectedIntersectionArea = 0;
                if (interX1 > interX0 && interY1 > interY0) {
                    expectedIntersectionArea = (interX1 - interX0) * (interY1 - interY0);
                }

                int64_t summedTileClippedArea = 0;
                for (const auto& t : tiles) {
                    if (t.pageIndex != pIdx) continue;
                    int64_t tX0 = std::max(vp.x, t.docX);
                    int64_t tY0 = std::max(vp.y, t.docY);
                    int64_t tX1 = std::min(vp.right(), t.docX + t.tileWidth);
                    int64_t tY1 = std::min(vp.bottom(), t.docY + t.tileHeight);

                    if (tX1 > tX0 && tY1 > tY0) {
                        summedTileClippedArea += (tX1 - tX0) * (tY1 - tY0);
                    }
                }

                if (summedTileClippedArea != expectedIntersectionArea) {
                    std::cerr << "[FAIL] Area conservation violated on page " << pIdx 
                              << ": Summed tile area=" << summedTileClippedArea 
                              << ", Expected viewport∩page=" << expectedIntersectionArea << std::endl;
                    return 1;
                }
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
    double reconstructedPtX = (docX2 - pAfter.leftPixel) / totalFactor;
    double reconstructedPtY = (docY2 - pAfter.topPixel) / totalFactor;

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

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 8 DOCUMENT LAYOUT TESTS PASSED!    " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
