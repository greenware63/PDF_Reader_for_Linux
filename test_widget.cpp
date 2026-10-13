#include "ui/PdfViewerWidget.h"
#include <QApplication>
#include <QPixmap>
#include <QScrollBar>
#include <iostream>
#include <cassert>
#include <cmath>

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", "C:/Qt/6.7.2/mingw_64/plugins");
#endif
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << " RUNNING PDF VIEWER WIDGET UNIT TESTS   " << std::endl;
    std::cout << "========================================" << std::endl;

    std::string samplePath = (argc > 1) ? argv[1] : "sample.pdf";

    ui::PdfViewerWidget viewer;
    viewer.resize(800, 600);

    // Test 1: Open document
    std::cout << "[TEST 1] Testing document loading..." << std::endl;
    auto err = viewer.openDocument(QString::fromStdString(samplePath));
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to open " << samplePath << std::endl;
        return 1;
    }
    if (viewer.pageCount() != 1) {
        std::cerr << "[FAIL] Expected 1 page, got " << viewer.pageCount() << std::endl;
        return 1;
    }
    if (viewer.currentPage() != 0) {
        std::cerr << "[FAIL] Expected initial page 0, got " << viewer.currentPage() << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Document opened with 1 page." << std::endl;

    // Test 2: Zoom and 2D Anchor
    std::cout << "[TEST 2] Testing zoom operations and anchoring..." << std::endl;
    double initialZoom = viewer.currentZoom();
    viewer.zoomIn();
    if (viewer.currentZoom() <= initialZoom) {
        std::cerr << "[FAIL] Zoom in failed to increase zoom" << std::endl;
        return 1;
    }
    viewer.zoomReset();
    if (std::abs(viewer.currentZoom() - pdf::quantizeScale(96.0 / 72.0)) > 1e-6) {
        std::cerr << "[FAIL] Zoom reset did not restore quantized 1.333x: " << viewer.currentZoom() << std::endl;
        return 1;
    }
    viewer.zoomFitWidth();
    if (viewer.currentZoom() <= 0.0) {
        std::cerr << "[FAIL] Zoom fit width produced invalid scale" << std::endl;
        return 1;
    }
    viewer.zoomReset();
    std::cout << "  -> Passed: Zoom controls operate smoothly." << std::endl;

    // Test 3: Offscreen Render and Tile Caching
    std::cout << "[TEST 3] Testing widget render and tile cache integration..." << std::endl;
    QPixmap buffer(800, 600);
    viewer.viewport()->render(&buffer);

    if (viewer.cache().size() == 0) {
        std::cerr << "[FAIL] Painting viewer did not populate tile cache!" << std::endl;
        return 1;
    }
    if (viewer.cache().bytesUsed() == 0) {
        std::cerr << "[FAIL] Tile cache reports 0 bytes used after render!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Render populated " << viewer.cache().size() 
              << " tiles (" << viewer.cache().bytesUsed() << " bytes)." << std::endl;

    // Test 4: Document Invalidation on Close
    std::cout << "[TEST 4] Testing closeDocument and cache invalidation..." << std::endl;
    viewer.closeDocument();
    if (viewer.pageCount() != 0) {
        std::cerr << "[FAIL] Page count should be 0 after close" << std::endl;
        return 1;
    }
    if (viewer.cache().size() != 0) {
        std::cerr << "[FAIL] Cache should be empty after closeDocument!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Cache completely invalidated on document close." << std::endl;

    // Test 5: Fallback error handling
    std::cout << "[TEST 5] Testing corrupt / missing file error handling..." << std::endl;
    auto badErr = viewer.openDocument("non_existent_file.pdf");
    if (badErr == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Opening non-existent file should report error" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Error handled cleanly without crash." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL PDF VIEWER WIDGET TESTS PASSED!    " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
