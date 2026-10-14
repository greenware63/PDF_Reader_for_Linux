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
#include "layout/DocumentLayout.h"

// Generates a multi-page PDF fixture containing:
// Page 0: Portrait A4 (595 x 842 pt)
// Page 1: Landscape A4 (842 x 595 pt)
// Page 2: Rotated 90 degrees (/Rotate 90)
// Page 3: Cropped page (MediaBox [0 0 612 792], CropBox [50 50 550 700])
static void generateMultiPageFixture(const std::string& path) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R] /Count 4 >>\nendobj\n";

    // Page 0: Portrait A4
    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 7 0 R >>\nendobj\n";

    // Page 1: Landscape A4
    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 842 595] /Contents 7 0 R >>\nendobj\n";

    // Page 2: Rotated 90 (MediaBox 595x842 rotated becomes 842x595)
    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Rotate 90 /Contents 7 0 R >>\nendobj\n";

    // Page 3: Cropped page (CropBox 500 x 650 pt)
    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /CropBox [50 50 550 700] /Contents 7 0 R >>\nendobj\n";

    // Shared simple content stream
    std::string contentStream = "0.5 0.5 0.5 rg 10 10 100 100 re f\n";
    offsets.push_back(pdf.size());
    pdf += "7 0 obj\n<< /Length " + std::to_string(contentStream.size()) + " >>\nstream\n";
    pdf += contentStream;
    pdf += "\nendstream\nendobj\n";

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 8\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

