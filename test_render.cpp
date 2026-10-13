#include <iostream>
#include <cassert>
#include <vector>
#include <QImage>
#include <QColor>
#include "engine/IDocument.h"
#include "engine/IPage.h"

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << " Running PDF Reader Engine Test Suite   " << std::endl;
    std::cout << "========================================" << std::endl;

    auto doc = pdf::createDocument();
    assert(doc != nullptr && "Document instance must not be null");

    // Negative Test 1: Non-existent file
    std::cout << "[TEST 1] Testing non-existent file handling..." << std::endl;
    pdf::DocumentError err = doc->loadFromFile("non_existent_file_xyz_123.pdf");
    if (err != pdf::DocumentError::FileNotFound) {
        std::cerr << "[FAIL] Expected FileNotFound error, got: " << static_cast<int>(err) << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: correctly reported FileNotFound." << std::endl;

    // Negative Test 2: Empty memory buffer
    std::cout << "[TEST 2] Testing empty memory buffer handling..." << std::endl;
    err = doc->loadFromMemory(nullptr, 0);
    if (err == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Expected error on empty buffer" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: correctly rejected empty buffer." << std::endl;

    // Test 3: Loading sample.pdf
    std::string pdfPath = "sample.pdf";
    if (argc > 1) {
        pdfPath = argv[1];
    }
    std::cout << "[TEST 3] Loading valid PDF: " << pdfPath << std::endl;
    err = doc->loadFromFile(pdfPath);
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to load sample PDF: " << pdfPath 
                  << " (Error code: " << static_cast<int>(err) << ")" << std::endl;
        return 1;
    }

    int pages = doc->pageCount();
    std::cout << "  -> Document has " << pages << " page(s)." << std::endl;
    if (pages <= 0) {
        std::cerr << "[FAIL] Expected at least 1 page" << std::endl;
        return 1;
    }

    // Test 4: Page dimensions and rendering
    std::cout << "[TEST 4] Retrieving and rendering page 0..." << std::endl;
    auto page = doc->getPage(0);
    if (!page) {
        std::cerr << "[FAIL] Failed to retrieve page 0" << std::endl;
        return 1;
    }

    double w = page->widthPoints();
    double h = page->heightPoints();
    std::cout << "  -> Page 0 points: " << w << " x " << h << std::endl;
    if (w <= 0.0 || h <= 0.0) {
        std::cerr << "[FAIL] Invalid page dimensions" << std::endl;
        return 1;
    }

    QImage rendered = page->render(1.0, 2.0); // 1.0 zoom, 2.0 DPR
    if (rendered.isNull()) {
        std::cerr << "[FAIL] Rendered QImage is null" << std::endl;
        return 1;
    }
    std::cout << "  -> Rendered bitmap size: " << rendered.width() << " x " << rendered.height() << std::endl;

    // Test 5: Verify pixel content is not all blank white
    std::cout << "[TEST 5] Checking rendered pixels for non-white content (text/graphics)..." << std::endl;
    bool hasNonWhitePixel = false;
    for (int y = 0; y < rendered.height() && !hasNonWhitePixel; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rendered.constScanLine(y));
        for (int x = 0; x < rendered.width(); ++x) {
            QRgb pixel = line[x];
            // If any pixel is not pure white (0xFFFFFFFF or R,G,B < 250)
            if (qRed(pixel) < 240 || qGreen(pixel) < 240 || qBlue(pixel) < 240) {
                hasNonWhitePixel = true;
                break;
            }
        }
    }

    if (!hasNonWhitePixel) {
        std::cerr << "[FAIL] Rendered image is completely blank white! Content not rendered." << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Non-white glyph/graphic pixels successfully detected." << std::endl;

    // Test 6: Ownership lifetime test (use-after-free regression test)
    std::cout << "[TEST 6] Testing shared ownership (closing document while holding page)..." << std::endl;
    doc->close(); // Close document!
    // Page must remain valid because it holds a shared DocumentHandle
    QImage pageAfterCloseRender = page->render(0.5, 1.0);
    if (pageAfterCloseRender.isNull()) {
        std::cerr << "[FAIL] Page failed to render after doc->close()!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Page safely outlived document closure without crash." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL TESTS PASSED SUCCESSFULLY!         " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
