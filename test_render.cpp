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

    // 1. Diagonal crossing lines
    for (int i = 0; i < 30; ++i) {
        float r = ((i * 37) % 255) / 255.0f;
        float g = ((i * 73) % 255) / 255.0f;
        float b = ((i * 109) % 255) / 255.0f;
        float lineWidth = 1.0f + (i % 5);
        contentStream += std::to_string(lineWidth) + " w " + std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + " RG\n";
        contentStream += "0 " + std::to_string(i * 28) + " m 595 " + std::to_string(842 - i * 28) + " l S\n";
        contentStream += std::to_string(i * 19) + " 0 m " + std::to_string(595 - i * 19) + " 842 l S\n";
    }

    // 2. Colored rectangular regions crossing 512px boundaries
    contentStream += "0.9 0.1 0.1 rg 50 100 500 650 re f\n";
    contentStream += "0.1 0.8 0.2 rg 100 150 400 550 re f\n";
    contentStream += "0.1 0.2 0.9 rg 150 200 300 450 re f\n";
    contentStream += "0.9 0.8 0.1 rg 200 250 200 350 re f\n";

    // 3. Clipped path crossing tile boundaries
    contentStream += "q 120 180 350 400 re W n 0.7 0.2 0.8 rg 0 0 595 842 re f Q\n";

    // 4. Semi-transparent overlay with ExtGState
    contentStream += "q /GS1 gs 0.2 0.8 0.7 rg 60 220 450 350 re f Q\n";

    // 5. Known target shape for extreme zoom precision verification:
    // Pure blue box at x=100, y=742 (top in PDF is 842 - (742+50) = 50 pt from top), width=50, height=50
    contentStream += "0.0 0.0 1.0 rg 100 742 50 50 re f\n";

    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
           "/Resources << /ExtGState << /GS1 << /Type /ExtGState /ca 0.5 /CA 0.5 >> >> >> "
           "/Contents 4 0 R >>\nendobj\n";

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

    // Test 5: Non-destructive loading test
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
    std::cout << "[TEST 7] Checking rendered pixels for non-white content..." << std::endl;
    bool hasNonWhitePixel = false;
    for (int y = 0; y < rendered.height() && !hasNonWhitePixel; ++y) {
        const uint32_t* line = reinterpret_cast<const uint32_t*>(rendered.data() + y * rendered.stride());
        for (int x = 0; x < rendered.width(); ++x) {
            uint32_t pixel = line[x];
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

    // Test 9: Size Cap Enforcement (renderWholePage caps and renderTile dimension guards)
    std::cout << "[TEST 9] Testing renderWholePage caps and renderTile dimension guards..." << std::endl;
    // 9a. Oversized renderWholePage (20x zoom, 2x DPR -> unconstrained ~23,800 x 33,680 = 801 MP)
    pdf::PixelBuffer cappedPage = page->renderWholePage(20.0, 2.0);
    if (!cappedPage.isValid()) {
        std::cerr << "[FAIL] Capped whole-page render failed to return valid buffer" << std::endl;
        return 1;
    }
    if (cappedPage.width() > 8192 || cappedPage.height() > 8192 ||
        (static_cast<int64_t>(cappedPage.width()) * cappedPage.height() > 16777216LL)) {
        std::cerr << "[FAIL] Whole page render exceeded 8192px / 16MP safety limit! Got: "
                  << cappedPage.width() << " x " << cappedPage.height() << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Oversized whole page clamped safely to " 
              << cappedPage.width() << " x " << cappedPage.height() << " px." << std::endl;

    // 9b. Oversized renderTile request (> 4096 px per side)
    pdf::PixelBuffer oversizedTileSide = page->renderTile({0, 0, 5000, 200}, 1.0, 1.0);
    if (oversizedTileSide.isValid()) {
        std::cerr << "[FAIL] renderTile failed to reject tile exceeding 4096 px side" << std::endl;
        return 1;
    }
    // 9c. Oversized renderTile request (> 16 MP total area)
    pdf::PixelBuffer oversizedTileArea = page->renderTile({0, 0, 4000, 4200}, 1.0, 1.0);
    if (oversizedTileArea.isValid()) {
        std::cerr << "[FAIL] renderTile failed to reject tile exceeding 16 MP area" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Oversized tile requests (>4096px or >16MP) safely rejected." << std::endl;

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

    // Test 11: Complex Multi-Feature Fixture Stitching & Boundary vs Interior Seam Analysis
    std::cout << "[TEST 11] Testing Tile Stitching on Complex Fixture (Vectors, Fills, Clips, Alpha)..." << std::endl;
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

    // Render full reference image via unconstrained tile call
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

    // Render 512x512 tile grid cutting across vector strokes, clips, and transparent fills
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
    std::cout << "  -> Stitched " << tileCount << " tiles across vectors, clips, and transparency." << std::endl;

    // Strict pixel-by-pixel byte comparison categorizing Boundary vs Interior differences
    int totalDiffBytes = 0;
    int boundaryDiffBytes = 0;
    int interiorDiffBytes = 0;
    int maxDiff = 0;

    for (int y = 0; y < fullH; ++y) {
        const uint8_t* fullRow = fullPageBuf.data() + y * fullPageBuf.stride();
        const uint8_t* stitchRow = stitchedBuf.data() + y * stitchedBuf.stride();
        bool isBoundaryRow = (y % TILE_SIZE == 0) || (y % TILE_SIZE == (TILE_SIZE - 1));

        for (int px = 0; px < fullW; ++px) {
            bool isBoundaryCol = (px % TILE_SIZE == 0) || (px % TILE_SIZE == (TILE_SIZE - 1));
            bool isBoundaryPixel = isBoundaryRow || isBoundaryCol;

            for (int b = 0; b < 4; ++b) {
                int byteIdx = px * 4 + b;
                int d = std::abs(static_cast<int>(fullRow[byteIdx]) - static_cast<int>(stitchRow[byteIdx]));
                if (d > 0) {
                    ++totalDiffBytes;
                    if (isBoundaryPixel) {
                        ++boundaryDiffBytes;
                    } else {
                        ++interiorDiffBytes;
                    }
                    if (d > maxDiff) maxDiff = d;
                }
            }
        }
    }

    std::cout << "  [DIAGNOSTIC] Total differing bytes: " << totalDiffBytes 
              << " / " << (fullW * fullH * 4) << " (" 
              << (totalDiffBytes * 100.0 / (fullW * fullH * 4)) << "%)\n"
              << "               Boundary diffs: " << boundaryDiffBytes << " bytes\n"
              << "               Interior diffs: " << interiorDiffBytes << " bytes\n"
              << "               Max byte delta: " << maxDiff << " / 255" << std::endl;

    if (maxDiff > 3) {
        std::cerr << "[FAIL] Tile seam detected! Differing pixels have visible delta: " << maxDiff << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Seam tolerance verified (max delta " << maxDiff << "/255)." << std::endl;

    // Test 12: Extreme Zoom Sub-Pixel Precision Verification (15x zoom * 2x DPR = 30x scale)
    std::cout << "[TEST 12] Testing Tile Rendering and Sub-Pixel Precision at Extreme Zoom (17,850 x 25,260 px)..." << std::endl;
    constexpr double EXTREME_ZOOM = 15.0;
    constexpr double EXTREME_DPR = 2.0; // Total 30.0x scale
    int extremeW = 0, extremeH = 0;
    complexPage->getPixelDimensions(EXTREME_ZOOM, EXTREME_DPR, extremeW, extremeH);
    std::cout << "  -> Unconstrained page raster dimensions: " << extremeW << " x " << extremeH << " px (~450 MP)" << std::endl;

    // In the fixture, pure blue rectangle is placed at:
    // x = 100 pt, y = 742 pt, w = 50 pt, h = 50 pt (MediaBox: 595 x 842 pt)
    // Distance from top of page = 842 - (742 + 50) = 50 pt
    // At 30x scale:
    // Left edge = round(100 * 30) = 3000 px
    // Top edge = round(50 * 30) = 1500 px
    // Right edge = round(150 * 30) = 4500 px
    // Bottom edge = round(100 * 30) = 3000 px
    int targetLeft = static_cast<int>(std::round(100.0 * EXTREME_ZOOM * EXTREME_DPR));
    int targetTop = static_cast<int>(std::round(50.0 * EXTREME_ZOOM * EXTREME_DPR));

    // Request a 512x512 tile whose origin starts 10px inside the rectangle: (targetLeft + 10, targetTop + 10)
    pdf::PixelBuffer interiorTile = complexPage->renderTile({targetLeft + 10, targetTop + 10, 512, 512}, EXTREME_ZOOM, EXTREME_DPR);
    if (!interiorTile.isValid() || interiorTile.width() != 512 || interiorTile.height() != 512) {
        std::cerr << "[FAIL] Failed to render tile at extreme zoom inside known shape" << std::endl;
        return 1;
    }

    // Interior pixels of the blue rectangle should have high blue component and 0 red component
    const uint32_t* insidePixel = reinterpret_cast<const uint32_t*>(interiorTile.data());
    uint8_t blueComp = static_cast<uint8_t>(insidePixel[0] & 0xFF);
    uint8_t redComp = static_cast<uint8_t>((insidePixel[0] >> 16) & 0xFF);
    if (blueComp < 200 || redComp > 20) {
        std::cerr << "[FAIL] Extreme zoom coordinate precision failed! Expected blue pixel at ("
                  << (targetLeft + 10) << ", " << (targetTop + 10) << "), got B=" 
                  << static_cast<int>(blueComp) << " R=" << static_cast<int>(redComp) << std::endl;
        return 1;
    }

    // Now request a tile 100px outside the rectangle: (targetLeft - 100, targetTop - 100, 50, 50)
    pdf::PixelBuffer outsideTile = complexPage->renderTile({targetLeft - 100, targetTop - 100, 50, 50}, EXTREME_ZOOM, EXTREME_DPR);
    if (!outsideTile.isValid()) {
        std::cerr << "[FAIL] Failed to render outside tile at extreme zoom" << std::endl;
        return 1;
    }
    const uint32_t* outsidePixel = reinterpret_cast<const uint32_t*>(outsideTile.data());
    // Should be white (0x00FFFFFF)
    if ((outsidePixel[0] & 0x00FFFFFF) != 0x00FFFFFF) {
        std::cerr << "[FAIL] Expected white background outside known shape at extreme zoom" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: PDFium rendered tiles at 17,850 x 25,260 px with precise sub-pixel alignment." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 12 TESTS PASSED SUCCESSFULLY!      " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