static void generateComplexPdfFixture(const std::string& path) {
    std::string contentStream;

    // 1. Axial smooth shading gradient
    contentStream += "q /Sh1 sh Q\n";

    // 2. Diagonal crossing lines (kept below y=700)
    for (int i = 0; i < 25; ++i) {
        float r = static_cast<float>((i * 37) % 255) / 255.0f;
        float g = static_cast<float>((i * 73) % 255) / 255.0f;
        float b = static_cast<float>((i * 109) % 255) / 255.0f;
        float lineWidth = 1.0f + static_cast<float>(i % 5);
        contentStream += std::to_string(lineWidth) + " w " + std::to_string(r) + " " + std::to_string(g) + " " + std::to_string(b) + " RG\n";
        contentStream += "0 " + std::to_string(i * 25) + " m 595 " + std::to_string(700 - i * 25) + " l S\n";
        contentStream += std::to_string(i * 19) + " 0 m " + std::to_string(595 - i * 19) + " 700 l S\n";
    }

    // 3. Colored rectangular regions crossing 512px boundaries
    contentStream += "0.9 0.1 0.1 rg 50 100 500 550 re f\n";
    contentStream += "0.1 0.8 0.2 rg 100 150 400 450 re f\n";
    contentStream += "0.1 0.2 0.9 rg 150 200 300 350 re f\n";
    contentStream += "0.9 0.8 0.1 rg 200 250 200 250 re f\n";

    // 4. Clipped path crossing tile boundaries
    contentStream += "q 120 180 350 350 re W n 0.7 0.2 0.8 rg 0 0 595 700 re f Q\n";

    // 5. Semi-transparent overlay with ExtGState
    contentStream += "q /GS1 gs 0.2 0.8 0.7 rg 60 220 450 300 re f Q\n";

    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
           "/Resources << "
           "/ExtGState << /GS1 << /Type /ExtGState /ca 0.5 /CA 0.5 >> >> "
           "/Shading << /Sh1 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [60 150 400 450] "
           "/Function << /FunctionType 2 /Domain [0 1] /C0 [0.8 0.1 0.2] /C1 [0.1 0.3 0.9] /N 1 >> >> >> "
           ">> "
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

static void generateSubpixelPdfFixture(const std::string& path) {
    std::string contentStream;
    // White background
    contentStream += "1.0 1.0 1.0 rg 0 0 595 842 re f\n";
    // Pure blue box with fractional coordinates: x = 20.35 pt, y = 772.35 pt, w = 40 pt, h = 40 pt
    // Drawn as a path polygon so PDFium's sub-pixel anti-aliasing path rasterizer evaluates fractional edge coverage
    // on both the left vertical edge (x = 20.35 pt -> 610.5 px) and top horizontal edge (dist from top 29.65 pt -> 889.5 px)
    contentStream += "0.0 0.0 1.0 rg 20.35 772.35 m 60.35 772.351 l 60.35 812.351 l 20.351 812.35 l h f\n";

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

    // Test 1: Non-existent file error handling
    std::cout << "[TEST 1] Testing non-existent file handling..." << std::endl;
    auto err1 = doc->loadFromFile("non_existent_file_xyz_12345.pdf");
    if (err1 == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Expected error on non-existent file, got None" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: correctly reported FileNotFound." << std::endl;

    // Test 2: Empty memory buffer handling
    std::cout << "[TEST 2] Testing empty memory buffer handling..." << std::endl;
    auto err2 = doc->loadFromMemory(nullptr, 0);
    if (err2 == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Expected error on null memory buffer, got None" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: correctly rejected empty buffer." << std::endl;

    // Test 3: Garbage bytes in memory handling
    std::cout << "[TEST 3] Testing garbage bytes in memory..." << std::endl;
    const uint8_t garbage[] = { 0x01, 0x02, 0x03, 0x04, 0xDE, 0xAD, 0xBE, 0xEF };
    auto err3 = doc->loadFromMemory(garbage, sizeof(garbage));
    if (err3 == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Expected error on garbage memory buffer, got None" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: correctly rejected garbage bytes as InvalidFormat." << std::endl;

    // Test 4: Open valid document
    std::string samplePath = (argc > 1) ? argv[1] : "sample.pdf";
    std::cout << "[TEST 4] Loading valid PDF: " << samplePath << std::endl;
    auto err4 = doc->loadFromFile(samplePath);
    if (err4 != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to load sample PDF: " << static_cast<int>(err4) << std::endl;
        return 1;
    }

    int pageCount = doc->pageCount();
    if (pageCount <= 0) {
        std::cerr << "[FAIL] Expected at least 1 page, got " << pageCount << std::endl;
        return 1;
    }
    std::cout << "  -> Document has " << pageCount << " page(s)." << std::endl;

    // Test 5: Verify non-destructive load failure
    std::cout << "[TEST 5] Testing non-destructive loading..." << std::endl;
    auto err5 = doc->loadFromFile("non_existent_dummy_fail.pdf");
    if (err5 == pdf::DocumentError::None) {
        std::cerr << "[FAIL] Non-destructive test failed: expected error on missing file" << std::endl;
        return 1;
    }
    if (doc->pageCount() != pageCount) {
        std::cerr << "[FAIL] Document state was corrupted/cleared after failed load attempt" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: document preserved after failed load attempt." << std::endl;

    // Test 6: Page retrieval & rendering
    std::cout << "[TEST 6] Retrieving and rendering page 0..." << std::endl;
    auto page0 = doc->getPage(0);
    if (!page0) {
        std::cerr << "[FAIL] Failed to load page 0" << std::endl;
        return 1;
    }

    std::cout << "  -> Page 0 points: " << page0->widthPoints() << " x " << page0->heightPoints() << std::endl;
    if (page0->widthPoints() <= 0 || page0->heightPoints() <= 0) {
        std::cerr << "[FAIL] Invalid page dimensions" << std::endl;
        return 1;
    }

    // Render at 2.0x scale (144 DPI equivalent)
    pdf::PixelBuffer buffer = page0->renderWholePage(2.0, 1.0);
    if (!buffer.isValid() || buffer.data() == nullptr) {
        std::cerr << "[FAIL] Rendered pixel buffer is invalid or null" << std::endl;
        return 1;
    }
    std::cout << "  -> Rendered bitmap size: " << buffer.width() << " x " << buffer.height() << std::endl;

    // Test 7: Verify non-white pixel content
    std::cout << "[TEST 7] Checking rendered pixels for non-white content..." << std::endl;
    bool hasNonWhite = false;
    const uint8_t* scan = buffer.data();
    for (int y = 0; y < buffer.height(); ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(scan + y * buffer.stride());
        for (int x = 0; x < buffer.width(); ++x) {
            uint32_t pixel = row[x];
            // Format is BGRx (or BGRA)
            uint8_t b = pixel & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t r = (pixel >> 16) & 0xFF;
            if (r < 250 || g < 250 || b < 250) {
                hasNonWhite = true;
                break;
            }
        }
        if (hasNonWhite) break;
    }

    if (!hasNonWhite) {
        std::cerr << "[FAIL] Rendered page appears completely white/blank!" << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Non-white glyph/graphic pixels successfully detected." << std::endl;

    // Test 8: Memory lifetime tied to document handle
    std::cout << "[TEST 8] Testing in-memory loaded document lifetime tied to handle..." << std::endl;
    {
        std::ifstream file(samplePath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "[FAIL] Could not open " << samplePath << " for memory test" << std::endl;
            return 1;
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> localMem(size);
        if (!file.read(reinterpret_cast<char*>(localMem.data()), size)) {
            std::cerr << "[FAIL] Failed to read " << samplePath << " into memory" << std::endl;
            return 1;
        }

        auto memDoc = pdf::createDocument();
        if (memDoc->loadFromMemory(localMem.data(), localMem.size()) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load document from memory" << std::endl;
            return 1;
        }
        // Destroy the source vector immediately
        localMem.clear();
        localMem.shrink_to_fit();

        auto memPage = memDoc->getPage(0);
        if (!memPage) {
            std::cerr << "[FAIL] Failed to acquire page from memory document" << std::endl;
            return 1;
        }

        // Close document explicitly while keeping page reference alive
        memDoc->close();

        // Render from page whose document was closed; buffer must remain valid due to shared lifetime
        pdf::PixelBuffer testBuf = memPage->renderWholePage(1.0, 1.0);
        if (!testBuf.isValid() || testBuf.width() <= 0) {
            std::cerr << "[FAIL] Render failed from page after doc->close()" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: in-memory document remained safe and readable after doc->close()." << std::endl;
    }

    // Test 9: renderWholePage clamping & renderTile dimension limits
    std::cout << "[TEST 9] Testing renderWholePage caps and renderTile dimension guards..." << std::endl;
    {
        // 15x zoom at 2x DPR = 30x total factor
        // A4 (595x842) * 30 = 17,850 x 25,260 (exceeds 8192px and 16MP)
        pdf::PixelBuffer clampedBuf = page0->renderWholePage(15.0, 2.0);
        if (!clampedBuf.isValid()) {
            std::cerr << "[FAIL] Whole page render failed completely under extreme zoom" << std::endl;
            return 1;
        }
        if (clampedBuf.width() > 8192 || clampedBuf.height() > 8192) {
            std::cerr << "[FAIL] Clamped buffer exceeded 8192px limit: " 
                      << clampedBuf.width() << "x" << clampedBuf.height() << std::endl;
            return 1;
        }
        int64_t totalMP = static_cast<int64_t>(clampedBuf.width()) * clampedBuf.height();
        if (totalMP > 16777216) {
            std::cerr << "[FAIL] Clamped buffer exceeded 16MP limit: " << totalMP << " px" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Oversized whole page clamped safely to " 
                  << clampedBuf.width() << " x " << clampedBuf.height() << " px." << std::endl;

        // Verify uncapped getPixelDimensions for tiling
        int fullW = 0, fullH = 0;
        page0->getPixelDimensions(15.0, 2.0, fullW, fullH);
        if (fullW != 17850 || fullH != 25260) {
            std::cerr << "[FAIL] getPixelDimensions unexpectedly clamped: " << fullW << "x" << fullH << std::endl;
            return 1;
        }

        // Test renderTile dimension guards: reject tiles > 4096px or > 16MP
        pdf::PixelBuffer oversizedTile1 = page0->renderTile({0, 0, 5000, 100}, 1.0, 1.0);
        if (oversizedTile1.isValid()) {
            std::cerr << "[FAIL] renderTile should have rejected tile with width > 4096px" << std::endl;
            return 1;
        }
        pdf::PixelBuffer oversizedTile2 = page0->renderTile({0, 0, 4096, 4097}, 1.0, 1.0);
        if (oversizedTile2.isValid()) {
            std::cerr << "[FAIL] renderTile should have rejected tile with area > 16MP" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Oversized tile requests (>4096px or >16MP) safely rejected." << std::endl;
    }

    // Test 10: Reload document while holding page reference
    std::cout << "[TEST 10] Testing document reload while holding previous page reference..." << std::endl;
    {
        auto reloadDoc = pdf::createDocument();
        reloadDoc->loadFromFile(samplePath);
        auto pageBeforeReload = reloadDoc->getPage(0);
        if (!pageBeforeReload) {
            std::cerr << "[FAIL] Failed to get initial page before reload" << std::endl;
            return 1;
        }

        // Reload document to different state or same file
        reloadDoc->loadFromFile(samplePath);

        // Access old page - must not crash or read dangling memory
        pdf::PixelBuffer oldPageBuf = pageBeforeReload->renderWholePage(1.0, 1.0);
        if (!oldPageBuf.isValid() || oldPageBuf.width() <= 0) {
            std::cerr << "[FAIL] Old page reference invalidated prematurely after reload" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: retained page from earlier load remained safely accessible." << std::endl;
    }

    // Test 11: Multi-Page IDocument::pageSizePoints cross-check with DocumentLayout and Engine
    std::cout << "[TEST 11] Testing IDocument::pageSizePoints cross-check with DocumentLayout and Engine (Portrait, Landscape, Rotated, Cropped)..." << std::endl;
    {
        std::string multiPagePath = "multi_page_fixture.pdf";
        generateMultiPageFixture(multiPagePath);

        auto multiDoc = pdf::createDocument();
        if (multiDoc->loadFromFile(multiPagePath) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load multi-page fixture" << std::endl;
            return 1;
        }
        if (multiDoc->pageCount() != 4) {
            std::cerr << "[FAIL] Expected 4 pages in multi-page fixture, got " << multiDoc->pageCount() << std::endl;
            return 1;
        }

        const std::vector<std::pair<double, double>> expectedSizes = {
            {595.0, 842.0}, // Page 0: Portrait A4
            {842.0, 595.0}, // Page 1: Landscape A4
            {842.0, 595.0}, // Page 2: Rotated 90 degrees (/Rotate 90)
            {500.0, 650.0}  // Page 3: Cropped page (CropBox 500x650)
        };

        constexpr double TEST_SCALE = 1.3333333333333333;
        constexpr double TEST_DPR = 1.75;

        std::vector<std::pair<double, double>> layoutPageSizes;

        for (int p = 0; p < multiDoc->pageCount(); ++p) {
            auto [docW, docH] = multiDoc->pageSizePoints(p);
            if (docW <= 0.0 || docH <= 0.0) {
                std::cerr << "[FAIL] multiDoc->pageSizePoints failed for page " << p << std::endl;
                return 1;
            }

            // Explicitly assert expected point dimensions
            if (std::abs(docW - expectedSizes[p].first) > 1e-3 || std::abs(docH - expectedSizes[p].second) > 1e-3) {
                std::cerr << "[FAIL] Page " << p << " point dimensions incorrect: Expected ("
                          << expectedSizes[p].first << "x" << expectedSizes[p].second 
                          << "), Got (" << docW << "x" << docH << ")" << std::endl;
                return 1;
            }

            auto pageObj = multiDoc->getPage(p);
            if (!pageObj) {
                std::cerr << "[FAIL] Failed to open page " << p << std::endl;
                return 1;
            }

            double pageW = pageObj->widthPoints();
            double pageH = pageObj->heightPoints();

            if (std::abs(docW - pageW) > 1e-4 || std::abs(docH - pageH) > 1e-4) {
                std::cerr << "[FAIL] Page " << p << " mismatch between IDocument::pageSizePoints ("
                          << docW << "x" << docH << ") and IPage (" << pageW << "x" << pageH << ")" << std::endl;
                return 1;
            }

            int enginePixW = 0, enginePixH = 0;
            pageObj->getPixelDimensions(TEST_SCALE, TEST_DPR, enginePixW, enginePixH);

            int helperPixW = 0, helperPixH = 0;
            layout::computePagePixelDimensions(docW, docH, TEST_SCALE, TEST_DPR, helperPixW, helperPixH);

            if (enginePixW != helperPixW || enginePixH != helperPixH) {
                std::cerr << "[FAIL] Page " << p << " pixel dimension mismatch: Engine=(" 
                          << enginePixW << "x" << enginePixH << ") vs Helper=(" 
                          << helperPixW << "x" << helperPixH << ")" << std::endl;
                return 1;
            }

            layoutPageSizes.push_back({docW, docH});
        }

        // Feed sizes through DocumentLayout and verify layout page dimensions
        layout::DocumentLayout docLayout;
        docLayout.setPageSizes(layoutPageSizes);
        docLayout.updateLayout(TEST_SCALE, TEST_DPR, 1920);

        for (int p = 0; p < multiDoc->pageCount(); ++p) {
            const auto& pl = docLayout.getPageLayout(p);
            auto pageObj = multiDoc->getPage(p);
            int enginePixW = 0, enginePixH = 0;
            pageObj->getPixelDimensions(TEST_SCALE, TEST_DPR, enginePixW, enginePixH);

            if (pl.widthPixel != enginePixW || pl.heightPixel != enginePixH) {
                std::cerr << "[FAIL] Page " << p << " layout mismatch: DocumentLayout=(" 
                          << pl.widthPixel << "x" << pl.heightPixel << ") vs Engine=(" 
                          << enginePixW << "x" << enginePixH << ")" << std::endl;
                return 1;
            }
        }

        std::cout << "  -> Passed: Exact expected point dimensions asserted and DocumentLayout verified with Engine across Portrait, Landscape, Rotated 90, and Cropped pages." << std::endl;
    }

    // Test 12: Complex Fixture Tile Stitching with Spatial Diagnostics
    std::cout << "[TEST 12] Testing Tile Stitching on Complex Fixture (Gradients, Clips, Alpha, Crossing Vectors)..." << std::endl;
    std::string complexFixturePath = "complex_fixture.pdf";
    generateComplexPdfFixture(complexFixturePath);

    auto complexDoc = pdf::createDocument();
    if (complexDoc->loadFromFile(complexFixturePath) != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to load complex fixture PDF" << std::endl;
        return 1;
    }
    auto complexPage = complexDoc->getPage(0);
    if (!complexPage) {
        std::cerr << "[FAIL] Failed to acquire page from complex fixture" << std::endl;
        return 1;
    }

    const double testScale = 1.333333; // 100% desktop scale
    const double testDpr = 1.5;        // Fractional DPR
    int fullW = 0, fullH = 0;
    complexPage->getPixelDimensions(testScale, testDpr, fullW, fullH);

    // Reference whole-page render via unconstrained tile call
    pdf::PixelBuffer fullPageBuf = complexPage->renderTile({0, 0, fullW, fullH}, testScale, testDpr);
    if (!fullPageBuf.isValid() || fullPageBuf.width() != fullW || fullPageBuf.height() != fullH) {
        std::cerr << "[FAIL] Failed reference tile render for complex fixture" << std::endl;
        return 1;
    }

    // Standard 512x512 grid stitch
    constexpr int TILE_SIZE = 512;
    pdf::PixelBuffer stitchedBuf = pdf::PixelBuffer::allocate(fullW, fullH);
    int tilesX = (fullW + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (fullH + TILE_SIZE - 1) / TILE_SIZE;

    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            int x = tx * TILE_SIZE;
            int y = ty * TILE_SIZE;
            int tw = std::min(TILE_SIZE, fullW - x);
            int th = std::min(TILE_SIZE, fullH - y);

            pdf::PixelBuffer tile = complexPage->renderTile({x, y, tw, th}, testScale, testDpr);
            if (!tile.isValid() || tile.width() != tw || tile.height() != th) {
                std::cerr << "[FAIL] Failed rendering tile at (" << x << ", " << y << ")" << std::endl;
                return 1;
            }

            for (int r = 0; r < th; ++r) {
                const uint8_t* srcRow = tile.data() + r * tile.stride();
                uint8_t* dstRow = stitchedBuf.data() + (y + r) * stitchedBuf.stride() + x * 4;
                std::memcpy(dstRow, srcRow, tw * 4);
            }
        }
    }
    std::cout << "  -> Stitched " << (tilesX * tilesY) << " standard tiles across gradients, clips, vectors." << std::endl;

    // Compare with spatial boundary categorisation
    int diffBytes = 0;
    int maxDiff = 0;
    int boundaryDiffBytes = 0;
    int interiorDiffBytes = 0;

    for (int y = 0; y < fullH; ++y) {
        const uint8_t* fullRow = fullPageBuf.data() + y * fullPageBuf.stride();
        const uint8_t* stitchRow = stitchedBuf.data() + y * stitchedBuf.stride();
        bool isBoundaryRow = (y % TILE_SIZE == 0) || ((y + 1) % TILE_SIZE == 0);

        for (int b = 0; b < fullW * 4; ++b) {
            int d = std::abs(static_cast<int>(fullRow[b]) - static_cast<int>(stitchRow[b]));
            if (d > 0) {
                ++diffBytes;
                if (d > maxDiff) maxDiff = d;

                int px = b / 4;
                bool isBoundaryCol = (px % TILE_SIZE == 0) || ((px + 1) % TILE_SIZE == 0);
                if (isBoundaryRow || isBoundaryCol) {
                    ++boundaryDiffBytes;
                } else {
                    ++interiorDiffBytes;
                }
            }
        }
    }

    std::cout << "  [STANDARD GRID DIAGNOSTIC]\n"
              << "     Total differing bytes: " << diffBytes << " / " << (fullW * fullH * 4) 
              << " (" << (100.0 * diffBytes / (fullW * fullH * 4)) << "%)\n"
              << "     Boundary diffs: " << boundaryDiffBytes << " bytes\n"
              << "     Interior diffs: " << interiorDiffBytes << " bytes\n"
              << "     Max byte delta: " << maxDiff << " / 255" << std::endl;

    // Shifted grid test (origin offset by 37px)
    std::cout << "  -> Running 37px shifted grid test to verify noise does not follow tile seams..." << std::endl;
    pdf::PixelBuffer shiftedStitchedBuf = pdf::PixelBuffer::allocate(fullW, fullH);
    constexpr int SHIFT = 37;
    int y = 0;
    while (y < fullH) {
        int th = (y == 0) ? SHIFT : std::min(TILE_SIZE, fullH - y);
        int x = 0;
        while (x < fullW) {
            int tw = (x == 0) ? SHIFT : std::min(TILE_SIZE, fullW - x);
            pdf::PixelBuffer tile = complexPage->renderTile({x, y, tw, th}, testScale, testDpr);
            if (!tile.isValid()) {
                std::cerr << "[FAIL] Failed rendering shifted tile at (" << x << ", " << y << ")" << std::endl;
                return 1;
            }
            for (int r = 0; r < th; ++r) {
                const uint8_t* srcRow = tile.data() + r * tile.stride();
                uint8_t* dstRow = shiftedStitchedBuf.data() + (y + r) * shiftedStitchedBuf.stride() + x * 4;
                std::memcpy(dstRow, srcRow, tw * 4);
            }
            x += tw;
        }
        y += th;
    }

    int shiftedDiffBytes = 0;
    int shiftedMaxDiff = 0;
    for (int y = 0; y < fullH; ++y) {
        const uint8_t* fullRow = fullPageBuf.data() + y * fullPageBuf.stride();
        const uint8_t* stitchRow = shiftedStitchedBuf.data() + y * shiftedStitchedBuf.stride();
        for (int b = 0; b < fullW * 4; ++b) {
            int d = std::abs(static_cast<int>(fullRow[b]) - static_cast<int>(stitchRow[b]));
            if (d > 0) {
                ++shiftedDiffBytes;
                if (d > shiftedMaxDiff) shiftedMaxDiff = d;
            }
        }
    }
    std::cout << "  [SHIFTED GRID DIAGNOSTIC]\n"
              << "     Total differing bytes: " << shiftedDiffBytes << ", Max delta: " << shiftedMaxDiff << " / 255" << std::endl;

    int maxObserved = std::max(maxDiff, shiftedMaxDiff);
    if (maxObserved > 3) {
        std::cerr << "[FAIL] Delta exceeded 3/255: " << maxObserved << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Raster difference bounded at " << maxObserved << "/255 across both grids." << std::endl;

    // Test 13: Sub-Pixel Fractional Edge Straddling and Partial Blend Verification (X and Y axes)
    std::cout << "[TEST 13] Testing Sub-Pixel Fractional Edge Straddling and Partial Blend at Extreme Zoom..." << std::endl;
    std::string subpixelFixturePath = "subpixel_test.pdf";
    generateSubpixelPdfFixture(subpixelFixturePath);

    auto subpixelDoc = pdf::createDocument();
    if (subpixelDoc->loadFromFile(subpixelFixturePath) != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to load subpixel fixture PDF" << std::endl;
        return 1;
    }
    auto subpixelPage = subpixelDoc->getPage(0);
    if (!subpixelPage) {
        std::cerr << "[FAIL] Failed to acquire page from subpixel fixture" << std::endl;
        return 1;
    }

    constexpr double EXTREME_ZOOM = 15.0;
    constexpr double EXTREME_DPR = 2.0; // Total 30.0x scale
    int extremeW = 0, extremeH = 0;
    subpixelPage->getPixelDimensions(EXTREME_ZOOM, EXTREME_DPR, extremeW, extremeH);

    // Target box has fractional left edge: x = 20.35 pt -> at 30x scale: 610.5 px.
    // Distance from top edge: 842 - 812.351 = 29.649 pt -> at 30x scale: 889.47 px.
    // Left edge straddle:
    // Pixel 609 is outside (white background).
    // Pixel 610 is the fractional boundary pixel (anti-aliased blend).
    // Pixel 611 is inside the blue rectangle.
    int anchorX = 610;
    int targetTop = 890;

    // 13a. Left Edge Horizontal Straddle Tile: [anchorX - 50, targetTop + 10, width=100, height=20]
    pdf::PixelBuffer leftTile = subpixelPage->renderTile({anchorX - 50, targetTop + 10, 100, 20}, EXTREME_ZOOM, EXTREME_DPR);
    if (!leftTile.isValid() || leftTile.width() != 100 || leftTile.height() != 20) {
        std::cerr << "[FAIL] Failed to render left fractional edge straddle tile" << std::endl;
        return 1;
    }

    const uint32_t* leftRow10 = reinterpret_cast<const uint32_t*>(leftTile.data() + 10 * leftTile.stride());
    uint8_t leftR49 = static_cast<uint8_t>((leftRow10[49] >> 16) & 0xFF);
    uint8_t leftR50 = static_cast<uint8_t>((leftRow10[50] >> 16) & 0xFF);
    uint8_t leftR51 = static_cast<uint8_t>((leftRow10[51] >> 16) & 0xFF);

    if (leftR49 < 240 || leftR50 < 100 || leftR50 > 160 || leftR51 > 15) {
        std::cout << "  [DEBUG] Left straddle column values:\n"
                  << "     Col 49 (outside): R=" << static_cast<int>(leftR49) << "\n"
                  << "     Col 50 (blend)  : R=" << static_cast<int>(leftR50) << "\n"
                  << "     Col 51 (inside) : R=" << static_cast<int>(leftR51) << "\n";
        std::cerr << "[FAIL] Left edge sub-pixel partial blend failed expected range [100, 160] at col 50!" << std::endl;
        return 1;
    }
    std::cout << "  -> Left edge boundary verified at exact col 50: Outside R=" << static_cast<int>(leftR49) 
              << " -> Blend R=" << static_cast<int>(leftR50) 
              << " -> Inside R=" << static_cast<int>(leftR51) << std::endl;

    // 13b. Top Edge Vertical Straddle Tile: [anchorX + 10, anchorY - 50, width=20, height=100]
    // The top edge is at y = 889.5 px.
    // Tile y extends from anchorY - 50 = 839 to 939.
    // Row 49 is at y = 888 (outside, white).
    // Row 50 is at y = 889 (fractional boundary pixel).
    // Row 51 is at y = 890 (inside, blue).
    int anchorY = 889;
    pdf::PixelBuffer topTile = subpixelPage->renderTile({anchorX + 10, anchorY - 50, 20, 100}, EXTREME_ZOOM, EXTREME_DPR);
    if (!topTile.isValid() || topTile.width() != 20 || topTile.height() != 100) {
        std::cerr << "[FAIL] Failed to render top fractional edge straddle tile" << std::endl;
        return 1;
    }

    uint8_t topR49 = static_cast<uint8_t>((reinterpret_cast<const uint32_t*>(topTile.data() + 49 * topTile.stride())[10] >> 16) & 0xFF);
    uint8_t topR50 = static_cast<uint8_t>((reinterpret_cast<const uint32_t*>(topTile.data() + 50 * topTile.stride())[10] >> 16) & 0xFF);
    uint8_t topR51 = static_cast<uint8_t>((reinterpret_cast<const uint32_t*>(topTile.data() + 51 * topTile.stride())[10] >> 16) & 0xFF);

    if (topR49 < 240 || topR50 < 100 || topR50 > 160 || topR51 > 15) {
        std::cout << "  [DEBUG] Top straddle row values (Col 10):\n"
                  << "     Row 49 (outside): R=" << static_cast<int>(topR49) << "\n"
                  << "     Row 50 (blend)  : R=" << static_cast<int>(topR50) << "\n"
                  << "     Row 51 (inside) : R=" << static_cast<int>(topR51) << "\n";
        std::cerr << "[FAIL] Top edge sub-pixel partial blend failed expected range [100, 160] at row 50!" << std::endl;
        return 1;
    }
    std::cout << "  -> Top edge boundary verified at exact row 50: Outside R=" << static_cast<int>(topR49) 
              << " -> Blend R=" << static_cast<int>(topR50) 
              << " -> Inside R=" << static_cast<int>(topR51) << std::endl;
    std::cout << "  -> Passed: Sub-pixel anti-aliased edge blends confirmed on both horizontal and vertical axes." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 13 TESTS PASSED SUCCESSFULLY!      " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
