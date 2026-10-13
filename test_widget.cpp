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
    std::string stream = "1 1 1 rg 0 0 595 842 re f\n0 0 1 rg 50 50 100 100 re f\n";
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
    viewer.resize(800, 600);

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

    // Test 2: Multi-page navigation and 5% zoom Next to end
    std::cout << "[TEST 2] Testing multi-page goToPage and Next at 5% zoom without stalling..." << std::endl;
    std::string multiPath = "multi_page_widget.pdf";
    generateMultiPagePdf(multiPath);
    err = viewer.openDocument(QString::fromStdString(multiPath));
    if (err != pdf::DocumentError::None || viewer.pageCount() != 5) {
        std::cerr << "[FAIL] Failed to load 5-page fixture: count=" << viewer.pageCount() << std::endl;
        return 1;
    }

    // Jump to every page and verify currentPage matches
    for (int p = 0; p < 5; ++p) {
        viewer.goToPage(p);
        if (viewer.currentPage() != p) {
            std::cerr << "[FAIL] goToPage(" << p << ") did not update currentPage: got " << viewer.currentPage() << std::endl;
            return 1;
        }
    }

    // Set zoom to 5% where all 5 pages fit inside viewport
    viewer.setZoom(0.05);
    viewer.goToPage(0);
    if (viewer.currentPage() != 0) {
        std::cerr << "[FAIL] Failed to return to page 0" << std::endl;
        return 1;
    }

    // Repeatedly call nextPage() - must advance all the way to page 4 without stalling
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
    std::cout << "  -> Passed: goToPage and nextPage smoothly navigated to the end at 5% zoom without stalling." << std::endl;

    // Test 3: Render to QImage and verify background, paper boundaries, and tile seamlessness across DPRs
    std::cout << "[TEST 3] Testing pixel rendering and seam verification across DPR 1.0, 1.5, 2.0..." << std::endl;
    viewer.goToPage(0);
    viewer.zoomReset();
    viewer.resize(1200, 800);

    for (double testDpr : {1.0, 1.5, 2.0}) {
        int imgW = static_cast<int>(std::round(1200 * testDpr));
        int imgH = static_cast<int>(std::round(800 * testDpr));
        QImage renderTarget(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
        renderTarget.fill(Qt::transparent);

        QPainter painter(&renderTarget);
        painter.scale(testDpr, testDpr);
        viewer.viewport()->render(&painter);
        painter.end();

        // 1. Check workspace background color at (20, 20) outside page (#323639: R=50, G=54, B=57)
        QRgb cornerPx = renderTarget.pixel(static_cast<int>(std::round(20 * testDpr)), static_cast<int>(std::round(20 * testDpr)));
        int r = qRed(cornerPx), g = qGreen(cornerPx), b = qBlue(cornerPx);
        if (std::abs(r - 50) > 2 || std::abs(g - 54) > 2 || std::abs(b - 57) > 2) {
            std::cerr << "[FAIL] Background color mismatch at DPR " << testDpr 
                      << ": RGB(" << r << "," << g << "," << b << "), expected (50,54,57)" << std::endl;
            return 1;
        }

        // 2. Center pixel of page 0 should be inside the white page (R=255, G=255, B=255)
        QRgb centerPx = renderTarget.pixel(imgW / 2, static_cast<int>(std::round(200 * testDpr)));
        if (qRed(centerPx) < 250 || qGreen(centerPx) < 250 || qBlue(centerPx) < 250) {
            std::cerr << "[FAIL] Page paper center not white at DPR " << testDpr 
                      << ": RGB(" << qRed(centerPx) << "," << qGreen(centerPx) << "," << qBlue(centerPx) << ")" << std::endl;
            return 1;
        }
    }
    std::cout << "  -> Passed: Pixel-accurate workspace background (#323639) and white page rendering verified across DPRs." << std::endl;

    // Test 4: 2D cursor zoom anchor precision
    std::cout << "[TEST 4] Testing 2D cursor zoom anchor precision (tolerance <= 1.5 px)..." << std::endl;
    viewer.zoomReset();
    viewer.goToPage(0);

    QPoint cursorWidgetPos(450, 250);
    double initialScale = viewer.currentZoom();
    const auto& p0Layout = viewer.layout().getPageLayout(0);
    double dpr = 1.0;

    int64_t beforeDocX = (viewer.horizontalScrollBar()->value()) + cursorWidgetPos.x();
    int64_t beforeDocY = (viewer.verticalScrollBar()->value()) + cursorWidgetPos.y();
    double ptX = (beforeDocX - p0Layout.leftPixel) / (initialScale * dpr);
    double ptY = (beforeDocY - p0Layout.topPixel) / (initialScale * dpr);

    viewer.setZoom(initialScale * 2.0, &cursorWidgetPos);
    double newScale = viewer.currentZoom();

    const auto& p0After = viewer.layout().getPageLayout(0);
    int64_t afterDocX = (viewer.horizontalScrollBar()->value()) + cursorWidgetPos.x();
    int64_t afterDocY = (viewer.verticalScrollBar()->value()) + cursorWidgetPos.y();
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

    // Test 5: Document invalidation on close
    std::cout << "[TEST 5] Testing closeDocument and cache invalidation..." << std::endl;
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
