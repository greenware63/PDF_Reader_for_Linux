#include <iostream>
#include <vector>
#include <fstream>
#include <cmath>
#include <QImage>
#include <QColor>
#include "engine/IDocument.h"
#include "engine/IPage.h"

int main(int argc, char* argv[]) {
    std::cout << "========================================" << std::endl;
    std::cout << " Running PDF Reader Engine Test Suite   " << std::endl;
    std::cout << "========================================" << std::endl;

    auto doc = pdf::createDocument();
    if (!doc) {
        std::cerr << "[FAIL] Failed to create IDocument instance" << std::endl;
        return 1;
    }

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

    // Negative Test 3: Garbage bytes in memory
    std::cout << "[TEST 3] Testing garbage bytes in memory..." << std::endl;
    const uint8_t garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03};
    err = doc->loadFromMemory(garbage, sizeof(garbage));
    if (err != pdf::DocumentError::InvalidFormat) {
        std::cerr << "[FAIL] Expected InvalidFormat for garbage bytes, got: " << static_cast<int>(err) << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: correctly rejected garbage bytes as InvalidFormat." << std::endl;

    // Test 4: Loading valid sample.pdf
    std::string pdfPath = "sample.pdf";
    if (argc > 1) {
        pdfPath = argv[1];
    }
    std::cout << "[TEST 4] Loading valid PDF: " << pdfPath << std::endl;
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

    // Test 5: Non-destructive loading test (failed load preserves currently open document)
    std::cout << "[TEST 5] Testing non-destructive loading..." << std::endl;
    pdf::DocumentError failedErr = doc->loadFromFile("definitely_missing_file.pdf");
    if (failedErr != pdf::DocumentError::FileNotFound) {
        std::cerr << "[FAIL] Expected FileNotFound for missing file" << std::endl;
        return 1;
    }
    if (!doc->isOpen()) {
        std::cerr << "[FAIL] Previously open document was destroyed by failed load!" << std::endl;
        return 1;
    }
    if (doc->pageCount() != pages) {
        std::cerr << "[FAIL] Page count altered by failed load!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: document preserved after failed load attempt." << std::endl;

    // Test 6: Page dimensions and rendering
    std::cout << "[TEST 6] Retrieving and rendering page 0..." << std::endl;
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

    // Test 7: Verify pixel content is not all blank white
    std::cout << "[TEST 7] Checking rendered pixels for non-white content (text/graphics)..." << std::endl;
    bool hasNonWhitePixel = false;
    for (int y = 0; y < rendered.height() && !hasNonWhitePixel; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rendered.constScanLine(y));
        for (int x = 0; x < rendered.width(); ++x) {
            QRgb pixel = line[x];
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

    // Test 8: Memory buffer lifetime test (Problem 1 fix verification)
    std::cout << "[TEST 8] Testing in-memory loaded document lifetime tied to handle..." << std::endl;
    std::ifstream file(pdfPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[FAIL] Unable to open " << pdfPath << " for reading binary data" << std::endl;
        return 1;
    }
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> fileBuffer(fileSize);
    if (!file.read(fileBuffer.data(), fileSize)) {
        std::cerr << "[FAIL] Failed to read PDF file into buffer" << std::endl;
        return 1;
    }

    auto memDoc = pdf::createDocument();
    err = memDoc->loadFromMemory(fileBuffer.data(), fileBuffer.size());
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to load PDF from memory: " << static_cast<int>(err) << std::endl;
        return 1;
    }

    auto memPage = memDoc->getPage(0);
    if (!memPage) {
        std::cerr << "[FAIL] Failed to get page from memory document" << std::endl;
        return 1;
    }

    // Now close memDoc while memPage is still alive.
    // The underlying buffer must NOT be deallocated because memPage holds DocumentHandle.
    memDoc->close();

    QImage memRendered = memPage->render(1.0, 1.0);
    if (memRendered.isNull()) {
        std::cerr << "[FAIL] Memory-backed page failed to render after doc->close()!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: in-memory document remained safe and readable after doc->close()." << std::endl;

    // Test 9: Logical size preservation under clamping (Problem 2 fix verification)
    std::cout << "[TEST 9] Testing logical size preservation under extreme zoom clamping..." << std::endl;
    constexpr double EXTREME_ZOOM = 15.0; // 15x zoom triggers pixel clamping
    QImage clampedImage = page->render(EXTREME_ZOOM, 2.0);
    if (clampedImage.isNull()) {
        std::cerr << "[FAIL] Extreme zoom render returned null image" << std::endl;
        return 1;
    }
    double logicalW = clampedImage.width() / clampedImage.devicePixelRatio();
    double expectedLogicalW = w * EXTREME_ZOOM;
    if (std::abs(logicalW - expectedLogicalW) > 1.0) {
        std::cerr << "[FAIL] Page logical width shrank! Expected: " << expectedLogicalW 
                  << ", Got: " << logicalW << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: logical size perfectly preserved (expected: " 
              << expectedLogicalW << " pt, logical: " << logicalW << " pt)." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 9 TESTS PASSED SUCCESSFULLY!       " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
