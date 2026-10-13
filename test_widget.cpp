#include "ui/PdfViewerWidget.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cmath>
#include <vector>

static void generateMultiPagePdf(const std::string& path) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R] /Count 5 >>\nendobj\n";

    // 5 pages (A4 595 x 842 pt)
    for (int i = 0; i < 5; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 8 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    // Solid blue fill across the entire 595 x 842 pt page
    std::string stream = "0 0 1 rg 0 0 595 842 re f\n";
    pdf += "8 0 obj\n<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream\nendobj\n";

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\n";
    pdf += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

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
    viewer.resize(1000, 700);
    viewer.show();
    viewer.ensurePolished();

    if (viewer.viewport()->width() < 800) {
        std::cerr << "[FAIL] Viewport geometry uninitialized! width=" << viewer.viewport()->width() << std::endl;
        return 1;
    }

    // Test 1: Open good document, then bad document -> good document remains open!
    std::cout << "[TEST 1] Testing failed open preservation (good document remains active)..." << std::endl;
    auto err = viewer.openDocument(QString::fromStdString(samplePath));
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to open " << samplePath << std::endl;
        return 1;
    }
    int origCount = viewer.pageCount();
    if (origCount <= 0) {
        std::cerr << "[FAIL] Expected positive page count" << std::endl;
        return 1;
    }

    // Attempt opening non-existent file
    auto badErr = viewer.openDocument("non_existent_file_definitely_missing.pdf");
    if (badErr == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Bad file open should report error" << std::endl;
        return 1;
    }
    if (viewer.pageCount() != origCount) {
        std::cerr << "[FAIL] Failed open destroyed active document! Page count changed: " 
                  << viewer.pageCount() << " != " << origCount << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Failed open preserves active document intact." << std::endl;

    // Test 2: Multi-page navigation at zoom where bottom clamps
    std::cout << "[TEST 2] Testing multi-page navigation when scroll position clamps at bottom..." << std::endl;
    std::string multiPath = "multi_page_widget.pdf";
    generateMultiPagePdf(multiPath);
    err = viewer.openDocument(QString::fromStdString(multiPath));
    if (err != pdf::DocumentError::None || viewer.pageCount() != 5) {
        std::cerr << "[FAIL] Failed to load 5-page fixture: count=" << viewer.pageCount() << std::endl;
        return 1;
    }

    // Set zoom to 0.3 where total doc height is ~1330 px and viewport is 700 px (scrollbar range > 0, bottom clamps)
    viewer.setZoom(0.3);
    viewer.goToPage(0);
    if (viewer.currentPage() != 0) {
        std::cerr << "[FAIL] Failed to go to page 0" << std::endl;
        return 1;
    }

    // Jump directly to page 4 (which clamps vertical scrollbar at maximum bottom offset)
    viewer.goToPage(4);
    if (viewer.currentPage() != 4) {
        std::cerr << "[FAIL] goToPage(4) clamped bottom offset overwrote target page! Got: " << viewer.currentPage() << std::endl;
        return 1;
    }

    // Return to page 0 and step nextPage() sequentially all the way to page 4
    viewer.goToPage(0);
    for (int step = 0; step < 4; ++step) {
        int prevPage = viewer.currentPage();
        viewer.nextPage();
        if (viewer.currentPage() != prevPage + 1) {
            std::cerr << "[FAIL] Next stalled at page " << prevPage 
                      << " instead of advancing to " << (prevPage + 1) << "!" << std::endl;
            return 1;
        }
    }
    if (viewer.currentPage() != 4) {
        std::cerr << "[FAIL] Did not reach last page (page 4), current is " << viewer.currentPage() << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Navigation correctly reaches clamped bottom pages without stalling." << std::endl;

    // Test 3: Offscreen rendering, DPR injection (1.0, 1.5, 2.0), and tile seam verification
    std::cout << "[TEST 3] Testing DPR injection (1.0, 1.5, 2.0) and tile seam continuity..." << std::endl;
    viewer.goToPage(0);
    viewer.zoomReset();
    viewer.resize(1200, 800);

    for (double testDpr : {1.0, 1.5, 2.0}) {
        viewer.setDevicePixelRatioOverride(testDpr);
        viewer.verticalScrollBar()->setValue(0);
        viewer.horizontalScrollBar()->setValue(0);

        int imgW = viewer.viewport()->width();
        int imgH = viewer.viewport()->height();
        QImage renderTarget(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
        renderTarget.fill(Qt::transparent);

        QPainter painter(&renderTarget);
        viewer.viewport()->render(&painter);
        painter.end();

        // 1. Verify workspace background color at top-left corner (#323639: R=50, G=54, B=57)
        QRgb cornerPx = renderTarget.pixel(5, 5);
        int r = qRed(cornerPx), g = qGreen(cornerPx), b = qBlue(cornerPx);
        if (std::abs(r - 50) > 3 || std::abs(g - 54) > 3 || std::abs(b - 57) > 3) {
            std::cerr << "[FAIL] Background color mismatch at DPR " << testDpr 
                      << ": RGB(" << r << "," << g << "," << b << "), expected (50,54,57)" << std::endl;
            return 1;
        }

        // 2. Scan across horizontal tile boundary on page 0 across column seam (512 px)
        const auto& p0Layout = viewer.layout().getPageLayout(0);
        int64_t tileSeamDocX = p0Layout.leftPixel + 512 - viewer.currentDocScrollX();
        int logicalSeamX = static_cast<int>(std::round(tileSeamDocX / testDpr));

        int64_t scanDocY = p0Layout.topPixel + 200 - viewer.currentDocScrollY();
        int scanY = static_cast<int>(std::round(scanDocY / testDpr));

        if (logicalSeamX > 20 && logicalSeamX < imgW - 20 && scanY > 0 && scanY < imgH) {
            // Scan across the seam: 10 pixels to the left and 10 pixels to the right
            for (int dx = -10; dx <= 10; ++dx) {
                QRgb seamPx = renderTarget.pixel(logicalSeamX + dx, scanY);
                // Blue page: R=0, G=0, B=255. Must have high blue and low red/green
                if (qBlue(seamPx) < 200 || qRed(seamPx) > 50) {
                    std::cerr << "[FAIL] Tile seam gap or color discontinuity detected at DPR " << testDpr 
                              << " at x=" << (logicalSeamX + dx) << ", y=" << scanY 
                              << ": RGB(" << qRed(seamPx) << "," << qGreen(seamPx) << "," << qBlue(seamPx) << ")" << std::endl;
                    return 1;
                }
            }
        }
    }
    viewer.setDevicePixelRatioOverride(0.0); // Reset override
    std::cout << "  -> Passed: DPR injection (1.0, 1.5, 2.0) and zero tile seam gap verified." << std::endl;

    // Test 4: 2D cursor zoom anchor precision
    std::cout << "[TEST 4] Testing 2D cursor zoom anchor precision (tolerance <= 1.5 px)..." << std::endl;
    viewer.zoomReset();
    viewer.goToPage(0);

    QPoint cursorWidgetPos(450, 250);
    double initialScale = viewer.currentZoom();
    const auto& p0Layout = viewer.layout().getPageLayout(0);
    double dpr = viewer.effectiveDpr();

    int64_t beforeDocX = (viewer.horizontalScrollBar()->value()) + static_cast<int64_t>(std::round(cursorWidgetPos.x() * dpr));
    int64_t beforeDocY = (viewer.verticalScrollBar()->value()) + static_cast<int64_t>(std::round(cursorWidgetPos.y() * dpr));
    double ptX = (beforeDocX - p0Layout.leftPixel) / (initialScale * dpr);
    double ptY = (beforeDocY - p0Layout.topPixel) / (initialScale * dpr);

    viewer.setZoom(initialScale * 2.0, &cursorWidgetPos);
    double newScale = viewer.currentZoom();

    const auto& p0After = viewer.layout().getPageLayout(0);
    int64_t afterDocX = (viewer.horizontalScrollBar()->value()) + static_cast<int64_t>(std::round(cursorWidgetPos.x() * dpr));
    int64_t afterDocY = (viewer.verticalScrollBar()->value()) + static_cast<int64_t>(std::round(cursorWidgetPos.y() * dpr));
    double reconstructedPtX = (afterDocX - p0After.leftPixel) / (newScale * dpr);
    double reconstructedPtY = (afterDocY - p0After.topPixel) / (newScale * dpr);

    double driftX = std::abs(reconstructedPtX - ptX) * newScale * dpr;
    double driftY = std::abs(reconstructedPtY - ptY) * newScale * dpr;

    if (driftX > 1.5 || driftY > 1.5) {
        std::cerr << "[FAIL] Anchor drift exceeded 1.5 device pixels! driftX=" 
                  << driftX << " px, driftY=" << driftY << " px" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Anchor drift strictly within 1.5 device pixels (driftX=" 
              << driftX << " px, driftY=" << driftY << " px)." << std::endl;

    // Test 5: Good-to-good document swap purges previous document's cache
    std::cout << "[TEST 5] Testing document swap cache invalidation..." << std::endl;
    // Cache has entries from multiPath
    size_t prevCacheSize = viewer.cache().size();
    if (prevCacheSize == 0) {
        std::cerr << "[FAIL] Expected non-empty cache before swap" << std::endl;
        return 1;
    }

    // Open samplePath again
    err = viewer.openDocument(QString::fromStdString(samplePath));
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to open samplePath during swap test" << std::endl;
        return 1;
    }
    // Previous document's tiles should be gone from cache
    if (viewer.cache().size() != 0) {
        std::cerr << "[FAIL] Cache should be flushed after swapping to new document!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Document swap completely invalidated previous document cache." << std::endl;

    // Test 6: Document invalidation on close
    std::cout << "[TEST 6] Testing closeDocument and clean state teardown..." << std::endl;
    viewer.closeDocument();
    if (viewer.pageCount() != 0 || viewer.cache().size() != 0) {
        std::cerr << "[FAIL] closeDocument failed to clear page count or cache" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Clean teardown and cache invalidation verified." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL PDF VIEWER WIDGET TESTS PASSED!    " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
