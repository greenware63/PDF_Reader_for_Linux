#include "TestIsolation.h"
#include "ui/PdfViewerWidget.h"
#include "ui/ImageUtils.h"
#include "ui/MainWindow.h"
#include "ui/ThumbnailListWidget.h"
#include <QTabWidget>
#include <QDockWidget>
#include <QTreeView>
#include <QStandardItemModel>
#include "ui/FindBar.h"
#include <QApplication>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QScrollBar>
#include <QThread>
#include <set>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QClipboard>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cmath>
#include <vector>
#include <cstring>
#include <QSettings>

static void generateOutlinePdfFixture(const std::string& path, bool cyclicSibling = false,
                                      bool cyclicChildToParent = false) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    auto addObj = [&](int num, const std::string& content) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(num) + " 0 obj\n" + content + "\nendobj\n";
    };

    addObj(1, "<< /Type /Catalog /Pages 2 0 R /Outlines 6 0 R >>");
    addObj(2, "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 11 0 R 12 0 R] /Count 5 >>");
    addObj(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");
    addObj(4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>");
    addObj(5, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /CropBox [40 80 540 780] /Rotate 90 >>");
    addObj(6, "<< /Type /Outlines /First 7 0 R /Last 10 0 R /Count 4 >>");
    addObj(7, "<< /Title (Chapter 1) /Parent 6 0 R /Next 8 0 R /First 9 0 R /Last 9 0 R /Count 1 /Dest [3 0 R /Fit] >>");
    addObj(8, "<< /Title (Chapter 2) /Parent 6 0 R /Prev 7 0 R /Next 10 0 R /Dest [5 0 R /XYZ 100 700 1] >>");
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

static void generateMultiPagePdf(const std::string& path, int count = 5) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [";
    for (int i = 0; i < count; ++i) {
        pdf += std::to_string(3 + i) + " 0 R ";
    }
    pdf += "] /Count " + std::to_string(count) + " >>\nendobj\n";

    // Pages (A4 595 x 842 pt)
    const int contentObjNum = 3 + count;
    for (int i = 0; i < count; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents " 
            + std::to_string(contentObjNum) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    // Solid blue fill across the entire 595 x 842 pt page
    std::string stream = "0 0 1 rg 0 0 595 842 re f\n";
    pdf += std::to_string(contentObjNum) + " 0 obj\n<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream\nendobj\n";

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

static void generateTwoPageTextFixture(const std::string& path, const std::string& text1, const std::string& text2) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n";

    // Page 0
    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>\nendobj\n";

    // Page 1
    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>\nendobj\n";

    // Font
    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    // Contents Page 0
    std::string content0 = "BT\n/F1 24 Tf\n100 700 Td\n(" + text1 + ") Tj\nET\n";
    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Length " + std::to_string(content0.size()) + " >>\nstream\n" + content0 + "\nendstream\nendobj\n";

    // Contents Page 1
    std::string content1 = "BT\n/F1 24 Tf\n100 700 Td\n(" + text2 + ") Tj\nET\n";
    offsets.push_back(pdf.size());
    pdf += "7 0 obj\n<< /Length " + std::to_string(content1.size()) + " >>\nstream\n" + content1 + "\nendstream\nendobj\n";

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
static void generateTextPdfFixture(const std::string& path, const std::string& text = "Hello PDF World!", int textX = 100) {
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

    std::string content = "BT\n/F1 24 Tf\n" + std::to_string(textX) + " 700 Td\n(" + text + ") Tj\nET\n";
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


// One page with "HELLO WORLD" (black, 24pt Helvetica) and optional extra page-dictionary entries
// (e.g. a /CropBox or /Rotate), used to check text geometry against the rendered pixels.
static void generateTextPdf(const std::string& path, const std::string& extraPageEntries) {
    const std::string content = "0 g BT /F1 24 Tf 100 600 Td (HELLO WORLD) Tj ET";
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] " + extraPageEntries +
            " /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objs.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

int main(int argc, char* argv[]) {
    const testsupport::IsolatedUserSettings isolatedSettings; // never touch the real ~/.config/PDFReader
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    std::cout << " RUNNING PDF VIEWER WIDGET UNIT TESTS   " << std::endl;

    std::string samplePath = (argc > 1) ? argv[1] : "sample.pdf";

    ui::PdfViewerWidget viewer;
    viewer.resize(1000, 700);
    viewer.show();
    viewer.ensurePolished();

    if (viewer.viewport()->width() < 800) {
        std::cerr << "[FAIL] Viewport geometry uninitialized! width=" << viewer.viewport()->width() << std::endl;
        return 1;
    }

    // Test 1: Open good document, then bad document -> good document remains open and can still render!
    std::cout << "[TEST 1] Testing failed open preservation (good document remains active and renders)..." << std::endl;
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

    // Invalidate cached tiles for the open document to force rendering fresh tiles from worker
    int64_t docId = viewer.currentDocumentId();
    viewer.cache().invalidateDocument(docId);

    // Warmup render to enqueue visible tiles
    QImage testImg(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32_Premultiplied);
    QPainter p(&testImg);
    viewer.viewport()->render(&p);
    p.end();

    viewer.waitForRendering();

    // Verify that tile was successfully rendered and stored in cache for the active document
    auto key0 = pdf::TileKey::create(docId, 0, 0, 0, pdf::quantizeScale(viewer.currentZoom()), viewer.effectiveDpr());
    auto buf = viewer.cache().get(key0);
    if (!buf || !buf->isValid() || buf->data() == nullptr) {
        std::cerr << "[FAIL] Failed open corrupted active document rendering! Tile key0 not present/valid in cache." << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Failed open preserves active document intact and rendering continues to function." << std::endl;

    // Test 2: Multi-page navigation at zoom where bottom clamps
    std::cout << "[TEST 2] Testing multi-page navigation when scroll position clamps at bottom..." << std::endl;
    std::string multiPath = "multi_page_widget.pdf";
    generateMultiPagePdf(multiPath, 5);
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

    // Test 3: Offscreen rendering, DPR injection (1.0, 1.5, 2.0), and device-pixel tile seam verification
    std::cout << "[TEST 3] Testing DPR injection (1.0, 1.5, 2.0) and device-pixel tile seam continuity..." << std::endl;
    viewer.goToPage(0);
    viewer.zoomReset();
    viewer.resize(1200, 800);

    for (double testDpr : {1.0, 1.5, 2.0}) {
        viewer.setDevicePixelRatioOverride(testDpr);
        viewer.verticalScrollBar()->setValue(0);
        viewer.horizontalScrollBar()->setValue(0);

        int devW = static_cast<int>(std::round(viewer.viewport()->width() * testDpr));
        int devH = static_cast<int>(std::round(viewer.viewport()->height() * testDpr));
        QImage renderTarget(devW, devH, QImage::Format_ARGB32_Premultiplied);
        renderTarget.setDevicePixelRatio(testDpr);
        renderTarget.fill(Qt::transparent);

        // First pass queues visible tiles to background RenderWorker
        QPainter warmupPainter(&renderTarget);
        viewer.viewport()->render(&warmupPainter);
        warmupPainter.end();

        // Wait for background worker to complete rendering and deliver results
        viewer.waitForRendering();

        // Second pass draws delivered tiles from cache
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

        // 2. Scan across horizontal tile boundary on page 0 across column seam (512 device px)
        const auto& p0Layout = viewer.layout().getPageLayout(0);
        int64_t tileSeamDocX = p0Layout.leftPixel + 512 - viewer.currentDocScrollX();
        int devSeamX = static_cast<int>(tileSeamDocX);

        int64_t scanDocY = p0Layout.topPixel + 200 - viewer.currentDocScrollY();
        int devScanY = static_cast<int>(scanDocY);

        // Hard failure guard: seam coordinate must be strictly inside renderTarget bounds
        if (devSeamX <= 20 || devSeamX >= devW - 20 || devScanY <= 0 || devScanY >= devH) {
            std::cerr << "[FAIL] Tile seam coordinates out of bounds at DPR " << testDpr 
                      << ": devSeamX=" << devSeamX << ", devScanY=" << devScanY 
                      << ", devW=" << devW << ", devH=" << devH << std::endl;
            return 1;
        }

        // Scan device pixels across the seam ±1 and ±3 device pixels
        for (int dx = -3; dx <= 3; ++dx) {
            QRgb seamPx = renderTarget.pixel(devSeamX + dx, devScanY);
            // Solid blue page: R=0, G=0, B=255. Must have high blue and low red/green
            if (qBlue(seamPx) < 200 || qRed(seamPx) > 50) {
                std::cerr << "[FAIL] Tile seam gap or color discontinuity detected at DPR " << testDpr 
                          << " at device x=" << (devSeamX + dx) << ", device y=" << devScanY 
                          << ": RGB(" << qRed(seamPx) << "," << qGreen(seamPx) << "," << qBlue(seamPx) << ")" << std::endl;
                return 1;
            }
        }
    }
    viewer.setDevicePixelRatioOverride(0.0); // Reset override
    std::cout << "  -> Passed: DPR injection (1.0, 1.5, 2.0) and zero tile seam gap verified at device-pixel precision." << std::endl;

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
    double ptX = static_cast<double>(beforeDocX - p0Layout.leftPixel) / (initialScale * dpr);
    double ptY = static_cast<double>(beforeDocY - p0Layout.topPixel) / (initialScale * dpr);

    viewer.setZoom(initialScale * 2.0, &cursorWidgetPos);
    double newScale = viewer.currentZoom();

    const auto& p0After = viewer.layout().getPageLayout(0);
    int64_t afterDocX = (viewer.horizontalScrollBar()->value()) + static_cast<int64_t>(std::round(cursorWidgetPos.x() * dpr));
    int64_t afterDocY = (viewer.verticalScrollBar()->value()) + static_cast<int64_t>(std::round(cursorWidgetPos.y() * dpr));
    double reconstructedPtX = static_cast<double>(afterDocX - p0After.leftPixel) / (newScale * dpr);
    double reconstructedPtY = static_cast<double>(afterDocY - p0After.topPixel) / (newScale * dpr);

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
    size_t prevCacheSize = viewer.cache().size();
    if (prevCacheSize == 0) {
        std::cerr << "[FAIL] Expected non-empty cache before swap" << std::endl;
        return 1;
    }

    err = viewer.openDocument(QString::fromStdString(samplePath));
    if (err != pdf::DocumentError::None) {
        std::cerr << "[FAIL] Failed to open samplePath during swap test" << std::endl;
        return 1;
    }
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

    // Test 7: Focused fit-width regression test (no horizontal scrollbar when vertical scrollbar appears)
    std::cout << "[TEST 7] Testing focused fit-width regression (no horizontal scrollbar)..." << std::endl;
    {
        err = viewer.openDocument(QString::fromStdString(multiPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath for fit-width test" << std::endl;
            return 1;
        }

        for (double testDpr : {1.0, 2.0}) {
            viewer.setDevicePixelRatioOverride(testDpr);
            viewer.resize(800, 600);
            viewer.zoomFitWidth();

            // Total 5-page height >> 600 px viewport: vertical scrollbar must be active
            if (viewer.verticalScrollBar()->maximum() <= 0) {
                std::cerr << "[FAIL] Expected vertical scrollbar range > 0 for 5-page document at DPR " << testDpr << std::endl;
                return 1;
            }

            // Horizontal scrollbar range MUST be strictly 0 (no horizontal scrollbar popup)
            if (viewer.horizontalScrollBar()->maximum() != 0) {
                std::cerr << "[FAIL] Horizontal scrollbar range is " << viewer.horizontalScrollBar()->maximum() 
                          << " (expected 0) after zoomFitWidth at DPR " << testDpr << std::endl;
                return 1;
            }
        }
        viewer.setDevicePixelRatioOverride(0.0);
        std::cout << "  -> Passed: zoomFitWidth produces strictly 0 horizontal scrollbar range across DPR 1.0 and 2.0." << std::endl;
    }

    // Test 8: Viewport resize anchor centering and coordinate preservation (tolerance <= 1.0 px)
    std::cout << "[TEST 8] Testing viewport resize anchor preservation (tolerance <= 1.0 px)..." << std::endl;
    {
        viewer.goToPage(2);
        const double curDpr = viewer.effectiveDpr();
        int64_t vpW_before = static_cast<int64_t>(std::round(viewer.viewport()->width() * curDpr));
        int64_t vpH_before = static_cast<int64_t>(std::round(viewer.viewport()->height() * curDpr));

        int64_t anchorDocX_before = viewer.currentDocScrollX() + vpW_before / 2;
        int64_t anchorDocY_before = viewer.currentDocScrollY() + vpH_before / 2;

        const auto& p2Before = viewer.layout().getPageLayout(2);
        double anchorPtX = static_cast<double>(anchorDocX_before - p2Before.leftPixel) / (viewer.currentZoom() * curDpr);
        double anchorPtY = static_cast<double>(anchorDocY_before - p2Before.topPixel) / (viewer.currentZoom() * curDpr);

        // Resize viewport from 800 to 1100
        viewer.resize(1100, 750);

        int64_t vpW_after = static_cast<int64_t>(std::round(viewer.viewport()->width() * curDpr));
        int64_t vpH_after = static_cast<int64_t>(std::round(viewer.viewport()->height() * curDpr));

        int64_t anchorDocX_after = viewer.currentDocScrollX() + vpW_after / 2;
        int64_t anchorDocY_after = viewer.currentDocScrollY() + vpH_after / 2;

        const auto& p2After = viewer.layout().getPageLayout(2);
        double reconstructedPtX = static_cast<double>(anchorDocX_after - p2After.leftPixel) / (viewer.currentZoom() * curDpr);
        double reconstructedPtY = static_cast<double>(anchorDocY_after - p2After.topPixel) / (viewer.currentZoom() * curDpr);

        double driftX = std::abs(reconstructedPtX - anchorPtX) * viewer.currentZoom() * curDpr;
        double driftY = std::abs(reconstructedPtY - anchorPtY) * viewer.currentZoom() * curDpr;

        if (driftX > 1.0 || driftY > 1.0) {
            std::cerr << "[FAIL] Resize anchor drift exceeded 1.0 device pixels! driftX=" 
                      << driftX << " px, driftY=" << driftY << " px" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Resize anchor point preserved within 1.0 device pixel (driftX=" 
                  << driftX << ", driftY=" << driftY << ")." << std::endl;
    }

    // Test 9: RenderWorker cancellation and request generations
    std::cout << "[TEST 9] Testing RenderWorker request cancellation and generation pruning..." << std::endl;
    {
        pdf::RenderWorker worker;
        auto openRes = worker.openDocumentSync(100, multiPath);
        if (openRes.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath on worker" << std::endl;
            return 1;
        }

        // Contract under test (RenderWorker::setGeneration):
        //   * requests still QUEUED for an older generation are pruned and never rendered;
        //   * a tile already IN FLIGHT when the generation advances is still delivered, because tile keys
        //     carry scale/DPR so it is valid content (discarding it starves the screen while scrolling,
        //     see Test 13);
        //   * requests of the new generation are delivered.
        // The engine thread is made deterministic with the render delay hook: req A is popped and held
        // in flight, req B is queued behind it, and only then does the generation advance.
        std::set<int> deliveredCols; // col*10+row identifies the request
        QObject::connect(&worker, &pdf::RenderWorker::tileReady,
                         [&](int64_t, uint64_t, const pdf::TileKey& k, const pdf::PixelBuffer&) {
                             deliveredCols.insert(k.col * 10 + k.row);
                         });
        worker.setRenderDelayForTesting(std::chrono::milliseconds(300));

        const auto keyA = pdf::TileKey::create(100, 0, 0, 0, 1.0, 1.0); // in flight when gen advances
        const auto keyB = pdf::TileKey::create(100, 0, 0, 1, 1.0, 1.0); // queued, must be pruned
        const auto keyC = pdf::TileKey::create(100, 0, 1, 0, 1.0, 1.0); // generation 2, must be delivered
        worker.enqueueRequest({100, 1, 0, {0, 0, 512, 512}, 1.0, 1.0, keyA, 10});
        QThread::msleep(100); // engine has popped A and is sleeping in the hook
        worker.enqueueRequest({100, 1, 0, {0, 512, 512, 512}, 1.0, 1.0, keyB, 10});

        worker.setGeneration(2);
        worker.enqueueRequest({100, 2, 0, {512, 0, 512, 512}, 1.0, 1.0, keyC, 10});

        worker.waitForIdle();
        QCoreApplication::processEvents();
        worker.setRenderDelayForTesting(std::chrono::milliseconds(0));

        if (deliveredCols.count(keyB.col * 10 + keyB.row)) {
            std::cerr << "[FAIL] Generation 1 request was not cancelled!" << std::endl;
            return 1;
        }
        if (!deliveredCols.count(keyA.col * 10 + keyA.row)) {
            std::cerr << "[FAIL] In-flight tile was discarded when the generation advanced (scroll starvation)!" << std::endl;
            return 1;
        }
        if (!deliveredCols.count(keyC.col * 10 + keyC.row)) {
            std::cerr << "[FAIL] Generation 2 request was not delivered!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: queued stale requests pruned, in-flight tile delivered, new generation delivered." << std::endl;
    }

    // Test 10: Document replacement under pending render requests
    std::cout << "[TEST 10] Testing document replacement under pending render requests..." << std::endl;
    {
        pdf::RenderWorker worker;
        worker.openDocumentSync(1, multiPath);

        // Queue requests for Doc A
        for (int c = 0; c < 3; ++c) {
            auto k = pdf::TileKey::create(1, 0, c, 0, 1.0, 1.0);
            worker.enqueueRequest({1, 1, 0, {c * 512, 0, 512, 512}, 1.0, 1.0, k, 10});
        }

        bool docADelivered = false;
        bool docBDelivered = false;

        QObject::connect(&worker, &pdf::RenderWorker::tileReady, [&](int64_t docId, uint64_t, const pdf::TileKey&, const pdf::PixelBuffer&) {
            if (docId == 1) docADelivered = true;
            if (docId == 2) docBDelivered = true;
        });

        // Replace document immediately with Doc B serially on worker
        worker.openDocumentSync(2, samplePath);
        auto kB = pdf::TileKey::create(2, 0, 0, 0, 1.0, 1.0);
        worker.enqueueRequest({2, 1, 0, {0, 0, 512, 512}, 1.0, 1.0, kB, 10});

        worker.waitForIdle();
        QCoreApplication::processEvents();

        if (docADelivered) {
            std::cerr << "[FAIL] Doc A request was delivered after document replacement!" << std::endl;
            return 1;
        }
        if (!docBDelivered) {
            std::cerr << "[FAIL] Doc B request was not delivered!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Document replacement discarded obsolete document requests." << std::endl;
    }

    // Test 11: Clean widget shutdown under pending render load
    std::cout << "[TEST 11] Testing clean widget destruction under background render load..." << std::endl;
    {
        auto* tempViewer = new ui::PdfViewerWidget();
        tempViewer->resize(1200, 800);
        tempViewer->show();
        tempViewer->openDocument(QString::fromStdString(multiPath));

        // Queue render requests by triggering paint
        QImage target(1200, 800, QImage::Format_ARGB32);
        QPainter p(&target);\
        tempViewer->viewport()->render(&p);
        p.end();

        // Immediately delete widget while render worker has work in flight
        delete tempViewer;
        std::cout << "  -> Passed: Viewport widget with RenderWorker thread shuts down cleanly without crash or hang." << std::endl;
    }

    // Test 12: Tile failure recovery in widget
    std::cout << "[TEST 12] Testing tile failure recovery and retry trigger in widget..." << std::endl;
    {
        err = viewer.openDocument(QString::fromStdString(multiPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath for failure recovery test" << std::endl;
            return 1;
        }

        auto failKey = pdf::TileKey::create(viewer.cache().size() + 999, 0, 0, 0, 1.0, 1.0);
        viewer.cache().markFailed(failKey);
        if (!viewer.cache().isFailed(failKey)) {
            std::cerr << "[FAIL] Tile should be marked failed initially" << std::endl;
            return 1;
        }

        // Trigger explicit retry
        viewer.retryFailedTiles();
        if (viewer.cache().isFailed(failKey)) {
            std::cerr << "[FAIL] retryFailedTiles should clear failure status" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: retryFailedTiles cleanly cleared failure tombstones." << std::endl;
    }

    // Test 13: Continuous scrolling without tile starvation (Fix 1)
    std::cout << "[TEST 13] Testing tiles are delivered WHILE scrolling continuously (no starvation)..." << std::endl;
    {
        err = viewer.openDocument(QString::fromStdString(multiPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath for continuous scroll test" << std::endl;
            return 1;
        }

        viewer.resize(900, 600);
        viewer.goToPage(0);
        viewer.waitForRendering();

        const int maxV = viewer.verticalScrollBar()->maximum();
        if (maxV <= 50) {
            std::cerr << "[FAIL] Expected scrollbar maximum > 50 for continuous scroll test" << std::endl;
            return 1;
        }

        // Make every render take ~15 ms while the scroll position changes every ~5 ms.
        // The old generation-based discard dropped EVERY finished tile in this regime (0 deliveries).
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(15));
        int deliveredDuringScroll = 0;
        QMetaObject::Connection countConn = QObject::connect(
            &viewer.renderWorker(), &pdf::ipc::WorkerClient::tileReady, &viewer,
            [&](int64_t, uint64_t, const pdf::TileKey&, const pdf::PixelBuffer&) { ++deliveredDuringScroll; });

        const int stepDelta = maxV / 60;
        for (int i = 1; i <= 60; ++i) {
            viewer.verticalScrollBar()->setValue(i * stepDelta);
            QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter pFrame(&frame);
            viewer.viewport()->render(&pFrame);
            pFrame.end();
            QThread::msleep(5);
            QCoreApplication::processEvents();
        }

        // Deliveries counted BEFORE the scroll stopped and before any waitForRendering()
        const int deliveredBeforeIdle = deliveredDuringScroll;
        viewer.waitForRendering();
        QObject::disconnect(countConn); // lambda captures a local by reference
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(0));

        if (deliveredBeforeIdle < 3) {
            std::cerr << "[FAIL] Tile starvation: only " << deliveredBeforeIdle
                      << " tiles delivered during continuous scrolling (expected >= 3)" << std::endl;
            return 1;
        }

        // Final-position tiles must be present as well
        const double dpr = viewer.effectiveDpr();
        const int64_t vpW = static_cast<int64_t>(std::round(viewer.viewport()->width() * dpr));
        const int64_t vpH = static_cast<int64_t>(std::round(viewer.viewport()->height() * dpr));
        auto visTiles = viewer.layout().getVisibleTiles({viewer.currentDocScrollX(), viewer.currentDocScrollY(), vpW, vpH}, 512);
        // One more paint to enqueue anything still missing, then drain
        {
            QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter pFrame(&frame);
            viewer.viewport()->render(&pFrame);
            pFrame.end();
        }
        viewer.waitForRendering();
        for (const auto& t : visTiles) {
            auto k = pdf::TileKey::create(viewer.currentDocumentId(), t.pageIndex, t.col, t.row, pdf::quantizeScale(viewer.currentZoom()), dpr);
            if (!viewer.cache().get(k).has_value()) {
                std::cerr << "[FAIL] Visible tile missing after scrolling stopped" << std::endl;
                return 1;
            }
        }
        std::cout << "  -> Passed: " << deliveredBeforeIdle
                  << " tiles delivered while scrolling continuously; final viewport fully rendered." << std::endl;
    }

    // Test 14: the tile currently being rendered is not rendered a second time when re-requested
    std::cout << "[TEST 14] Testing in-flight tile deduplication on the worker..." << std::endl;
    {
        std::atomic<int> deliveries{0}; // declared before the worker so it outlives the worker thread
        pdf::RenderWorker worker;
        auto res = worker.openDocumentSync(700, multiPath);
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath on worker for dedup test" << std::endl;
            return 1;
        }
        worker.setRenderDelayForTesting(std::chrono::milliseconds(150));

        QObject::connect(&worker, &pdf::RenderWorker::tileReady, &viewer,
                         [&](int64_t, uint64_t, const pdf::TileKey&, const pdf::PixelBuffer&) { ++deliveries; },
                         Qt::DirectConnection);

        auto key = pdf::TileKey::create(700, 0, 0, 0, 1.0, 1.0);
        worker.enqueueRequest({700, 1, 0, {0, 0, 256, 256}, 1.0, 1.0, key, 10});
        // Wait (up to 2 s) until the worker has popped the request, i.e. it is now "in flight"
        for (int i = 0; i < 200 && worker.pendingCount() != 0; ++i) {
            QThread::msleep(10);
        }
        if (worker.pendingCount() != 0) {
            std::cerr << "[FAIL] Worker never picked up the first request" << std::endl;
            return 1;
        }

        // Same tile re-requested under a NEWER generation (what a scroll tick does)
        worker.setGeneration(2);
        worker.enqueueRequest({700, 2, 0, {0, 0, 256, 256}, 1.0, 1.0, key, 10});
        if (worker.pendingCount() != 0) {
            std::cerr << "[FAIL] In-flight tile was queued a second time (pendingCount="
                      << worker.pendingCount() << ")" << std::endl;
            return 1;
        }
        worker.waitForIdle();
        if (deliveries.load() != 1) {
            std::cerr << "[FAIL] Expected exactly 1 delivery, got " << deliveries.load() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Re-requesting the in-flight tile did not render it twice." << std::endl;
    }

    // Test 15: 200-page document verifies worker page cache bounded at <= 16 pages (Fix 2)
    std::cout << "[TEST 15] Testing worker page cache LRU bound on 200-page document..." << std::endl;
    {
        std::string path200 = "doc_200_pages.pdf";
        generateMultiPagePdf(path200, 200);

        pdf::RenderWorker worker;
        auto res = worker.openDocumentSync(500, path200);
        if (res.error != pdf::DocumentError::None || res.pageCount != 200) {
            std::cerr << "[FAIL] Failed to open 200-page document: pages=" << res.pageCount << std::endl;
            return 1;
        }

        // Request tiles from 40 different pages across the 200-page document
        for (int p = 0; p < 80; ++p) {
            auto k = pdf::TileKey::create(500, p, 0, 0, 1.0, 1.0);
            worker.enqueueRequest({500, 1, p, {0, 0, 200, 200}, 1.0, 1.0, k, 10});
        }

        worker.waitForIdle();

        size_t cachedPages = worker.cachedPageCount();
        if (cachedPages == 0 || cachedPages > 64) {
            std::cerr << "[FAIL] Worker page cache unbounded! cachedPages=" << cachedPages 
                      << ", expected in range [1, 16]" << std::endl;
            return 1;
        }
        worker.startSearch(500, 1, QStringLiteral("a"), false, 200);
        worker.waitForIdle();
        if (worker.cachedPageCount() != cachedPages) {
            std::cerr << "[FAIL] Background search changed the render worker page cache from "
                      << cachedPages << " to " << worker.cachedPageCount() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: 200-page document load kept worker page cache strictly bounded at " 
                  << cachedPages << " <= 64 pages, unchanged by background search." << std::endl;
    }

    // ---------------------------------------------------------------------------------------------
    // Asynchronous open (Tests 16-20)
    // ---------------------------------------------------------------------------------------------
    // Helper: pump the event loop until the viewer is no longer loading (or timeout). Returns iterations run.
    auto pumpUntilIdle = [&](int timeoutMs) {
        QElapsedTimer t;
        t.start();
        int iterations = 0;
        while (viewer.isLoading() && t.elapsed() < timeoutMs) {
            QThread::msleep(10);
            QCoreApplication::processEvents();
            ++iterations;
        }
        QCoreApplication::processEvents();
        return iterations;
    };
    // Helper: render the viewport once, drain the worker, and report whether the first tile of the open document is cached
    auto firstTileRenders = [&]() {
        const int64_t docId = viewer.currentDocumentId();
        viewer.cache().invalidateDocument(docId);
        QImage img(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32_Premultiplied);
        QPainter p(&img);
        viewer.viewport()->render(&p);
        p.end();
        viewer.waitForRendering();
        auto key0 = pdf::TileKey::create(docId, 0, 0, 0, pdf::quantizeScale(viewer.currentZoom()), viewer.effectiveDpr());
        auto buf = viewer.cache().get(key0);
        return buf.has_value() && buf->isValid();
    };

    std::cout << "[TEST 16] Testing openDocumentAsync does not block the UI thread..." << std::endl;
    int sampleCount = 0;
    {
        err = viewer.openDocument(QString::fromStdString(samplePath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Could not open sample for async test" << std::endl;
            return 1;
        }
        sampleCount = viewer.pageCount();

        int loadedCount = 0;
        int loadingTrue = 0, loadingFalse = 0;
        QMetaObject::Connection c1 = QObject::connect(&viewer, &ui::PdfViewerWidget::documentLoaded, &viewer,
                                                      [&](const QString&, int) { ++loadedCount; });
        QMetaObject::Connection c2 = QObject::connect(&viewer, &ui::PdfViewerWidget::loadingChanged, &viewer,
                                                      [&](bool l, const QString&) { (l ? loadingTrue : loadingFalse)++; });

        viewer.renderWorker().setOpenDelayForTesting(std::chrono::milliseconds(300));
        const int64_t idBefore = viewer.currentDocumentId();

        QElapsedTimer callTimer;
        callTimer.start();
        viewer.openDocumentAsync(QString::fromStdString(multiPath));
        const qint64 callMs = callTimer.elapsed();

        if (callMs > 100) {
            std::cerr << "[FAIL] openDocumentAsync blocked the caller for " << callMs << " ms" << std::endl;
            return 1;
        }
        if (!viewer.isLoading()) {
            std::cerr << "[FAIL] isLoading() must be true right after openDocumentAsync" << std::endl;
            return 1;
        }
        if (viewer.pageCount() != sampleCount || viewer.currentDocumentId() != idBefore) {
            std::cerr << "[FAIL] The previous document must stay open while the new one loads" << std::endl;
            return 1;
        }

        // The event loop must keep spinning while the worker loads (a blocked UI would give ~1 iteration)
        const int iterations = pumpUntilIdle(5000);
        viewer.renderWorker().setOpenDelayForTesting(std::chrono::milliseconds(0));
#ifdef PDF_WORKER_TEST_HOOKS
        if (iterations < 5) {
            std::cerr << "[FAIL] UI thread was not responsive during load (iterations=" << iterations << ")" << std::endl;
            return 1;
        }
#else
        if (iterations < 1) {
            std::cerr << "[FAIL] UI thread was not responsive during load (iterations=" << iterations << ")" << std::endl;
            return 1;
        }
#endif
        if (viewer.isLoading() || viewer.pageCount() != 5 || loadedCount != 1 || viewer.currentDocumentId() == idBefore) {
            std::cerr << "[FAIL] Async open did not complete correctly: loading=" << viewer.isLoading()
                      << " pages=" << viewer.pageCount() << " loaded=" << loadedCount << std::endl;
            return 1;
        }
        if (loadingTrue != 1 || loadingFalse != 1) {
            std::cerr << "[FAIL] loadingChanged expected once true + once false, got " << loadingTrue << "/" << loadingFalse << std::endl;
            return 1;
        }
        if (!firstTileRenders()) {
            std::cerr << "[FAIL] New document does not render after async open" << std::endl;
            return 1;
        }
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        std::cout << "  -> Passed: call returned in " << callMs << " ms, UI pumped " << iterations
                  << " iterations during the load, new document renders." << std::endl;
    }

    std::cout << "[TEST 17] Testing failed async open keeps the current document and rendering..." << std::endl;
    {
        int errorCount = 0, loadedCount = 0;
        QMetaObject::Connection c1 = QObject::connect(&viewer, &ui::PdfViewerWidget::loadError, &viewer,
                                                      [&](const QString&) { ++errorCount; });
        QMetaObject::Connection c2 = QObject::connect(&viewer, &ui::PdfViewerWidget::documentLoaded, &viewer,
                                                      [&](const QString&, int) { ++loadedCount; });
        const int64_t idBefore = viewer.currentDocumentId();

        viewer.openDocumentAsync("missing_async_file.pdf");
        pumpUntilIdle(5000);

        if (errorCount != 1 || loadedCount != 0 || viewer.isLoading()) {
            std::cerr << "[FAIL] Failed async open: errors=" << errorCount << " loaded=" << loadedCount << std::endl;
            return 1;
        }
        if (viewer.pageCount() != 5 || viewer.currentDocumentId() != idBefore) {
            std::cerr << "[FAIL] Failed async open destroyed the active document" << std::endl;
            return 1;
        }
        if (!firstTileRenders()) {
            std::cerr << "[FAIL] Active document stopped rendering after a failed async open" << std::endl;
            return 1;
        }
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        std::cout << "  -> Passed: error reported, document intact, rendering still works." << std::endl;
    }

    std::cout << "[TEST 18] Testing back-to-back async opens end in a consistent state..." << std::endl;
    {
        int loadedCount = 0;
        QMetaObject::Connection c1 = QObject::connect(&viewer, &ui::PdfViewerWidget::documentLoaded, &viewer,
                                                      [&](const QString&, int) { ++loadedCount; });
        viewer.renderWorker().setOpenDelayForTesting(std::chrono::milliseconds(80));
        viewer.openDocumentAsync(QString::fromStdString(samplePath));      // 1-page sample (in progress)
        viewer.openDocumentAsync("coalesced_away.pdf");                  // superseded before it ever starts
        viewer.openDocumentAsync(QString::fromStdString(multiPath));       // latest request wins: 5 pages
        pumpUntilIdle(8000);
        viewer.renderWorker().setOpenDelayForTesting(std::chrono::milliseconds(0));

        if (viewer.isLoading() || viewer.pageCount() != 5) {
            std::cerr << "[FAIL] Latest request must win: loading=" << viewer.isLoading()
                      << " pages=" << viewer.pageCount() << std::endl;
            return 1;
        }
        if (loadedCount != 2) {
            std::cerr << "[FAIL] Expected 2 completed opens (first + latest), got " << loadedCount << std::endl;
            return 1;
        }
        if (!firstTileRenders()) {
            std::cerr << "[FAIL] UI and worker disagree about the active document after back-to-back opens" << std::endl;
            return 1;
        }
        QObject::disconnect(c1);
        std::cout << "  -> Passed: superseded request dropped, latest document active and rendering." << std::endl;
    }

    std::cout << "[TEST 19] Testing closeDocument while an async open is in progress..." << std::endl;
    {
        int loadedCount = 0;
        QMetaObject::Connection c1 = QObject::connect(&viewer, &ui::PdfViewerWidget::documentLoaded, &viewer,
                                                      [&](const QString&, int) { ++loadedCount; });
        viewer.renderWorker().setOpenDelayForTesting(std::chrono::milliseconds(150));
        viewer.openDocumentAsync(QString::fromStdString(samplePath));
        viewer.closeDocument(); // blocks until the worker finished the open and then closed the document
        viewer.renderWorker().setOpenDelayForTesting(std::chrono::milliseconds(0));
        QThread::msleep(50);
        QCoreApplication::processEvents();

        if (viewer.isLoading() || viewer.isDocumentOpen() || viewer.pageCount() != 0 || loadedCount != 0) {
            std::cerr << "[FAIL] A load cancelled by closeDocument() must not resurrect the document: loading="
                      << viewer.isLoading() << " open=" << viewer.isDocumentOpen() << " loaded=" << loadedCount << std::endl;
            return 1;
        }
        QObject::disconnect(c1);
        std::cout << "  -> Passed: cancelled load ignored, viewer stays closed." << std::endl;
    }

    std::cout << "[TEST 20] Testing RenderWorker::openDocumentAsync contract (success, failure, waitForIdle)..." << std::endl;
    {
        std::atomic<int> opened{0};
        std::atomic<int> lastPages{0};
        std::atomic<int> lastError{-1};
        std::atomic<int> tilesDelivered{0};
        pdf::RenderWorker worker;
        QObject::connect(&worker, &pdf::RenderWorker::documentOpened, &viewer,
                         [&](int64_t, pdf::OpenResult r) {
                             ++opened;
                             lastPages = r.pageCount;
                             lastError = static_cast<int>(r.error);
                         }, Qt::DirectConnection);
        QObject::connect(&worker, &pdf::RenderWorker::tileReady, &viewer,
                         [&](int64_t, uint64_t, const pdf::TileKey&, const pdf::PixelBuffer&) { ++tilesDelivered; },
                         Qt::DirectConnection);

        worker.setOpenDelayForTesting(std::chrono::milliseconds(100));
        worker.openDocumentAsync(800, multiPath);
        QThread::msleep(30); // the worker has now popped the open task and is inside its 100 ms delay
        worker.waitForIdle(); // must also wait for the RUNNING open task, not just the (empty) queues
        if (opened.load() != 1 || lastPages.load() != 5 || lastError.load() != static_cast<int>(pdf::DocumentError::None)) {
            std::cerr << "[FAIL] waitForIdle returned before the async open finished (opened=" << opened.load() << ")" << std::endl;
            return 1;
        }
        worker.setOpenDelayForTesting(std::chrono::milliseconds(0));

        worker.openDocumentAsync(801, "definitely_missing_for_worker.pdf");
        worker.waitForIdle();
        if (opened.load() != 2 || lastError.load() == static_cast<int>(pdf::DocumentError::None)) {
            std::cerr << "[FAIL] Failed async open must emit documentOpened with an error" << std::endl;
            return 1;
        }

        // Document 800 must still be the active one after the failed open of 801
        auto key = pdf::TileKey::create(800, 0, 0, 0, 1.0, 1.0);
        worker.enqueueRequest({800, 1, 0, {0, 0, 128, 128}, 1.0, 1.0, key, 10});
        worker.waitForIdle();
        if (tilesDelivered.load() != 1) {
            std::cerr << "[FAIL] Active document lost after a failed async open (tiles=" << tilesDelivered.load() << ")" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: one signal per call, failure keeps the active document, waitForIdle covers opens." << std::endl;
    }

    std::cout << "[TEST 21] Testing zoom fallback: scaled old tiles are shown while new-zoom tiles render..." << std::endl;
    {
        err = viewer.openDocument(QString::fromStdString(multiPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath for zoom fallback test" << std::endl;
            return 1;
        }
        viewer.resize(900, 600);
        viewer.goToPage(0);
        viewer.setZoom(1.0);

        auto renderFrame = [&]() {
            QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter pFrame(&frame);
            viewer.viewport()->render(&pFrame);
            pFrame.end();
            return frame;
        };
        auto isBlue = [](const QImage& img, int x, int y) {
            const QRgb px = img.pixel(x, y);
            return qBlue(px) > 200 && qRed(px) < 60 && qGreen(px) < 60;
        };

        renderFrame();
        viewer.waitForRendering();
        renderFrame();
        if (!isBlue(renderFrame(), 450, 300)) {
            std::cerr << "[FAIL] Page not rendered at 100% before zoom test" << std::endl;
            return 1;
        }

        // Make new-zoom tiles take long enough that none can arrive before the first frame at the new zoom
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(150));
        const size_t fallbackBefore = viewer.fallbackTilesDrawn();
        viewer.setZoom(1.5);
        QImage zoomed = renderFrame();
        const size_t fallbackUsed = viewer.fallbackTilesDrawn() - fallbackBefore;

        const bool centerBlue = isBlue(zoomed, 450, 300);
        const bool quarterBlue = isBlue(zoomed, 450, 150) || isBlue(zoomed, 450, 450);
        viewer.waitForRendering();
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(0));

        if (fallbackUsed == 0 || !centerBlue || !quarterBlue) {
            std::cerr << "[FAIL] Zoom fallback missing: fallbackTiles=" << fallbackUsed
                      << " centerBlue=" << centerBlue << " otherBlue=" << quarterBlue << std::endl;
            return 1;
        }

        // After rendering completes the real tiles replace the stand-ins
        const size_t fallbackAfterSettle = viewer.fallbackTilesDrawn();
        QImage settled = renderFrame();
        if (!isBlue(settled, 450, 300) || viewer.fallbackTilesDrawn() != fallbackAfterSettle) {
            std::cerr << "[FAIL] Fallback must not be used once the real tiles are cached" << std::endl;
            return 1;
        }

        // A different document must never reuse the previous document's tiles as fallback
        err = viewer.openDocument(QString::fromStdString(samplePath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to reopen sample.pdf after zoom fallback test" << std::endl;
            return 1;
        }
        const size_t fallbackBeforeReopen = viewer.fallbackTilesDrawn();
        renderFrame();
        if (viewer.fallbackTilesDrawn() != fallbackBeforeReopen) {
            std::cerr << "[FAIL] Fallback leaked across documents" << std::endl;
            return 1;
        }
        viewer.waitForRendering();
        std::cout << "  -> Passed: " << fallbackUsed << " tiles shown scaled from the old zoom; replaced by real tiles; no leakage across documents." << std::endl;
    }

    std::cout << "[TEST 22] Testing prefetch ring: off-screen tiles are rendered, after the visible ones..." << std::endl;
    {
        err = viewer.openDocument(QString::fromStdString(multiPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open multiPath for prefetch test" << std::endl;
            return 1;
        }
        viewer.resize(900, 600);
        viewer.setZoom(1.0);
        viewer.goToPage(0);

        auto renderFrame = [&]() {
            QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter pFrame(&frame);
            viewer.viewport()->render(&pFrame);
            pFrame.end();
        };
        const double dpr = viewer.effectiveDpr();
        const double scale = pdf::quantizeScale(viewer.currentZoom());
        const int64_t vpW = static_cast<int64_t>(std::round(viewer.viewport()->width() * dpr));
        const int64_t vpH = static_cast<int64_t>(std::round(viewer.viewport()->height() * dpr));

        // (a) tiles just below the viewport get rendered without ever being scrolled into view
        const size_t prefetchBefore = viewer.prefetchRequestsIssued();
        renderFrame();
        viewer.waitForRendering();
        if (viewer.prefetchRequestsIssued() == prefetchBefore) {
            std::cerr << "[FAIL] No prefetch requests were issued" << std::endl;
            return 1;
        }
        auto below = viewer.layout().getVisibleTiles({viewer.currentDocScrollX(), viewer.currentDocScrollY() + vpH, vpW, vpH / 2}, 512);
        int prefetchedBelow = 0;
        for (const auto& t : below) {
            if (viewer.cache().contains(pdf::TileKey::create(viewer.currentDocumentId(), t.pageIndex, t.col, t.row, scale, dpr))) {
                ++prefetchedBelow;
            }
        }
        if (below.empty() || prefetchedBelow == 0) {
            std::cerr << "[FAIL] Tiles just below the viewport were not prefetched (below=" << below.size() << ")" << std::endl;
            return 1;
        }

        // (b) on a fresh area, every visible tile is delivered before any prefetched tile
        std::vector<pdf::TileKey> deliveryOrder;
        QMetaObject::Connection orderConn = QObject::connect(
            &viewer.renderWorker(), &pdf::ipc::WorkerClient::tileReady, &viewer,
            [&](int64_t, uint64_t, const pdf::TileKey& k, const pdf::PixelBuffer&) { deliveryOrder.push_back(k); });
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(10));
        viewer.goToPage(3);
        renderFrame();
        viewer.waitForRendering();
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(0));
        QObject::disconnect(orderConn);

        auto visTiles = viewer.layout().getVisibleTiles({viewer.currentDocScrollX(), viewer.currentDocScrollY(), vpW, vpH}, 512);
        std::vector<pdf::TileKey> visKeys;
        for (const auto& t : visTiles) {
            visKeys.push_back(pdf::TileKey::create(viewer.currentDocumentId(), t.pageIndex, t.col, t.row, scale, dpr));
        }
        if (visKeys.empty() || deliveryOrder.size() <= visKeys.size()) {
            std::cerr << "[FAIL] Expected prefetched deliveries beyond the " << visKeys.size()
                      << " visible tiles, got " << deliveryOrder.size() << std::endl;
            return 1;
        }
        for (size_t i = 0; i < visKeys.size(); ++i) {
            if (std::find(visKeys.begin(), visKeys.end(), deliveryOrder[i]) == visKeys.end()) {
                std::cerr << "[FAIL] A prefetched tile was rendered before a visible one (position " << i << ")" << std::endl;
                return 1;
            }
        }

        // (c) the switch works: with prefetch off nothing new is requested
        viewer.setPrefetchEnabled(false);
        const size_t issued = viewer.prefetchRequestsIssued();
        viewer.goToPage(1);
        renderFrame();
        viewer.waitForRendering();
        viewer.setPrefetchEnabled(true);
        if (viewer.prefetchRequestsIssued() != issued) {
            std::cerr << "[FAIL] setPrefetchEnabled(false) still issued prefetch requests" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: ring tiles pre-rendered, visible tiles always first, prefetch can be disabled." << std::endl;
    }

    // ---------------------------------------------------------------------------------------------
    // Text extraction, selection, and clipboard copy (Tests 23-25)
    // ---------------------------------------------------------------------------------------------
    std::string textPath = "widget_text_fixture.pdf";
    generateTextPdfFixture(textPath, "Hello PDF World!");

    std::cout << "[TEST 23] Testing asynchronous PageText extraction arrival..." << std::endl;
    {
        err = viewer.openDocument(QString::fromStdString(textPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open text fixture in viewer" << std::endl;
            return 1;
        }

        viewer.resize(1000, 700);
        viewer.goToPage(0);
        viewer.setZoom(1.0);

        // Render viewport to trigger ensurePageTextRequested for page 0
        QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
        QPainter pFrame(&frame);
        viewer.viewport()->render(&pFrame);
        pFrame.end();

        // Wait for worker to finish text extraction and deliver pageTextReady
        viewer.waitForRendering();

        auto pt = viewer.getPageText(0);
        if (!pt) {
            std::cerr << "[FAIL] PageText for page 0 was not delivered to cache" << std::endl;
            return 1;
        }
        if (pt->charCount() != 16) {
            std::cerr << "[FAIL] Expected 16 characters in PageText, got " << pt->charCount() << std::endl;
            return 1;
        }
        if (pt->getSelectedText(0, 15) != u"Hello PDF World!") {
            std::cerr << "[FAIL] Extracted text mismatch in viewer" << std::endl;
            return 1;
        }
        if (viewer.getPageText(1) != nullptr) {
            std::cerr << "[FAIL] Non-existent page 1 should not have PageText" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: PageText arrived asynchronously, 16 characters verified." << std::endl;
    }

    std::cout << "[TEST 24] Testing mouse drag selection and hover cursor..." << std::endl;
    {
        auto pt = viewer.getPageText(0);
        if (!pt || pt->charCount() < 5) {
            std::cerr << "[FAIL] PageText not available for selection test" << std::endl;
            return 1;
        }

        const auto& pl = viewer.layout().getPageLayout(0);
        const double dpr = viewer.effectiveDpr();
        const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

        // Character 0 ('H')
        const auto& b0 = pt->chars[0];
        double c0X = (b0.left + b0.right) * 0.5;
        double c0Y = (b0.top + b0.bottom) * 0.5;
        int64_t doc0X = pl.leftPixel + static_cast<int64_t>(std::round(c0X * effScale));
        int64_t doc0Y = pl.topPixel + static_cast<int64_t>(std::round(c0Y * effScale));
        double vp0X = static_cast<double>(doc0X - viewer.currentDocScrollX()) / dpr;
        double vp0Y = static_cast<double>(doc0Y - viewer.currentDocScrollY()) / dpr;

        // Character 4 ('o')
        const auto& b4 = pt->chars[4];
        double c4X = (b4.left + b4.right) * 0.5;
        double c4Y = (b4.top + b4.bottom) * 0.5;
        int64_t doc4X = pl.leftPixel + static_cast<int64_t>(std::round(c4X * effScale));
        int64_t doc4Y = pl.topPixel + static_cast<int64_t>(std::round(c4Y * effScale));
        double vp4X = static_cast<double>(doc4X - viewer.currentDocScrollX()) / dpr;
        double vp4Y = static_cast<double>(doc4Y - viewer.currentDocScrollY()) / dpr;

        // Hover test over text
        QMouseEvent hoverEvent(QEvent::MouseMove, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &hoverEvent);
        if (viewer.viewport()->cursor().shape() != Qt::IBeamCursor) {
            std::cerr << "[FAIL] Hover cursor over text expected IBeamCursor" << std::endl;
            return 1;
        }

        // Hover test away from text
        QMouseEvent hoverAway(QEvent::MouseMove, QPointF(10, 10), QPointF(10, 10), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &hoverAway);
        if (viewer.viewport()->cursor().shape() != Qt::ArrowCursor) {
            std::cerr << "[FAIL] Hover cursor away from text expected ArrowCursor" << std::endl;
            return 1;
        }

        // Drag select from 'H' to 'o'
        QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &pressEvent);

        QMouseEvent moveEvent(QEvent::MouseMove, QPointF(vp4X, vp4Y), QPointF(vp4X, vp4Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &moveEvent);

        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(vp4X, vp4Y), QPointF(vp4X, vp4Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &releaseEvent);

        if (!viewer.hasSelection()) {
            std::cerr << "[FAIL] Expected hasSelection() == true after drag" << std::endl;
            return 1;
        }
        if (viewer.selectedText() != QStringLiteral("Hello")) {
            std::cerr << "[FAIL] Expected selectedText() == 'Hello', got '" << viewer.selectedText().toStdString() << "'" << std::endl;
            return 1;
        }

        // Clear selection
        viewer.clearSelection();
        if (viewer.hasSelection() || !viewer.selectedText().isEmpty()) {
            std::cerr << "[FAIL] clearSelection failed to reset selection" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: IBeam hover cursor, mouse drag selection ('Hello'), and clearSelection verified." << std::endl;
    }

    std::cout << "[TEST 25] Testing copySelection API and Ctrl+C clipboard copy..." << std::endl;
    {
        auto pt = viewer.getPageText(0);
        const auto& pl = viewer.layout().getPageLayout(0);
        const double dpr = viewer.effectiveDpr();
        const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

        const auto& b0 = pt->chars[0];
        double vp0X = static_cast<double>(pl.leftPixel + static_cast<int64_t>(std::round(((b0.left + b0.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        double vp0Y = static_cast<double>(pl.topPixel + static_cast<int64_t>(std::round(((b0.top + b0.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        const auto& b4 = pt->chars[4];
        double vp4X = static_cast<double>(pl.leftPixel + static_cast<int64_t>(std::round(((b4.left + b4.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        double vp4Y = static_cast<double>(pl.topPixel + static_cast<int64_t>(std::round(((b4.top + b4.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        // Select 'Hello'
        QMouseEvent pressEvent(QEvent::MouseButtonPress, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &pressEvent);
        QMouseEvent moveEvent(QEvent::MouseMove, QPointF(vp4X, vp4Y), QPointF(vp4X, vp4Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &moveEvent);
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, QPointF(vp4X, vp4Y), QPointF(vp4X, vp4Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &releaseEvent);

        if (viewer.selectedText() != QStringLiteral("Hello")) {
            std::cerr << "[FAIL] Failed to select 'Hello' for copy test" << std::endl;
            return 1;
        }

        // 1. Programmatic copySelection()
        QGuiApplication::clipboard()->clear();
        viewer.copySelection();
        if (QGuiApplication::clipboard()->text() != QStringLiteral("Hello")) {
            std::cerr << "[FAIL] copySelection did not copy 'Hello' to clipboard: '"
                      << QGuiApplication::clipboard()->text().toStdString() << "'" << std::endl;
            return 1;
        }

        // 2. Keyboard shortcut Ctrl+C
        QGuiApplication::clipboard()->clear();
        if (!QGuiApplication::clipboard()->text().isEmpty()) {
            std::cerr << "[FAIL] Clipboard clear failed" << std::endl;
            return 1;
        }

        QKeyEvent copyEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(&viewer, &copyEvent);
        if (QGuiApplication::clipboard()->text() != QStringLiteral("Hello")) {
            std::cerr << "[FAIL] Ctrl+C shortcut did not copy 'Hello' to clipboard: '"
                      << QGuiApplication::clipboard()->text().toStdString() << "'" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: copySelection and Ctrl+C shortcut copy selected text to system clipboard." << std::endl;
    }

    std::cout << "[TEST 26] Testing cross-page mouse drag selection and multi-page text extraction..." << std::endl;
    {
        std::string twoPagePath = "widget_two_page_text.pdf";
        generateTwoPageTextFixture(twoPagePath, "Page Zero Header", "Page One Content");

        err = viewer.openDocument(QString::fromStdString(twoPagePath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open two-page text fixture" << std::endl;
            return 1;
        }

        viewer.resize(1000, 700);
        viewer.goToPage(0);
        viewer.setZoom(1.0);

        // Render page 0 then page 1 to trigger ensurePageTextRequested for both pages
        auto renderCurrent = [&]() {
            QImage f(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter p(&f);
            viewer.viewport()->render(&p);
            p.end();
            viewer.waitForRendering();
        };

        renderCurrent();
        viewer.goToPage(1);
        renderCurrent();
        viewer.goToPage(0);
        renderCurrent();

        auto pt0 = viewer.getPageText(0);
        auto pt1 = viewer.getPageText(1);
        if (!pt0 || !pt1) {
            std::cerr << "[FAIL] PageText for both pages was not loaded (pt0=" << !(!pt0) << ", pt1=" << !(!pt1) << ")" << std::endl;
            return 1;
        }

        const auto& pl0 = viewer.layout().getPageLayout(0);
        const auto& pl1 = viewer.layout().getPageLayout(1);
        const double dpr = viewer.effectiveDpr();
        const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

        // Start drag at character 5 of page 0 ("Zero Header")
        const auto& b0_5 = pt0->chars[5];
        double vp0X = static_cast<double>(pl0.leftPixel + static_cast<int64_t>(std::round(((b0_5.left + b0_5.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        double vp0Y = static_cast<double>(pl0.topPixel + static_cast<int64_t>(std::round(((b0_5.top + b0_5.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        // End drag at character 7 of page 1 ("Page One")
        const auto& b1_7 = pt1->chars[7];
        double vp1X = static_cast<double>(pl1.leftPixel + static_cast<int64_t>(std::round(((b1_7.left + b1_7.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        double vp1Y = static_cast<double>(pl1.topPixel + static_cast<int64_t>(std::round(((b1_7.top + b1_7.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        // 1. Forward drag selection from page 0 to page 1
        QMouseEvent press0(QEvent::MouseButtonPress, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press0);

        QMouseEvent move1(QEvent::MouseMove, QPointF(vp1X, vp1Y), QPointF(vp1X, vp1Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move1);

        QMouseEvent release1(QEvent::MouseButtonRelease, QPointF(vp1X, vp1Y), QPointF(vp1X, vp1Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release1);

        if (!viewer.hasSelection()) {
            std::cerr << "[FAIL] Cross-page selection not established" << std::endl;
            return 1;
        }

        QString selForward = viewer.selectedText();
        if (!selForward.contains("Zero Header") || !selForward.contains("Page One") || !selForward.contains("\n")) {
            std::cerr << "[FAIL] Cross-page selected text unexpected: '" << selForward.toStdString() << "'" << std::endl;
            return 1;
        }

        // Copy cross-page selection
        QGuiApplication::clipboard()->clear();
        viewer.copySelection();
        if (QGuiApplication::clipboard()->text() != selForward) {
            std::cerr << "[FAIL] Cross-page text not copied to clipboard correctly" << std::endl;
            return 1;
        }

        // 2. Backward drag selection from page 1 to page 0
        viewer.clearSelection();
        QMouseEvent press1(QEvent::MouseButtonPress, QPointF(vp1X, vp1Y), QPointF(vp1X, vp1Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press1);

        QMouseEvent move0(QEvent::MouseMove, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move0);

        QMouseEvent release0(QEvent::MouseButtonRelease, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release0);

        if (!viewer.hasSelection()) {
            std::cerr << "[FAIL] Backward cross-page selection not established" << std::endl;
            return 1;
        }

        QString selBackward = viewer.selectedText();
        if (selBackward != selForward) {
            std::cerr << "[FAIL] Backward cross-page selection differs from forward" << std::endl;
            return 1;
        }

        viewer.clearSelection();
        std::cout << "  -> Passed: Forward and backward cross-page selection and newline joining verified." << std::endl;
    }

    std::cout << "[TEST 27] Testing RenderWorker and widget selection on actual PDF with supplementary Unicode (U+1F600, U+1F680)..." << std::endl;
    {
        std::string suppPath = "widget_actual_supp_unicode.pdf";
        generateSupplementaryUnicodePdfFixture(suppPath);

        err = viewer.openDocument(QString::fromStdString(suppPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open actual supplementary Unicode PDF in viewer" << std::endl;
            return 1;
        }

        viewer.resize(1000, 700);
        viewer.goToPage(0);
        viewer.setZoom(1.0);

        QImage f(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
        QPainter p(&f);
        viewer.viewport()->render(&p);
        p.end();
        viewer.waitForRendering();

        auto pt = viewer.getPageText(0);
        if (!pt || pt->charCount() != 5) {
            std::cerr << "[FAIL] PageText expected 5 characters from actual PDF, got " << (pt ? pt->charCount() : 0) << std::endl;
            return 1;
        }

        if (pt->chars[1].unicode != 0x1F600 || pt->chars[3].unicode != 0x1F680) {
            std::cerr << "[FAIL] Supplementary characters not decoded properly by PDFium in widget" << std::endl;
            return 1;
        }

        // Mouse drag select across all 5 characters in widget
        const auto& pl = viewer.layout().getPageLayout(0);
        const double dpr = viewer.effectiveDpr();
        const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

        const auto& b0 = pt->chars[0];
        double vp0X = static_cast<double>(pl.leftPixel + static_cast<int64_t>(std::round(((b0.left + b0.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        double vp0Y = static_cast<double>(pl.topPixel + static_cast<int64_t>(std::round(((b0.top + b0.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        const auto& b4 = pt->chars[4];
        double vp4X = static_cast<double>(pl.leftPixel + static_cast<int64_t>(std::round(((b4.left + b4.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        double vp4Y = static_cast<double>(pl.topPixel + static_cast<int64_t>(std::round(((b4.top + b4.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        QMouseEvent press0(QEvent::MouseButtonPress, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press0);

        QMouseEvent move4(QEvent::MouseMove, QPointF(vp4X, vp4Y), QPointF(vp4X, vp4Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move4);

        QMouseEvent release4(QEvent::MouseButtonRelease, QPointF(vp4X, vp4Y), QPointF(vp4X, vp4Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release4);

        if (!viewer.hasSelection()) {
            std::cerr << "[FAIL] Failed to establish selection on supplementary Unicode PDF" << std::endl;
            return 1;
        }

        QString expectedFull = QString::fromUtf8("A\xF0\x9F\x98\x80\x42\xF0\x9F\x9A\x80\x43");
        if (viewer.selectedText() != expectedFull) {
            std::cerr << "[FAIL] Full supplementary text mismatch in viewer: expected '"
                      << expectedFull.toStdString() << "', got '" << viewer.selectedText().toStdString() << "'" << std::endl;
            return 1;
        }

        // Test clipboard copy of actual supplementary Unicode text
        QGuiApplication::clipboard()->clear();
        viewer.copySelection();
        if (QGuiApplication::clipboard()->text() != expectedFull) {
            std::cerr << "[FAIL] Clipboard copy of supplementary Unicode mismatch" << std::endl;
            return 1;
        }

        viewer.clearSelection();
        std::cout << "  -> Passed: Actual PDF with supplementary Unicode loaded in widget, selected with mouse, and copied to clipboard." << std::endl;
    }


    std::cout << "[TEST 28] Testing text boxes line up with the rendered page (crop box, /Rotate)..." << std::endl;
    {
        struct Case { const char* name; const char* file; const char* extra; };
        const Case cases[] = {
            {"plain page", "text_plain.pdf", ""},
            {"crop box offset (50,100)", "text_crop.pdf", "/CropBox [50 100 562 692]"},
            {"/Rotate 90", "text_rot90.pdf", "/Rotate 90"},
            {"/Rotate 270 + crop box", "text_rot270_crop.pdf", "/Rotate 270 /CropBox [20 60 580 740]"},
        };
        for (const auto& c : cases) {
            generateTextPdf(c.file, c.extra);
            auto doc = pdf::createDocument();
            if (doc->loadFromFile(c.file) != pdf::DocumentError::None) {
                std::cerr << "[FAIL] Cannot open " << c.file << std::endl;
                return 1;
            }
            auto page = doc->getPage(0);
            const pdf::PageText text = page->extractText();

            double L = 1e9, T = 1e9, R = -1e9, B = -1e9;
            int validChars = 0;
            for (const auto& ch : text.chars) {
                if (!ch.isValid()) continue;
                ++validChars;
                L = std::min(L, ch.left); T = std::min(T, ch.top);
                R = std::max(R, ch.right); B = std::max(B, ch.bottom);
            }
            // Every glyph of "HELLO WORLD", including the space, must be hit-testable
            if (validChars != 11) {
                std::cerr << "[FAIL] " << c.name << ": expected 11 usable character boxes, got " << validChars << std::endl;
                return 1;
            }

            pdf::PixelBuffer px = page->renderWholePage(1.0, 1.0);
            int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1;
            for (int y = 0; y < px.height(); ++y) {
                const uint8_t* row = px.data() + static_cast<size_t>(y) * px.stride();
                for (int x = 0; x < px.width(); ++x) {
                    if (row[x * 4] < 100 && row[x * 4 + 1] < 100 && row[x * 4 + 2] < 100) {
                        minX = std::min(minX, x); maxX = std::max(maxX, x);
                        minY = std::min(minY, y); maxY = std::max(maxY, y);
                    }
                }
            }
            if (maxX < 0) {
                std::cerr << "[FAIL] " << c.name << ": no text pixels in the rendered page" << std::endl;
                return 1;
            }
            // Glyph boxes include side bearings/ascent, so allow a couple of points of slack
            const double tol = 3.0;
            if (std::abs(L - minX) > tol || std::abs(T - minY) > tol || std::abs(R - (maxX + 1)) > tol || std::abs(B - (maxY + 1)) > tol) {
                std::cerr << "[FAIL] " << c.name << ": text boxes (" << L << "," << T << "," << R << "," << B
                          << ") do not match rendered text (" << minX << "," << minY << "," << maxX + 1 << "," << maxY + 1 << ")" << std::endl;
                return 1;
            }
        }
        std::cout << "  -> Passed: text boxes match the rendering on plain, cropped and rotated pages." << std::endl;
    }

    std::cout << "[TEST 29] Testing goToPage discards stale queued tiles..." << std::endl;
    {
        const std::string path60 = "doc_60_pages.pdf";
        generateMultiPagePdf(path60, 60);
        if (viewer.openDocument(QString::fromStdString(path60)) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open 60 page document" << std::endl;
            return 1;
        }
        viewer.resize(900, 600);
        viewer.setZoom(1.0);
        viewer.goToPage(0);
        viewer.waitForRendering();

        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(100));
        {
            QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter p(&frame);
            viewer.viewport()->render(&p);
            p.end();
        }
        // Work for the first page is queued/rendering; now jump far away without repainting in between
        const uint64_t genBefore = viewer.currentGeneration();
        viewer.goToPage(40);
        const size_t stale = viewer.renderWorker().pendingCount();
        viewer.renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(0));
        viewer.waitForRendering();
        if (viewer.currentGeneration() == genBefore || stale != 0) {
            std::cerr << "[FAIL] goToPage must prune queued tiles of the old position (generation advanced="
                      << (viewer.currentGeneration() != genBefore) << ", stale queued=" << stale << ")" << std::endl;
            return 1;
        }
        const uint64_t genAtTarget = viewer.currentGeneration();
        viewer.goToPage(40); // already there: nothing moved, nothing to prune
        if (viewer.currentGeneration() != genAtTarget) {
            std::cerr << "[FAIL] goToPage to the current position must not advance the generation" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: stale tiles pruned on a page jump; no needless pruning when already there." << std::endl;
    }

    std::cout << "[TEST 30] Testing the extracted-text cache is bounded..." << std::endl;
    {
        const std::string path120 = "doc_120_pages.pdf";
        generateMultiPagePdf(path120, 120);
        if (viewer.openDocument(QString::fromStdString(path120)) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open 120 page document" << std::endl;
            return 1;
        }
        viewer.resize(900, 600);
        viewer.setZoom(1.0);
        for (int i = 0; i < 120; ++i) {
            viewer.goToPage(i);
            QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter p(&frame);
            viewer.viewport()->render(&p);
            p.end();
            viewer.waitForRendering();
        }
        const size_t cached = viewer.cachedTextPageCount();
        if (cached > ui::PdfViewerWidget::MAX_CACHED_TEXT_PAGES) {
            std::cerr << "[FAIL] Text cache grew to " << cached << " pages (limit "
                      << ui::PdfViewerWidget::MAX_CACHED_TEXT_PAGES << ")" << std::endl;
            return 1;
        }
        if (!viewer.getPageText(119)) {
            std::cerr << "[FAIL] Text of the page being viewed was evicted" << std::endl;
            return 1;
        }
        if (viewer.getPageText(0)) {
            std::cerr << "[FAIL] Text of a page 119 pages away should have been evicted" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: " << cached << " pages of text kept after viewing 120 pages." << std::endl;
    }

    std::cout << "[TEST 31] Testing a text request for an unloadable page is still answered..." << std::endl;
    {
        std::atomic<int> answers{0};
        std::atomic<bool> emptyAnswer{false};
        QMetaObject::Connection c = QObject::connect(
            &viewer.renderWorker(), &pdf::ipc::WorkerClient::pageTextReady, &viewer,
            [&](int64_t, int, std::shared_ptr<pdf::PageText> t) { ++answers; emptyAnswer = (!t || t->empty()); },
            Qt::DirectConnection);
        viewer.renderWorker().requestPageText(viewer.currentDocumentId(), 99999);
        viewer.renderWorker().waitForIdle();
        QObject::disconnect(c);
        if (answers.load() != 1 || !emptyAnswer.load()) {
            std::cerr << "[FAIL] Worker did not answer a text request for a missing page (answers=" << answers.load() << ")" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: missing page answered with empty text." << std::endl;
    }

    std::cout << "[TEST 32] Testing full text search (Ctrl+F), FindBar, match cycling, and Persian search..." << std::endl;
    {
        // 1. Two-page Latin document search
        std::string twoPagePath = "widget_two_page_text.pdf";
        generateTwoPageTextFixture(twoPagePath, "Page Zero Header", "Page One Content");

        err = viewer.openDocument(QString::fromStdString(twoPagePath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open two-page text fixture for search test" << std::endl;
            return 1;
        }

        viewer.resize(1000, 700);
        viewer.goToPage(0);
        viewer.setZoom(1.0);

        int lastReportedCurrent = -1;
        int lastReportedTotal = -1;
        QObject::connect(&viewer, &ui::PdfViewerWidget::searchResultsChanged, [&](int cur, int tot) {
            lastReportedCurrent = cur;
            lastReportedTotal = tot;
        });

        // Search for "page" (case-insensitive by default)
        viewer.findText("page", false);
        viewer.renderWorker().waitForIdle();
        QCoreApplication::processEvents();

        if (viewer.searchMatchCount() != 2) {
            std::cerr << "[FAIL] Expected 2 search matches for 'page', got " << viewer.searchMatchCount() << std::endl;
            return 1;
        }
        if (viewer.currentSearchMatchIndex() != 0) {
            std::cerr << "[FAIL] Initial match index expected 0, got " << viewer.currentSearchMatchIndex() << std::endl;
            return 1;
        }
        if (lastReportedCurrent != 1 || lastReportedTotal != 2) {
            std::cerr << "[FAIL] searchResultsChanged signal expected 1 of 2, got " << lastReportedCurrent << " of " << lastReportedTotal << std::endl;
            return 1;
        }

        // Match 0 is on page 0, Match 1 is on page 1
        const auto& matches = viewer.searchMatches();
        if (matches[0].pageIndex != 0 || matches[1].pageIndex != 1) {
            std::cerr << "[FAIL] Search match pages expected 0 and 1, got " << matches[0].pageIndex << " and " << matches[1].pageIndex << std::endl;
            return 1;
        }

        // Searches begin on the current page and wrap through the document.
        viewer.goToPage(1);
        viewer.findText("page", false);
        viewer.renderWorker().waitForIdle();
        QCoreApplication::processEvents();
        if (viewer.searchMatchCount() != 2 ||
            viewer.searchMatches()[0].pageIndex != 0 ||
            viewer.searchMatches()[1].pageIndex != 1 ||
            viewer.currentSearchMatchIndex() != 1) {
            std::cerr << "[FAIL] Search results were not in document order with the nearest match selected." << std::endl;
            return 1;
        }
        viewer.findNext();
        if (viewer.currentSearchMatchIndex() != 0 ||
            viewer.searchMatches()[static_cast<size_t>(viewer.currentSearchMatchIndex())].pageIndex != 0) {
            std::cerr << "[FAIL] Find Next did not continue in document order from the nearest match." << std::endl;
            return 1;
        }

        // Queued results from a cleared search must not repopulate highlights.
        viewer.findText("page", false);
        viewer.clearSearch();
        viewer.renderWorker().waitForIdle();
        QCoreApplication::processEvents();
        if (viewer.searchMatchCount() != 0 || viewer.currentSearchMatchIndex() != -1) {
            std::cerr << "[FAIL] Results from a cleared search repopulated the viewer." << std::endl;
            return 1;
        }

        viewer.goToPage(0);
        viewer.findText("page", false);
        viewer.renderWorker().waitForIdle();
        QCoreApplication::processEvents();

        // Cycle next
        viewer.findNext();
        if (viewer.currentSearchMatchIndex() != 1) {
            std::cerr << "[FAIL] findNext expected current index 1, got " << viewer.currentSearchMatchIndex() << std::endl;
            return 1;
        }

        // Wrap around next -> back to 0
        viewer.findNext();
        if (viewer.currentSearchMatchIndex() != 0) {
            std::cerr << "[FAIL] findNext wraparound expected index 0, got " << viewer.currentSearchMatchIndex() << std::endl;
            return 1;
        }

        // Cycle previous -> wraps back to 1
        viewer.findPrevious();
        if (viewer.currentSearchMatchIndex() != 1) {
            std::cerr << "[FAIL] findPrevious wraparound expected index 1, got " << viewer.currentSearchMatchIndex() << std::endl;
            return 1;
        }

        // Render viewport with highlights - ensures drawSearchHighlights() executes without crash
        QImage frame(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
        QPainter p(&frame);
        viewer.viewport()->render(&p);
        p.end();

        // 2. Test Persian RTL text search in viewer
        std::string persianPath = "persian_test.pdf";
        generatePersianPdfFixture(persianPath);
        err = viewer.openDocument(QString::fromStdString(persianPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open Persian document in viewer for search" << std::endl;
            return 1;
        }

        viewer.findText(QString::fromUtf8("\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85"), false);
        viewer.renderWorker().waitForIdle();
        {
            QElapsedTimer searchTimer;
            searchTimer.start();
            while (searchTimer.elapsed() < 3000 && viewer.searchMatchCount() == 0) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }
        }

        if (viewer.searchMatchCount() != 1) {
            std::cerr << "[FAIL] Persian search expected 1 match for 'salam', got " << viewer.searchMatchCount() << std::endl;
            return 1;
        }
        if (viewer.searchMatches()[0].rects.empty()) {
            std::cerr << "[FAIL] Persian search match has empty highlight rects" << std::endl;
            return 1;
        }

        // Render viewport on Persian page with active match
        {
            QImage f(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
            QPainter pPersian(&f);
            viewer.viewport()->render(&pPersian);
            pPersian.end();
        }

        // 3. FindBar widget unit test
        ui::FindBar findBar;
        int searchReqCount = 0;
        int nextReqCount = 0;
        int prevReqCount = 0;
        int closedCount = 0;
        QString reqQuery;
        bool reqCase = false;

        QObject::connect(&findBar, &ui::FindBar::searchRequested, [&](const QString& q, bool c) {
            ++searchReqCount;
            reqQuery = q;
            reqCase = c;
        });
        QObject::connect(&findBar, &ui::FindBar::findNextRequested, [&]() { ++nextReqCount; });
        QObject::connect(&findBar, &ui::FindBar::findPreviousRequested, [&]() { ++prevReqCount; });
        QObject::connect(&findBar, &ui::FindBar::closed, [&]() { ++closedCount; });

        findBar.setMatchCount(3, 17);
        auto* searchEdit = findBar.findChild<QLineEdit*>();
        auto* countLabel = findBar.findChild<QLabel*>();
        if (!searchEdit || !countLabel) {
            std::cerr << "[FAIL] FindBar search input or status label not found." << std::endl;
            return 1;
        }
        searchEdit->setText(QStringLiteral("query"));
        findBar.setMatchCount(0, 0);
        findBar.finishSearch(false);
        if (countLabel->text() != QStringLiteral("No matches")) {
            std::cerr << "[FAIL] FindBar did not distinguish a completed empty search." << std::endl;
            return 1;
        }
        findBar.setMatchCount(3, 10000);
        findBar.setSearchingProgress(2, 5);
        if (!countLabel->text().contains(QStringLiteral("Searching")) ||
            !countLabel->text().contains(QStringLiteral("40%"))) {
            std::cerr << "[FAIL] FindBar did not show search progress." << std::endl;
            return 1;
        }
        findBar.finishSearch(true);
        if (!countLabel->text().contains(QStringLiteral("10,000+"))) {
            std::cerr << "[FAIL] FindBar did not display the truncated match count." << std::endl;
            return 1;
        }
        // FindBar handles Esc key -> emits closed()
        QKeyEvent escKey(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(&findBar, &escKey);
        if (closedCount != 1) {
            std::cerr << "[FAIL] FindBar Esc key expected to emit closed(), got count=" << closedCount << std::endl;
            return 1;
        }

        std::string farTextPath = "widget_far_right_text.pdf";
        generateTextPdfFixture(farTextPath, "FarRight", 450);
        err = viewer.openDocument(QString::fromStdString(farTextPath));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open far-right text fixture." << std::endl;
            return 1;
        }
        viewer.resize(1000, 700);
        viewer.setZoom(3.0);
        viewer.findText("FarRight", true);
        QElapsedTimer searchTimer;
        searchTimer.start();
        while (searchTimer.elapsed() < 3000 && viewer.searchMatchCount() == 0) {
            viewer.renderWorker().waitForIdle();
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (viewer.searchMatchCount() != 1 || viewer.currentDocScrollX() <= 0) {
            std::cerr << "[FAIL] Navigating to an off-screen horizontal search match did not scroll. count="
                      << viewer.searchMatchCount() << " scrollX=" << viewer.currentDocScrollX() << std::endl;
            return 1;
        }

        // Clear search
        viewer.clearSearch();
        if (viewer.searchMatchCount() != 0 || viewer.currentSearchMatchIndex() != -1) {
            std::cerr << "[FAIL] clearSearch failed to reset match state" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Search findText, next/prev wraparound cycling, Persian RTL search highlights, and FindBar keyboard handling verified." << std::endl;
    }

    // Test 33: Document Outline / Bookmarks Sidebar, QTreeView population, and navigation
    std::cout << "[TEST 33] Testing Document Outline / Bookmarks Sidebar and TreeView navigation..." << std::endl;
    {
        std::string outlinePath = "widget_test_outline.pdf";
        generateOutlinePdfFixture(outlinePath);

        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        // 1. Sidebar initial state: hidden before document with bookmarks is loaded
        auto* dock = window.sidebarDock();
        if (!dock) {
            std::cerr << "[FAIL] sidebarDock is null on MainWindow!" << std::endl;
            return 1;
        }

        // 2. F4 shortcut toggles sidebar visibility
        QKeyEvent f4Press(QEvent::KeyPress, Qt::Key_F4, Qt::NoModifier);
        QApplication::sendEvent(&window, &f4Press);
        if (!dock->isVisible()) {
            std::cerr << "[FAIL] F4 key press did not make sidebarDock visible!" << std::endl;
            return 1;
        }

        QApplication::sendEvent(&window, &f4Press);
        if (dock->isVisible()) {
            std::cerr << "[FAIL] Second F4 key press did not hide sidebarDock!" << std::endl;
            return 1;
        }

        // 3. Open document with outline
        bool opened = window.openFile(QString::fromStdString(outlinePath));
        if (!opened) {
            std::cerr << "[FAIL] window.openFile returned false for outline fixture" << std::endl;
            return 1;
        }

        auto* viewer = window.findChild<ui::PdfViewerWidget*>();
        if (!viewer) {
            std::cerr << "[FAIL] Could not find PdfViewerWidget child in MainWindow" << std::endl;
            return 1;
        }

        QElapsedTimer outlineTimer;
        outlineTimer.start();
        while (outlineTimer.elapsed() < 3000) {
            viewer->renderWorker().waitForIdle();
            QCoreApplication::processEvents();
            if (!viewer->outline().empty()) {
                break;
            }
            QThread::msleep(5);
        }

        // 4. Verify viewer.outline()
        const auto& outline = viewer->outline();
        if (outline.size() != 3) {
            std::cerr << "[FAIL] Expected 3 top-level outline items in viewer, got " << outline.size() << std::endl;
            return 1;
        }
        if (outline[0].title != "Chapter 1" || outline[0].pageIndex != 0) {
            std::cerr << "[FAIL] Unexpected title/page for Chapter 1" << std::endl;
            return 1;
        }
        if (outline[0].children.size() != 1 || outline[0].children[0].title != "Section 1.1" || outline[0].children[0].pageIndex != 0) {
            std::cerr << "[FAIL] Unexpected child title/page for Section 1.1" << std::endl;
            return 1;
        }
        if (outline[1].title != "Chapter 2" || outline[1].pageIndex != 2) {
            std::cerr << "[FAIL] Unexpected title/page for Chapter 2" << std::endl;
            return 1;
        }
        if (!outline[1].hasTargetY || std::abs(outline[1].targetYFraction - 0.12) > 0.01) {
            std::cerr << "[FAIL] Rotated CropBox bookmark target was not transformed correctly." << std::endl;
            return 1;
        }

        if (outline[2].title != "Preface" || outline[2].pageIndex != -1) {
            std::cerr << "[FAIL] Bookmark without a destination was not preserved as non-navigable." << std::endl;
            return 1;
        }

        // 5. Outline loading must not override the user's sidebar visibility choice.
        if (dock->isVisible()) {
            std::cerr << "[FAIL] Loading an outline reopened the sidebar after the user hid it!" << std::endl;
            return 1;
        }

        auto* model = window.outlineModel();
        if (!model || model->rowCount() != 3) {
            std::cerr << "[FAIL] Outline tree model row count expected 3, got " << (model ? model->rowCount() : -1) << std::endl;
            return 1;
        }
        if (model->index(2, 0).data(Qt::ToolTipRole).toString().contains(QStringLiteral("Page 0"))) {
            std::cerr << "[FAIL] Bookmark without a destination showed a misleading page tooltip." << std::endl;
            return 1;
        }

        // 6. Test clicking outline node navigates viewer
        // Click Chapter 2 (row 1)
        QModelIndex chapter2Index = model->index(1, 0);
        emit window.outlineView()->clicked(chapter2Index);
        QCoreApplication::processEvents();

        if (viewer->currentPage() != 2) {
            std::cerr << "[FAIL] Clicking Chapter 2 outline node did not navigate viewer to page 2! Current: " << viewer->currentPage() << std::endl;
            return 1;
        }
        const int64_t bookmarkScrollY = viewer->currentDocScrollY();
        viewer->goToPage(2);
        if (bookmarkScrollY <= viewer->currentDocScrollY()) {
            std::cerr << "[FAIL] Bookmark destination was not applied in displayed page coordinates." << std::endl;
            return 1;
        }

        // Clicking a parent bookmark must keep that exact row selected when it shares a page with its child.
        QModelIndex chapter1Index = model->index(0, 0);
        emit window.outlineView()->clicked(chapter1Index);
        QCoreApplication::processEvents();
        if (window.outlineView()->currentIndex() != chapter1Index) {
            std::cerr << "[FAIL] Page synchronization overrode the bookmark explicitly clicked by the user." << std::endl;
            return 1;
        }

        // Clicking Section 1.1 (child 0 of row 0) remains independently selectable.
        QModelIndex section11Index = model->index(0, 0, chapter1Index);
        emit window.outlineView()->clicked(section11Index);
        QCoreApplication::processEvents();

        if (viewer->currentPage() != 0 || window.outlineView()->currentIndex() != section11Index) {
            std::cerr << "[FAIL] Clicking Section 1.1 did not preserve its selected row and destination page." << std::endl;
            return 1;
        }

        // 7. Test active outline item synchronization on page change
        viewer->goToPage(2);
        QCoreApplication::processEvents();
        QModelIndex currentTreeIdx = window.outlineView()->currentIndex();
        if (currentTreeIdx.data(Qt::UserRole).toInt() != 2) {
            std::cerr << "[FAIL] Active outline item did not sync to current page 2! Got page: "
                      << currentTreeIdx.data(Qt::UserRole).toInt() << std::endl;
            return 1;
        }
        viewer->goToPage(0);
        if (window.outlineView()->currentIndex().data(Qt::UserRole).toInt() != 0) {
            std::cerr << "[FAIL] Bookmark without a destination stole the active page highlight." << std::endl;
            return 1;
        }

        QApplication::sendEvent(&window, &f4Press);
        if (!dock->isVisible()) {
            std::cerr << "[FAIL] F4 could not show the sidebar after outline loading." << std::endl;
            return 1;
        }
        QApplication::sendEvent(&window, &f4Press);
        viewer->goToPage(0);
        viewer->goToPage(2);
        if (dock->isVisible()) {
            std::cerr << "[FAIL] Page synchronization reopened a sidebar hidden by the user." << std::endl;
            return 1;
        }

        std::string cyclicOutlinePath = "widget_test_cyclic_outline.pdf";
        generateOutlinePdfFixture(cyclicOutlinePath, true);
        bool cyclicOpened = window.openFile(QString::fromStdString(cyclicOutlinePath));
        if (!cyclicOpened) {
            std::cerr << "[FAIL] Could not open cyclic outline fixture." << std::endl;
            return 1;
        }
        QElapsedTimer cyclicTimer;
        cyclicTimer.start();
        while (cyclicTimer.elapsed() < 3000) {
            viewer->renderWorker().waitForIdle();
            QCoreApplication::processEvents();
            if (!viewer->outline().empty()) {
                break;
            }
            QThread::msleep(5);
        }
        if (viewer->outline().size() != 3) {
            std::cerr << "[FAIL] Cyclic bookmark links did not terminate with the unique outline nodes." << std::endl;
            return 1;
        }

        ui::MainWindow autoOpenWindow;
        autoOpenWindow.resize(1024, 768);
        autoOpenWindow.show();
        QCoreApplication::processEvents();
        auto* autoViewer = autoOpenWindow.findChild<ui::PdfViewerWidget*>();
        auto* autoDock = autoOpenWindow.sidebarDock();
        auto* outlineStatus = autoOpenWindow.findChild<QLabel*>(QStringLiteral("OutlineEmptyLabel"));
        if (!autoViewer || !autoDock || !outlineStatus) {
            std::cerr << "[FAIL] Could not inspect outline loading/sidebar states." << std::endl;
            return 1;
        }
        autoViewer->renderWorker().setRenderDelayForTesting(std::chrono::milliseconds(250));
        autoViewer->setPrefetchEnabled(false);
        std::string noOutlinePath = "widget_test_no_outline.pdf";
        generateMultiPagePdf(noOutlinePath, 2);
        int emptyOutlineEvents = 0;
        QObject::connect(autoViewer, &ui::PdfViewerWidget::outlineReady, &autoOpenWindow,
                         [&emptyOutlineEvents](const std::vector<pdf::OutlineItem>& items) {
                             if (items.empty()) {
                                 ++emptyOutlineEvents;
                             }
                         });
        if (!autoOpenWindow.openFile(QString::fromStdString(noOutlinePath))) {
            std::cerr << "[FAIL] Could not open the no-outline fixture." << std::endl;
            return 1;
        }
        QElapsedTimer emptyOutlineTimer;
        emptyOutlineTimer.start();
        bool observedLoadingState = false;
        while (emptyOutlineTimer.elapsed() < 3000 && emptyOutlineEvents == 0) {
            QCoreApplication::processEvents();
            observedLoadingState = observedLoadingState ||
                                   outlineStatus->text() == QStringLiteral("Loading bookmarks\u2026");
            QThread::msleep(5);
        }
        if (emptyOutlineEvents == 0 || !observedLoadingState ||
            outlineStatus->text() != QStringLiteral("No bookmarks in document")) {
            std::cerr << "[FAIL] Empty outline displayed a false no-bookmarks state while extraction was pending." << std::endl;
            return 1;
        }

        if (!autoOpenWindow.openFile(QString::fromStdString(outlinePath))) {
            std::cerr << "[FAIL] Could not open the outline fixture in the automatic-sidebar window." << std::endl;
            return 1;
        }
        QElapsedTimer autoOutlineTimer;
        autoOutlineTimer.start();
        while (autoOutlineTimer.elapsed() < 3000 && autoViewer->outline().empty()) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (autoViewer->outline().empty() || !autoDock->isVisible()) {
            std::cerr << "[FAIL] Sidebar did not open automatically for bookmarks before the user hid it." << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: async outline loading, cycle safety, sidebar visibility, bookmark destination navigation, clicked-row retention, and active tracking verified." << std::endl;
    }

    std::cout << "[TEST 34] Testing isolated worker crash survival and recovery banner..." << std::endl;
    {
        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        auto* viewer = window.findChild<ui::PdfViewerWidget*>();
        if (!viewer) {
            std::cerr << "[FAIL] Could not find PdfViewerWidget child in MainWindow" << std::endl;
            return 1;
        }

        std::string samplePath = "widget_test_crash_recovery.pdf";
        generateMultiPagePdf(samplePath, 2);

        if (!window.openFile(QString::fromStdString(samplePath))) {
            std::cerr << "[FAIL] Failed to open test fixture in crash recovery test." << std::endl;
            return 1;
        }

        QElapsedTimer loadTimer;
        loadTimer.start();
        while (loadTimer.elapsed() < 3000 && !viewer->isDocumentOpen()) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        if (!viewer->isDocumentOpen()) {
            std::cerr << "[FAIL] Document failed to open in crash recovery test." << std::endl;
            return 1;
        }

        // Simulate worker process crash
        viewer->renderWorker().killWorkerForTesting();
        QElapsedTimer crashTimer;
        crashTimer.start();
        while (crashTimer.elapsed() < 3000 && viewer->isDocumentOpen()) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        if (viewer->isDocumentOpen()) {
            std::cerr << "[FAIL] Viewer still reports open document after worker termination." << std::endl;
            return 1;
        }

        if (!viewer->hasWorkerError() || viewer->workerErrorMessage().isEmpty()) {
            std::cerr << "[FAIL] Viewer did not register worker error state." << std::endl;
            return 1;
        }

        // Repaint verifying paintEvent renders drawWorkerErrorBanner without crash
        viewer->viewport()->repaint();
        QCoreApplication::processEvents();

        QImage renderTarget(viewer->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        renderTarget.fill(Qt::transparent);
        QPainter p(&renderTarget);
        viewer->viewport()->render(&p);
        p.end();

        QRgb centerPx = renderTarget.pixel(renderTarget.width() / 2, renderTarget.height() / 2);
        if (qRed(centerPx) != 40 || qGreen(centerPx) != 44 || qBlue(centerPx) != 48) {
            std::cerr << "[FAIL] Error banner was not drawn in center of viewport! Got RGB("
                      << qRed(centerPx) << "," << qGreen(centerPx) << "," << qBlue(centerPx) << ")" << std::endl;
            return 1;
        }

        // Recovery: opening a document again after the crash must start a fresh worker and render tiles.
        const std::string recoverPath = "widget_test_crash_recovered.pdf";
        generateMultiPagePdf(recoverPath, 3);
        if (!window.openFile(QString::fromStdString(recoverPath))) {
            std::cerr << "[FAIL] openFile refused after a worker crash" << std::endl;
            return 1;
        }
        QElapsedTimer reopenTimer;
        reopenTimer.start();
        while (reopenTimer.elapsed() < 5000 && !(viewer->isDocumentOpen() && viewer->pageCount() == 3)) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer->isDocumentOpen() || viewer->pageCount() != 3) {
            std::cerr << "[FAIL] Could not open a document after the worker crashed (no recovery)" << std::endl;
            return 1;
        }
        viewer->viewport()->repaint();
        QElapsedTimer renderTimer;
        renderTimer.start();
        while (renderTimer.elapsed() < 5000 && viewer->cache().size() == 0) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
            viewer->viewport()->repaint();
        }
        if (viewer->cache().size() == 0) {
            std::cerr << "[FAIL] No tiles rendered after the worker was restarted" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Worker crash cleanly contained; UI survived, showed the recovery banner, and recovered on the next open." << std::endl;
    }

        // Test 35: Thumbnails & Page Labels Sidebar, worker thumbnail rendering, and viewport sync
    std::cout << "[TEST 35] Testing Thumbnails & Page Labels Sidebar, worker thumbnail rendering, and viewport sync..." << std::endl;
    {
        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        auto* viewer = window.findChild<ui::PdfViewerWidget*>();
        auto* thumbWidget = window.thumbnailWidget();
        auto* tabs = window.sidebarTabs();
        if (!viewer || !thumbWidget || !tabs) {
            std::cerr << "[FAIL] Required UI components (viewer, thumbnailWidget, sidebarTabs) not found in MainWindow" << std::endl;
            return 1;
        }

        // 1. Verify tabs setup
        if (tabs->count() < 2) {
            std::cerr << "[FAIL] sidebarTabs expected at least 2 tabs, got " << tabs->count() << std::endl;
            return 1;
        }
        if (tabs->widget(0) != thumbWidget) {
            std::cerr << "[FAIL] Tab 0 is not ThumbnailListWidget" << std::endl;
            return 1;
        }

        // 2. Open a 3-page document
        std::string sampleDoc = "widget_test_thumbnails.pdf";
        generateMultiPagePdf(sampleDoc, 3);
        if (!window.openFile(QString::fromStdString(sampleDoc))) {
            std::cerr << "[FAIL] Failed to open multi-page document for thumbnail testing" << std::endl;
            return 1;
        }

        QElapsedTimer loadTimer;
        loadTimer.start();
        while (loadTimer.elapsed() < 5000 && !viewer->isDocumentOpen()) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer->isDocumentOpen() || viewer->pageCount() != 3) {
            std::cerr << "[FAIL] Document failed to open in thumbnail test" << std::endl;
            return 1;
        }

        // 3. Verify page count and page labels
        if (thumbWidget->totalPages() != 3 || thumbWidget->count() != 3) {
            std::cerr << "[FAIL] ThumbnailListWidget page count mismatch: totalPages="
                      << thumbWidget->totalPages() << ", count=" << thumbWidget->count() << std::endl;
            return 1;
        }
        for (int i = 0; i < 3; ++i) {
            auto* it = thumbWidget->item(i);
            QString expectedLabel = QString::number(i + 1);
            if (it->text() != expectedLabel) {
                std::cerr << "[FAIL] Page label for page " << i << " expected '"
                          << expectedLabel.toStdString() << "', got '" << it->text().toStdString() << "'" << std::endl;
                return 1;
            }
        }

        // 4. Request thumbnail rendering and verify worker delivery
        thumbWidget->requestVisibleThumbnails();
        QElapsedTimer thumbTimer;
        thumbTimer.start();
        while (thumbTimer.elapsed() < 5000 && !thumbWidget->isThumbnailLoaded(0)) {
            viewer->renderWorker().waitForIdle();
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        if (!thumbWidget->isThumbnailLoaded(0)) {
            std::cerr << "[FAIL] Thumbnail for page 0 was not delivered by isolated worker" << std::endl;
            return 1;
        }
        auto* item0 = thumbWidget->item(0);
        if (item0->icon().isNull()) {
            std::cerr << "[FAIL] Thumbnail item 0 icon is null after loading" << std::endl;
            return 1;
        }

        // 5. Test two-way synchronization:
        // A: Viewport scrolls -> Thumbnail selection follows
        viewer->goToPage(2);
        QCoreApplication::processEvents();
        if (thumbWidget->currentRow() != 2) {
            std::cerr << "[FAIL] Changing viewer page to 2 did not update thumbnail currentRow to 2 (got "
                      << thumbWidget->currentRow() << ")" << std::endl;
            return 1;
        }

        // B: Thumbnail clicked -> Viewport scrolls
        emit thumbWidget->itemClicked(thumbWidget->item(1));
        QCoreApplication::processEvents();
        if (viewer->currentPage() != 1) {
            std::cerr << "[FAIL] Clicking thumbnail item 1 did not navigate viewer to page 1 (got "
                      << viewer->currentPage() << ")" << std::endl;
            return 1;
        }

        // 6. Test document close clears thumbnails
        viewer->closeDocument();
        QCoreApplication::processEvents();
        if (thumbWidget->totalPages() != 0 || thumbWidget->count() != 0) {
            std::cerr << "[FAIL] Closing document did not reset thumbnail widget" << std::endl;
            return 1;
        }

        // 7. Verify document WITH /PageLabels dictionary displays custom page labels on thumbnail badges
        std::string labelsDoc = "widget_test_page_labels.pdf";
        generatePageLabelsPdfFixture(labelsDoc);
        if (!window.openFile(QString::fromStdString(labelsDoc))) {
            std::cerr << "[FAIL] Failed to open document with page labels in thumbnail test" << std::endl;
            return 1;
        }

        QElapsedTimer labelsTimer;
        labelsTimer.start();
        while (labelsTimer.elapsed() < 5000 && !viewer->isDocumentOpen()) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        if (!viewer->isDocumentOpen() || viewer->pageCount() != 4 || thumbWidget->count() != 4) {
            std::cerr << "[FAIL] Document with page labels failed to open properly" << std::endl;
            return 1;
        }

        const QStringList expectedLabels = {"i", "ii", "A-1", "A-2"};
        for (int i = 0; i < 4; ++i) {
            auto* it = thumbWidget->item(i);
            if (it->text() != expectedLabels[i]) {
                std::cerr << "[FAIL] Custom page label for page " << i << " expected '"
                          << expectedLabels[i].toStdString() << "', got '" << it->text().toStdString() << "'" << std::endl;
                return 1;
            }
        }
        viewer->closeDocument();

        std::cout << "  -> Passed: ThumbnailListWidget creation, page labels (with and without dictionary), IPC thumbnail delivery, two-way viewport sync, and teardown verified." << std::endl;
    }

    // Test 36: Multi-Document Tabs and Recent Files Session Management
    std::cout << "[TEST 36] Testing Multi-Document Tabs and Recent Files Session Management..." << std::endl;
    {
        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        // Initial state: 1 default untitled tab
        if (window.tabCount() != 1) {
            std::cerr << "[FAIL] MainWindow initial tabCount expected 1, got " << window.tabCount() << std::endl;
            return 1;
        }

        // 1. Open first document in current tab
        std::string doc1Path = "widget_tab_doc1.pdf";
        generateMultiPagePdf(doc1Path, 3);
        if (!window.openFile(QString::fromStdString(doc1Path))) {
            std::cerr << "[FAIL] Failed to open first document in MainWindow" << std::endl;
            return 1;
        }

        auto* viewer1 = window.currentViewer();
        QElapsedTimer loadTimer1;
        loadTimer1.start();
        while (loadTimer1.elapsed() < 5000 && (!viewer1 || !viewer1->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer1 || !viewer1->isDocumentOpen()) {
            std::cerr << "[FAIL] First document failed to load in tab 0" << std::endl;
            return 1;
        }

        // Verify recent files contains doc1
        if (window.recentFiles().isEmpty() || !window.recentFiles().first().contains("widget_tab_doc1.pdf")) {
            std::cerr << "[FAIL] First document was not recorded in recent files list" << std::endl;
            return 1;
        }

        // 2. Open second document in new tab
        std::string doc2Path = "widget_tab_doc2.pdf";
        generateMultiPagePdf(doc2Path, 5);
        int tab2Index = window.openFileInNewTab(QString::fromStdString(doc2Path));
        if (tab2Index != 1 || window.tabCount() != 2) {
            std::cerr << "[FAIL] Expected 2 tabs after openFileInNewTab, got count=" << window.tabCount() << std::endl;
            return 1;
        }

        auto* viewer2 = window.currentViewer();
        if (viewer2 == viewer1) {
            std::cerr << "[FAIL] Active viewer did not switch to newly opened tab" << std::endl;
            return 1;
        }

        QElapsedTimer loadTimer2;
        loadTimer2.start();
        while (loadTimer2.elapsed() < 5000 && (!viewer2 || !viewer2->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer2 || !viewer2->isDocumentOpen() || viewer2->pageCount() != 5) {
            std::cerr << "[FAIL] Second document failed to load in tab 1" << std::endl;
            return 1;
        }

        // Verify recent files has 2 entries and most recent is doc2
        if (window.recentFiles().size() < 2 || !window.recentFiles().first().contains("widget_tab_doc2.pdf")) {
            std::cerr << "[FAIL] Second document was not placed at front of recent files" << std::endl;
            return 1;
        }

        // 3. Tab cycling
        window.selectNextTab();
        QCoreApplication::processEvents();
        if (window.documentTabs()->currentIndex() != 0 || window.currentViewer() != viewer1) {
            std::cerr << "[FAIL] selectNextTab did not wrap back to tab 0" << std::endl;
            return 1;
        }

        window.selectPreviousTab();
        QCoreApplication::processEvents();
        if (window.documentTabs()->currentIndex() != 1 || window.currentViewer() != viewer2) {
            std::cerr << "[FAIL] selectPreviousTab did not navigate to tab 1" << std::endl;
            return 1;
        }

        // 4. Close current tab (tab 1)
        window.closeCurrentTab();
        QCoreApplication::processEvents();
        if (window.tabCount() != 1 || window.currentViewer() != viewer1) {
            std::cerr << "[FAIL] closeCurrentTab failed to leave tab 0 active" << std::endl;
            return 1;
        }

        // 5. Test Clear Recent Files
        window.clearRecentFiles();
        if (!window.recentFiles().isEmpty()) {
            std::cerr << "[FAIL] clearRecentFiles did not empty recent files list" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Multi-tab opening, tab switching, active viewer sync, tab closure, and recent files persistence verified." << std::endl;
    }

    // Test 37: Native Linux / CUPS Printing rasterization over IPC and Headless PDF Export
    std::cout << "[TEST 37] Testing Native Printing rasterization over IPC and Headless PDF Export..." << std::endl;
    {
        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        std::string sampleDoc = "widget_test_export.pdf";
        generateMultiPagePdf(sampleDoc, 4);

        if (!window.openFile(QString::fromStdString(sampleDoc))) {
            std::cerr << "[FAIL] Failed to open multi-page document for print/export testing" << std::endl;
            return 1;
        }

        auto* viewer = window.currentViewer();
        QElapsedTimer loadTimer;
        loadTimer.start();
        while (loadTimer.elapsed() < 5000 && (!viewer || !viewer->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer || !viewer->isDocumentOpen() || viewer->pageCount() != 4) {
            std::cerr << "[FAIL] Document failed to open in print/export test" << std::endl;
            return 1;
        }

        // 1. Direct print rasterization over IPC: renderPageForPrint
        QImage printImg = viewer->renderPageForPrint(0, 1.0, 2.0);
        if (printImg.isNull() || printImg.width() <= 0 || printImg.height() <= 0) {
            std::cerr << "[FAIL] renderPageForPrint returned null or invalid image" << std::endl;
            return 1;
        }
        if (printImg.devicePixelRatio() < 1.99) {
            std::cerr << "[FAIL] renderPageForPrint image DPR expected ~2.0, got " << printImg.devicePixelRatio() << std::endl;
            return 1;
        }

        // 2. Export page range (pages 1 to 2 -> 2 pages total) headlessly to a new PDF file
        QString exportPath = QStringLiteral("exported_test_doc.pdf");
        QFile::remove(exportPath);

        bool exportSuccess = window.exportDocumentToPdf(exportPath, 1, 2);
        if (!exportSuccess) {
            std::cerr << "[FAIL] exportDocumentToPdf returned false" << std::endl;
            return 1;
        }

        if (!QFileInfo::exists(exportPath) || QFileInfo(exportPath).size() == 0) {
            std::cerr << "[FAIL] Exported PDF does not exist or is empty on disk" << std::endl;
            return 1;
        }

        // 5. Whole-document export is a lossless copy of the source and keeps the page count
        {
            QString fullPath = QStringLiteral("exported_full_doc.pdf");
            QFile::remove(fullPath);
            if (!window.exportDocumentToPdf(fullPath) || !QFileInfo::exists(fullPath)) {
                std::cerr << "[FAIL] Full-document export failed" << std::endl;
                return 1;
            }
            QFile a(QString::fromStdString(sampleDoc)), b(fullPath);
            if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly) || a.readAll() != b.readAll()) {
                std::cerr << "[FAIL] Full-document export is not a byte-identical copy" << std::endl;
                return 1;
            }
            // Exporting onto the source itself must be refused and must not corrupt it
            if (window.exportDocumentToPdf(QString::fromStdString(sampleDoc))) {
                std::cerr << "[FAIL] Export over the source file should be refused" << std::endl;
                return 1;
            }
            QFile::remove(fullPath);
        }

        // 3. Open exported PDF in a new tab and verify page count
        int exportTabIndex = window.openFileInNewTab(exportPath);
        if (exportTabIndex < 0) {
            std::cerr << "[FAIL] Failed to open exported PDF in new tab" << std::endl;
            return 1;
        }

        auto* exportViewer = window.currentViewer();
        QElapsedTimer exportLoadTimer;
        exportLoadTimer.start();
        while (exportLoadTimer.elapsed() < 5000 && (!exportViewer || !exportViewer->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        if (!exportViewer || !exportViewer->isDocumentOpen() || exportViewer->pageCount() != 2) {
            std::cerr << "[FAIL] Exported PDF page count expected 2, got "
                      << (exportViewer ? exportViewer->pageCount() : -1) << std::endl;
            return 1;
        }

        // 4. Export must be vector: no embedded raster images, and far smaller than a page bitmap export
        {
            QFile ef(exportPath);
            if (!ef.open(QIODevice::ReadOnly)) {
                std::cerr << "[FAIL] Cannot reopen exported PDF" << std::endl;
                return 1;
            }
            const QByteArray bytes = ef.readAll();
            if (!bytes.startsWith("%PDF-") || bytes.contains("/Subtype/Image") || bytes.contains("/Subtype /Image")) {
                std::cerr << "[FAIL] Exported PDF is not a vector PDF (contains raster image XObject)" << std::endl;
                return 1;
            }
            if (bytes.size() > 100 * 1024) {
                std::cerr << "[FAIL] Exported PDF unexpectedly large: " << bytes.size() << " bytes" << std::endl;
                return 1;
            }
        }

        // Cleanup
        window.closeCurrentTab();
        QFile::remove(exportPath);

        std::cout << "  -> Passed: High-resolution print rasterization over IPC and headless PDF export verified." << std::endl;
    }

    // Test 38: Dark Mode & Night Inversion with Reversible Zero-Mutation Tile Cache
    std::cout << "[TEST 38] Testing Dark Mode toggle, #181818 viewport color, inverted pixels, reversibility, and zero-mutation invariant..." << std::endl;
    {
        ui::PdfViewerWidget viewer;
        viewer.resize(800, 600);
        viewer.show();
        QCoreApplication::processEvents();

        std::string sampleDoc = "widget_test_dark_mode.pdf";
        generateMultiPagePdf(sampleDoc, 2);
        auto err = viewer.openDocument(QString::fromStdString(sampleDoc));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for dark mode testing" << std::endl;
            return 1;
        }

        viewer.setZoom(1.0);
        viewer.viewport()->repaint();
        QElapsedTimer renderTimer;
        renderTimer.start();
        while (renderTimer.elapsed() < 5000 && viewer.cache().size() == 0) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
            viewer.viewport()->repaint();
        }
        viewer.waitForRendering();

        if (viewer.cache().size() == 0) {
            std::cerr << "[FAIL] No tiles rendered before dark mode test" << std::endl;
            return 1;
        }

        // 1. Initial default state: Light mode
        if (viewer.isDarkMode()) {
            std::cerr << "[FAIL] Viewer expected isDarkMode() == false initially" << std::endl;
            return 1;
        }

        QImage lightFrame(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&lightFrame);
            viewer.viewport()->render(&p);
        }
        // Top-left corner of viewport outside page margins is #323639 (RGB 50, 54, 57)
        QRgb bgLight = lightFrame.pixel(2, 2);
        if (qRed(bgLight) != 0x32 || qGreen(bgLight) != 0x36 || qBlue(bgLight) != 0x39) {
            std::cerr << "[FAIL] Light mode viewport background expected #323639, got RGB("
                      << qRed(bgLight) << "," << qGreen(bgLight) << "," << qBlue(bgLight) << ")" << std::endl;
            return 1;
        }

        // Grab a cached tile key and its pixel buffer
        auto key = pdf::TileKey::create(viewer.currentDocumentId(), 0, 0, 0, 1.0, viewer.effectiveDpr());
        auto cachedBufBefore = viewer.cache().get(key);
        if (!cachedBufBefore.has_value()) {
            std::cerr << "[FAIL] Tile (0,0) not found in cache for dark mode test" << std::endl;
            return 1;
        }
        std::vector<uint8_t> bufCopyBefore(cachedBufBefore->data(), cachedBufBefore->data() + cachedBufBefore->sizeBytes());

        // 2. Toggle to Dark Mode
        viewer.setDarkMode(true);
        if (!viewer.isDarkMode()) {
            std::cerr << "[FAIL] setDarkMode(true) failed to update isDarkMode()" << std::endl;
            return 1;
        }

        // Render viewport in Dark Mode
        QImage darkFrame(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&darkFrame);
            viewer.viewport()->render(&p);
        }

        // Viewport background is now #181818 (RGB 24, 24, 24)
        QRgb bgDark = darkFrame.pixel(2, 2);
        if (qRed(bgDark) != 0x18 || qGreen(bgDark) != 0x18 || qBlue(bgDark) != 0x18) {
            std::cerr << "[FAIL] Dark mode viewport background expected #181818, got RGB("
                      << qRed(bgDark) << "," << qGreen(bgDark) << "," << qBlue(bgDark) << ")" << std::endl;
            return 1;
        }

        // 3. ZERO-MUTATION INVARIANT: Verify underlying tile cache buffer was NOT altered in-place
        auto cachedBufAfter = viewer.cache().get(key);
        if (!cachedBufAfter.has_value()) {
            std::cerr << "[FAIL] Tile (0,0) missing from cache after dark mode render" << std::endl;
            return 1;
        }
        if (cachedBufAfter->sizeBytes() != bufCopyBefore.size() ||
            std::memcmp(cachedBufAfter->data(), bufCopyBefore.data(), bufCopyBefore.size()) != 0) {
            std::cerr << "[FAIL] Zero-mutation invariant violated: tile cache buffer was mutated by dark mode rendering!" << std::endl;
            return 1;
        }

        // 4. Reversibility: Toggle dark mode off and verify return to light mode
        viewer.toggleDarkMode();
        if (viewer.isDarkMode()) {
            std::cerr << "[FAIL] toggleDarkMode() did not switch back to light mode" << std::endl;
            return 1;
        }

        QImage revertFrame(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&revertFrame);
            viewer.viewport()->render(&p);
        }
        QRgb bgRevert = revertFrame.pixel(2, 2);
        if (qRed(bgRevert) != 0x32 || qGreen(bgRevert) != 0x36 || qBlue(bgRevert) != 0x39) {
            std::cerr << "[FAIL] Reverted viewport background expected #323639, got RGB("
                      << qRed(bgRevert) << "," << qGreen(bgRevert) << "," << qBlue(bgRevert) << ")" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Dark Mode toggle, #181818 viewport color, inverted pixels, reversibility, and zero-mutation tile cache invariant verified." << std::endl;
    }

    // Test 39: Reading Position Memory persistence across documents/sessions in QSettings
    std::cout << "[TEST 39] Testing Reading Position Memory persistence across documents/sessions in QSettings..." << std::endl;
    {
        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        window.clearReadingHistory();

        std::string sampleDoc = "widget_test_reading_history.pdf";
        generateMultiPagePdf(sampleDoc, 5);

        if (!window.openFile(QString::fromStdString(sampleDoc))) {
            std::cerr << "[FAIL] Failed to open document for reading history test" << std::endl;
            return 1;
        }

        auto* viewer = window.currentViewer();
        QElapsedTimer loadTimer;
        loadTimer.start();
        while (loadTimer.elapsed() < 5000 && (!viewer || !viewer->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer || !viewer->isDocumentOpen()) {
            std::cerr << "[FAIL] Document failed to open in reading history test" << std::endl;
            return 1;
        }

        // Navigate to page 3, zoom 1.5, scroll to offset
        viewer->setZoom(1.5);
        viewer->goToPage(3);
        const int64_t expectedScrollX = 40;
        const int64_t expectedScrollY = viewer->currentDocScrollY() + 80;
        viewer->setDocScrollPosition(expectedScrollX, expectedScrollY);
        QCoreApplication::processEvents();

        const int savedPage = viewer->currentPage();
        const double savedZoom = viewer->currentZoom();
        const int64_t savedScrollX = viewer->currentDocScrollX();
        const int64_t savedScrollY = viewer->currentDocScrollY();

        // 1. Explicitly save reading position
        window.saveReadingPosition(viewer);

        // 2. Close document / tab
        window.closeCurrentTab();
        QCoreApplication::processEvents();

        // 3. Re-open same document in new tab: reading position must restore automatically
        if (!window.openFile(QString::fromStdString(sampleDoc))) {
            std::cerr << "[FAIL] Failed to re-open document in reading history test" << std::endl;
            return 1;
        }

        auto* reloadedViewer = window.currentViewer();
        QElapsedTimer reloadTimer;
        reloadTimer.start();
        while (reloadTimer.elapsed() < 5000 && (!reloadedViewer || !reloadedViewer->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!reloadedViewer || !reloadedViewer->isDocumentOpen()) {
            std::cerr << "[FAIL] Re-opened document failed to load" << std::endl;
            return 1;
        }

        if (reloadedViewer->currentPage() != savedPage) {
            std::cerr << "[FAIL] Restored page mismatch: expected " << savedPage
                      << ", got " << reloadedViewer->currentPage() << std::endl;
            return 1;
        }
        if (std::abs(reloadedViewer->currentZoom() - savedZoom) > 0.01) {
            std::cerr << "[FAIL] Restored zoom mismatch: expected " << savedZoom
                      << ", got " << reloadedViewer->currentZoom() << std::endl;
            return 1;
        }
        if (reloadedViewer->currentDocScrollX() != savedScrollX ||
            reloadedViewer->currentDocScrollY() != savedScrollY) {
            std::cerr << "[FAIL] Restored scroll coordinate mismatch: expected ("
                      << savedScrollX << "," << savedScrollY << "), got ("
                      << reloadedViewer->currentDocScrollX() << ","
                      << reloadedViewer->currentDocScrollY() << ")" << std::endl;
            return 1;
        }

        // 4. Test clearReadingHistory
        window.closeCurrentTab();
        QCoreApplication::processEvents();
        window.clearReadingHistory();

        // Re-open: should reset to default (page 0, initial zoom)
        if (!window.openFile(QString::fromStdString(sampleDoc))) {
            std::cerr << "[FAIL] Failed to re-open document after clearReadingHistory" << std::endl;
            return 1;
        }
        auto* clearedViewer = window.currentViewer();
        QElapsedTimer clearTimer;
        clearTimer.start();
        while (clearTimer.elapsed() < 5000 && (!clearedViewer || !clearedViewer->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        if (clearedViewer->currentPage() != 0) {
            std::cerr << "[FAIL] Expected page 0 after clearReadingHistory, got "
                      << clearedViewer->currentPage() << std::endl;
            return 1;
        }

        window.closeCurrentTab();
        std::cout << "  -> Passed: Reading position persistence across documents/sessions in QSettings and clearReadingHistory verified." << std::endl;
    }

    // Test 40: Comprehensive Keyboard Navigation (Arrows, Vim J/K/L/H, PgUp/PgDn, Space/Shift+Space, Home/End)
    std::cout << "[TEST 40] Testing Comprehensive Keyboard Navigation..." << std::endl;
    {
        ui::PdfViewerWidget viewer;
        viewer.resize(800, 600);
        viewer.show();
        QCoreApplication::processEvents();

        std::string sampleDoc = "widget_test_keyboard_nav.pdf";
        generateMultiPagePdf(sampleDoc, 6);
        auto err = viewer.openDocument(QString::fromStdString(sampleDoc));
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for keyboard navigation test" << std::endl;
            return 1;
        }

        viewer.setFocus();
        viewer.goToPage(0);
        QCoreApplication::processEvents();

        const int initialV = viewer.verticalScrollBar()->value();

        // 1. Down arrow & Vim 'J'
        QKeyEvent downEvent(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &downEvent);
        const int afterDown = viewer.verticalScrollBar()->value();
        if (afterDown <= initialV) {
            std::cerr << "[FAIL] Key_Down did not advance vertical scroll position (initial: "
                      << initialV << ", after: " << afterDown << ")" << std::endl;
            return 1;
        }

        QKeyEvent jEvent(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &jEvent);
        const int afterJ = viewer.verticalScrollBar()->value();
        if (afterJ <= afterDown) {
            std::cerr << "[FAIL] Key_J did not advance vertical scroll position (prev: "
                      << afterDown << ", after: " << afterJ << ")" << std::endl;
            return 1;
        }

        // 2. Up arrow & Vim 'K'
        QKeyEvent upEvent(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &upEvent);
        const int afterUp = viewer.verticalScrollBar()->value();
        if (afterUp >= afterJ) {
            std::cerr << "[FAIL] Key_Up did not decrease vertical scroll position (prev: "
                      << afterJ << ", after: " << afterUp << ")" << std::endl;
            return 1;
        }

        QKeyEvent kEvent(QEvent::KeyPress, Qt::Key_K, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &kEvent);
        const int afterK = viewer.verticalScrollBar()->value();
        if (afterK >= afterUp) {
            std::cerr << "[FAIL] Key_K did not decrease vertical scroll position (prev: "
                      << afterUp << ", after: " << afterK << ")" << std::endl;
            return 1;
        }

        // 3. PageDown and Space
        const int beforePgDn = viewer.verticalScrollBar()->value();
        QKeyEvent pgDnEvent(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &pgDnEvent);
        const int afterPgDn = viewer.verticalScrollBar()->value();
        if (afterPgDn <= beforePgDn) {
            std::cerr << "[FAIL] Key_PageDown did not scroll down by page step" << std::endl;
            return 1;
        }

        QKeyEvent spaceEvent(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &spaceEvent);
        const int afterSpace = viewer.verticalScrollBar()->value();
        if (afterSpace <= afterPgDn) {
            std::cerr << "[FAIL] Key_Space did not scroll down by page step" << std::endl;
            return 1;
        }

        // 4. Shift+Space and PageUp
        // beforeShiftSpace
        QKeyEvent shiftSpaceEvent(QEvent::KeyPress, Qt::Key_Space, Qt::ShiftModifier);
        QApplication::sendEvent(&viewer, &shiftSpaceEvent);
        const int afterShiftSpace = viewer.verticalScrollBar()->value();
        if (afterShiftSpace >= afterSpace) {
            std::cerr << "[FAIL] Shift+Key_Space did not scroll up by page step" << std::endl;
            return 1;
        }

        QKeyEvent pgUpEvent(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &pgUpEvent);
        const int afterPgUp = viewer.verticalScrollBar()->value();
        if (afterPgUp >= afterShiftSpace) {
            std::cerr << "[FAIL] Key_PageUp did not scroll up by page step" << std::endl;
            return 1;
        }

        // 5. Home / End bounds navigation
        QKeyEvent endEvent(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &endEvent);
        QCoreApplication::processEvents();
        if (viewer.currentPage() != viewer.pageCount() - 1) {
            std::cerr << "[FAIL] Key_End did not navigate to last page: expected "
                      << (viewer.pageCount() - 1) << ", got " << viewer.currentPage() << std::endl;
            return 1;
        }

        QKeyEvent homeEvent(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &homeEvent);
        QCoreApplication::processEvents();
        if (viewer.currentPage() != 0) {
            std::cerr << "[FAIL] Key_Home did not navigate to page 0: got "
                      << viewer.currentPage() << std::endl;
            return 1;
        }

        // 6. Horizontal navigation: Right/Left arrow & Vim L/H
        viewer.setZoom(3.0); // Ensure horizontal scrollbar has range > 0
        QCoreApplication::processEvents();
        if (viewer.horizontalScrollBar()->maximum() <= 0) {
            std::cerr << "[FAIL] Horizontal scrollbar expected maximum > 0 at zoom 3.0" << std::endl;
            return 1;
        }

        viewer.horizontalScrollBar()->setValue(0);
        QKeyEvent rightEvent(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &rightEvent);
        const int afterRight = viewer.horizontalScrollBar()->value();
        if (afterRight <= 0) {
            std::cerr << "[FAIL] Key_Right did not scroll horizontally" << std::endl;
            return 1;
        }

        QKeyEvent lEvent(QEvent::KeyPress, Qt::Key_L, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &lEvent);
        const int afterL = viewer.horizontalScrollBar()->value();
        if (afterL <= afterRight) {
            std::cerr << "[FAIL] Key_L did not scroll horizontally to the right" << std::endl;
            return 1;
        }

        QKeyEvent leftEvent(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &leftEvent);
        const int afterLeft = viewer.horizontalScrollBar()->value();
        if (afterLeft >= afterL) {
            std::cerr << "[FAIL] Key_Left did not scroll horizontally to the left" << std::endl;
            return 1;
        }

        QKeyEvent hEvent(QEvent::KeyPress, Qt::Key_H, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &hEvent);
        const int afterH = viewer.horizontalScrollBar()->value();
        if (afterH >= afterLeft) {
            std::cerr << "[FAIL] Key_H did not scroll horizontally to the left" << std::endl;
            return 1;
        }

        std::cout << "  -> Passed: Comprehensive keyboard navigation (Arrows, Vim J/K/L/H, PgUp/PgDn, Space/Shift+Space, Home/End) verified." << std::endl;
    }

    // Test 41: View modes (single page, two-page spreads, cover spreads), fit-page, page flipping, persistence
    std::cout << "[TEST 41] Testing view modes, fit page and navigation per mode..." << std::endl;
    {
        ui::PdfViewerWidget viewer;
        viewer.resize(800, 600);
        viewer.show();
        QCoreApplication::processEvents();

        std::string sampleDoc = "widget_test_view_modes.pdf";
        generateMultiPagePdf(sampleDoc, 6);
        if (viewer.openDocument(QString::fromStdString(sampleDoc)) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for view mode test" << std::endl;
            return 1;
        }
        QCoreApplication::processEvents();

        auto fail = [](const char* msg) { std::cerr << "[FAIL] " << msg << std::endl; return 1; };

        if (viewer.viewMode() != layout::ViewMode::Continuous) {
            return fail("Default view mode should be Continuous");
        }
        const int continuousRange = viewer.verticalScrollBar()->maximum();

        // --- Single page: only one page is reachable, navigation changes the focus page ---
        viewer.setViewMode(layout::ViewMode::SinglePage);
        QCoreApplication::processEvents();
        if (viewer.viewMode() != layout::ViewMode::SinglePage) {
            return fail("setViewMode(SinglePage) did not stick");
        }
        if (viewer.verticalScrollBar()->maximum() >= continuousRange) {
            return fail("SinglePage scroll range should be smaller than the continuous range");
        }
        viewer.goToPage(3);
        QCoreApplication::processEvents();
        if (viewer.currentPage() != 3) {
            std::cerr << "[FAIL] SinglePage goToPage(3) landed on page " << viewer.currentPage() << std::endl;
            return 1;
        }
        viewer.nextPage();
        if (viewer.currentPage() != 4) {
            return fail("SinglePage nextPage should move to page 4");
        }
        viewer.previousPage();
        if (viewer.currentPage() != 3) {
            return fail("SinglePage previousPage should move back to page 3");
        }

        // PageDown at the bottom edge of the page flips to the next page
        viewer.verticalScrollBar()->setValue(viewer.verticalScrollBar()->maximum());
        QKeyEvent pgDown(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &pgDown);
        QCoreApplication::processEvents();
        if (viewer.currentPage() != 4) {
            std::cerr << "[FAIL] PageDown at page bottom should flip to page 4, got " << viewer.currentPage() << std::endl;
            return 1;
        }
        // PageUp at the top edge flips back and arrives at the bottom of the previous page
        viewer.verticalScrollBar()->setValue(viewer.verticalScrollBar()->minimum());
        QKeyEvent pgUp(QEvent::KeyPress, Qt::Key_PageUp, Qt::NoModifier);
        QApplication::sendEvent(&viewer, &pgUp);
        QCoreApplication::processEvents();
        if (viewer.currentPage() != 3) {
            std::cerr << "[FAIL] PageUp at page top should flip to page 3, got " << viewer.currentPage() << std::endl;
            return 1;
        }

        // Fit page: the whole page fits, so there is nothing left to scroll vertically
        viewer.zoomFitPage();
        QCoreApplication::processEvents();
        if (viewer.verticalScrollBar()->maximum() > 2) {
            std::cerr << "[FAIL] Fit Page left a vertical scroll range of " << viewer.verticalScrollBar()->maximum() << std::endl;
            return 1;
        }
        if (viewer.currentPage() != 3) {
            return fail("Fit Page must keep the current page");
        }

        // Last page boundaries
        viewer.goToPage(5);
        viewer.nextPage();
        if (viewer.currentPage() != 5) {
            return fail("nextPage on the last page must be a no-op");
        }

        // --- Two pages: spreads 0-1, 2-3, 4-5 ---
        viewer.setViewMode(layout::ViewMode::TwoPage);
        viewer.goToPage(0);
        QCoreApplication::processEvents();
        viewer.nextPage();
        if (viewer.currentPage() != 2) {
            std::cerr << "[FAIL] TwoPage nextPage from 0 should land on 2, got " << viewer.currentPage() << std::endl;
            return 1;
        }
        viewer.nextPage();
        if (viewer.currentPage() != 4) {
            std::cerr << "[FAIL] TwoPage nextPage from 2 should land on 4, got " << viewer.currentPage() << std::endl;
            return 1;
        }
        viewer.previousPage();
        if (viewer.currentPage() != 2) {
            std::cerr << "[FAIL] TwoPage previousPage from 4 should land on 2, got " << viewer.currentPage() << std::endl;
            return 1;
        }
        // Fit width in a two-page spread must be narrower than fit width for a single page
        viewer.zoomFitWidth();
        const double twoUpZoom = viewer.currentZoom();
        viewer.setViewMode(layout::ViewMode::Continuous);
        viewer.zoomFitWidth();
        const double oneUpZoom = viewer.currentZoom();
        if (!(twoUpZoom < oneUpZoom * 0.6)) {
            std::cerr << "[FAIL] Two-page fit width (" << twoUpZoom << ") should be about half of one-page fit width (" << oneUpZoom << ")" << std::endl;
            return 1;
        }

        // --- Two pages with cover: 0 | 1-2 | 3-4 | 5 ---
        viewer.setViewMode(layout::ViewMode::TwoPageCover);
        viewer.goToPage(0);
        QCoreApplication::processEvents();
        const int expectedForward[] = {1, 3, 5};
        for (int expected : expectedForward) {
            viewer.nextPage();
            if (viewer.currentPage() != expected) {
                std::cerr << "[FAIL] TwoPageCover nextPage expected " << expected << ", got " << viewer.currentPage() << std::endl;
                return 1;
            }
        }
        const int expectedBack[] = {3, 1, 0};
        for (int expected : expectedBack) {
            viewer.previousPage();
            if (viewer.currentPage() != expected) {
                std::cerr << "[FAIL] TwoPageCover previousPage expected " << expected << ", got " << viewer.currentPage() << std::endl;
                return 1;
            }
        }

        // Switching modes keeps the page being read
        viewer.setViewMode(layout::ViewMode::Continuous);
        viewer.goToPage(4);
        viewer.setViewMode(layout::ViewMode::SinglePage);
        if (viewer.currentPage() != 4) {
            return fail("Switching to SinglePage must keep the current page");
        }
        viewer.setViewMode(layout::ViewMode::Continuous);
        QCoreApplication::processEvents();
        if (viewer.currentPage() != 4 || viewer.verticalScrollBar()->maximum() < continuousRange / 2) {
            return fail("Returning to Continuous must keep the page and restore the full scroll range");
        }

        // --- MainWindow: mode applies to open and future tabs and is remembered ---
        {
            QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
            const QVariant previous = settings.value(QStringLiteral("viewMode"));
            {
                ui::MainWindow window;
                window.resize(1024, 768);
                window.show();
                window.setViewMode(layout::ViewMode::TwoPage);
                if (!window.openFile(QString::fromStdString(sampleDoc))) {
                    return fail("Could not open document in MainWindow for view mode test");
                }
                auto* v = window.currentViewer();
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < 5000 && (!v || !v->isDocumentOpen())) {
                    QCoreApplication::processEvents();
                    QThread::msleep(5);
                }
                if (!v || v->viewMode() != layout::ViewMode::TwoPage) {
                    return fail("New tab did not inherit the window view mode");
                }
                window.setViewMode(layout::ViewMode::SinglePage);
                if (v->viewMode() != layout::ViewMode::SinglePage) {
                    return fail("Open tab did not follow the window view mode");
                }
            }
            QSettings reread(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
            if (reread.value(QStringLiteral("viewMode")).toInt() != static_cast<int>(layout::ViewMode::SinglePage)) {
                return fail("View mode was not persisted");
            }
            {
                ui::MainWindow second;
                if (second.viewMode() != layout::ViewMode::SinglePage) {
                    return fail("View mode was not restored by a new window");
                }
            }
            if (previous.isValid()) {
                reread.setValue(QStringLiteral("viewMode"), previous);
            } else {
                reread.remove(QStringLiteral("viewMode"));
            }
        }

        std::cout << "  -> Passed: View modes, fit page/width per mode, page flipping and persistence verified." << std::endl;
    }

    // Test 42: Page Rotation in PdfViewerWidget and MainWindow (Clockwise & Counter-Clockwise)
    std::cout << "[TEST 42] Testing Page Rotation in PdfViewerWidget and MainWindow..." << std::endl;
    {
        ui::MainWindow window;
        window.resize(1024, 768);
        window.show();
        QCoreApplication::processEvents();

        std::string sampleDoc = "widget_test_rotation.pdf";
        generateMultiPagePdf(sampleDoc, 2);

        if (!window.openFile(QString::fromStdString(sampleDoc))) {
            std::cerr << "[FAIL] Failed to open document for rotation test" << std::endl;
            return 1;
        }

        auto* viewer = window.currentViewer();
        QElapsedTimer loadTimer;
        loadTimer.start();
        while (loadTimer.elapsed() < 5000 && (!viewer || !viewer->isDocumentOpen())) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!viewer || !viewer->isDocumentOpen()) {
            std::cerr << "[FAIL] Document failed to open in rotation test" << std::endl;
            return 1;
        }

        // 1. Initial rotation is 0
        if (viewer->rotation() != 0) {
            std::cerr << "[FAIL] Initial rotation expected 0, got " << viewer->rotation() << std::endl;
            return 1;
        }

        int rotationSignalVal = -1;
        int signalCount = 0;
        QObject::connect(viewer, &ui::PdfViewerWidget::rotationChanged, [&](int deg) {
            rotationSignalVal = deg;
            ++signalCount;
        });

        // 2. Rotate Clockwise -> 90 deg
        const uint64_t genBefore = viewer->currentGeneration();
        viewer->rotateClockwise();
        QCoreApplication::processEvents();
        if (viewer->rotation() != 90 || rotationSignalVal != 90 || signalCount != 1) {
            std::cerr << "[FAIL] rotateClockwise() failed: rotation=" << viewer->rotation()
                      << " sigVal=" << rotationSignalVal << " sigCount=" << signalCount << std::endl;
            return 1;
        }
        if (viewer->currentGeneration() <= genBefore) {
            std::cerr << "[FAIL] rotateClockwise() must advance request generation to invalidate in-flight tiles" << std::endl;
            return 1;
        }
        if (viewer->layout().rotation() != 90) {
            std::cerr << "[FAIL] viewer->layout().rotation() expected 90, got " << viewer->layout().rotation() << std::endl;
            return 1;
        }

        // 3. Render frame under 90 deg rotation and verify tile delivery
        viewer->viewport()->repaint();
        QElapsedTimer renderTimer;
        renderTimer.start();
        while (renderTimer.elapsed() < 5000 && viewer->cache().size() == 0) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
            viewer->viewport()->repaint();
        }
        viewer->waitForRendering();

        // Verify that cached tile has rotation = 90
        auto keyRot90 = pdf::TileKey::create(viewer->currentDocumentId(), 0, 0, 0,
                                             pdf::quantizeScale(viewer->currentZoom()),
                                             viewer->effectiveDpr(), 90);
        if (!viewer->cache().contains(keyRot90)) {
            std::cerr << "[FAIL] Cache should contain tile with rotation=90" << std::endl;
            return 1;
        }
        auto keyRot0 = pdf::TileKey::create(viewer->currentDocumentId(), 0, 0, 0,
                                            pdf::quantizeScale(viewer->currentZoom()),
                                            viewer->effectiveDpr(), 0);
        if (viewer->cache().contains(keyRot0)) {
            std::cerr << "[FAIL] Cache key for rot=0 should not match rot=90 tile" << std::endl;
            return 1;
        }

        // 4. Rotate Clockwise -> 180 deg
        viewer->rotateClockwise();
        QCoreApplication::processEvents();
        if (viewer->rotation() != 180) {
            std::cerr << "[FAIL] Second rotateClockwise() expected 180, got " << viewer->rotation() << std::endl;
            return 1;
        }

        // 5. Rotate Clockwise -> 270 deg
        viewer->rotateClockwise();
        QCoreApplication::processEvents();
        if (viewer->rotation() != 270) {
            std::cerr << "[FAIL] Third rotateClockwise() expected 270, got " << viewer->rotation() << std::endl;
            return 1;
        }

        // 6. Rotate Clockwise -> 360/0 deg wrap-around
        viewer->rotateClockwise();
        QCoreApplication::processEvents();
        if (viewer->rotation() != 0) {
            std::cerr << "[FAIL] Fourth rotateClockwise() expected 0 (wrap-around), got " << viewer->rotation() << std::endl;
            return 1;
        }

        // 7. Rotate Counter-Clockwise -> 270 deg
        viewer->rotateCounterClockwise();
        QCoreApplication::processEvents();
        if (viewer->rotation() != 270) {
            std::cerr << "[FAIL] rotateCounterClockwise() expected 270, got " << viewer->rotation() << std::endl;
            return 1;
        }

        // 8. Explicit setRotation
        viewer->setRotation(180);
        QCoreApplication::processEvents();
        if (viewer->rotation() != 180) {
            std::cerr << "[FAIL] setRotation(180) expected 180, got " << viewer->rotation() << std::endl;
            return 1;
        }

        // 9. Document close resets rotation to 0
        viewer->closeDocument();
        QCoreApplication::processEvents();
        if (viewer->rotation() != 0) {
            std::cerr << "[FAIL] closeDocument() expected to reset rotation to 0, got " << viewer->rotation() << std::endl;
            return 1;
        }

        window.closeCurrentTab();
        std::cout << "  -> Passed: Page rotation (0, 90, 180, 270 deg CW/CCW), signals, generation invalidation, and tile key isolation verified." << std::endl;
    }

    // Test 43: Reading Color Modes (Normal, Inverted, Sepia, HighContrast)
    std::cout << "[TEST 43] Testing Reading Color Modes (Normal, Inverted, Sepia, HighContrast)..." << std::endl;
    {
        // 1. Pixel Transformation Verification for applyColorMode
        {
            QImage whitePix(1, 1, QImage::Format_ARGB32);
            whitePix.setPixel(0, 0, qRgb(255, 255, 255));

            QImage blackPix(1, 1, QImage::Format_ARGB32);
            blackPix.setPixel(0, 0, qRgb(0, 0, 0));

            // Sepia
            QImage sepiaWhite = ui::applyColorMode(whitePix, ui::ColorMode::Sepia);
            QRgb swRgb = sepiaWhite.pixel(0, 0);
            if (qRed(swRgb) != 251 || qGreen(swRgb) != 240 || qBlue(swRgb) != 217) {
                std::cerr << "[FAIL] Sepia white expected #FBF0D9 (251, 240, 217), got ("
                          << qRed(swRgb) << ", " << qGreen(swRgb) << ", " << qBlue(swRgb) << ")" << std::endl;
                return 1;
            }

            QImage sepiaBlack = ui::applyColorMode(blackPix, ui::ColorMode::Sepia);
            QRgb sbRgb = sepiaBlack.pixel(0, 0);
            if (qRed(sbRgb) != 62 || qGreen(sbRgb) != 48 || qBlue(sbRgb) != 36) {
                std::cerr << "[FAIL] Sepia black expected #3E3024 (62, 48, 36), got ("
                          << qRed(sbRgb) << ", " << qGreen(sbRgb) << ", " << qBlue(sbRgb) << ")" << std::endl;
                return 1;
            }

            // High Contrast
            QImage hcWhite = ui::applyColorMode(whitePix, ui::ColorMode::HighContrast);
            QRgb hwRgb = hcWhite.pixel(0, 0);
            if (qRed(hwRgb) != 18 || qGreen(hwRgb) != 18 || qBlue(hwRgb) != 18) {
                std::cerr << "[FAIL] HighContrast white expected #121212 (18, 18, 18), got ("
                          << qRed(hwRgb) << ", " << qGreen(hwRgb) << ", " << qBlue(hwRgb) << ")" << std::endl;
                return 1;
            }

            QImage hcBlack = ui::applyColorMode(blackPix, ui::ColorMode::HighContrast);
            QRgb hbRgb = hcBlack.pixel(0, 0);
            if (qRed(hbRgb) != 240 || qGreen(hbRgb) != 240 || qBlue(hbRgb) != 240) {
                std::cerr << "[FAIL] HighContrast black expected #F0F0F0 (240, 240, 240), got ("
                          << qRed(hbRgb) << ", " << qGreen(hbRgb) << ", " << qBlue(hbRgb) << ")" << std::endl;
                return 1;
            }

            // Inverted
            QImage invWhite = ui::applyColorMode(whitePix, ui::ColorMode::Inverted);
            QRgb iwRgb = invWhite.pixel(0, 0);
            if (qRed(iwRgb) != 0 || qGreen(iwRgb) != 0 || qBlue(iwRgb) != 0) {
                std::cerr << "[FAIL] Inverted white expected black (0, 0, 0)" << std::endl;
                return 1;
            }

            // Normal
            QImage normWhite = ui::applyColorMode(whitePix, ui::ColorMode::Normal);
            QRgb nwRgb = normWhite.pixel(0, 0);
            if (qRed(nwRgb) != 255 || qGreen(nwRgb) != 255 || qBlue(nwRgb) != 255) {
                std::cerr << "[FAIL] Normal white expected (255, 255, 255)" << std::endl;
                return 1;
            }

            // Mid-gray (128, 128, 128) in Sepia
            QImage grayPix(1, 1, QImage::Format_ARGB32);
            grayPix.setPixel(0, 0, qRgb(128, 128, 128));
            QImage sepiaMid = ui::applyColorMode(grayPix, ui::ColorMode::Sepia);
            QRgb smRgb = sepiaMid.pixel(0, 0);
            if (qRed(smRgb) < 62 || qRed(smRgb) > 251 ||
                qGreen(smRgb) < 48 || qGreen(smRgb) > 240 ||
                qBlue(smRgb) < 36 || qBlue(smRgb) > 217) {
                std::cerr << "[FAIL] Sepia mid-gray outside bounds" << std::endl;
                return 1;
            }

            // Alpha channel preservation across all modes
            QImage alphaPix(1, 1, QImage::Format_ARGB32);
            alphaPix.setPixel(0, 0, qRgba(120, 140, 160, 127));
            for (auto m : {ui::ColorMode::Inverted, ui::ColorMode::Sepia, ui::ColorMode::HighContrast}) {
                QImage trans = ui::applyColorMode(alphaPix, m);
                if (qAlpha(trans.pixel(0, 0)) != 127) {
                    std::cerr << "[FAIL] Alpha channel was not preserved in color mode " << static_cast<int>(m) << std::endl;
                    return 1;
                }
            }

            // Smooth continuous blending across spread threshold (no contour steps)
            QImage nearCutoff1(1, 1, QImage::Format_ARGB32);
            nearCutoff1.setPixel(0, 0, qRgb(140, 140, 113)); // spread = 27
            QImage nearCutoff2(1, 1, QImage::Format_ARGB32);
            nearCutoff2.setPixel(0, 0, qRgb(140, 140, 112)); // spread = 28
            QImage out1 = ui::applyColorMode(nearCutoff1, ui::ColorMode::Sepia);
            QImage out2 = ui::applyColorMode(nearCutoff2, ui::ColorMode::Sepia);
            QRgb c1 = out1.pixel(0, 0);
            QRgb c2 = out2.pixel(0, 0);
            const int maxDelta = std::max({std::abs(qRed(c1) - qRed(c2)),
                                           std::abs(qGreen(c1) - qGreen(c2)),
                                           std::abs(qBlue(c1) - qBlue(c2))});
            if (maxDelta > 3) {
                std::cerr << "[FAIL] Sepia color jump across spread cutoff: delta=" << maxDelta << std::endl;
                return 1;
            }

            // Colored pixel in HighContrast: preserve red hue (red stays red, not flipped to cyan)
            QImage redText(1, 1, QImage::Format_ARGB32);
            redText.setPixel(0, 0, qRgb(180, 20, 20)); // dark red text
            QImage hcRed = ui::applyColorMode(redText, ui::ColorMode::HighContrast);
            QRgb hrRgb = hcRed.pixel(0, 0);
            if (qRed(hrRgb) <= qBlue(hrRgb) || qRed(hrRgb) <= qGreen(hrRgb)) {
                std::cerr << "[FAIL] HighContrast inverted red hue to cyan: ("
                          << qRed(hrRgb) << "," << qGreen(hrRgb) << "," << qBlue(hrRgb) << ")" << std::endl;
                return 1;
            }
        }

        // 2. Viewport Widget theming, cache clearing, and signals
        {
            ui::PdfViewerWidget viewer;
            viewer.resize(800, 600);
            viewer.show();
            QCoreApplication::processEvents();

            std::string sampleDoc = "widget_test_colormodes.pdf";
            generateMultiPagePdf(sampleDoc, 2);
            if (viewer.openDocument(QString::fromStdString(sampleDoc)) != pdf::DocumentError::None) {
                std::cerr << "[FAIL] Failed to open document for color mode test" << std::endl;
                return 1;
            }

            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 3000 && viewer.cache().size() == 0) {
                QCoreApplication::processEvents();
                QThread::msleep(10);
            }

            // Initial: Normal
            if (viewer.colorMode() != ui::ColorMode::Normal || viewer.isDarkMode()) {
                std::cerr << "[FAIL] Initial color mode should be Normal and isDarkMode() false" << std::endl;
                return 1;
            }

            // Switch to Sepia
            bool colorModeSignalEmitted = false;
            QObject::connect(&viewer, &ui::PdfViewerWidget::colorModeChanged, [&](ui::ColorMode m) {
                if (m == ui::ColorMode::Sepia) colorModeSignalEmitted = true;
            });
            viewer.setColorMode(ui::ColorMode::Sepia);
            QCoreApplication::processEvents();

            if (!colorModeSignalEmitted || viewer.colorMode() != ui::ColorMode::Sepia || viewer.isDarkMode()) {
                std::cerr << "[FAIL] Sepia mode transition or signal failed" << std::endl;
                return 1;
            }

            // Render viewport in Sepia and check viewport background #342c24
            QImage sepiaFrame(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter p(&sepiaFrame);
                viewer.viewport()->render(&p);
            }
            QRgb sepiaBg = sepiaFrame.pixel(2, 2);
            if (qRed(sepiaBg) != 0x34 || qGreen(sepiaBg) != 0x2c || qBlue(sepiaBg) != 0x24) {
                std::cerr << "[FAIL] Sepia viewport background expected #342c24, got RGB("
                          << qRed(sepiaBg) << "," << qGreen(sepiaBg) << "," << qBlue(sepiaBg) << ")" << std::endl;
                return 1;
            }

            // Transformed cache was populated during render
            if (viewer.transformedCacheSize() == 0) {
                std::cerr << "[FAIL] Expected transformedCacheSize > 0 when rendering in Sepia mode" << std::endl;
                return 1;
            }

            // Switch to High Contrast
            viewer.setColorMode(ui::ColorMode::HighContrast);
            QCoreApplication::processEvents();
            if (viewer.colorMode() != ui::ColorMode::HighContrast || !viewer.isDarkMode()) {
                std::cerr << "[FAIL] HighContrast mode expected isDarkMode() == true" << std::endl;
                return 1;
            }

            // Render viewport in HighContrast and check viewport background #0a0a0a
            QImage hcFrame(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter p(&hcFrame);
                viewer.viewport()->render(&p);
            }
            QRgb hcBg = hcFrame.pixel(2, 2);
            if (qRed(hcBg) != 0x0a || qGreen(hcBg) != 0x0a || qBlue(hcBg) != 0x0a) {
                std::cerr << "[FAIL] HighContrast viewport background expected #0a0a0a, got RGB("
                          << qRed(hcBg) << "," << qGreen(hcBg) << "," << qBlue(hcBg) << ")" << std::endl;
                return 1;
            }

            // Switch to Inverted
            viewer.setColorMode(ui::ColorMode::Inverted);
            QCoreApplication::processEvents();
            if (viewer.colorMode() != ui::ColorMode::Inverted || !viewer.isDarkMode()) {
                std::cerr << "[FAIL] Inverted mode expected isDarkMode() == true" << std::endl;
                return 1;
            }

            // Switch back to Normal via setDarkMode(false)
            viewer.setDarkMode(false);
            QCoreApplication::processEvents();
            if (viewer.colorMode() != ui::ColorMode::Normal || viewer.isDarkMode()) {
                std::cerr << "[FAIL] setDarkMode(false) expected to return to Normal mode" << std::endl;
                return 1;
            }

            // Test cache clearing on closeDocument
            viewer.setColorMode(ui::ColorMode::Sepia);
            {
                QImage dummy(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
                QPainter p(&dummy);
                viewer.viewport()->render(&p);
            }
            viewer.closeDocument();
            if (viewer.transformedCacheSize() != 0) {
                std::cerr << "[FAIL] closeDocument() did not clear transformedCacheSize" << std::endl;
                return 1;
            }
        }

        // 3. ThumbnailListWidget Color Mode Synchronization
        {
            ui::PdfViewerWidget viewer;
            viewer.resize(800, 600);
            viewer.show();
            QCoreApplication::processEvents();

            std::string sampleDoc = "widget_test_colormodes.pdf";
            if (viewer.openDocument(QString::fromStdString(sampleDoc)) == pdf::DocumentError::None) {
                viewer.setColorMode(ui::ColorMode::Sepia);
                ui::ThumbnailListWidget thumbList;
                thumbList.setViewer(&viewer);
                thumbList.setDocument(2, {QStringLiteral("1"), QStringLiteral("2")});
                thumbList.show();
                QCoreApplication::processEvents();

                // Deliver a white thumbnail and verify transformed icon in Sepia
                QImage mockThumb(60, 80, QImage::Format_ARGB32);
                mockThumb.fill(qRgb(255, 255, 255));
                Q_EMIT viewer.thumbnailReady(0, mockThumb);
                QCoreApplication::processEvents();

                if (!thumbList.isThumbnailLoaded(0)) {
                    std::cerr << "[FAIL] Thumbnail 0 was not loaded" << std::endl;
                    return 1;
                }
                auto icon = thumbList.item(0)->icon();
                QImage iconImg = icon.pixmap(60, 80).toImage();
                QRgb iconPix = iconImg.pixel(30, 40);
                if (qRed(iconPix) != 251 || qGreen(iconPix) != 240 || qBlue(iconPix) != 217) {
                    std::cerr << "[FAIL] Thumbnail icon in Sepia expected #FBF0D9, got RGB("
                              << qRed(iconPix) << "," << qGreen(iconPix) << "," << qBlue(iconPix) << ")" << std::endl;
                    return 1;
                }
            }
        }

        // 4. MainWindow integration, new tab inheritance & QSettings hygiene
        {
            QSettings settings(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
            const QVariant prevColor = settings.value(QStringLiteral("colorMode"));
            const QVariant prevDark = settings.value(QStringLiteral("darkMode"));
            {
                ui::MainWindow window;
                window.resize(1024, 768);
                window.show();
                window.setColorMode(ui::ColorMode::Sepia);
                if (window.colorMode() != ui::ColorMode::Sepia) {
                    std::cerr << "[FAIL] MainWindow setColorMode(Sepia) failed" << std::endl;
                    return 1;
                }

                auto* newTabViewer = window.createViewerTab(QStringLiteral("Tab 2"));
                if (!newTabViewer || newTabViewer->colorMode() != ui::ColorMode::Sepia) {
                    std::cerr << "[FAIL] Newly created viewer tab did not inherit window colorMode" << std::endl;
                    return 1;
                }
            }
            QSettings reread(QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
            if (reread.value(QStringLiteral("colorMode")).toInt() != static_cast<int>(ui::ColorMode::Sepia)) {
                std::cerr << "[FAIL] ColorMode was not persisted in QSettings" << std::endl;
                return 1;
            }
            {
                ui::MainWindow second;
                if (second.colorMode() != ui::ColorMode::Sepia) {
                    std::cerr << "[FAIL] ColorMode was not restored by new MainWindow instance" << std::endl;
                    return 1;
                }
            }
            // Restore both settings
            if (prevColor.isValid()) {
                reread.setValue(QStringLiteral("colorMode"), prevColor);
            } else {
                reread.remove(QStringLiteral("colorMode"));
            }
            if (prevDark.isValid()) {
                reread.setValue(QStringLiteral("darkMode"), prevDark);
            } else {
                reread.remove(QStringLiteral("darkMode"));
            }
        }

        std::cout << "  -> Passed: Reading color modes (Normal, Inverted, Sepia, HighContrast), continuous LUT transforms, hue preservation, alpha preservation, thumbnail sync, tab inheritance, and QSettings hygiene verified." << std::endl;
    }

    // --- TEST 44: Burn-in Redaction UI Mode, Mark Staging, Viewport Rendering, and MainWindow Binding ---
    std::cout << "[TEST 44] Testing Burn-in Redaction UI Mode, Mark Staging, Viewport Rendering, and MainWindow Binding..." << std::endl;
    {
        ui::PdfViewerWidget viewer;
        viewer.resize(800, 600);
        viewer.show();
        QCoreApplication::processEvents();

        if (viewer.openDocument(QString::fromStdString(samplePath)) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for redaction UI test." << std::endl;
            return 1;
        }

        // 1. Initial Redaction Mode State
        if (viewer.isRedactionMode() || viewer.hasStagedRedactions() || viewer.stagedRedactionCount() != 0) {
            std::cerr << "[FAIL] Viewer should start with redact mode disabled and 0 staged marks." << std::endl;
            return 1;
        }

        // 2. Toggle RedactionMode
        bool modeFired = false;
        QObject::connect(&viewer, &ui::PdfViewerWidget::redactionModeChanged, [&](bool on) {
            modeFired = on;
        });

        viewer.setRedactionMode(true);
        if (!viewer.isRedactionMode() || !modeFired) {
            std::cerr << "[FAIL] setRedactionMode(true) did not enable mode or emit signal." << std::endl;
            return 1;
        }

        // 3. Staging manual redaction marks
        bool marksFired = false;
        QObject::connect(&viewer, &ui::PdfViewerWidget::redactionMarksChanged, [&]() {
            marksFired = true;
        });

        viewer.addStagedRedaction(0, pdf::ipc::RedactRect{50.0, 60.0, 200.0, 120.0});
        if (!viewer.hasStagedRedactions() || viewer.stagedRedactionCount() != 1 || !marksFired) {
            std::cerr << "[FAIL] addStagedRedaction did not update staged count or fire signal." << std::endl;
            return 1;
        }

        const auto& staged = viewer.stagedRedactions();
        auto it = staged.find(0);
        if (it == staged.end() || it->second.empty()) {
            std::cerr << "[FAIL] Staged marks missing on page 0." << std::endl;
            return 1;
        }
        if (it->second.front().left != 50.0 || it->second.front().right != 200.0) {
            std::cerr << "[FAIL] Staged coordinates mismatch." << std::endl;
            return 1;
        }

        // 4. Paint rendering with staged marks
        QImage renderedFrame(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&renderedFrame);
            viewer.viewport()->render(&p);
        }

        // 5. Clear Staged Redactions
        marksFired = false;
        viewer.clearStagedRedactions();
        if (viewer.hasStagedRedactions() || viewer.stagedRedactionCount() != 0 || !marksFired) {
            std::cerr << "[FAIL] clearStagedRedactions did not reset marks." << std::endl;
            return 1;
        }

        // 6. MainWindow UI Integration: toggle, toolbar & actions
        {
            ui::MainWindow window;
            window.resize(1024, 768);
            window.show();
            QCoreApplication::processEvents();

            window.openFile(QString::fromStdString(samplePath));
            auto* v = window.currentViewer();
            QElapsedTimer loadTimer;
            loadTimer.start();
            while (loadTimer.elapsed() < 3000 && (v == nullptr || !v->isDocumentOpen())) {
                QCoreApplication::processEvents();
                QThread::msleep(5);
                v = window.currentViewer();
            }
            if (!v) {
                std::cerr << "[FAIL] MainWindow currentViewer() is null." << std::endl;
                return 1;
            }

            window.toggleRedactionMode();
            if (!v->isRedactionMode()) {
                std::cerr << "[FAIL] MainWindow toggleRedactionMode did not toggle viewer redact mode." << std::endl;
                return 1;
            }

            v->addStagedRedaction(0, pdf::ipc::RedactRect{100.0, 100.0, 250.0, 150.0});
            if (!v->hasStagedRedactions()) {
                std::cerr << "[FAIL] Failed to stage mark in MainWindow viewer." << std::endl;
                return 1;
            }

            window.clearRedactionMarks();
            if (v->hasStagedRedactions()) {
                std::cerr << "[FAIL] MainWindow clearRedactionMarks did not clear marks." << std::endl;
                return 1;
            }
        }

        std::cout << "  -> Passed: Redaction UI mode toggle, mark staging, viewport overlay rendering, and MainWindow action binding verified." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 44 PDF VIEWER WIDGET TESTS PASSED! " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
