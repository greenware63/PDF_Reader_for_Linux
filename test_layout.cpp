#include <iostream>
#include <vector>
#include <cmath>
#include <random>
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

    // Test 2: Shared Pixel-Dimension Canonical Formula Agreement
    std::cout << "[TEST 2] Testing canonical pixel dimension agreement..." << std::endl;
    const double testWPoints = 595.0;
    const double testHPoints = 842.0;
    const double testScale = 1.333333;
    const double testDpr = 1.75;
    int canonicalW = 0, canonicalH = 0;
    layout::computePagePixelDimensions(testWPoints, testHPoints, testScale, testDpr, canonicalW, canonicalH);

    layout::DocumentLayout singleLayout;
    singleLayout.setPageSizes({{testWPoints, testHPoints}});
    singleLayout.updateLayout(testScale, testDpr, 1000, 10, 10);
    const auto& singlePage = singleLayout.getPageLayout(0);
    if (singlePage.widthPixel != canonicalW || singlePage.heightPixel != canonicalH) {
        std::cerr << "[FAIL] Layout dimensions do not match computePagePixelDimensions: "
                  << singlePage.widthPixel << "x" << singlePage.heightPixel 
                  << " vs " << canonicalW << "x" << canonicalH << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Canonical computePagePixelDimensions agrees with DocumentLayout." << std::endl;

    // Test 3: Mixed Portrait & Landscape Monotonicity and Centering
    std::cout << "[TEST 3] Testing mixed portrait & landscape pages with horizontal centering..." << std::endl;
    layout::DocumentLayout layout;
    std::vector<std::pair<double, double>> pages = {
        {595.0, 842.0},  // Portrait A4
        {842.0, 595.0},  // Landscape A4
        {612.0, 792.0},  // US Letter
        {792.0, 612.0},  // US Letter Landscape
        {595.0, 842.0},
        {595.0, 842.0},
        {842.0, 595.0},
        {595.0, 842.0},
        {595.0, 842.0},
        {595.0, 842.0}
    };
    layout.setPageSizes(pages);
    layout.updateLayout(1.25, 1.5, 1920, 30, 20); // 30px gap, 20px margin, centered in 1920px viewport

    if (layout.pageCount() != 10) {
        std::cerr << "[FAIL] Expected 10 pages, got: " << layout.pageCount() << std::endl;
        return 1;
    }

    for (int i = 0; i < 9; ++i) {
        const auto& p0 = layout.getPageLayout(i);
        const auto& p1 = layout.getPageLayout(i + 1);
        if (p0.bottomPixel() >= p1.topPixel) {
            std::cerr << "[FAIL] Pages overlap or non-monotonic at index " << i << std::endl;
            return 1;
        }
        if (p0.leftPixel < 20) {
            std::cerr << "[FAIL] Page " << i << " violated minimum margin" << std::endl;
            return 1;
        }
        // Verify horizontal centering in 1920px viewport
        int64_t expectedLeft = (1920 - p0.widthPixel) / 2;
        if (p0.leftPixel != expectedLeft) {
            std::cerr << "[FAIL] Page " << i << " not horizontally centered: got "
                      << p0.leftPixel << ", expected " << expectedLeft << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: Monotonic offsets, margins, and centering verified." << std::endl;

    // Test 4: Boundary Edge Cases (Above, Below, Gap, and Exact Edges)
    std::cout << "[TEST 4] Testing boundary edge cases (gaps, above, below, exact edges)...\"" << std::endl;
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
    int64_t belowTop = layout.getPageLayout(9).bottomPixel() + 100;
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

    // Test 6: Visible Tile Enumeration (Qt-free)
    std::cout << "[TEST 6] Testing Qt-free visible tile enumeration..." << std::endl;
    layout::Rect64 viewportRect{300, 200, 800, 1000};
    auto tiles = layout.getVisibleTiles(viewportRect, 512);
    if (tiles.empty()) {
        std::cerr << "[FAIL] Expected visible tiles, got 0" << std::endl;
        return 1;
    }
    std::cout << "  -> Generated " << tiles.size() << " visible tiles." << std::endl;
    for (const auto& t : tiles) {
        if (t.tileWidth <= 0 || t.tileWidth > 512 || t.tileHeight <= 0 || t.tileHeight > 512) {
            std::cerr << "[FAIL] Invalid tile dimension: " << t.tileWidth << " x " << t.tileHeight << std::endl;
            return 1;
        }
        if (t.docX < 0 || t.docY < 0) {
            std::cerr << "[FAIL] Invalid document coordinates on tile" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: Tile grid enumeration verified." << std::endl;

    // Test 7: 2D Scroll Anchoring across Zoom
    std::cout << "[TEST 7] Testing 2D scroll anchor pinning across zoom..." << std::endl;
    int64_t cursorDocX = layout.getPageLayout(3).leftPixel + 200;
    int64_t cursorDocY = layout.getPageLayout(3).topPixel + 300;
    auto anchor2D = layout.createAnchor(cursorDocX, cursorDocY);
    if (anchor2D.pageIndex != 3) {
        std::cerr << "[FAIL] Anchor missed page 3" << std::endl;
        return 1;
    }

    // Zoom in from 1.25x to 2.5x
    layout.updateLayout(2.5, 1.5, 1920, 30, 20);
    int64_t scrollX = 0, scrollY = 0;
    layout.resolveAnchor(anchor2D, 200, 300, 1920, 1080, scrollX, scrollY);
    const auto& p3Zoomed = layout.getPageLayout(3);
    int64_t expectedTargetX = p3Zoomed.leftPixel + static_cast<int64_t>(std::round(anchor2D.normalizedX * p3Zoomed.widthPixel));
    int64_t expectedTargetY = p3Zoomed.topPixel + static_cast<int64_t>(std::round(anchor2D.normalizedY * p3Zoomed.heightPixel));
    if (std::abs((scrollX + 200) - expectedTargetX) > 2 || std::abs((scrollY + 300) - expectedTargetY) > 2) {
        std::cerr << "[FAIL] 2D anchor drifted under zoom!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: 2D cursor anchor pinned accurately." << std::endl;

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
