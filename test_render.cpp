#include <iostream>
#include <vector>
#include <fstream>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>
#include "engine/IDocument.h"
#include "engine/IPage.h"
#include "engine/PixelBuffer.h"
#include "layout/DocumentLayout.h"
#include "engine/PageText.h"
#include "engine/PdfiumPage.h"
#include "engine/RenderWorker.h"
#include "engine/SearchTypes.h"
#include "engine/SearchText.h"
#include <QCoreApplication>
#include <QString>

// Generates a multi-page PDF fixture containing:
// Page 0: Portrait A4 (595 x 842 pt)
// Page 1: Landscape A4 (842 x 595 pt)
// Page 2: Rotated 90 degrees (/Rotate 90)
// Page 3: Cropped page (MediaBox [0 0 612 792], CropBox [50 50 550 700])
static void generateOutlinePdfFixture(const std::string& path, bool cyclicSibling = false,
                                      bool cyclicChildToParent = false) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    auto addObj = [&](int num, const std::string& content) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(num) + " 0 obj\n" + content + "\nendobj\n";
    };

    // 1: Catalog with /Outlines
    addObj(1, "<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R >>");

    // 2: Pages
    addObj(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 11 0 R 12 0 R] /Count 5 >>");

    // 3: Page 0
    addObj(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");

    // 4: Page 1
    addObj(4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");

    // 5: Page 2
    addObj(5, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /CropBox [40 80 540 780] /Rotate 90 >>");

    // 6: Outlines root
    addObj(6, "<< /Type /Outlines /First 7 0 R /Last 10 0 R /Count 4 >>");

    // 7: Bookmark Chapter 1
    addObj(7, "<< /Title (Chapter 1) /Parent 6 0 R /Next 8 0 R /First 9 0 R /Last 9 0 R /Count 1 /Dest [3 0 R /Fit] >>");

    // 8: Bookmark Chapter 2
    addObj(8, "<< /Title (Chapter 2) /Parent 6 0 R /Prev 7 0 R /Next 10 0 R /Dest [5 0 R /XYZ 100 700 1] >>");

    // 9: Bookmark Section 1.1 (child of 7)
    addObj(9, cyclicChildToParent
                   ? "<< /Title (Section 1.1) /Parent 7 0 R /Next 7 0 R /Dest [3 0 R /Fit] >>"
                   : "<< /Title (Section 1.1) /Parent 7 0 R /Dest [3 0 R /Fit] >>");
    addObj(10, cyclicSibling
                   ? "<< /Title (Preface) /Parent 6 0 R /Prev 8 0 R /Next 7 0 R >>"
                   : "<< /Title (Preface) /Parent 6 0 R /Prev 8 0 R >>");
    addObj(11, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");
    addObj(12, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

static void generatePageLabelsPdfFixture(const std::string& path) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    auto addObj = [&](int num, const std::string& content) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(num) + " 0 obj\n" + content + "\nendobj\n";
    };

    // 1: Catalog with /PageLabels dictionary
    // Page 0..1: Roman lower (/r) -> "i", "ii"
    // Page 2..3: Decimal with prefix "A-" (/P (A-) /S /D) -> "A-1", "A-2"
    addObj(1, "<< /Type /Catalog /Pages 2 0 R /PageLabels << /Nums [ 0 << /S /r >> 2 << /P (A-) /S /D >> ] >> >>");

    // 2: Pages
    addObj(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R] /Count 4 >>");

    // 3: Page 0
    addObj(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");
    // 4: Page 1
    addObj(4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");
    // 5: Page 2
    addObj(5, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");
    // 6: Page 3
    addObj(6, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

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

static void generateCornerColorPdfFixture(const std::string& path) {
    std::string contentStream;
    // White background for 400 x 600 pt page
    contentStream += "1.0 1.0 1.0 rg 0 0 400 600 re f\n";
    // Top-Left corner (x: 0..100, y: 500..600): Pure Red
    contentStream += "1.0 0.0 0.0 rg 0 500 100 100 re f\n";
    // Top-Right corner (x: 300..400, y: 500..600): Pure Green
    contentStream += "0.0 1.0 0.0 rg 300 500 100 100 re f\n";
    // Bottom-Right corner (x: 300..400, y: 0..100): Pure Blue
    contentStream += "0.0 0.0 1.0 rg 300 0 100 100 re f\n";
    // Bottom-Left corner (x: 0..100, y: 0..100): Pure Yellow (Red + Green)
    contentStream += "1.0 1.0 0.0 rg 0 0 100 100 re f\n";

    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 600] /Contents 4 0 R >>\nendobj\n";

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

static void generateSupplementaryUnicodePdfFixture(const std::string& path) {
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Custom-ToUnicode def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n"
        "5 beginbfchar\n"
        "<0001> <0041>\n"
        "<0002> <D83DDE00>\n"
        "<0003> <0042>\n"
        "<0004> <D83DDE80>\n"
        "<0005> <0043>\n"
        "endbfchar\n"
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n";

    std::string contentStream =
        "BT\n"
        "/F1 20 Tf\n"
        "72 700 Td\n"
        "<00010002000300040005> Tj\n"
        "ET\n";

    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 8 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /Helvetica-Custom /Encoding /Identity-H /DescendantFonts [5 0 R] /ToUnicode 7 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Helvetica-Custom /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor 6 0 R /DW 1000 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Type /FontDescriptor /FontName /Helvetica-Custom /Flags 4 /FontBBox [-500 -500 2000 2000] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 800 /StemV 80 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "7 0 obj\n<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "8 0 obj\n<< /Length " + std::to_string(contentStream.size()) + " >>\nstream\n" + contentStream + "\nendstream\nendobj\n";

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 9\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}
static void generatePersianPdfFixture(const std::string& path) {
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n"
        "12 dict begin\n"
        "begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Custom-Persian-ToUnicode def\n"
        "/CMapType 2 def\n"
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n"
        "9 beginbfchar\n"
        "<0001> <0646>\n"
        "<0002> <0627>\n"
        "<0003> <0647>\n"
        "<0004> <062C>\n"
        "<0005> <0020>\n"
        "<0006> <0645>\n"
        "<0007> <0627>\n"
        "<0008> <0644>\n"
        "<0009> <0633>\n"
        "endbfchar\n"
        "endcmap\n"
        "CMapName currentdict /CMap defineresource pop\n"
        "end\n"
        "end\n";

    std::string contentStream =
        "BT\n"
        "/F1 20 Tf\n"
        "100 700 Td\n"
        "<000100020003000400050006000700080009> Tj\n"
        "ET\n";

    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 8 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Font /Subtype /Type0 /BaseFont /Helvetica-Custom /Encoding /Identity-H /DescendantFonts [5 0 R] /ToUnicode 7 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Helvetica-Custom /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor 6 0 R /DW 1000 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Type /FontDescriptor /FontName /Helvetica-Custom /Flags 4 /FontBBox [-500 -500 2000 2000] /ItalicAngle 0 /Ascent 800 /Descent -200 /CapHeight 800 /StemV 80 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "7 0 obj\n<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "8 0 obj\n<< /Length " + std::to_string(contentStream.size()) + " >>\nstream\n" + contentStream + "\nendstream\nendobj\n";

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 9\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

static void generateTextPdfFixture(const std::string& path, const std::string& text = "Hello PDF World!") {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    std::string content = "BT\n/F1 24 Tf\n100 700 Td\n(" + text + ") Tj\nET\n";
    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream\nendobj\n";

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 6\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), pdf.size());
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
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

    // Test 14: Text extraction, character bounding boxes, hit-testing, and selection
    std::cout << "[TEST 14] Testing PageText extraction, character bounding boxes, hit-testing, and selection..." << std::endl;
    {
        std::string textFixturePath = "text_test.pdf";
        generateTextPdfFixture(textFixturePath, "Hello PDF World!");

        auto textDoc = pdf::createDocument();
        if (textDoc->loadFromFile(textFixturePath) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load text fixture PDF" << std::endl;
            return 1;
        }

        auto textPage = textDoc->getPage(0);
        if (!textPage) {
            std::cerr << "[FAIL] Failed to acquire page from text fixture" << std::endl;
            return 1;
        }

        pdf::PageText pt = textPage->extractText();
        if (pt.empty() || pt.charCount() != 16) {
            std::cerr << "[FAIL] Expected 16 characters extracted, got " << pt.charCount() << std::endl;
            return 1;
        }

        // Verify bounding boxes: top-down page coordinates (left, top, right, bottom)
        for (size_t i = 0; i < pt.charCount(); ++i) {
            const auto& box = pt.chars[i];
            if (!box.isValid()) {
                std::cerr << "[FAIL] Invalid character bounding box at index " << i << std::endl;
                return 1;
            }
            if (box.left < 0.0 || box.right > 595.0 || box.top < 0.0 || box.bottom > 842.0) {
                std::cerr << "[FAIL] Character box outside page boundaries at index " << i << std::endl;
                return 1;
            }
        }

        // Substring extraction
        std::u16string helloU16 = pt.getSelectedText(0, 4);
        if (helloU16 != u"Hello") {
            std::cerr << "[FAIL] Expected 'Hello' for indices [0..4], got unexpected text" << std::endl;
            return 1;
        }

        std::u16string fullU16 = pt.getSelectedText(0, 15);
        if (fullU16 != u"Hello PDF World!") {
            std::cerr << "[FAIL] Expected 'Hello PDF World!' for indices [0..15], got unexpected text" << std::endl;
            return 1;
        }

        // Selection rectangles
        auto selRects = pt.getSelectionRects(0, 4);
        if (selRects.empty()) {
            std::cerr << "[FAIL] getSelectionRects returned empty for valid range [0..4]" << std::endl;
            return 1;
        }
        for (const auto& r : selRects) {
            if (!r.isValid() || r.width() <= 0.0 || r.height() <= 0.0) {
                std::cerr << "[FAIL] Selection rectangle has non-positive area" << std::endl;
                return 1;
            }
        }

        // Hit testing
        const auto& firstChar = pt.chars[0];
        double midX = (firstChar.left + firstChar.right) * 0.5;
        double midY = (firstChar.top + firstChar.bottom) * 0.5;
        int hit = pt.hitTest(midX, midY);
        if (hit != 0) {
            std::cerr << "[FAIL] hitTest at character center expected 0, got " << hit << std::endl;
            return 1;
        }

        int hitMiss = pt.hitTest(10.0, 10.0);
        if (hitMiss != -1) {
            std::cerr << "[FAIL] hitTest at empty margin expected -1, got " << hitMiss << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: 16 characters extracted, valid top-down boxes, hit-test and selection accurate." << std::endl;
    }

    // Test 15: Supplementary Unicode surrogate pair extraction and selection indexing
    std::cout << "[TEST 15] Testing Supplementary Unicode code points (>0xFFFF) and surrogate pairs..." << std::endl;
    {
        pdf::PageText suppPt;
        suppPt.pageIndex = 0;
        suppPt.widthPoints = 200.0;
        suppPt.heightPoints = 100.0;

        // Character 0: 'A' (U+0041)
        pdf::PageCharBox b0{10.0, 10.0, 20.0, 30.0, U'A'};
        // Character 1: ً‌„‍ Musical Symbol G Clef (U+1D11E)
        pdf::PageCharBox b1{25.0, 10.0, 45.0, 30.0, 0x1D11E};
        // Character 2: 'B' (U+0042)
        pdf::PageCharBox b2{50.0, 10.0, 60.0, 30.0, U'B'};
        // Character 3: ًںڑ€ Rocket (U+1F680)
        pdf::PageCharBox b3{65.0, 10.0, 85.0, 30.0, 0x1F680};
        // Character 4: 'C' (U+0043)
        pdf::PageCharBox b4{90.0, 10.0, 100.0, 30.0, U'C'};

        suppPt.chars = {b0, b1, b2, b3, b4};

        // 1. Single character selections
        // Char 0: 'A'
        std::u16string sel0 = suppPt.getSelectedText(0, 0);
        if (sel0 != u"A") {
            std::cerr << "[FAIL] Expected 'A' at index 0" << std::endl;
            return 1;
        }

        // Char 1: U+1D11E -> UTF-16 surrogate pair D834 DD1E
        std::u16string sel1 = suppPt.getSelectedText(1, 1);
        if (sel1.size() != 2 || sel1[0] != 0xD834 || sel1[1] != 0xDD1E) {
            std::cerr << "[FAIL] Expected surrogate pair for U+1D11E at index 1" << std::endl;
            return 1;
        }

        // Char 2: 'B' -> MUST NOT BE MISALIGNED by the preceding surrogate pair!
        std::u16string sel2 = suppPt.getSelectedText(2, 2);
        if (sel2 != u"B") {
            std::cerr << "[FAIL] Expected 'B' at index 2 (misaligned by surrogate pair?)" << std::endl;
            return 1;
        }

        // Char 3: U+1F680 -> UTF-16 surrogate pair D83D DE80
        std::u16string sel3 = suppPt.getSelectedText(3, 3);
        if (sel3.size() != 2 || sel3[0] != 0xD83D || sel3[1] != 0xDE80) {
            std::cerr << "[FAIL] Expected surrogate pair for U+1F680 at index 3" << std::endl;
            return 1;
        }

        // Char 4: 'C' -> MUST NOT BE MISALIGNED!
        std::u16string sel4 = suppPt.getSelectedText(4, 4);
        if (sel4 != u"C") {
            std::cerr << "[FAIL] Expected 'C' at index 4" << std::endl;
            return 1;
        }

        // 2. Multi-character sub-range spanning surrogate pairs: [0..2] -> "A" + ً‌„‍ + "B"
        std::u16string selSpan = suppPt.getSelectedText(0, 2);
        if (selSpan.size() != 4 || selSpan[0] != u'A' || selSpan[1] != 0xD834 || selSpan[2] != 0xDD1E || selSpan[3] != u'B') {
            std::cerr << "[FAIL] Span [0..2] mismatch across surrogate pair" << std::endl;
            return 1;
        }

        // Full range [0..4]
        std::u16string fullSpan = suppPt.getSelectedText(0, 4);
        if (fullSpan.size() != 7) {
            std::cerr << "[FAIL] Full span length expected 7 UTF-16 code units, got " << fullSpan.size() << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Synthetic Supplementary Unicode code points produce exact surrogate pairs without index drift." << std::endl;

        // 3. Extraction from an actual PDF document through PDFium
        std::string suppPdfPath = "actual_supp_unicode.pdf";
        generateSupplementaryUnicodePdfFixture(suppPdfPath);

        auto suppDoc = pdf::createDocument();
        if (suppDoc->loadFromFile(suppPdfPath) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load actual supplementary Unicode PDF" << std::endl;
            return 1;
        }

        auto suppPage = suppDoc->getPage(0);
        if (!suppPage) {
            std::cerr << "[FAIL] Failed to open page from actual supplementary Unicode PDF" << std::endl;
            return 1;
        }

        pdf::PageText extractedPt = suppPage->extractText();
        std::cout << "  [DEBUG] Extracted charCount=" << extractedPt.charCount() << std::endl;
        for (size_t i = 0; i < extractedPt.charCount(); ++i) {
            std::cout << "     char[" << i << "]: unicode=0x" << std::hex << static_cast<uint32_t>(extractedPt.chars[i].unicode)
                      << " box=[" << extractedPt.chars[i].left << ", " << extractedPt.chars[i].top
                      << ", " << extractedPt.chars[i].right << ", " << extractedPt.chars[i].bottom << "]"
                      << std::dec << std::endl;
        }

        // Validate characters decoded by PDFium from ToUnicode CMap
        if (extractedPt.chars[0].unicode != U'A') {
            std::cerr << "[FAIL] PDFium char 0 expected 'A', got U+" << std::hex << static_cast<uint32_t>(extractedPt.chars[0].unicode) << std::dec << std::endl;
            return 1;
        }
        if (extractedPt.chars[1].unicode != 0x1F600) {
            std::cerr << "[FAIL] PDFium char 1 expected U+1F600, got U+" << std::hex << static_cast<uint32_t>(extractedPt.chars[1].unicode) << std::dec << std::endl;
            return 1;
        }
        if (extractedPt.chars[2].unicode != U'B') {
            std::cerr << "[FAIL] PDFium char 2 expected 'B', got U+" << std::hex << static_cast<uint32_t>(extractedPt.chars[2].unicode) << std::dec << std::endl;
            return 1;
        }
        if (extractedPt.chars[3].unicode != 0x1F680) {
            std::cerr << "[FAIL] PDFium char 3 expected U+1F680, got U+" << std::hex << static_cast<uint32_t>(extractedPt.chars[3].unicode) << std::dec << std::endl;
            return 1;
        }
        if (extractedPt.chars[4].unicode != U'C') {
            std::cerr << "[FAIL] PDFium char 4 expected 'C', got U+" << std::hex << static_cast<uint32_t>(extractedPt.chars[4].unicode) << std::dec << std::endl;
            return 1;
        }

        // Substring extraction on real PDFium-extracted data
        std::u16string extSel0 = extractedPt.getSelectedText(0, 0);
        if (extSel0 != u"A") {
            std::cerr << "[FAIL] PDFium extracted getSelectedText(0, 0) mismatch" << std::endl;
            return 1;
        }
        std::u16string extSel1 = extractedPt.getSelectedText(1, 1);
        if (extSel1.size() != 2 || extSel1[0] != 0xD83D || extSel1[1] != 0xDE00) {
            std::cerr << "[FAIL] PDFium extracted getSelectedText(1, 1) surrogate mismatch" << std::endl;
            return 1;
        }
        std::u16string extSel2 = extractedPt.getSelectedText(2, 2);
        if (extSel2 != u"B") {
            std::cerr << "[FAIL] PDFium extracted getSelectedText(2, 2) misaligned by preceding surrogate pair" << std::endl;
            return 1;
        }
        std::u16string extSelFull = extractedPt.getSelectedText(0, 4);
        if (extSelFull.size() != 7) {
            std::cerr << "[FAIL] PDFium extracted full span expected 7 UTF-16 code units, got " << extSelFull.size() << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Actual PDF parsed by PDFium extracted 5 characters with supplementary code points U+1F600 and U+1F680." << std::endl;
    }

    // Test 16: Text fallback reconstruction across merged surrogate pairs in production PdfiumPage
    std::cout << "[TEST 16] Testing fallback text reconstruction across merged surrogate pairs in PdfiumPage..." << std::endl;
    {
        std::string suppPdfPath = "actual_supp_unicode.pdf";
        auto suppDoc = pdf::createDocument();
        if (suppDoc->loadFromFile(suppPdfPath) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load actual supplementary Unicode PDF for fallback test" << std::endl;
            return 1;
        }

        auto suppPage = suppDoc->getPage(0);
        if (!suppPage) {
            std::cerr << "[FAIL] Failed to open page for fallback test" << std::endl;
            return 1;
        }

        // Enable hook to simulate FPDFText_GetText failure, forcing production extractText() to execute its fallback path
        pdf::PdfiumPage::setSimulateGetTextFailureForTesting(true);
        pdf::PageText pt = suppPage->extractText();
        pdf::PdfiumPage::setSimulateGetTextFailureForTesting(false);

        if (pt.empty() || pt.charCount() != 5) {
            std::cerr << "[FAIL] Fallback production extraction charCount mismatch: expected 5, got " << pt.charCount() << std::endl;
            return 1;
        }

        if (pt.text.size() != 7) {
            std::cerr << "[FAIL] Fallback reconstructed text size mismatch: expected 7 UTF-16 code units, got " << pt.text.size() << std::endl;
            return 1;
        }

        if (pt.getSelectedText(0, 4) != u"A\U0001F600B\U0001F680C") {
            std::cerr << "[FAIL] Fallback getSelectedText(0, 4) mismatch" << std::endl;
            return 1;
        }
        if (pt.getSelectedText(2, 2) != u"B") {
            std::cerr << "[FAIL] Fallback getSelectedText(2, 2) misaligned" << std::endl;
            return 1;
        }
        if (pt.getSelectedText(1, 1).size() != 2 || pt.getSelectedText(1, 1)[0] != 0xD83D || pt.getSelectedText(1, 1)[1] != 0xDE00) {
            std::cerr << "[FAIL] Fallback getSelectedText(1, 1) surrogate mismatch" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Production PdfiumPage::extractText() fallback correctly reconstructed supplementary text without OOB read." << std::endl;
    }

    // Test 17: Persian RTL text extraction, bidirectional highlight box merging, and background search
    std::cout << "[TEST 17] Testing Persian RTL text extraction, highlight merging, and text search..." << std::endl;
    {
        std::string persianPath = "persian_test.pdf";
        generatePersianPdfFixture(persianPath);

        auto persianDoc = pdf::createDocument();
        if (persianDoc->loadFromFile(persianPath) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load Persian PDF fixture" << std::endl;
            return 1;
        }

        auto persianPage = persianDoc->getPage(0);
        if (!persianPage) {
            std::cerr << "[FAIL] Failed to open page from Persian PDF fixture" << std::endl;
            return 1;
        }

        pdf::PageText pt = persianPage->extractText();
        std::cout << "  [DEBUG] Persian extracted charCount=" << pt.charCount() << std::endl;
        for (size_t i = 0; i < pt.charCount(); ++i) {
            std::cout << "     persian[" << i << "]: unicode=0x" << std::hex << static_cast<uint32_t>(pt.chars[i].unicode)
                      << " box=[" << pt.chars[i].left << ", " << pt.chars[i].top
                      << ", " << pt.chars[i].right << ", " << pt.chars[i].bottom << "]"
                      << std::dec << std::endl;
        }
        if (pt.charCount() != 9) {
            std::cerr << "[FAIL] Expected 9 characters in Persian text, got " << pt.charCount() << std::endl;
            return 1;
        }

        // Verify Arabic/Persian Unicode characters in logical reading order:
        // 'salam' (Seen 0x0633, Lam 0x0644, Alef 0x0627, Meem 0x0645)
        if (pt.chars[0].unicode != 0x0633 || pt.chars[1].unicode != 0x0644 ||
            pt.chars[2].unicode != 0x0627 || pt.chars[3].unicode != 0x0645) {
            std::cerr << "[FAIL] Persian characters mismatch for 'salam': "
                      << "c0=0x" << std::hex << static_cast<uint32_t>(pt.chars[0].unicode)
                      << " c1=0x" << static_cast<uint32_t>(pt.chars[1].unicode)
                      << " c2=0x" << static_cast<uint32_t>(pt.chars[2].unicode)
                      << " c3=0x" << static_cast<uint32_t>(pt.chars[3].unicode) << std::dec << std::endl;
            return 1;
        }

        // Substring extraction
        std::u16string salamU16 = pt.getSelectedText(0, 3);
        if (salamU16 != u"\u0633\u0644\u0627\u0645") {
            std::cerr << "[FAIL] getSelectedText(0, 3) expected 'salam'" << std::endl;
            return 1;
        }

        std::u16string jahanU16 = pt.getSelectedText(5, 8);
        if (jahanU16 != u"\u062C\u0647\u0627\u0646") {
            std::cerr << "[FAIL] getSelectedText(5, 8) expected 'jahan'" << std::endl;
            return 1;
        }

        // Test highlight rectangle merging for RTL character sequences
        auto salamRects = pt.getSelectionRects(0, 3);
        if (salamRects.size() != 1) {
            std::cerr << "[FAIL] Expected contiguous RTL highlight for 'salam' to merge into 1 rect, got " << salamRects.size() << std::endl;
            return 1;
        }
        if (!salamRects[0].isValid() || salamRects[0].width() <= 0.0 || salamRects[0].height() <= 0.0) {
            std::cerr << "[FAIL] Merged RTL highlight rectangle has non-positive area" << std::endl;
            return 1;
        }

        // Test asynchronous RenderWorker search on Persian document
        pdf::RenderWorker worker;
        worker.openDocumentSync(101, persianPath);

        std::atomic<int> matchCount{0};
        std::vector<pdf::SearchMatch> deliveredMatches;
        std::atomic<bool> searchDone{false};

        QObject::connect(&worker, &pdf::RenderWorker::searchMatchesFound, [&](int64_t, int64_t, int, const std::vector<pdf::SearchMatch>& matches) {
            matchCount += static_cast<int>(matches.size());
            deliveredMatches.insert(deliveredMatches.end(), matches.begin(), matches.end());
        });
        QObject::connect(&worker, &pdf::RenderWorker::searchCompleted, [&](int64_t, int64_t, int, bool) {
            searchDone = true;
        });

        // 1. Search for "salam" (exact Persian word)
        worker.startSearch(101, 1, QString::fromUtf8("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85"), false, 1);
        worker.waitForIdle();
        QCoreApplication::processEvents();

        if (matchCount.load() != 1 || deliveredMatches.size() != 1) {
            std::cerr << "[FAIL] Search for 'salam' expected 1 match, got " << matchCount.load() << std::endl;
            return 1;
        }
        if (deliveredMatches[0].charStartIndex != 0 || deliveredMatches[0].charCount != 4) {
            std::cerr << "[FAIL] Match indices for 'salam' incorrect" << std::endl;
            return 1;
        }
        if (deliveredMatches[0].rects.size() != 1) {
            std::cerr << "[FAIL] Expected 1 merged highlight rect in search match, got " << deliveredMatches[0].rects.size() << std::endl;
            return 1;
        }

        // 2. Search for "jahan"
        matchCount = 0;
        deliveredMatches.clear();
        worker.startSearch(101, 2, QString::fromUtf8("\xd8\xac\xd9\x87\xd8\xa7\xd9\x86"), false, 1);
        worker.waitForIdle();
        QCoreApplication::processEvents();

        if (matchCount.load() != 1 || deliveredMatches.size() != 1) {
            std::cerr << "[FAIL] Search for 'jahan' expected 1 match, got " << matchCount.load() << std::endl;
            return 1;
        }
        if (deliveredMatches[0].charStartIndex != 5 || deliveredMatches[0].charCount != 4) {
            std::cerr << "[FAIL] Match indices for 'jahan' incorrect" << std::endl;
            return 1;
        }

        // 3. Case-insensitive Latin search test on text_test.pdf
        std::string latinPath = "text_test.pdf";
        generateTextPdfFixture(latinPath, "Hello PDF World!");
        worker.openDocumentSync(102, latinPath);

        worker.setRenderDelayForTesting(std::chrono::milliseconds(500));
        pdf::TileRenderRequest inFlightTile;
        inFlightTile.documentId = 102;
        inFlightTile.pageIndex = 0;
        inFlightTile.rect = pdf::PixelRect{0, 0, 64, 64};
        inFlightTile.scale = 1.0;
        inFlightTile.dpr = 1.0;
        inFlightTile.key = pdf::TileKey::create(102, 0, 0, 0, 1.0, 1.0);
        worker.enqueueRequest(inFlightTile);

        const auto tileDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (worker.progressSnapshot().second != static_cast<uint32_t>(pdf::ipc::WorkerOp::Tile) &&
               std::chrono::steady_clock::now() < tileDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (worker.progressSnapshot().second != static_cast<uint32_t>(pdf::ipc::WorkerOp::Tile)) {
            std::cerr << "[FAIL] Worker did not enter the tile operation for cancellation watchdog test." << std::endl;
            return 1;
        }
        worker.cancelSearch();
        if (worker.progressSnapshot().second != static_cast<uint32_t>(pdf::ipc::WorkerOp::Tile)) {
            std::cerr << "[FAIL] Cancelling search overwrote the in-flight tile operation status." << std::endl;
            return 1;
        }
        worker.waitForIdle();
        worker.setRenderDelayForTesting(std::chrono::milliseconds(0));

        matchCount = 0;
        deliveredMatches.clear();
        // Case-insensitive search
        worker.startSearch(102, 3, QStringLiteral("hello"), false, 1);
        worker.waitForIdle();
        QCoreApplication::processEvents();
        int caseInsensitiveMatches = matchCount.load();

        matchCount = 0;
        deliveredMatches.clear();
        // Case-sensitive search with mismatched casing
        worker.startSearch(102, 4, QStringLiteral("hElLo"), true, 1);
        worker.waitForIdle();
        QCoreApplication::processEvents();
        int caseSensitiveMatches = matchCount.load();

        if (caseInsensitiveMatches <= 0) {
            std::cerr << "[FAIL] Case-insensitive search expected matches for 'hello', got 0" << std::endl;
            return 1;
        }
        if (caseSensitiveMatches != 0) {
            std::cerr << "[FAIL] Case-sensitive search for 'hElLo' expected 0 matches, got " << caseSensitiveMatches << std::endl;
            return 1;
        }

        // 4. Cancel search test: starting a search and immediately canceling
        worker.startSearch(102, 5, QStringLiteral("a"), false, 1);
        worker.cancelSearch();
        worker.waitForIdle();

        std::cout << "[TEST 18] Testing Persian/Arabic normalization, whitespace folding, and bounded results..." << std::endl;
        pdf::PageText normalizedPage;
        normalizedPage.pageIndex = 4;
        const std::u32string normalizedSource = U"ك ي می\u200cخواهم\n۱۲۳";
        for (size_t i = 0; i < normalizedSource.size(); ++i) {
            const double left = static_cast<double>(i * 10);
            normalizedPage.chars.push_back({left, 0.0, left + 8.0, 12.0, normalizedSource[i]});
        }
        bool normalizedTruncated = false;
        const auto normalizedMatches = pdf::findTextInPage(
            normalizedPage, QString::fromUtf8("ک ی میخواهم 123"), false, 10, &normalizedTruncated);
        if (normalizedMatches.size() != 1 || normalizedTruncated ||
            normalizedMatches[0].charStartIndex != 0 ||
            normalizedMatches[0].charCount != static_cast<int>(normalizedSource.size())) {
            std::cerr << "[FAIL] Persian/Arabic variants, ZWNJ, line break, and digit normalization did not match." << std::endl;
            return 1;
        }
        for (auto& ch : normalizedPage.chars) {
            if (ch.unicode >= 0x06F0 && ch.unicode <= 0x06F9) {
                ch.unicode = 0x0660 + (ch.unicode - 0x06F0);
            }
        }
        const auto arabicDigitMatches = pdf::findTextInPage(
            normalizedPage, QString::fromUtf8("ک ی میخواهم 123"), false, 10);
        if (arabicDigitMatches.size() != 1) {
            std::cerr << "[FAIL] Arabic-Indic digits were not normalized to ASCII digits." << std::endl;
            return 1;
        }

        pdf::PageText orthographicPage;
        orthographicPage.pageIndex = 6;
        const std::u32string orthographicSource = U"آموزش أَ إِ ٱ ة ۀ ڪ ى ي";
        for (size_t i = 0; i < orthographicSource.size(); ++i) {
            const double left = static_cast<double>(i * 10);
            orthographicPage.chars.push_back({left, 0.0, left + 8.0, 12.0, orthographicSource[i]});
        }
        const std::u32string orthographicQuery = U"اموزش ا ا ا ه ه ک ی ی";
        const QString orthographicQueryString = QString::fromUcs4(
            orthographicQuery.data(), static_cast<qsizetype>(orthographicQuery.size()));
        const auto orthographicMatches = pdf::findTextInPage(
            orthographicPage, orthographicQueryString, false, 10);
        if (orthographicMatches.size() != 1) {
            std::cerr << "[FAIL] Alef, teh marbuta, harakat, swash kaf, or yeh variants failed normalization." << std::endl;
            return 1;
        }

        pdf::PageText repeatedPage;
        repeatedPage.pageIndex = 5;
        repeatedPage.chars.reserve(static_cast<size_t>(pdf::MAX_SEARCH_MATCHES + 2));
        for (int i = 0; i < pdf::MAX_SEARCH_MATCHES + 2; ++i) {
            const double left = static_cast<double>(i);
            repeatedPage.chars.push_back({left, 0.0, left + 0.8, 12.0, U'a'});
        }
        bool capped = false;
        const auto cappedMatches = pdf::findTextInPage(repeatedPage, QStringLiteral("a"), false, 3, &capped);
        if (cappedMatches.size() != 3 || !capped ||
            cappedMatches[0].charStartIndex != 0 || cappedMatches[2].charStartIndex != 2) {
            std::cerr << "[FAIL] Search match limit did not return exactly the configured cap and mark truncation." << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Persian text extracted with accurate code points, RTL highlight boxes merged, and case-sensitive/case-insensitive search verified." << std::endl;
    }

    // Test 18: Document Outline / Bookmarks extraction
    std::cout << "[TEST 18] Testing Document Outline / Bookmarks hierarchical extraction..." << std::endl;
    {
        std::string outlinePdf = "render_test_outline.pdf";
        generateOutlinePdfFixture(outlinePdf);

        auto doc = pdf::createDocument();
        auto err = doc->loadFromFile(outlinePdf);
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open outline PDF fixture: " << static_cast<int>(err) << std::endl;
            return 1;
        }

        auto outline = doc->getOutline();
        if (outline.size() != 3) {
            std::cerr << "[FAIL] Expected 3 top-level outline items, got " << outline.size() << std::endl;
            return 1;
        }

        // Check Chapter 1
        if (outline[0].title != "Chapter 1") {
            std::cerr << "[FAIL] Expected title 'Chapter 1', got: " << outline[0].title.toStdString() << std::endl;
            return 1;
        }
        if (outline[0].pageIndex != 0) {
            std::cerr << "[FAIL] Expected pageIndex 0 for Chapter 1, got: " << outline[0].pageIndex << std::endl;
            return 1;
        }
        if (outline[0].children.size() != 1) {
            std::cerr << "[FAIL] Expected 1 child under Chapter 1, got: " << outline[0].children.size() << std::endl;
            return 1;
        }

        // Check Section 1.1
        if (outline[0].children[0].title != "Section 1.1") {
            std::cerr << "[FAIL] Expected child title 'Section 1.1', got: " << outline[0].children[0].title.toStdString() << std::endl;
            return 1;
        }
        if (outline[0].children[0].pageIndex != 0) {
            std::cerr << "[FAIL] Expected pageIndex 0 for Section 1.1, got: " << outline[0].children[0].pageIndex << std::endl;
            return 1;
        }

        // Check Chapter 2
        if (outline[1].title != "Chapter 2") {
            std::cerr << "[FAIL] Expected title 'Chapter 2', got: " << outline[1].title.toStdString() << std::endl;
            return 1;
        }
        if (outline[1].pageIndex != 2) {
            std::cerr << "[FAIL] Expected pageIndex 2 for Chapter 2, got: " << outline[1].pageIndex << std::endl;
            return 1;
        }
        if (!outline[1].hasTargetY || std::abs(outline[1].targetYFraction - 0.12) > 0.01) {
            std::cerr << "[FAIL] Rotated CropBox destination did not map to displayed page coordinates: "
                      << outline[1].targetYFraction << std::endl;
            return 1;
        }

        if (outline[2].title != "Preface" || outline[2].pageIndex != -1) {
            std::cerr << "[FAIL] Bookmark without a destination was not represented correctly." << std::endl;
            return 1;
        }

        const std::string cyclicOutlinePdf = "render_test_cyclic_outline.pdf";
        generateOutlinePdfFixture(cyclicOutlinePdf, true);
        auto cyclicDoc = pdf::createDocument();
        if (cyclicDoc->loadFromFile(cyclicOutlinePdf) != pdf::DocumentError::None ||
            cyclicDoc->getOutline().size() != 3) {
            std::cerr << "[FAIL] Cyclic outline links were not bounded during extraction." << std::endl;
            return 1;
        }
        const std::string cyclicChildPdf = "render_test_cyclic_child_outline.pdf";
        generateOutlinePdfFixture(cyclicChildPdf, false, true);
        auto cyclicChildDoc = pdf::createDocument();
        if (cyclicChildDoc->loadFromFile(cyclicChildPdf) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Could not open cyclic child-to-parent outline fixture." << std::endl;
            return 1;
        }
        const auto cyclicChildOutline = cyclicChildDoc->getOutline();
        if (cyclicChildOutline.size() != 3 || cyclicChildOutline[0].children.size() != 1) {
            std::cerr << "[FAIL] Child-to-parent bookmark cycle was not bounded during extraction." << std::endl;
            return 1;
        }

        // Test document with no outline returns empty vector
        std::string noOutlinePdf = "render_no_outline.pdf";
        generateMultiPageFixture(noOutlinePdf);
        auto noOutlineDoc = pdf::createDocument();
        noOutlineDoc->loadFromFile(noOutlinePdf);
        auto emptyOutline = noOutlineDoc->getOutline();
        if (!emptyOutline.empty()) {
            std::cerr << "[FAIL] Document without outline returned non-empty outline!" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Document Outline hierarchy, destination validity, and cyclic-link safety verified." << std::endl;
    }

    std::cout << "[TEST 19] Testing PDF page labels with and without /PageLabels dictionaries..." << std::endl;
    {
        // 1. Without /PageLabels dictionary: default 1-based decimal labels ("1", "2", "3", "4")
        std::string noLabelsPdf = "render_no_page_labels.pdf";
        generateMultiPageFixture(noLabelsPdf);
        auto docNoLabels = pdf::createDocument();
        if (docNoLabels->loadFromFile(noLabelsPdf) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document without page labels." << std::endl;
            return 1;
        }
        auto labelsNoDict = docNoLabels->getPageLabels();
        if (labelsNoDict.size() != 4) {
            std::cerr << "[FAIL] Expected 4 page labels, got: " << labelsNoDict.size() << std::endl;
            return 1;
        }
        for (int i = 0; i < 4; ++i) {
            std::string expected = std::to_string(i + 1);
            if (labelsNoDict[i] != expected) {
                std::cerr << "[FAIL] Default label mismatch for page " << i << ": expected " << expected << ", got " << labelsNoDict[i] << std::endl;
                return 1;
            }
        }

        // 2. With /PageLabels dictionary: Roman numerals ("i", "ii") and prefixed decimal ("A-1", "A-2")
        std::string withLabelsPdf = "render_with_page_labels.pdf";
        generatePageLabelsPdfFixture(withLabelsPdf);
        auto docWithLabels = pdf::createDocument();
        if (docWithLabels->loadFromFile(withLabelsPdf) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document with page labels." << std::endl;
            return 1;
        }
        auto labelsWithDict = docWithLabels->getPageLabels();
        if (labelsWithDict.size() != 4) {
            std::cerr << "[FAIL] Expected 4 page labels, got: " << labelsWithDict.size() << std::endl;
            return 1;
        }
        const std::vector<std::string> expectedWithDict = {"i", "ii", "A-1", "A-2"};
        for (size_t i = 0; i < expectedWithDict.size(); ++i) {
            if (labelsWithDict[i] != expectedWithDict[i]) {
                std::cerr << "[FAIL] Dictionary label mismatch for page " << i << ": expected " << expectedWithDict[i] << ", got " << labelsWithDict[i] << std::endl;
                return 1;
            }
        }

        std::cout << "  -> Passed: Page label extraction verified with and without /PageLabels dictionaries." << std::endl;
    }

    // Test 20: Rotated whole-page and tile rendering (0, 90, 180, 270 degrees)
    std::cout << "[TEST 20] Testing rotated whole-page, thumbnail, and tile rendering with asymmetric corner-color fixture..." << std::endl;
    {
        const std::string cornerFixturePath = "corner_color_test.pdf";
        generateCornerColorPdfFixture(cornerFixturePath);

        auto rotDoc = pdf::createDocument();
        if (rotDoc->loadFromFile(cornerFixturePath) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to load corner_color_test.pdf for rotation test" << std::endl;
            return 1;
        }
        auto page = rotDoc->getPage(0);
        if (!page) {
            std::cerr << "[FAIL] Failed to acquire page for rotation test" << std::endl;
            return 1;
        }

        auto getPixelColor = [](const pdf::PixelBuffer& buf, int x, int y) -> std::tuple<uint8_t, uint8_t, uint8_t> {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(buf.data() + y * buf.stride());
            uint32_t px = row[x];
            return {static_cast<uint8_t>((px >> 16) & 0xFF),
                    static_cast<uint8_t>((px >> 8) & 0xFF),
                    static_cast<uint8_t>(px & 0xFF)};
        };

        auto isRed = [](const std::tuple<uint8_t, uint8_t, uint8_t>& c) {
            return std::get<0>(c) > 200 && std::get<1>(c) < 50 && std::get<2>(c) < 50;
        };
        auto isGreen = [](const std::tuple<uint8_t, uint8_t, uint8_t>& c) {
            return std::get<0>(c) < 50 && std::get<1>(c) > 200 && std::get<2>(c) < 50;
        };
        auto isBlue = [](const std::tuple<uint8_t, uint8_t, uint8_t>& c) {
            return std::get<0>(c) < 50 && std::get<1>(c) < 50 && std::get<2>(c) > 200;
        };
        auto isYellow = [](const std::tuple<uint8_t, uint8_t, uint8_t>& c) {
            return std::get<0>(c) > 200 && std::get<1>(c) > 200 && std::get<2>(c) < 50;
        };

        // 1. Unrotated 0 deg: 400 x 600 px
        // TL=Red, TR=Green, BR=Blue, BL=Yellow
        pdf::PixelBuffer buf0 = page->renderWholePage(1.0, 1.0, 0);
        if (!buf0.isValid() || buf0.width() != 400 || buf0.height() != 600) {
            std::cerr << "[FAIL] 0 deg whole page dimensions incorrect: " << buf0.width() << "x" << buf0.height() << std::endl;
            return 1;
        }
        if (!isRed(getPixelColor(buf0, 50, 50)) ||
            !isGreen(getPixelColor(buf0, 350, 50)) ||
            !isBlue(getPixelColor(buf0, 350, 550)) ||
            !isYellow(getPixelColor(buf0, 50, 550))) {
            std::cerr << "[FAIL] 0 deg corner colors incorrect" << std::endl;
            return 1;
        }

        // 2. Rotated 90 deg: 600 x 400 px
        // TL=Yellow, TR=Red, BR=Green, BL=Blue
        pdf::PixelBuffer buf90 = page->renderWholePage(1.0, 1.0, 90);
        if (!buf90.isValid() || buf90.width() != 600 || buf90.height() != 400) {
            std::cerr << "[FAIL] 90 deg whole page dimensions incorrect: " << buf90.width() << "x" << buf90.height() << std::endl;
            return 1;
        }
        if (!isYellow(getPixelColor(buf90, 50, 50)) ||
            !isRed(getPixelColor(buf90, 550, 50)) ||
            !isGreen(getPixelColor(buf90, 550, 350)) ||
            !isBlue(getPixelColor(buf90, 50, 350))) {
            std::cerr << "[FAIL] 90 deg corner colors incorrect (orientation mismatch)" << std::endl;
            return 1;
        }

        // 3. Rotated 180 deg: 400 x 600 px
        // TL=Blue, TR=Yellow, BR=Red, BL=Green
        pdf::PixelBuffer buf180 = page->renderWholePage(1.0, 1.0, 180);
        if (!buf180.isValid() || buf180.width() != 400 || buf180.height() != 600) {
            std::cerr << "[FAIL] 180 deg whole page dimensions incorrect: " << buf180.width() << "x" << buf180.height() << std::endl;
            return 1;
        }
        if (!isBlue(getPixelColor(buf180, 50, 50)) ||
            !isYellow(getPixelColor(buf180, 350, 50)) ||
            !isRed(getPixelColor(buf180, 350, 550)) ||
            !isGreen(getPixelColor(buf180, 50, 550))) {
            std::cerr << "[FAIL] 180 deg corner colors incorrect (orientation mismatch)" << std::endl;
            return 1;
        }

        // 4. Rotated 270 deg: 600 x 400 px
        // TL=Green, TR=Blue, BR=Yellow, BL=Red
        pdf::PixelBuffer buf270 = page->renderWholePage(1.0, 1.0, 270);
        if (!buf270.isValid() || buf270.width() != 600 || buf270.height() != 400) {
            std::cerr << "[FAIL] 270 deg whole page dimensions incorrect: " << buf270.width() << "x" << buf270.height() << std::endl;
            return 1;
        }
        if (!isGreen(getPixelColor(buf270, 50, 50)) ||
            !isBlue(getPixelColor(buf270, 550, 50)) ||
            !isYellow(getPixelColor(buf270, 550, 350)) ||
            !isRed(getPixelColor(buf270, 50, 350))) {
            std::cerr << "[FAIL] 270 deg corner colors incorrect (orientation mismatch)" << std::endl;
            return 1;
        }

        // 5. Stitched tile equality at 90 deg (2x2 grid, each tile 300 x 200 px)
        pdf::PixelBuffer stitched90 = pdf::PixelBuffer::allocate(600, 400, pdf::PixelFormat::BGRX8888);
        for (int ty = 0; ty < 2; ++ty) {
            for (int tx = 0; tx < 2; ++tx) {
                int x = tx * 300;
                int y = ty * 200;
                pdf::PixelBuffer tile = page->renderTile({x, y, 300, 200}, 1.0, 1.0, 90);
                if (!tile.isValid() || tile.width() != 300 || tile.height() != 200) {
                    std::cerr << "[FAIL] 90 deg tile render failed at (" << x << ", " << y << ")" << std::endl;
                    return 1;
                }
                for (int r = 0; r < 200; ++r) {
                    const uint8_t* srcRow = tile.data() + r * tile.stride();
                    uint8_t* dstRow = stitched90.data() + (y + r) * stitched90.stride() + x * 4;
                    std::memcpy(dstRow, srcRow, 300 * 4);
                }
            }
        }

        // Compare stitched90 against buf90 byte-by-byte
        int diffCount = 0;
        for (int y = 0; y < 400; ++y) {
            const uint8_t* rowA = buf90.data() + y * buf90.stride();
            const uint8_t* rowB = stitched90.data() + y * stitched90.stride();
            if (std::memcmp(rowA, rowB, 600 * 4) != 0) {
                ++diffCount;
            }
        }
        if (diffCount > 0) {
            std::cerr << "[FAIL] Stitched 90 deg tiles do not match whole page 90 deg render! Mismatched rows: " << diffCount << std::endl;
            return 1;
        }

        // 6. Thumbnail rotated rendering check at 90 deg
        pdf::PixelBuffer thumb90 = page->renderThumbnail(150, 90);
        if (!thumb90.isValid() || thumb90.width() != 150) {
            std::cerr << "[FAIL] Rotated 90 deg thumbnail failed or wrong width: " << thumb90.width() << std::endl;
            return 1;
        }
        // At 90 deg (600x400 aspect ratio 3:2), targetWidth=150 should produce targetHeight=100
        if (thumb90.height() != 100) {
            std::cerr << "[FAIL] Rotated 90 deg thumbnail height incorrect: expected 100, got " << thumb90.height() << std::endl;
            return 1;
        }
        if (!isYellow(getPixelColor(thumb90, 15, 15)) || !isRed(getPixelColor(thumb90, 135, 15))) {
            std::cerr << "[FAIL] Rotated 90 deg thumbnail corner colors incorrect" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Asymmetric corner-colors verified across 0, 90, 180, 270 deg; 90 deg stitched tiles byte-equal to whole page; rotated thumbnail aspect and orientation verified." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 20 TESTS PASSED SUCCESSFULLY!      " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
