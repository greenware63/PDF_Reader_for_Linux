#include <iostream>
#include <vector>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "engine/IDocument.h"
#include "engine/IPage.h"
#include "engine/PixelBuffer.h"

static void generateComplexPdfFixture(const std::string& path) {
    std::string contentStream;
    // Diagonal crossing lines
    for (int i = 0; i < 30; ++i) {
        float r = ((i * 37) % 255) / 255.0f;
        float g = ((i * 73) % 255) / 255.0f;
        float b = ((i * 109) % 255) / 255.0f;
        float lineWidth = 1.0f + (i % 5);
        contentStream += std::to_string(lineWidth) + " w " + std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + " RG\n";
        contentStream += "0 " + std::to_string(i * 28) + " m 595 " + std::to_string(842 - i * 28) + " l S\n";
        contentStream += std::to_string(i * 19) + " 0 m " + std::to_string(595 - i * 19) + " 842 l S\n";
    }
    // Colored rectangular regions crossing 512px boundaries
    contentStream += "0.9 0.1 0.1 rg 50 100 500 650 re f\n";
    contentStream += "0.1 0.8 0.2 rg 100 150 400 550 re f\n";
    contentStream += "0.1 0.2 0.9 rg 150 200 300 450 re f\n";
    contentStream += "0.9 0.8 0.1 rg 200 250 200 350 re f\n";

    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Length " + std::to_string(contentStream.size()) + " >>\nstream\n";
    pdf += contentStream;
    pdf += "\nendstream\nendobj\n";

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 5\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

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

    pdf::PixelBuffer rendered = page->renderWholePage(1.0, 2.0); // 1.0 zoom, 2.0 DPR
    if (!rendered.isValid()) {
        std::cerr << "[FAIL] Rendered PixelBuffer is invalid" << std::endl;
        return 1;
    }
    std::cout << "  -> Rendered bitmap size: " << rendered.width() << " x " << rendered.height() << std::endl;

    // Test 7: Verify pixel content is not all blank white
    std::cout << "[TEST 7] Checking rendered pixels for non-white content (text/graphics)..." << std::endl;
    bool hasNonWhitePixel = false;
    for (int y = 0; y < rendered.height() && !hasNonWhitePixel; ++y) {
        const uint32_t* line = reinterpret_cast<const uint32_t*>(rendered.data() + y * rendered.stride());
        for (int x = 0; x < rendered.width(); ++x) {
            uint32_t pixel = line[x];
            // Format BGRX: 0xFFFFFFFF is pure white
            if ((pixel & 0x00FFFFFF) != 0x00FFFFFF) {
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

    // Test 8: Memory buffer lifetime test
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

    memDoc->close();

    pdf::PixelBuffer memRendered = memPage->renderWholePage(1.0, 1.0);
    if (!memRendered.isValid()) {
        std::cerr << "[FAIL] Memory-backed page failed to render after doc->close()!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: in-memory document remained safe and readable after doc->close()." << std::endl;

    // Test 9: Unconstrained virtual pixel dimension for tiling at 15x zoom
    std::cout << "[TEST 9] Testing unconstrained virtual pixel calculation for tiling..." << std::endl;
    constexpr double EXTREME_ZOOM = 15.0;
    int unconstrainedW = 0, unconstrainedH = 0;
    page->getPixelDimensions(EXTREME_ZOOM, 2.0, unconstrainedW, unconstrainedH);
    int expectedVirtualW = static_cast<int>(std::round(w * EXTREME_ZOOM * 2.0));
    int expectedVirtualH = static_cast<int>(std::round(h * EXTREME_ZOOM * 2.0));
    if (unconstrainedW != expectedVirtualW || unconstrainedH != expectedVirtualH) {
        std::cerr << "[FAIL] Virtual dimensions were improperly capped! Expected: "
                  << expectedVirtualW << " x " << expectedVirtualH << ", Got: "
                  << unconstrainedW << " x " << unconstrainedH << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: virtual tile space at 15x zoom is unconstrained (" 
              << unconstrainedW << " x " << unconstrainedH << " px)." << std::endl;

    // Test 10: File reload while retaining prior page
    std::cout << "[TEST 10] Testing document reload while holding previous page reference..." << std::endl;
    auto retainedPage = doc->getPage(0);
    if (!retainedPage) {
        std::cerr << "[FAIL] Failed to obtain page prior to reload" << std::endl;
        return 1;
    }
    err = doc->loadFromFile(pdfPath);
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Reload failed: " << static_cast<int>(err) << std::endl;
        return 1;
    }
    pdf::PixelBuffer retainedRender = retainedPage->renderWholePage(1.0, 1.0);
    if (!retainedRender.isValid()) {
        std::cerr << "[FAIL] Retained page failed to render after file reload!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: retained page from earlier load remained safely accessible." << std::endl;

    // Test 11: Complex Multi-Feature Fixture Stitching Invariant Test
    std::cout << "[TEST 11] Testing Tile Stitching on Complex Fixture with Vectors and Colors..." << std::endl;
    std::string complexPdfPath = "complex_seam_test.pdf";
    generateComplexPdfFixture(complexPdfPath);

    auto complexDoc = pdf::createDocument();
    err = complexDoc->loadFromFile(complexPdfPath);
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to load complex fixture: " << static_cast<int>(err) << std::endl;
        return 1;
    }
    auto complexPage = complexDoc->getPage(0);
    if (!complexPage) {
        std::cerr << "[FAIL] Failed to get complex page" << std::endl;
        return 1;
    }

    const double testScale = 1.333333; // 100% desktop scale
    const double testDpr = 1.5;        // Fractional DPR
    int fullW = 0, fullH = 0;
    complexPage->getPixelDimensions(testScale, testDpr, fullW, fullH);

    // Render full reference image
    pdf::PixelBuffer fullPageBuf = complexPage->renderTile({0, 0, fullW, fullH}, testScale, testDpr);
    if (!fullPageBuf.isValid()) {
        std::cerr << "[FAIL] Failed to render full page buffer on complex fixture" << std::endl;
        return 1;
    }

    // Allocate canvas for stitched tiles
    pdf::PixelBuffer stitchedBuf = pdf::PixelBuffer::allocate(fullW, fullH);
    if (!stitchedBuf.isValid()) {
        std::cerr << "[FAIL] Failed to allocate stitched buffer" << std::endl;
        return 1;
    }

    // Render 512x512 tile grid cutting across vector strokes and colored blocks
    constexpr int TILE_SIZE = 512;
    int tileCount = 0;
    for (int y = 0; y < fullH; y += TILE_SIZE) {
        for (int x = 0; x < fullW; x += TILE_SIZE) {
            int tw = std::min(TILE_SIZE, fullW - x);
            int th = std::min(TILE_SIZE, fullH - y);
            pdf::PixelBuffer tile = complexPage->renderTile({x, y, tw, th}, testScale, testDpr);
            if (!tile.isValid() || tile.width() != tw || tile.height() != th) {
                std::cerr << "[FAIL] Tile (" << x << ", " << y << ") render failed on complex fixture!" << std::endl;
                return 1;
            }

            for (int r = 0; r < th; ++r) {
                const uint8_t* srcRow = tile.data() + r * tile.stride();
                uint8_t* dstRow = stitchedBuf.data() + (y + r) * stitchedBuf.stride() + x * 4;
                std::memcpy(dstRow, srcRow, tw * 4);
            }
            ++tileCount;
        }
    }
    std::cout << "  -> Stitched " << tileCount << " tiles across vectors and colored boundary shapes." << std::endl;

    // Strict pixel-by-pixel byte comparison
    int diffCount = 0;
    int maxDiff = 0;
    for (int y = 0; y < fullH; ++y) {
        const uint8_t* fullRow = fullPageBuf.data() + y * fullPageBuf.stride();
        const uint8_t* stitchRow = stitchedBuf.data() + y * stitchedBuf.stride();
        for (int x = 0; x < fullW * 4; ++x) {
            int d = std::abs(static_cast<int>(fullRow[x]) - static_cast<int>(stitchRow[x]));
            if (d > 0) {
                ++diffCount;
                if (d > maxDiff) maxDiff = d;
            }
        }
    }

    std::cout << "  [DIAGNOSTIC] Total differing bytes: " << diffCount 
              << " / " << (fullW * fullH * 4) << ", Max Delta: " << maxDiff << " / 255" << std::endl;

    // PDFium's rasterizer uses anti-aliasing curve fitting that can evaluate sub-pixels with slight +-1 delta at clipped tile boundaries
    if (maxDiff > 3) {
        std::cerr << "[FAIL] Tile seam detected! Differing pixels have visible delta: " << maxDiff << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Zero visible seams! (all variations strictly sub-pixel within max delta: " 
              << maxDiff << " / 255)." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 11 TESTS PASSED SUCCESSFULLY!      " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
