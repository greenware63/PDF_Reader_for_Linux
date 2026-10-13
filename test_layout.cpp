#include <iostream>
#include <vector>
#include <cmath>
#include "layout/DocumentLayout.h"

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running DocumentLayout Unit Tests      " << std::endl;
    std::cout << "========================================" << std::endl;

    layout::DocumentLayout layout;

    // Create 10 A4 pages (595 x 842 pt)
    std::vector<std::pair<double, double>> pages(10, {595.0, 842.0});
    layout.setPageSizes(pages);
    layout.updateLayout(1.0, 1.0, 20); // 20px gap

    // Test 1: Monotonicity
    std::cout << "[TEST 1] Testing monotonic increasing page offsets..." << std::endl;
    if (layout.pageCount() != 10) {
        std::cerr << "[FAIL] Expected 10 pages, got: " << layout.pageCount() << std::endl;
        return 1;
    }

    for (int i = 0; i < 9; ++i) {
        const auto& p0 = layout.getPageLayout(i);
        const auto& p1 = layout.getPageLayout(i + 1);
        if (p0.topPixel + p0.heightPixel >= p1.topPixel) {
            std::cerr << "[FAIL] Pages overlap or non-monotonic at index " << i << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: Monotonic offsets verified." << std::endl;

    // Test 2: Visible page detection
    std::cout << "[TEST 2] Testing binary-search visible page range..." << std::endl;
    // Viewport positioned around page 2 (height 842, top around 2 * 862 + 20)
    int64_t vpTop = layout.getPageLayout(2).topPixel + 100;
    int64_t vpH = 500;
    auto range = layout.getVisiblePages(vpTop, vpH);
    if (!range.isValid() || range.firstVisiblePage != 2 || range.lastVisiblePage != 2) {
        std::cerr << "[FAIL] Expected only page 2 visible, got [" 
                  << range.firstVisiblePage << ", " << range.lastVisiblePage << "]" << std::endl;
        return 1;
    }

    // Viewport spanning across page 2 and page 3
    vpTop = layout.getPageLayout(2).topPixel + layout.getPageLayout(2).heightPixel - 100;
    vpH = 400; // Spans across the gap into page 3
    range = layout.getVisiblePages(vpTop, vpH);
    if (!range.isValid() || range.firstVisiblePage != 2 || range.lastVisiblePage != 3) {
        std::cerr << "[FAIL] Expected pages 2 and 3 visible, got [" 
                  << range.firstVisiblePage << ", " << range.lastVisiblePage << "]" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Exact visible page boundaries detected." << std::endl;

    // Test 3: Anchor preservation across zoom updates
    std::cout << "[TEST 3] Testing scroll anchor preservation across zoom changes..." << std::endl;
    int64_t initialCenter = layout.getPageLayout(4).topPixel + (layout.getPageLayout(4).heightPixel / 2);
    auto anchor = layout.createAnchor(initialCenter);
    if (anchor.pageIndex != 4 || std::abs(anchor.normalizedY - 0.5) > 0.01) {
        std::cerr << "[FAIL] Anchor failed to capture page 4 center: page " 
                  << anchor.pageIndex << ", normY " << anchor.normalizedY << std::endl;
        return 1;
    }

    // Zoom in 3x
    layout.updateLayout(3.0, 1.0, 20);
    int64_t resolvedTop = layout.resolveAnchor(anchor, 600);
    int64_t newCenter = resolvedTop + 300;
    const auto& zoomedPage4 = layout.getPageLayout(4);
    int64_t expectedCenter = zoomedPage4.topPixel + (zoomedPage4.heightPixel / 2);
    if (std::abs(newCenter - expectedCenter) > 2) {
        std::cerr << "[FAIL] Anchor drifted after 3x zoom! Expected: " 
                  << expectedCenter << ", Got: " << newCenter << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Anchor pinned center of page 4 across 3x zoom change." << std::endl;

    // Test 4: 64-bit coordinate space (> 2^31 pixels without overflow)
    std::cout << "[TEST 4] Testing 64-bit coordinates exceeding 2^31 (> 2.14 billion pixels)..." << std::endl;
    std::vector<std::pair<double, double>> largeDoc(50000, {595.0, 842.0}); // 50,000 pages
    layout::DocumentLayout hugeLayout;
    hugeLayout.setPageSizes(largeDoc);
    hugeLayout.updateLayout(100.0, 1.0, 50); // 100x zoom -> each page is 84,200 px tall

    int64_t totalH = hugeLayout.totalDocumentHeight();
    // 50,000 * 84,250 ≈ 4.21 billion pixels (exceeds int32_t 2.14B)
    std::cout << "  -> Total height: " << totalH << " pixels (2^31 is 2,147,483,648)." << std::endl;
    if (totalH <= 2147483647LL) {
        std::cerr << "[FAIL] Height calculation failed to exceed 2^31 pixels" << std::endl;
        return 1;
    }

    // Query visible range at 3.5 billion pixels down the document
    int64_t deepOffset = 3500000000LL;
    auto deepRange = hugeLayout.getVisiblePages(deepOffset, 1200);
    if (!deepRange.isValid() || deepRange.firstVisiblePage < 0) {
        std::cerr << "[FAIL] Deep 64-bit query failed!" << std::endl;
        return 1;
    }
    std::cout << "  -> Deep query at Y = 3.5B pixels returned valid pages [" 
              << deepRange.firstVisiblePage << ", " << deepRange.lastVisiblePage << "]." << std::endl;
    std::cout << "  -> Passed: 64-bit coordinates handled with zero overflow." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL DOCUMENT LAYOUT TESTS PASSED!      " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
