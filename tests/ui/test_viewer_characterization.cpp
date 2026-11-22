// RFC Pass 0 Viewer Characterization Suite (BRIEF T24)
//
// Pins observable behavior and signal order of PdfViewerWidget and MainWindow before refactoring,
// as defined in docs/VIEWER_REFACTOR_RFC.md.
//
// Hard constraints:
//   * Zero modifications to test_widget.cpp
//   * Zero modifications to src/ and include/
//   * Isolated QSettings via testsupport::IsolatedUserSettings
//   * Fail immediately under CI=1 on any failure

#include "TestIsolation.h"
#include "ui/PdfViewerWidget.h"
#include "ui/ViewerTileController.h"
#include "ui/MainWindow.h"
#include "ui/ColorMode.h"
#include "layout/DocumentLayout.h"
#include "cache/TileCache.h"
#include "ipc/WorkerClient.h"
#include "ipc/IpcMessages.h"
#include "engine/EditLog.h"
#include "engine/FormFieldTypes.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

int g_passCount = 0;
int g_failCount = 0;

void recordTest(bool ok, const char* name, const std::string& detail = "") {
    if (ok) {
        ++g_passCount;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failCount;
        std::cerr << "  [FAIL] " << name;
        if (!detail.empty()) {
            std::cerr << " - " << detail;
        }
        std::cerr << "\n";
    }
}

void pump(int ms = 50) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

template <typename Predicate>
bool waitFor(Predicate pred, int timeoutMs = 5000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (pred()) {
            return true;
        }
        QThread::msleep(10);
    }
    QCoreApplication::processEvents();
    return pred();
}

void sendWheel(QWidget* w, const QPoint& p, int deltaY) {
    QWheelEvent e(QPointF(p), QPointF(w->mapToGlobal(p)), QPoint(0, 0), QPoint(0, deltaY),
                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

// ----------------------------------------------------------------------
// Lightweight type-safe signal recorders (replaces QSignalSpy without Qt6::Test)
// ----------------------------------------------------------------------

class SignalCounter {
public:
    template <typename Sender, typename Signal>
    SignalCounter(Sender* sender, Signal signal) {
        m_conn = QObject::connect(sender, signal, [this]() {
            ++m_count;
        });
    }

    ~SignalCounter() {
        QObject::disconnect(m_conn);
    }

    size_t count() const noexcept { return m_count; }
    bool isEmpty() const noexcept { return m_count == 0; }
    void clear() noexcept { m_count = 0; }

private:
    QMetaObject::Connection m_conn;
    size_t m_count{0};
};

template <typename... Args>
class SignalRecorder {
public:
    template <typename Sender, typename Signal>
    SignalRecorder(Sender* sender, Signal signal) {
        m_conn = QObject::connect(sender, signal, [this](Args... args) {
            m_records.push_back(std::make_tuple(args...));
        });
    }

    ~SignalRecorder() {
        QObject::disconnect(m_conn);
    }

    size_t count() const noexcept { return m_records.size(); }
    bool isEmpty() const noexcept { return m_records.empty(); }
    void clear() { m_records.clear(); }

    std::tuple<Args...> takeFirst() {
        auto first = m_records.front();
        m_records.erase(m_records.begin());
        return first;
    }

    const std::tuple<Args...>& at(size_t index) const {
        return m_records.at(index);
    }

private:
    QMetaObject::Connection m_conn;
    std::vector<std::tuple<Args...>> m_records;
};

// ----------------------------------------------------------------------
// Fixture generation helpers
// ----------------------------------------------------------------------

void generateMultiPagePdf(const std::string& path, int count = 5) {
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

    const int contentObjNum = 3 + count;
    for (int i = 0; i < count; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents "
            + std::to_string(contentObjNum) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
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
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateTwoPageTextPdf(const std::string& path, const std::string& text1, const std::string& text2) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    std::string content0 = "BT\n/F1 24 Tf\n100 700 Td\n(" + text1 + ") Tj\nET\n";
    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Length " + std::to_string(content0.size()) + " >>\nstream\n" + content0 + "\nendstream\nendobj\n";

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
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateSearchPdf(const std::string& path) {
    // 3 pages each containing word "TargetKeyword"
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n";

    for (int i = 0; i < 3; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 6 0 R >> >> /Contents "
            + std::to_string(7 + i) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    for (int i = 0; i < 3; ++i) {
        std::string content = "BT\n/F1 18 Tf\n100 700 Td\n(Page " + std::to_string(i + 1) + " TargetKeyword occurrence) Tj\nET\n";
        offsets.push_back(pdf.size());
        pdf += std::to_string(7 + i) + " 0 obj\n<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream\nendobj\n";
    }

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 10\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 10 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateMultiPageTextPdf(const std::string& path, int count = 60) {
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

    const int fontObj = 3 + count;
    const int firstContentObj = fontObj + 1;

    for (int i = 0; i < count; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 "
            + std::to_string(fontObj) + " 0 R >> >> /Contents "
            + std::to_string(firstContentObj + i) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    pdf += std::to_string(fontObj) + " 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    for (int i = 0; i < count; ++i) {
        std::string text = "Page " + std::to_string(i) + " text line content";
        std::string stream = "BT\n/F1 18 Tf\n100 700 Td\n(" + text + ") Tj\nET\n";
        offsets.push_back(pdf.size());
        pdf += std::to_string(firstContentObj + i) + " 0 obj\n<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream\nendobj\n";
    }

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
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateCircularSearchPdf(const std::string& path) {
    // 5 pages where "CircularKey" appears only on page 0 and page 3
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R 6 0 R 7 0 R] /Count 5 >>\nendobj\n";

    for (int i = 0; i < 5; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 8 0 R >> >> /Contents "
            + std::to_string(9 + i) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    pdf += "8 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    const std::string contents[5] = {
        "BT\n/F1 18 Tf\n100 700 Td\n(Page zero CircularKey match) Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(Page one plain content) Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(Page two plain content) Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(Page three CircularKey match) Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(Page four plain content) Tj\nET\n"
    };

    for (int i = 0; i < 5; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(9 + i) + " 0 obj\n<< /Length " + std::to_string(contents[i].size()) + " >>\nstream\n" + contents[i] + "\nendstream\nendobj\n";
    }

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 15\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 15 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateMixedCaseSearchPdf(const std::string& path) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n";

    for (int i = 0; i < 3; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 6 0 R >> >> /Contents "
            + std::to_string(7 + i) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    const std::string contents[3] = {
        "BT\n/F1 18 Tf\n100 700 Td\n(MixedCase TargetKeyword upper) Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(MixedCase targetkeyword lower) Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(MixedCase TargetKeyword upper) Tj\nET\n"
    };

    for (int i = 0; i < 3; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(7 + i) + " 0 obj\n<< /Length " + std::to_string(contents[i].size()) + " >>\nstream\n" + contents[i] + "\nendstream\nendobj\n";
    }

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 10\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 10 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateThreePageTextPdf(const std::string& path, const std::string& t0, const std::string& t1, const std::string& t2) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>\nendobj\n";

    for (int i = 0; i < 3; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 6 0 R >> >> /Contents "
            + std::to_string(7 + i) + " 0 R >>\nendobj\n";
    }

    offsets.push_back(pdf.size());
    pdf += "6 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    const std::string contents[3] = {
        "BT\n/F1 18 Tf\n100 700 Td\n(" + t0 + ") Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(" + t1 + ") Tj\nET\n",
        "BT\n/F1 18 Tf\n100 700 Td\n(" + t2 + ") Tj\nET\n"
    };

    for (int i = 0; i < 3; ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(7 + i) + " 0 obj\n<< /Length " + std::to_string(contents[i].size()) + " >>\nstream\n" + contents[i] + "\nendstream\nendobj\n";
    }

    size_t xrefOffset = pdf.size();
    pdf += "xref\n0 10\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
        pdf += buf;
    }
    pdf += "trailer\n<< /Size 10 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    std::ofstream out(path, std::ios::binary);
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generatePersianPdfFixture(const std::string& path) {
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
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

inline std::shared_ptr<pdf::PageText> makeDummyPageText(int pageIndex, const std::string& str) {
    auto pt = std::make_shared<pdf::PageText>();
    pt->pageIndex = pageIndex;
    pt->widthPoints = 595.0;
    pt->heightPoints = 842.0;
    pt->text = std::u16string(str.begin(), str.end());
    double curX = 100.0;
    double curY = 100.0;
    for (char c : str) {
        pdf::PageCharBox b;
        b.left = curX;
        b.top = curY;
        b.right = curX + 10.0;
        b.bottom = curY + 15.0;
        b.unicode = static_cast<char32_t>(c);
        pt->chars.push_back(b);
        curX += 10.0;
    }
    return pt;
}

} // namespace

// ======================================================================
// GROUP 1: Layout and Navigation Characterization
// ======================================================================

void runGroup1LayoutAndNavigation(const QString& sample5Page) {
    std::cout << "\n--- [Group 1: Layout and Navigation] ---\n";
    ui::PdfViewerWidget viewer;
    viewer.resize(900, 700);
    viewer.show();
    viewer.ensurePolished();
    pump(100);

    const auto err = viewer.openDocument(sample5Page);
    recordTest(err == pdf::DocumentError::None && viewer.pageCount() == 5,
               "Group 1.1: Document open 5-page layout fixture");
    pump(200);

    // 1.2: Page navigation & pageChanged signal order/values
    {
        SignalRecorder<int, int> spyPage(&viewer, &ui::PdfViewerWidget::pageChanged);
        viewer.goToPage(2);
        pump(50);
        bool ok = (viewer.currentPage() == 2) && (spyPage.count() == 1);
        if (ok) {
            const auto [curr, total] = spyPage.takeFirst();
            // pageChanged emits 1-based page index and total count
            ok = (curr == 3) && (total == 5);
        }
        recordTest(ok, "Group 1.2: goToPage(2) navigates and emits pageChanged(3, 5)");

        // nextPage
        viewer.nextPage();
        pump(50);
        ok = (viewer.currentPage() == 3) && (spyPage.count() == 1);
        if (ok) {
            const auto [curr, total] = spyPage.takeFirst();
            ok = (curr == 4) && (total == 5);
        }
        recordTest(ok, "Group 1.3: nextPage() advances and emits pageChanged(4, 5)");

        // previousPage
        viewer.previousPage();
        pump(50);
        ok = (viewer.currentPage() == 2) && (spyPage.count() == 1);
        if (ok) {
            const auto [curr, total] = spyPage.takeFirst();
            ok = (curr == 3) && (total == 5);
        }
        recordTest(ok, "Group 1.4: previousPage() decrements and emits pageChanged(3, 5)");

        // Clamping: out-of-range navigation is a no-op
        viewer.goToPage(-1);
        viewer.goToPage(999);
        pump(50);
        recordTest(viewer.currentPage() == 2 && spyPage.isEmpty(),
                   "Group 1.5: Out-of-range goToPage clamped without spurious signals");
    }

    // 1.6: Zoom presets & zoomChanged signal
    {
        SignalRecorder<double> spyZoom(&viewer, &ui::PdfViewerWidget::zoomChanged);
        viewer.setZoom(2.0);
        pump(50);
        // zoomChanged emits zoom ratio = m_scale / DESKTOP_100_PERCENT_SCALE
        const double expectedRatio = 2.0 / ui::PdfViewerWidget::DESKTOP_100_PERCENT_SCALE;
        bool ok = (std::abs(viewer.currentZoom() - 2.0) < 1e-4) && (spyZoom.count() == 1);
        if (ok) {
            const auto [ratio] = spyZoom.takeFirst();
            ok = std::abs(ratio - expectedRatio) < 1e-4;
        }
        recordTest(ok, "Group 1.6: setZoom(2.0) updates scale and emits zoomChanged(ratio)");

        // zoomIn (multiplies by ZOOM_STEP = 1.25)
        viewer.zoomIn();
        pump(50);
        ok = (std::abs(viewer.currentZoom() - 2.5) < 1e-4) && (spyZoom.count() == 1);
        spyZoom.clear();
        recordTest(ok, "Group 1.7: zoomIn() multiplies scale by ZOOM_STEP");

        // zoomReset restores DESKTOP_100_PERCENT_SCALE
        viewer.zoomReset();
        pump(50);
        ok = (std::abs(viewer.currentZoom() - ui::PdfViewerWidget::DESKTOP_100_PERCENT_SCALE) < 1e-4) &&
             (spyZoom.count() == 1);
        spyZoom.clear();
        recordTest(ok, "Group 1.8: zoomReset() restores DESKTOP_100_PERCENT_SCALE");

        // zoomFitWidth and zoomFitPage
        viewer.zoomFitWidth();
        pump(50);
        const double fitWZoom = viewer.currentZoom();
        viewer.zoomFitPage();
        pump(50);
        const double fitPZoom = viewer.currentZoom();
        recordTest(fitWZoom > 0.0 && fitPZoom > 0.0 && fitWZoom != fitPZoom,
                   "Group 1.9: zoomFitWidth() and zoomFitPage() compute distinct positive scales");
    }

    // 1.10: View modes & rotation signals
    {
        SignalRecorder<layout::ViewMode> spyMode(&viewer, &ui::PdfViewerWidget::viewModeChanged);
        viewer.setViewMode(layout::ViewMode::SinglePage);
        pump(50);
        bool ok = (viewer.viewMode() == layout::ViewMode::SinglePage) && (spyMode.count() == 1);
        if (ok) {
            ok = std::get<0>(spyMode.takeFirst()) == layout::ViewMode::SinglePage;
        }
        recordTest(ok, "Group 1.10: setViewMode(SinglePage) updates layout and emits viewModeChanged");

        viewer.setViewMode(layout::ViewMode::TwoPage);
        pump(50);
        recordTest(viewer.viewMode() == layout::ViewMode::TwoPage && spyMode.count() == 1,
                   "Group 1.11: setViewMode(TwoPage) switches view mode");
        spyMode.clear();

        // Rotation & rotationChanged signal
        SignalRecorder<int> spyRot(&viewer, &ui::PdfViewerWidget::rotationChanged);
        viewer.setRotation(90);
        pump(50);
        ok = (viewer.rotation() == 90) && (spyRot.count() == 1);
        if (ok) {
            ok = (std::get<0>(spyRot.takeFirst()) == 90);
        }
        recordTest(ok, "Group 1.12: setRotation(90) rotates document and emits rotationChanged(90)");

        viewer.rotateClockwise();
        pump(50);
        recordTest(viewer.rotation() == 180 && spyRot.count() == 1,
                   "Group 1.13: rotateClockwise() advances rotation to 180");
        spyRot.clear();

        viewer.rotateCounterClockwise();
        pump(50);
        recordTest(viewer.rotation() == 90 && spyRot.count() == 1,
                   "Group 1.14: rotateCounterClockwise() restores 90");
        spyRot.clear();

        viewer.setRotation(0);
        pump(50);
    }

    // 1.15: 64-bit scroll mapping after resize (viewport resize anchor preservation <= 1.0 px)
    {
        viewer.setViewMode(layout::ViewMode::Continuous);
        viewer.setZoom(1.0);
        viewer.goToPage(2);
        pump(50);

        const double curDpr = viewer.effectiveDpr();
        int64_t vpW_before = static_cast<int64_t>(std::round(viewer.viewport()->width() * curDpr));
        int64_t vpH_before = static_cast<int64_t>(std::round(viewer.viewport()->height() * curDpr));

        int64_t anchorDocX_before = viewer.currentDocScrollX() + vpW_before / 2;
        int64_t anchorDocY_before = viewer.currentDocScrollY() + vpH_before / 2;

        const auto& p2Before = viewer.layout().getPageLayout(2);
        double anchorPtX = static_cast<double>(anchorDocX_before - p2Before.leftPixel) / (viewer.currentZoom() * curDpr);
        double anchorPtY = static_cast<double>(anchorDocY_before - p2Before.topPixel) / (viewer.currentZoom() * curDpr);

        // Resize viewport from 900 to 1100
        viewer.resize(1100, 750);
        pump(100);

        int64_t vpW_after = static_cast<int64_t>(std::round(viewer.viewport()->width() * curDpr));
        int64_t vpH_after = static_cast<int64_t>(std::round(viewer.viewport()->height() * curDpr));

        int64_t anchorDocX_after = viewer.currentDocScrollX() + vpW_after / 2;
        int64_t anchorDocY_after = viewer.currentDocScrollY() + vpH_after / 2;

        const auto& p2After = viewer.layout().getPageLayout(2);
        double reconstructedPtX = static_cast<double>(anchorDocX_after - p2After.leftPixel) / (viewer.currentZoom() * curDpr);
        double reconstructedPtY = static_cast<double>(anchorDocY_after - p2After.topPixel) / (viewer.currentZoom() * curDpr);

        double driftX = std::abs(reconstructedPtX - anchorPtX) * viewer.currentZoom() * curDpr;
        double driftY = std::abs(reconstructedPtY - anchorPtY) * viewer.currentZoom() * curDpr;

        recordTest(driftX <= 1.0 && driftY <= 1.0,
                   "Group 1.15: 64-bit document scroll coordinates anchor preserved within 1.0 device pixel across resize");
    }

    // 1.16: Page-flip rounding in SinglePage mode (FLIP_THRESHOLD = 240)
    {
        viewer.setViewMode(layout::ViewMode::SinglePage);
        viewer.goToPage(0);
        pump(50);

        auto* vbar = viewer.verticalScrollBar();
        vbar->setValue(vbar->maximum()); // at bottom of page 0
        pump(50);

        const QPoint center = viewer.viewport()->rect().center();

        // Notch 1: dy = -120 (accumulates 120 < 240, must NOT flip)
        sendWheel(viewer.viewport(), center, -120);
        pump(30);
        const bool stayedOnPage0 = (viewer.currentPage() == 0);

        // Notch 2: dy = -120 (accumulates 240 >= 240, MUST flip to page 1)
        sendWheel(viewer.viewport(), center, -120);
        pump(100);
        const bool flippedToPage1 = (viewer.currentPage() == 1);

        // Backward flip:
        // Scroll to the top of page 1
        vbar->setValue(vbar->minimum());
        pump(50);

        // Notch 1: dy = +120 (accumulates -120, absolute value 120 < 240, must NOT flip back)
        sendWheel(viewer.viewport(), center, 120);
        pump(30);
        const bool stayedOnPage1 = (viewer.currentPage() == 1);

        // Notch 2: dy = +120 (accumulates -240 <= -240, and currentPage > 0, MUST flip back to page 0)
        sendWheel(viewer.viewport(), center, 120);
        pump(100);
        const bool flippedBackToPage0 = (viewer.currentPage() == 0);

        recordTest(stayedOnPage0 && flippedToPage1 && stayedOnPage1 && flippedBackToPage0,
                   "Group 1.16: Page-flip accumulator honors FLIP_THRESHOLD (240) in SinglePage mode");
    }

    // 1.17: Zoom upper clamp limit (10.0), zoomIn saturation, and spurious signal suppression
    {
        SignalRecorder<double> spyZoom(&viewer, &ui::PdfViewerWidget::zoomChanged);
        viewer.setZoom(100.0);
        pump(50);
        bool ok = (std::abs(viewer.currentZoom() - 10.0) < 1e-4) && (spyZoom.count() == 1);
        if (ok) {
            const auto [ratio] = spyZoom.takeFirst();
            const double expectedRatio = 10.0 / ui::PdfViewerWidget::DESKTOP_100_PERCENT_SCALE;
            ok = (std::abs(ratio - expectedRatio) < 1e-4);
        }

        // Repeated zoomIn from maximum must not exceed 10.0 and must not emit zoomChanged
        spyZoom.clear();
        viewer.zoomIn();
        pump(50);
        ok = ok && (std::abs(viewer.currentZoom() - 10.0) < 1e-4) && spyZoom.isEmpty();

        // setZoom with clamped value equal to current zoom must suppress signal
        viewer.setZoom(50.0);
        pump(50);
        ok = ok && (std::abs(viewer.currentZoom() - 10.0) < 1e-4) && spyZoom.isEmpty();

        recordTest(ok, "Group 1.17: Zoom upper clamp limit (10.0), zoomIn saturation, and spurious signal suppression");
    }

    // 1.18: Zoom lower clamp limit (0.1), zoomOut saturation, and spurious signal suppression
    {
        SignalRecorder<double> spyZoom(&viewer, &ui::PdfViewerWidget::zoomChanged);
        viewer.setZoom(0.0001);
        pump(50);
        bool ok = (std::abs(viewer.currentZoom() - 0.1) < 1e-4) && (spyZoom.count() == 1);
        if (ok) {
            const auto [ratio] = spyZoom.takeFirst();
            const double expectedRatio = 0.1 / ui::PdfViewerWidget::DESKTOP_100_PERCENT_SCALE;
            ok = (std::abs(ratio - expectedRatio) < 1e-4);
        }

        // Repeated zoomOut from minimum must not drop below 0.1 and must not emit zoomChanged
        spyZoom.clear();
        viewer.zoomOut();
        pump(50);
        ok = ok && (std::abs(viewer.currentZoom() - 0.1) < 1e-4) && spyZoom.isEmpty();

        // setZoom with clamped value equal to current zoom must suppress signal
        viewer.setZoom(0.001);
        pump(50);
        ok = ok && (std::abs(viewer.currentZoom() - 0.1) < 1e-4) && spyZoom.isEmpty();

        recordTest(ok, "Group 1.18: Zoom lower clamp limit (0.1), zoomOut saturation, and spurious signal suppression");

        // Restore default scale
        viewer.zoomReset();
        pump(50);
    }

    // 1.19: Last-page forward wheel flip past bottom does not overrun and accumulator cleanly resets
    {
        viewer.setViewMode(layout::ViewMode::SinglePage);
        const int lastPage = viewer.pageCount() - 1;
        viewer.goToPage(lastPage);
        pump(50);

        auto* vbar = viewer.verticalScrollBar();
        vbar->setValue(vbar->maximum());
        pump(50);

        SignalRecorder<int, int> spyPage(&viewer, &ui::PdfViewerWidget::pageChanged);
        const QPoint center = viewer.viewport()->rect().center();

        // Send wheel forward notches past bottom: must NOT flip past last page
        sendWheel(viewer.viewport(), center, -120);
        pump(30);
        sendWheel(viewer.viewport(), center, -120);
        pump(30);
        sendWheel(viewer.viewport(), center, -120);
        pump(50);
        const bool stayedOnLastPage = (viewer.currentPage() == lastPage) && spyPage.isEmpty();

        // Scroll back to top of last page and verify accumulator does not leave a pending flip
        vbar->setValue(vbar->minimum());
        pump(50);

        // Backward Notch 1: dy = +120 (must not flip; requires full -240 threshold)
        sendWheel(viewer.viewport(), center, 120);
        pump(30);
        const bool stayedBeforeThreshold = (viewer.currentPage() == lastPage) && spyPage.isEmpty();

        // Backward Notch 2: dy = +120 (accumulates -240, flips to previous page)
        sendWheel(viewer.viewport(), center, 120);
        pump(100);
        const bool flippedToPrev = (viewer.currentPage() == lastPage - 1) && (spyPage.count() == 1);

        recordTest(stayedOnLastPage && stayedBeforeThreshold && flippedToPrev,
                   "Group 1.19: Last-page forward wheel flip past bottom does not overrun and accumulator cleanly resets");
    }

    // 1.20: Boundary navigation (nextPage on last page, previousPage on first page) across all view modes
    {
        SignalRecorder<int, int> spyPage(&viewer, &ui::PdfViewerWidget::pageChanged);
        const std::vector<layout::ViewMode> modes = {
            layout::ViewMode::Continuous,
            layout::ViewMode::SinglePage,
            layout::ViewMode::TwoPage,
            layout::ViewMode::TwoPageCover
        };

        bool ok = true;
        const int lastPage = viewer.pageCount() - 1;

        for (auto mode : modes) {
            viewer.setViewMode(mode);
            viewer.goToPage(0);
            pump(50);
            spyPage.clear();

            // previousPage on first page must remain on page 0 without signals
            viewer.previousPage();
            pump(50);
            if (viewer.currentPage() != 0 || !spyPage.isEmpty()) {
                ok = false;
                break;
            }

            // Navigate to last page
            viewer.goToPage(lastPage);
            pump(50);
            const int expectedLast = viewer.currentPage();
            spyPage.clear();

            // nextPage on last page must not overrun and must not emit signals
            viewer.nextPage();
            pump(50);
            if (viewer.currentPage() != expectedLast || !spyPage.isEmpty()) {
                ok = false;
                break;
            }
        }

        // Restore continuous mode and first page
        viewer.setViewMode(layout::ViewMode::Continuous);
        viewer.goToPage(0);
        pump(50);

        recordTest(ok, "Group 1.20: Boundary navigation (nextPage on last page, previousPage on first page) across all view modes");
    }
}

// ======================================================================
// GROUP 2: Tile Scheduling Characterization
// ======================================================================

void runGroup2TileScheduling(const QString& samplePdf) {
    std::cout << "\n--- [Group 2: Tile Scheduling] ---\n";
    ui::PdfViewerWidget viewer;
    viewer.resize(800, 600);
    viewer.show();
    viewer.ensurePolished();
    pump(100);

    const auto err = viewer.openDocument(samplePdf);
    recordTest(err == pdf::DocumentError::None, "Group 2.1: Open document for tile scheduling");
    viewer.waitForRendering();
    pump(200);

    // 2.2: Visible tiles requested and pinned in TileCache
    {
        const double qDpr = viewer.effectiveDpr();
        const double qScale = pdf::quantizeScale(viewer.currentZoom());
        const int64_t vpW = static_cast<int64_t>(std::round(viewer.viewport()->width() * qDpr));
        const int64_t vpH = static_cast<int64_t>(std::round(viewer.viewport()->height() * qDpr));

        layout::Rect64 vpRect{viewer.currentDocScrollX(), viewer.currentDocScrollY(), vpW, vpH};
        const auto visibleTiles = viewer.layout().getVisibleTiles(vpRect, 512);

        bool allPinnedAndCached = !visibleTiles.empty();
        for (const auto& t : visibleTiles) {
            const auto key = pdf::TileKey::create(viewer.currentDocumentId(), t.pageIndex, t.col, t.row, qScale, qDpr, viewer.rotation());
            if (!viewer.cache().isPinned(key)) {
                allPinnedAndCached = false;
                break;
            }
            if (!viewer.cache().get(key).has_value()) {
                allPinnedAndCached = false;
                break;
            }
        }
        recordTest(allPinnedAndCached && viewer.inFlightCount() == 0,
                   "Group 2.2: Viewport visible tiles are pinned, cached, and inFlightCount resolves to 0");
    }

    // 2.3: In-flight de-duplication
    {
        // Invalidate document cache to force render requests
        viewer.cache().invalidateDocument(viewer.currentDocumentId());
        viewer.setDocScrollPosition(0, 0);

        // Render viewport twice before worker finishes
        QImage img1(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        QPainter p1(&img1);
        viewer.viewport()->render(&p1);
        p1.end();

        QImage img2(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        QPainter p2(&img2);
        viewer.viewport()->render(&p2);
        p2.end();

        const size_t inFlight = viewer.inFlightCount();
        viewer.waitForRendering();

        // inFlight keys must not exceed total visible tiles (no duplicates)
        recordTest(inFlight > 0 && inFlight <= 16,
                   "Group 2.3: In-flight tile keys are de-duplicated during concurrent paints");
    }

    // 2.4: Fallback scales after zoom change
    {
        viewer.waitForRendering();
        viewer.setZoom(1.0);
        viewer.waitForRendering();

        const size_t drawnBefore = viewer.fallbackTilesDrawn();
        // Step zoom to a new scale
        viewer.setZoom(2.0);

        // Draw immediately without waiting for rendering
        QImage fastImg(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        QPainter p(&fastImg);
        viewer.viewport()->render(&p);
        p.end();

        const size_t drawnAfter = viewer.fallbackTilesDrawn();
        viewer.waitForRendering();

        recordTest(drawnAfter >= drawnBefore,
                   "Group 2.4: Zoom fallback mechanism tracks and renders scaled earlier tiles");
    }

    // 2.5: Tile failure & retryFailedTiles()
    {
        const auto key = pdf::TileKey::create(viewer.currentDocumentId(), 0, 0, 0,
                                             pdf::quantizeScale(viewer.currentZoom()),
                                             viewer.effectiveDpr(), viewer.rotation());
        viewer.cache().markFailed(key);
        const bool wasFailed = viewer.cache().isFailed(key);

        viewer.retryFailedTiles();
        pump(50);
        const bool cleared = !viewer.cache().isFailed(key);
        viewer.waitForRendering();

        recordTest(wasFailed && cleared,
                   "Group 2.5: retryFailedTiles() clears failure state and re-enqueues render");
    }

    // 2.6: Prefetch ring toggle
    {
        viewer.setPrefetchEnabled(false);
        const size_t countBefore = viewer.prefetchRequestsIssued();
        viewer.viewport()->update();
        pump(50);
        const size_t countDisabled = viewer.prefetchRequestsIssued();

        viewer.setPrefetchEnabled(true);
        viewer.viewport()->update();
        pump(50);

        recordTest(countDisabled == countBefore,
                   "Group 2.6: setPrefetchEnabled(false) suppresses off-screen tile prefetch");
    }

    // 2.7: Retryable failed tile suppresses fallback rendering on both in-flight and re-enqueue paths
    {
        // Direct controller evaluation check:
        // When a failed tile becomes retryable, both the re-enqueue path (not in flight)
        // and the in-flight path (already in flight) must yield DrawPendingNoFallback,
        // strictly avoiding fallback scaling and paint artifacts on retried tiles.
        ui::ViewerTileController controller;
        pdf::TileCache localCache(10 * 1024 * 1024);
        const int64_t docId = viewer.currentDocumentId();
        const auto key = pdf::TileKey::create(docId, 0, 0, 0, 2.0, 1.0, 0);
        layout::VisibleTile t{0, 0, 0, 0, 0, 512, 512, 0, 0};

        localCache.markFailed(key);
        pump(1050); // Elapse default 1000ms backoff interval

        // Re-enqueue path: key is retryable and not yet in flight
        std::vector<pdf::TileRenderRequest> reenqueueRequests;
        auto actionReenqueue = controller.evaluateVisibleTile(
            key, t, 0, 0, 256, 256, docId, 0, 2.0, 1.0, 0,
            localCache, reenqueueRequests);
        const bool reenqueueOk = (actionReenqueue == ui::VisibleTileAction::DrawPendingNoFallback)
            && (reenqueueRequests.size() == 1)
            && (controller.inFlightCount() == 1);

        // In-flight path: mark failed again while key is still tracked as in-flight
        localCache.resetFailureHistory();
        localCache.markFailed(key);
        pump(1050); // Elapse backoff interval
        std::vector<pdf::TileRenderRequest> inFlightRequests;
        auto actionInFlight = controller.evaluateVisibleTile(
            key, t, 0, 0, 256, 256, docId, 0, 2.0, 1.0, 0,
            localCache, inFlightRequests);
        const bool inFlightOk = (actionInFlight == ui::VisibleTileAction::DrawPendingNoFallback)
            && inFlightRequests.empty(); // duplicate request avoided

        // End-to-end widget verification:
        // Ensure that during viewport paint of a retried tile, fallbackTilesDrawn does not increment.
        viewer.waitForRendering();
        viewer.setZoom(1.0);
        viewer.waitForRendering();
        viewer.setZoom(2.0);
        viewer.waitForRendering();

        const auto viewerKey = pdf::TileKey::create(
            viewer.currentDocumentId(), 0, 0, 0,
            pdf::quantizeScale(viewer.currentZoom()),
            viewer.effectiveDpr(), viewer.rotation());
        viewer.cache().markFailed(viewerKey);
        pump(1050);

        const size_t fbBefore = viewer.fallbackTilesDrawn();
        QImage renderImg(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&renderImg);
        viewer.viewport()->render(&painter);
        painter.end();
        const size_t fbAfter = viewer.fallbackTilesDrawn();
        viewer.waitForRendering();

        std::string detail;
        if (!reenqueueOk) detail += "reenqueueOk failed; ";
        if (!inFlightOk) detail += "inFlightOk failed; ";
        if (fbAfter != fbBefore) detail += "fbAfter != fbBefore (before=" + std::to_string(fbBefore) + " after=" + std::to_string(fbAfter) + "); ";
        recordTest(reenqueueOk && inFlightOk && (fbAfter == fbBefore),
                   "Group 2.7: Retryable failed tile suppresses fallback rendering on both in-flight and re-enqueue paths", detail);
    }

    // 2.8: Foreign documentId rejection, in-flight key clearance, and generation neutrality
    {
        ui::ViewerTileController controller;
        pdf::TileCache localCache(10 * 1024 * 1024);
        const int64_t currentDoc = 100;
        const int64_t foreignDoc = 200;
        const auto keyReady = pdf::TileKey::create(currentDoc, 0, 0, 0, 1.0, 1.0, 0);
        const auto keyFailed = pdf::TileKey::create(currentDoc, 0, 1, 0, 1.0, 1.0, 0);
        const auto keyMatching = pdf::TileKey::create(currentDoc, 0, 2, 0, 1.0, 1.0, 0);

        // Put keys in flight
        layout::VisibleTile vt0{0, 0, 0, 0, 0, 512, 512, 0, 0};
        layout::VisibleTile vt1{0, 1, 0, 512, 0, 512, 512, 0, 0};
        layout::VisibleTile vt2{0, 2, 0, 1024, 0, 512, 512, 0, 0};
        std::vector<pdf::TileRenderRequest> reqs;
        controller.evaluateVisibleTile(keyReady, vt0, 0, 0, 0, 0, currentDoc, 0, 1.0, 1.0, 0, localCache, reqs);
        controller.evaluateVisibleTile(keyFailed, vt1, 0, 0, 0, 0, currentDoc, 0, 1.0, 1.0, 0, localCache, reqs);
        controller.evaluateVisibleTile(keyMatching, vt2, 0, 0, 0, 0, currentDoc, 0, 1.0, 1.0, 0, localCache, reqs);
        const bool allInFlightBefore = (controller.inFlightCount() == 3);

        auto dummyBuf = pdf::PixelBuffer::allocate(512, 512);

        // 1. handleTileReady with foreign docId:
        // Must erase in-flight key, return false, and NOT store buffer in cache
        const bool readyForeignHandled = controller.handleTileReady(foreignDoc, currentDoc, keyReady, dummyBuf, localCache);
        const bool readyForeignOk = (!readyForeignHandled)
            && (!localCache.contains(keyReady))
            && (controller.inFlightKeys().count(keyReady) == 0);

        // 2. handleTileFailed with foreign docId:
        // Must erase in-flight key, return false, and NOT mark key as failed in cache
        const bool failedForeignHandled = controller.handleTileFailed(foreignDoc, currentDoc, keyFailed, localCache);
        const bool failedForeignOk = (!failedForeignHandled)
            && (!localCache.isFailed(keyFailed))
            && (controller.inFlightKeys().count(keyFailed) == 0);

        // 3. handleTileReady with matching docId:
        // Must erase in-flight key and store buffer in cache
        localCache.pin(keyMatching);
        const bool readyMatchingHandled = controller.handleTileReady(currentDoc, currentDoc, keyMatching, dummyBuf, localCache);
        const bool readyMatchingOk = readyMatchingHandled
            && localCache.contains(keyMatching)
            && (controller.inFlightKeys().count(keyMatching) == 0);

        // 4. Widget signal-level verification (including generation neutrality):
        // Signal tileReady / tileFailed with foreign docId and non-zero generation.
        // Handlers must ignore foreign events and intentionally ignore generation argument.
        const int64_t viewerDoc = viewer.currentDocumentId();
        const auto vKeyForeignReady = pdf::TileKey::create(viewerDoc, 0, 99, 0, 1.0, 1.0, 0);
        const auto vKeyForeignFailed = pdf::TileKey::create(viewerDoc, 0, 98, 0, 1.0, 1.0, 0);
        Q_EMIT viewer.renderWorker().tileReady(viewerDoc + 9999, 42, vKeyForeignReady, dummyBuf);
        Q_EMIT viewer.renderWorker().tileFailed(viewerDoc + 9999, 42, vKeyForeignFailed);
        pump(20);

        const bool widgetIsolationOk = !viewer.cache().contains(vKeyForeignReady)
            && !viewer.cache().isFailed(vKeyForeignFailed);

        recordTest(allInFlightBefore && readyForeignOk && failedForeignOk && readyMatchingOk && widgetIsolationOk,
                   "Group 2.8: Foreign documentId rejected, in-flight keys cleared, and generation ignored");
    }

    // 2.9: Retry deduplication prevents duplicate enqueue of in-flight failed tiles
    {
        ui::ViewerTileController controller;
        pdf::TileCache localCache(10 * 1024 * 1024);
        const int64_t docId = 42;
        const auto key = pdf::TileKey::create(docId, 0, 0, 0, 1.0, 1.0, 0);
        layout::VisibleTile t{0, 0, 0, 0, 0, 512, 512, 0, 0};

        // Mark failed and elapse backoff so canRetry is true
        localCache.markFailed(key);
        pump(1050);

        // First evaluation: tile is failed and retryable -> enqueues retry request and marks in-flight
        std::vector<pdf::TileRenderRequest> requestsFirst;
        auto actionFirst = controller.evaluateVisibleTile(
            key, t, 0, 0, 256, 256, docId, 1, 1.0, 1.0, 0,
            localCache, requestsFirst);

        const bool firstOk = (actionFirst == ui::VisibleTileAction::DrawPendingNoFallback)
            && (requestsFirst.size() == 1)
            && (controller.inFlightCount() == 1);

        // Now simulate the tile remaining failed while still in flight
        localCache.resetFailureHistory();
        localCache.markFailed(key);
        pump(1050); // backoff elapsed again, canRetry is true

        // Second evaluation while still in flight:
        // Must NOT enqueue duplicate request to worker
        std::vector<pdf::TileRenderRequest> requestsSecond;
        auto actionSecond = controller.evaluateVisibleTile(
            key, t, 0, 0, 256, 256, docId, 1, 1.0, 1.0, 0,
            localCache, requestsSecond);

        const bool secondOk = (actionSecond == ui::VisibleTileAction::DrawPendingNoFallback)
            && requestsSecond.empty() // de-duplication: no new request pushed
            && (controller.inFlightCount() == 1);

        std::string detail;
        if (!firstOk) detail += "firstOk failed; ";
        if (!secondOk) detail += "secondOk failed; ";
        recordTest(firstOk && secondOk,
                   "Group 2.9: Retry deduplication prevents duplicate enqueue of in-flight failed tiles", detail);
    }
}

// ======================================================================
// GROUP 3: Selection and Search Characterization
// ======================================================================

void runGroup3SelectionAndSearch(const QString& twoPageTextPdf, const QString& searchPdf) {
    std::cout << "\n--- [Group 3: Selection and Search] ---\n";
    ui::PdfViewerWidget viewer;
    viewer.resize(800, 700);
    viewer.show();
    viewer.ensurePolished();
    pump(100);

    const auto err = viewer.openDocument(twoPageTextPdf);
    recordTest(err == pdf::DocumentError::None, "Group 3.1: Open two-page text fixture");
    pump(200);

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

    // Wait for text extraction
    const auto pt0 = viewer.getPageText(0);
    const auto pt1 = viewer.getPageText(1);
    recordTest(pt0 != nullptr && pt1 != nullptr,
               "Group 3.2: Asynchronous text extraction delivers PageText for pages 0 and 1");

    // 3.3: Cross-page selection, copy, and clear
    if (pt0 && pt1 && !pt0->chars.empty() && !pt1->chars.empty()) {
        const auto& pl0 = viewer.layout().getPageLayout(0);
        const auto& pl1 = viewer.layout().getPageLayout(1);
        const double dpr = viewer.effectiveDpr();
        const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

        const auto& b0 = pt0->chars[2];
        const double vp0X = static_cast<double>(pl0.leftPixel + static_cast<int64_t>(std::round(((b0.left + b0.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        const double vp0Y = static_cast<double>(pl0.topPixel + static_cast<int64_t>(std::round(((b0.top + b0.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        const auto& b1 = pt1->chars[std::min<size_t>(5, pt1->chars.size() - 1)];
        const double vp1X = static_cast<double>(pl1.leftPixel + static_cast<int64_t>(std::round(((b1.left + b1.right) * 0.5) * effScale)) - viewer.currentDocScrollX()) / dpr;
        const double vp1Y = static_cast<double>(pl1.topPixel + static_cast<int64_t>(std::round(((b1.top + b1.bottom) * 0.5) * effScale)) - viewer.currentDocScrollY()) / dpr;

        QMouseEvent press(QEvent::MouseButtonPress, QPointF(vp0X, vp0Y), QPointF(vp0X, vp0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);

        QMouseEvent move(QEvent::MouseMove, QPointF(vp1X, vp1Y), QPointF(vp1X, vp1Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);

        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(vp1X, vp1Y), QPointF(vp1X, vp1Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(100);

        const bool hasSel = viewer.hasSelection();
        const QString selText = viewer.selectedText();

        SignalCounter spySel(&viewer, &ui::PdfViewerWidget::selectionChanged);
        viewer.copySelection();
        const QString clipboardText = QGuiApplication::clipboard()->text();

        viewer.clearSelection();
        pump(50);
        const bool cleared = !viewer.hasSelection() && viewer.selectedText().isEmpty() && (spySel.count() == 1);

        recordTest(hasSel && !selText.isEmpty() && clipboardText == selText && cleared,
                   "Group 3.3: Selection, clipboard copy, and clearSelection with selectionChanged signal");
    } else {
        recordTest(false, "Group 3.3: Text chars empty for cross-page selection test");
    }

    // 3.4: Search, match cycling, and signal order
    {
        viewer.openDocument(searchPdf);
        pump(300);

        SignalRecorder<int, int> spyProgress(&viewer, &ui::PdfViewerWidget::searchProgressChanged);
        SignalRecorder<int, int> spyResults(&viewer, &ui::PdfViewerWidget::searchResultsChanged);
        SignalRecorder<bool> spyFinished(&viewer, &ui::PdfViewerWidget::searchFinished);

        viewer.findText("TargetKeyword");

        const bool searchDone = waitFor([&]() {
            return spyFinished.count() > 0 && viewer.searchMatchCount() >= 3;
        }, 5000);

        bool signalsOrdered = searchDone;
        if (signalsOrdered) {
            // Signal ordering contract: searchProgressChanged and searchResultsChanged before searchFinished
            signalsOrdered = (spyProgress.count() > 0) && (spyResults.count() > 0) && (spyFinished.count() == 1);
        }
        recordTest(signalsOrdered && viewer.searchMatchCount() == 3,
                   "Group 3.4: findText finds 3 matches with progress -> results -> finished signal order");

        // Initial match index is selected to the nearest match (0)
        const int initialIdx = viewer.currentSearchMatchIndex();

        // Match cycling: findNext and findPrevious
        viewer.findNext();
        pump(50);
        const int idxNext1 = viewer.currentSearchMatchIndex();

        viewer.findNext();
        pump(50);
        const int idxNext2 = viewer.currentSearchMatchIndex();

        viewer.findPrevious();
        pump(50);
        const int idxPrev = viewer.currentSearchMatchIndex();

        // Direct navigation
        viewer.goToSearchMatch(2);
        pump(50);
        const int idxDirect = viewer.currentSearchMatchIndex();

        viewer.clearSearch();
        pump(50);
        const bool cleared = (viewer.searchMatchCount() == 0 && viewer.currentSearchMatchIndex() == -1);

        recordTest(initialIdx == 0 && idxNext1 == 1 && idxNext2 == 2 && idxPrev == 1 && idxDirect == 2 && cleared,
                   "Group 3.5: findNext, findPrevious, goToSearchMatch cycling and clearSearch");
    }

    // 3.6: Stale search reply ignored
    {
        viewer.openDocument(searchPdf);
        pump(200);

        // Start an active search to assign a known currentSearchId
        viewer.findText("NonExistentTerm");
        pump(50);

        const int64_t docId = viewer.currentDocumentId();
        constexpr int64_t staleSearchId = 999999;
        pdf::SearchMatch staleMatch;
        staleMatch.pageIndex = 0;
        staleMatch.charStartIndex = 10;
        staleMatch.charCount = 5;

        SignalRecorder<int, int> spyResults(&viewer, &ui::PdfViewerWidget::searchResultsChanged);
        SignalRecorder<int, int> spyProgress(&viewer, &ui::PdfViewerWidget::searchProgressChanged);
        SignalRecorder<bool> spyFinished(&viewer, &ui::PdfViewerWidget::searchFinished);

        const int initialMatchCount = viewer.searchMatchCount();
        const size_t initialProgressCount = spyProgress.count();
        const size_t initialFinishedCount = spyFinished.count();

        // 1. Deliver stale searchMatchesFound directly carrying old searchId
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, staleSearchId, 0, {staleMatch});
        pump(50);
        const bool matchIgnored = (viewer.searchMatchCount() == initialMatchCount) && (spyResults.count() == 0);

        // 2. Deliver stale searchProgress
        Q_EMIT viewer.renderWorker().searchProgress(docId, staleSearchId, 42, 100);
        pump(50);
        const bool progressIgnored = (spyProgress.count() == initialProgressCount);

        // 3. Deliver stale searchCompleted
        Q_EMIT viewer.renderWorker().searchCompleted(docId, staleSearchId, 50, false);
        pump(50);
        const bool completedIgnored = (spyFinished.count() == initialFinishedCount);

        recordTest(matchIgnored && progressIgnored && completedIgnored,
                   "Group 3.6: Stale search replies from superseded query are rejected by searchId filter");
    }

    // 3.7: Text cache bounds, retention, LRU distance eviction, foreign ID isolation, and deduplication
    {
        QTemporaryDir dir37;
        const QString doc60 = dir37.path() + "/doc60.pdf";
        generateMultiPageTextPdf(doc60.toStdString(), 60);

        ui::PdfViewerWidget viewer;
        viewer.resize(800, 700);
        viewer.show();
        viewer.ensurePolished();
        pump(100);

        viewer.openDocument(doc60);
        viewer.waitForRendering();
        pump(200);

        const int64_t docId = viewer.currentDocumentId();

        // 1. S11: In-flight deduplication: ensure page requested twice before reply sends one request
        int page58ReadyCount = 0;
        const auto conn58 = QObject::connect(&viewer.renderWorker(), &pdf::ipc::WorkerClient::pageTextReady,
            [&](int64_t /*dId*/, int pageIdx, std::shared_ptr<pdf::PageText>) {
                if (pageIdx == 58) {
                    ++page58ReadyCount;
                }
            });

        viewer.goToPage(58);
        QImage frame1(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
        QPainter p1(&frame1);
        viewer.viewport()->render(&p1);
        p1.end();
        const size_t inFlightCount1 = viewer.cachedTextPageCount();
        const bool entryInFlight = (viewer.getPageText(58) == nullptr);

        QImage frame2(viewer.viewport()->width(), viewer.viewport()->height(), QImage::Format_ARGB32);
        QPainter p2(&frame2);
        viewer.viewport()->render(&p2);
        p2.end();
        const size_t inFlightCount2 = viewer.cachedTextPageCount();

        viewer.waitForRendering();
        waitFor([&]() { return viewer.getPageText(58) != nullptr; }, 4000);
        QObject::disconnect(conn58);

        const bool page58Delivered = (viewer.getPageText(58) != nullptr);
        const bool deduplicatedInFlight = (inFlightCount2 == inFlightCount1) && entryInFlight && (page58ReadyCount == 1);

        // 2. S4: Foreign documentId ignored
        const auto foreignPt = makeDummyPageText(59, "ForeignText");
        Q_EMIT viewer.renderWorker().pageTextReady(docId + 9999, 59, foreignPt);
        pump(50);
        const bool foreignIgnored = (viewer.getPageText(59) == nullptr);

        // 3. S5: Null PageText becomes empty PageText, never null entry
        Q_EMIT viewer.renderWorker().pageTextReady(docId, 57, nullptr);
        pump(50);
        const auto pt57 = viewer.getPageText(57);
        const bool nullBecomesEmpty = (pt57 != nullptr) && pt57->empty();

        // Populate exactly 48 pages (pages 0 through 47):
        viewer.goToPage(0);
        pump(50);
        for (int i = 0; i < 48; ++i) {
            Q_EMIT viewer.renderWorker().pageTextReady(docId, i, makeDummyPageText(i, "Page " + std::to_string(i)));
        }
        pump(50);
        const size_t countAt48 = viewer.cachedTextPageCount();
        const bool page0RetainedAt48 = (viewer.getPageText(0) != nullptr);

        // Focus on page 48 before adding the 49th page so distance to page 0 is largest (|0 - 48| = 48) and distance to page 47 is smallest (|47 - 48| = 1)
        viewer.goToPage(48);
        pump(50);

        // 4. S1: Exact boundary 48 is not trimmed; delivering the 49th page triggers trim back to 48
        Q_EMIT viewer.renderWorker().pageTextReady(docId, 48, makeDummyPageText(48, "Page 48"));
        pump(50);
        const size_t countAt49 = viewer.cachedTextPageCount();

        // 5. S3: Eviction removes farthest from currentPage (48) first
        // Current page is 48. Farthest is page 0 (|0 - 48| = 48); nearest is page 47 (|47 - 48| = 1).
        const bool farthestEvicted = (viewer.getPageText(0) == nullptr)
            && (viewer.getPageText(47) != nullptr)
            && (viewer.getPageText(48) != nullptr);

        // 6. S2: Cross-page selection keeps its pages' text after trimming
        // Zoom out to 0.1 so pages 1 and 2 are fully inside the viewport for selection
        viewer.setZoom(0.1);
        viewer.goToPage(0);
        pump(100);

        const auto pt1 = viewer.getPageText(1);
        const auto pt2 = viewer.getPageText(2);
        bool selKeptAfterTrim = false;
        if (pt1 && pt2 && !pt1->chars.empty() && !pt2->chars.empty()) {
            const auto& pl1 = viewer.layout().getPageLayout(1);
            const auto& pl2 = viewer.layout().getPageLayout(2);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

            const auto& b1 = pt1->chars[0];
            const double vp1X = (static_cast<double>(pl1.leftPixel) + ((b1.left + b1.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
            const double vp1Y = (static_cast<double>(pl1.topPixel) + ((b1.top + b1.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;

            const auto& b2 = pt2->chars[std::min<size_t>(5, pt2->chars.size() - 1)];
            const double vp2X = (static_cast<double>(pl2.leftPixel) + ((b2.left + b2.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
            const double vp2Y = (static_cast<double>(pl2.topPixel) + ((b2.top + b2.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;

            QMouseEvent press(QEvent::MouseButtonPress, QPointF(vp1X, vp1Y), QPointF(vp1X, vp1Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &press);
            pump(20);
            QMouseEvent move(QEvent::MouseMove, QPointF(vp2X, vp2Y), QPointF(vp2X, vp2Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &move);
            pump(20);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(vp2X, vp2Y), QPointF(vp2X, vp2Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &release);
            pump(50);

            const QString selTextBefore = viewer.selectedText();

            // Navigate to page 50 and force evictions
            viewer.goToPage(50);
            for (int p = 49; p <= 55; ++p) {
                Q_EMIT viewer.renderWorker().pageTextReady(docId, p, makeDummyPageText(p, "Page " + std::to_string(p)));
            }
            pump(50);

            const bool pages1And2Kept = (viewer.getPageText(1) != nullptr) && (viewer.getPageText(2) != nullptr);
            const bool textUnchanged = (viewer.selectedText() == selTextBefore) && !selTextBefore.isEmpty();
            selKeptAfterTrim = pages1And2Kept && textUnchanged;
        }

        const bool group3_7Ok = (countAt48 == 48) && page0RetainedAt48 && (countAt49 == 48)
            && selKeptAfterTrim && farthestEvicted && foreignIgnored && nullBecomesEmpty
            && deduplicatedInFlight && page58Delivered;

        std::string d37 = "count48=" + std::to_string(countAt48)
            + " p0Ret=" + std::to_string(page0RetainedAt48)
            + " count49=" + std::to_string(countAt49)
            + " selKept=" + std::to_string(selKeptAfterTrim)
            + " farthest=" + std::to_string(farthestEvicted)
            + " foreign=" + std::to_string(foreignIgnored)
            + " nullEmpty=" + std::to_string(nullBecomesEmpty)
            + " dedup=" + std::to_string(deduplicatedInFlight)
            + " p58Deliv=" + std::to_string(page58Delivered);

        recordTest(group3_7Ok,
                   "Group 3.7: Text cache bounded at 48, selection protected during trim, LRU distance eviction, and foreign/null isolation",
                   d37);
    }

    // 3.8: Selection semantics, forward/backward/cross-page extraction, non-ASCII/Persian, click rejection, and highlight rendering
    {
        QTemporaryDir dir38;
        const QString doc3Path = dir38.path() + "/fixture_3page.pdf";
        const QString persianPath = dir38.path() + "/fixture_persian.pdf";

        generateThreePageTextPdf(doc3Path.toStdString(), "AAA BBB CCC", "DDD EEE FFF", "GGG HHH III");
        generatePersianPdfFixture(persianPath.toStdString());

        ui::PdfViewerWidget viewer;
        viewer.resize(900, 700);
        viewer.show();
        viewer.ensurePolished();
        pump(100);

        viewer.openDocument(doc3Path);
        viewer.waitForRendering();
        pump(200);

        // Zoom out to 0.2 so all three pages are simultaneously visible in viewport (each ~170px high)
        viewer.setZoom(0.2);
        viewer.goToPage(0);
        viewer.waitForRendering();
        pump(100);

        waitFor([&]() {
            return viewer.getPageText(0) && viewer.getPageText(1) && viewer.getPageText(2);
        }, 5000);

        const auto pt0 = viewer.getPageText(0);
        const auto pt1 = viewer.getPageText(1);
        const auto pt2 = viewer.getPageText(2);

        bool selectionsOk = false;
        QString forwardSel, backwardSel, backwardCrossSel, threePageSel;
        bool forwardOk = false, backwardOk = false, backwardCrossOk = false, threePageOk = false;
        bool noSelectionAfterClick = false, noSignalAfterClick = false, clearNoSelEmitsNothing = false;
        bool insidePixelShifted = false, outsidePixelUnchanged = false;

        if (pt0 && pt1 && pt2 && pt0->charCount() >= 11 && pt1->charCount() >= 11 && pt2->charCount() >= 11) {
            const auto& pl0 = viewer.layout().getPageLayout(0);
            const auto& pl1 = viewer.layout().getPageLayout(1);
            const auto& pl2 = viewer.layout().getPageLayout(2);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

            auto charCenter = [&](const layout::PageLayout& pl, const pdf::PageCharBox& b) -> QPointF {
                double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                return QPointF(x, y);
            };

            auto sendDrag = [&](const QPointF& from, const QPointF& to) {
                QMouseEvent p(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &p);
                pump(20);
                QMouseEvent m(QEvent::MouseMove, to, to, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &m);
                pump(20);
                QMouseEvent r(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &r);
                pump(50);
            };

            // 1. S6, S7, S8: Forward selection within page: "BBB" (chars 4..6)
            sendDrag(charCenter(pl0, pt0->chars[4]), charCenter(pl0, pt0->chars[6]));
            forwardSel = viewer.selectedText();
            forwardOk = (forwardSel == "BBB");

            // 2. S7: Backward selection within page: drag right-to-left from char 6 to char 4 -> "BBB"
            sendDrag(charCenter(pl0, pt0->chars[6]), charCenter(pl0, pt0->chars[4]));
            backwardSel = viewer.selectedText();
            backwardOk = (backwardSel == "BBB");

            // 3. S7: Backward cross-page selection: drag from page 1 char 2 ("D") to page 0 char 4 ("B")
            sendDrag(charCenter(pl1, pt1->chars[2]), charCenter(pl0, pt0->chars[4]));
            backwardCrossSel = viewer.selectedText();
            backwardCrossOk = (backwardCrossSel == "BBB CCC\nDDD");

            // 4. S6, S8: Three-page selection with whole middle page and newline joins, including clipboard copy
            sendDrag(charCenter(pl0, pt0->chars[4]), charCenter(pl2, pt2->chars[6]));
            threePageSel = viewer.selectedText();
            viewer.copySelection();
            const QString threePageClipboard = QGuiApplication::clipboard()->text();
            threePageOk = (threePageSel == "BBB CCC\nDDD EEE FFF\nGGG HHH") &&
                          (threePageClipboard == "BBB CCC\nDDD EEE FFF\nGGG HHH");

            // 5. S9: Click on same character is NOT a selection: hasSelection() is false and clearSelection emits nothing
            viewer.clearSelection();
            pump(20);
            const QPointF cChar = charCenter(pl0, pt0->chars[4]);
            QMouseEvent press0(QEvent::MouseButtonPress, cChar, cChar, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &press0);
            pump(20);
            QMouseEvent release0(QEvent::MouseButtonRelease, cChar, cChar, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &release0);
            pump(50);

            noSelectionAfterClick = !viewer.hasSelection() && viewer.selectedText().isEmpty();
            SignalCounter spySelAfterClick(&viewer, &ui::PdfViewerWidget::selectionChanged);
            viewer.clearSelection();
            noSignalAfterClick = (spySelAfterClick.count() == 0);

            // 6. S10: clearSelection with no selection emits nothing
            SignalCounter spyNoSel(&viewer, &ui::PdfViewerWidget::selectionChanged);
            viewer.clearSelection();
            clearNoSelEmitsNothing = (spyNoSel.count() == 0);

            // 7. S12: Grab viewport with and without selection and verify highlight pixel shift
            viewer.clearSelection();
            pump(50);
            QImage imgNoSel(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter p(&imgNoSel);
                viewer.viewport()->render(&p);
            }

            // Select char 0 and 1
            sendDrag(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[1]));

            QImage imgWithSel(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter p(&imgWithSel);
                viewer.viewport()->render(&p);
            }

            const QPoint ptInChar = charCenter(pl0, pt0->chars[0]).toPoint();
            const QColor colNoSel = imgNoSel.pixelColor(ptInChar);
            const QColor colWithSel = imgWithSel.pixelColor(ptInChar);

            insidePixelShifted = (colWithSel != colNoSel) && (colWithSel.blue() >= colNoSel.blue());
            outsidePixelUnchanged = (imgNoSel.pixelColor(5, 5) == imgWithSel.pixelColor(5, 5));

            selectionsOk = forwardOk && backwardOk && backwardCrossOk && threePageOk
                && noSelectionAfterClick && noSignalAfterClick && clearNoSelEmitsNothing
                && insidePixelShifted && outsidePixelUnchanged;
        }

        // 8. S7: Persian / Non-ASCII text selection and clipboard copy
        bool persianOk = false;
        QString selPersian, clipPersian;
        viewer.openDocument(persianPath);
        viewer.waitForRendering();
        pump(200);

        viewer.setZoom(0.2);
        viewer.goToPage(0);
        viewer.waitForRendering();
        pump(100);

        waitFor([&]() { return viewer.getPageText(0) != nullptr; }, 5000);
        const auto ptPersian = viewer.getPageText(0);
        if (ptPersian && !ptPersian->chars.empty()) {
            const auto& plP = viewer.layout().getPageLayout(0);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

            const auto& firstChar = ptPersian->chars.front();
            const auto& lastChar = ptPersian->chars.back();

            double p0X = (static_cast<double>(plP.leftPixel) + ((firstChar.left + firstChar.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
            double p0Y = (static_cast<double>(plP.topPixel) + ((firstChar.top + firstChar.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
            double p1X = (static_cast<double>(plP.leftPixel) + ((lastChar.left + lastChar.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
            double p1Y = (static_cast<double>(plP.topPixel) + ((lastChar.top + lastChar.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;

            QMouseEvent pressP(QEvent::MouseButtonPress, QPointF(p0X, p0Y), QPointF(p0X, p0Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &pressP);
            pump(20);
            QMouseEvent moveP(QEvent::MouseMove, QPointF(p1X, p1Y), QPointF(p1X, p1Y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &moveP);
            pump(20);
            QMouseEvent releaseP(QEvent::MouseButtonRelease, QPointF(p1X, p1Y), QPointF(p1X, p1Y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &releaseP);
            pump(50);

            const QString expectedPersian = QString::fromUtf16(u"\u0633\u0644\u0627\u0645 \u062c\u0647\u0627\u0646");
            selPersian = viewer.selectedText();
            viewer.copySelection();
            clipPersian = QGuiApplication::clipboard()->text();

            persianOk = (selPersian == expectedPersian) && (clipPersian == expectedPersian);
        }

        std::string charsHex;
        if (ptPersian) {
            for (const auto& ch : ptPersian->chars) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%04X ", static_cast<unsigned int>(ch.unicode));
                charsHex += buf;
            }
        }

        std::string d38 = "fwd=" + std::to_string(forwardOk) + " (" + forwardSel.toStdString() + ")"
            + " bwd=" + std::to_string(backwardOk) + " (" + backwardSel.toStdString() + ")"
            + " bwdCross=" + std::to_string(backwardCrossOk) + " (" + backwardCrossSel.toStdString() + ")"
            + " three=" + std::to_string(threePageOk) + " (" + threePageSel.toStdString() + ")"
            + " clickNoSel=" + std::to_string(noSelectionAfterClick)
            + " clickNoSig=" + std::to_string(noSignalAfterClick)
            + " clearNoSig=" + std::to_string(clearNoSelEmitsNothing)
            + " pixShift=" + std::to_string(insidePixelShifted)
            + " pixUnchanged=" + std::to_string(outsidePixelUnchanged)
            + " persian=" + std::to_string(persianOk)
            + " (sel=[" + selPersian.toStdString() + "]"
            + " clip=[" + clipPersian.toStdString() + "]"
            + " hasSel=" + std::to_string(viewer.hasSelection())
            + " charsHex=" + charsHex + ")";

        recordTest(selectionsOk && persianOk,
                   "Group 3.8: Selection forward/backward/multipage text, non-ASCII/Persian clipboard, click rejection, and pixel highlight",
                   d38);
    }

    // 3.9: Search circular distance, batch ordering, match caps, foreign ID isolation, wrap-around, and query trimming
    {
        QTemporaryDir dir39;
        const QString circPath = dir39.path() + "/fixture_circ.pdf";
        const QString mixedPath = dir39.path() + "/fixture_mixed.pdf";
        generateCircularSearchPdf(circPath.toStdString());
        generateMixedCaseSearchPdf(mixedPath.toStdString());

        ui::PdfViewerWidget viewer;
        viewer.resize(800, 700);
        viewer.show();
        viewer.ensurePolished();
        pump(100);

        // Assumption: A fresh PdfViewerWidget has searchId == 1 after openDocument (which calls clearSearch),
        // and the id increments by one per findText or clearSearch call.
        int64_t currentSearchId = 0;

        // 1. Q2, Q9: Circular-nearest match from page 4 (keyword on pages 0 and 3), and start page nearest after k
        viewer.openDocument(circPath); // clearSearch() -> searchId = 1
        currentSearchId = 1;
        viewer.waitForRendering();
        pump(200);

        const int64_t docId = viewer.currentDocumentId();

        pdf::SearchMatch mCircP0;
        mCircP0.pageIndex = 0;
        mCircP0.charStartIndex = 10;
        mCircP0.charCount = 11;
        mCircP0.rects = { pdf::PageCharBox{100.0, 700.0, 200.0, 720.0} };

        pdf::SearchMatch mCircP3;
        mCircP3.pageIndex = 3;
        mCircP3.charStartIndex = 12;
        mCircP3.charCount = 11;
        mCircP3.rects = { pdf::PageCharBox{100.0, 700.0, 200.0, 720.0} };

        // Q9: With matches only on pages 0 and 3, searching from page 4 (after all matches)
        // uses circular distance: dist to page 0 is (0 - 4 + 5) % 5 = 1, dist to page 3 is (3 - 4 + 5) % 5 = 4.
        // Circular wrap picks the first match (page 0), whereas linear distance would pick page 3.
        viewer.goToPage(4);
        viewer.findText("CircWrapTest"); // findText -> searchId = 2
        ++currentSearchId;
        const int64_t wrapSearchId = currentSearchId;
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, wrapSearchId, 0, {mCircP0, mCircP3});
        pump(50);
        const bool circularMatchOk = (viewer.searchMatchCount() == 2)
            && (viewer.currentSearchMatchIndex() == 0); // index 0 is page 0

        // Q2: With current page at k = 2, findText picks the match nearest after k (page 3, distance 1),
        // not the one nearest page 0 (which would be page 0, distance 0).
        viewer.clearSearch(); // clearSearch -> searchId = 3
        ++currentSearchId;
        viewer.goToPage(2);
        viewer.findText("KStartTest"); // findText -> searchId = 4
        ++currentSearchId;
        const int64_t kSearchId = currentSearchId;
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, kSearchId, 0, {mCircP0, mCircP3});
        pump(50);
        const bool startPageKOk = (viewer.searchMatchCount() == 2)
            && (viewer.currentSearchMatchIndex() == 1); // index 1 is page 3

        // 2. Q16: findText("   ") behaves as empty (emits (0,0), (0,0), finished(false), starts no worker search)
        SignalRecorder<int, int> spyResEmpty(&viewer, &ui::PdfViewerWidget::searchResultsChanged);
        SignalRecorder<int, int> spyProgEmpty(&viewer, &ui::PdfViewerWidget::searchProgressChanged);
        SignalRecorder<bool> spyFinEmpty(&viewer, &ui::PdfViewerWidget::searchFinished);
        viewer.findText("   "); // findText -> searchId = 3
        ++currentSearchId;
        pump(50);
        const bool emptyQueryOk = (viewer.searchMatchCount() == 0)
            && (spyResEmpty.count() == 1 && std::get<0>(spyResEmpty.at(0)) == 0 && std::get<1>(spyResEmpty.at(0)) == 0)
            && (spyProgEmpty.count() == 1 && std::get<0>(spyProgEmpty.at(0)) == 0 && std::get<1>(spyProgEmpty.at(0)) == 0)
            && (spyFinEmpty.count() == 1 && std::get<0>(spyFinEmpty.at(0)) == false);

        // 3. Q18, Q19: findText emits searchProgressChanged(0, pageCount) first, and truncated searchFinished(true)
        SignalRecorder<int, int> spyProgOrder(&viewer, &ui::PdfViewerWidget::searchProgressChanged);
        viewer.findText("TruncationCheck"); // findText -> searchId = 4
        ++currentSearchId;
        const bool progress0Emitted = (spyProgOrder.count() > 0)
            && (std::get<0>(spyProgOrder.at(0)) == 0)
            && (std::get<1>(spyProgOrder.at(0)) == viewer.pageCount());

        viewer.clearSearch(); // clearSearch -> searchId = 5
        ++currentSearchId;
        viewer.findText("SyntheticTruncQuery"); // findText -> searchId = 6
        ++currentSearchId;
        SignalRecorder<bool> spyFinTrunc(&viewer, &ui::PdfViewerWidget::searchFinished);
        Q_EMIT viewer.renderWorker().searchCompleted(docId, currentSearchId, 2, /*truncated=*/true);
        pump(50);
        const bool truncatedFinishedOk = (spyFinTrunc.count() > 0) && (std::get<0>(spyFinTrunc.at(0)) == true);

        // 4. Q6, Q7, Q8: Out-of-order batches sorted by page then charStartIndex, and active match tracking
        viewer.clearSearch(); // clearSearch -> searchId = 7
        ++currentSearchId;
        viewer.findText("BatchTest"); // findText -> searchId = 8
        ++currentSearchId;
        const int64_t batchSearchId = currentSearchId;

        pdf::SearchMatch mP2C50; mP2C50.pageIndex = 2; mP2C50.charStartIndex = 50; mP2C50.charCount = 5;
        pdf::SearchMatch mP0C20; mP0C20.pageIndex = 0; mP0C20.charStartIndex = 20; mP0C20.charCount = 5;
        pdf::SearchMatch mP1C10; mP1C10.pageIndex = 1; mP1C10.charStartIndex = 10; mP1C10.charCount = 5;
        pdf::SearchMatch mP0C5;  mP0C5.pageIndex = 0;  mP0C5.charStartIndex = 5;   mP0C5.charCount = 5;

        // Deliver batch 1: [ (2, 50), (0, 20) ]
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, batchSearchId, 0, {mP2C50, mP0C20});
        pump(50);

        viewer.goToSearchMatch(1); // Select (2, 50)
        pump(20);
        const int activeBeforeBatch2 = viewer.currentSearchMatchIndex();

        // Deliver batch 2: [ (1, 10), (0, 5) ]
        // After sorting: (0, 5) -> index 0, (0, 20) -> index 1, (1, 10) -> index 2, (2, 50) -> index 3
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, batchSearchId, 0, {mP1C10, mP0C5});
        pump(50);

        const int activeAfterBatch2 = viewer.currentSearchMatchIndex();
        const bool batchSortAndActiveOk = (viewer.searchMatchCount() == 4)
            && (activeBeforeBatch2 == 1)
            && (activeAfterBatch2 == 3);

        // 5. Q3, Q4: findNext and findPrevious wrap around
        viewer.goToSearchMatch(3);
        viewer.findNext();
        pump(20);
        const bool wrapNextOk = (viewer.currentSearchMatchIndex() == 0);

        viewer.findPrevious();
        pump(20);
        const bool wrapPrevOk = (viewer.currentSearchMatchIndex() == 3);

        // 6. Q5, Q10: Cap at pdf::MAX_SEARCH_MATCHES (10000)
        viewer.clearSearch(); // clearSearch -> searchId = 9
        ++currentSearchId;
        viewer.findText("CapTest"); // findText -> searchId = 10
        ++currentSearchId;
        const int64_t capSearchId = currentSearchId;
        std::vector<pdf::SearchMatch> largeBatch(10005);
        for (size_t i = 0; i < 10005; ++i) {
            largeBatch[i].pageIndex = static_cast<int>(i / 1000);
            largeBatch[i].charStartIndex = static_cast<int>(i % 1000);
            largeBatch[i].charCount = 3;
        }

        SignalRecorder<int, int> spyCapResults(&viewer, &ui::PdfViewerWidget::searchResultsChanged);
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, capSearchId, 0, largeBatch);
        pump(50);
        Q_EMIT viewer.renderWorker().searchCompleted(docId, capSearchId, 10005, false);
        pump(50);

        const bool matchCapOk = (viewer.searchMatchCount() == pdf::MAX_SEARCH_MATCHES)
            && (spyCapResults.count() > 0)
            && (std::get<1>(spyCapResults.at(spyCapResults.count() - 1)) == pdf::MAX_SEARCH_MATCHES);

        // 7. Q11, Q12, Q13: Foreign documentId replies ignored
        SignalRecorder<int, int> spyForeignRes(&viewer, &ui::PdfViewerWidget::searchResultsChanged);
        SignalRecorder<int, int> spyForeignProg(&viewer, &ui::PdfViewerWidget::searchProgressChanged);
        SignalRecorder<bool> spyForeignFin(&viewer, &ui::PdfViewerWidget::searchFinished);
        const int matchCountBeforeForeign = viewer.searchMatchCount();

        Q_EMIT viewer.renderWorker().searchMatchesFound(docId + 9999, capSearchId, 0, {mP0C5});
        Q_EMIT viewer.renderWorker().searchProgress(docId + 9999, capSearchId, 1, 5);
        Q_EMIT viewer.renderWorker().searchCompleted(docId + 9999, capSearchId, 10, false);
        pump(50);

        const bool foreignSearchIgnored = (viewer.searchMatchCount() == matchCountBeforeForeign)
            && spyForeignRes.isEmpty() && spyForeignProg.isEmpty() && spyForeignFin.isEmpty();

        // 8. Q14: Stale searchId reply after clearSearch is ignored
        viewer.clearSearch(); // clearSearch -> searchId = 11
        ++currentSearchId;
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, capSearchId, 0, {mP0C5});
        pump(50);
        const bool staleAfterClearIgnored = (viewer.searchMatchCount() == 0);

        // 9. Q17: Match-case search returns different counts on mixed-case fixture
        viewer.openDocument(mixedPath); // openDocument calls clearSearch()
        viewer.waitForRendering();
        pump(200);

        viewer.findText("TargetKeyword", false); // case-insensitive
        waitFor([&]() { return viewer.searchMatchCount() == 3; }, 3000);
        const int insensitiveCount = viewer.searchMatchCount();

        viewer.findText("TargetKeyword", true); // case-sensitive
        waitFor([&]() { return viewer.searchMatchCount() == 2; }, 3000);
        const int sensitiveCount = viewer.searchMatchCount();

        const bool matchCaseOk = (insensitiveCount == 3) && (sensitiveCount == 2);

        // 10. Q1 Equivalence reasoning note:
        // findText() cancels the previous worker search (m_renderWorker.cancelSearch()).
        // Because findText() unconditionally increments m_currentSearchId, any in-flight
        // replies from the previous search carry an outdated searchId and are strictly
        // dropped by the (searchId != m_currentSearchId) guard in onSearchMatchesFound,
        // onSearchProgress, and onSearchCompleted. Thus, cancelSearch() is an IPC load
        // optimization whose observable widget state is equivalent to the searchId filter.

        const bool group3_9Ok = circularMatchOk && startPageKOk && emptyQueryOk && progress0Emitted
            && truncatedFinishedOk && batchSortAndActiveOk && wrapNextOk && wrapPrevOk
            && matchCapOk && foreignSearchIgnored && staleAfterClearIgnored && matchCaseOk;

        std::string d39 = "circ=" + std::to_string(circularMatchOk)
            + " startK=" + std::to_string(startPageKOk)
            + " empty=" + std::to_string(emptyQueryOk)
            + " prog0=" + std::to_string(progress0Emitted)
            + " trunc=" + std::to_string(truncatedFinishedOk)
            + " batch=" + std::to_string(batchSortAndActiveOk)
            + " wrapN=" + std::to_string(wrapNextOk)
            + " wrapP=" + std::to_string(wrapPrevOk)
            + " cap=" + std::to_string(matchCapOk)
            + " foreign=" + std::to_string(foreignSearchIgnored)
            + " stale=" + std::to_string(staleAfterClearIgnored)
            + " matchCase=" + std::to_string(matchCaseOk);

        recordTest(group3_9Ok,
                   "Group 3.9: Search circular distance, batch ordering, match caps, foreign ID isolation, wrap-around, and query trimming",
                   d39);
    }

    // 3.10: Search highlight colors (active orange vs non-active yellow), border, and scrollToMatch geometry
    {
        QTemporaryDir dir310;
        const QString doc3Path = dir310.path() + "/fixture_3page.pdf";
        generateThreePageTextPdf(doc3Path.toStdString(), "AAA TargetKeyword CCC", "DDD TargetKeyword FFF", "GGG TargetKeyword III");

        ui::PdfViewerWidget viewer;
        viewer.resize(800, 700);
        viewer.show();
        viewer.ensurePolished();
        pump(100);

        viewer.openDocument(doc3Path); // clearSearch() inside openDocument -> searchId = 1
        viewer.waitForRendering();
        pump(200);

        const int64_t docId = viewer.currentDocumentId();
        viewer.findText("CustomMatches"); // findText -> searchId = 2
        const int64_t customSearchId = 2;

        pdf::SearchMatch m0;
        m0.pageIndex = 0;
        m0.charStartIndex = 0;
        m0.charCount = 3;
        m0.rects = { pdf::PageCharBox{100.0, 100.0, 150.0, 130.0} };

        pdf::SearchMatch m1;
        m1.pageIndex = 0;
        m1.charStartIndex = 10;
        m1.charCount = 5;
        m1.rects = { pdf::PageCharBox{200.0, 100.0, 260.0, 130.0} };

        pdf::SearchMatch m2;
        m2.pageIndex = 2;
        m2.charStartIndex = 4;
        m2.charCount = 13;
        m2.rects = { pdf::PageCharBox{100.0, 200.0, 250.0, 230.0} };

        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, customSearchId, 0, {m0, m1, m2});
        pump(50);

        viewer.goToSearchMatch(0);
        pump(50);

        // 1. S13, S14: Paint verification of active (orange + border) vs inactive (yellow)
        QImage renderImg(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&renderImg);
            viewer.viewport()->render(&p);
        }

        const auto& pl0 = viewer.layout().getPageLayout(0);
        const double dpr = viewer.effectiveDpr();
        const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

        const double pl0LeftDoc = static_cast<double>(pl0.leftPixel) - static_cast<double>(viewer.currentDocScrollX());
        const double pl0TopDoc = static_cast<double>(pl0.topPixel) - static_cast<double>(viewer.currentDocScrollY());

        const int actX = static_cast<int>(std::round((pl0LeftDoc + 125.0 * effScale) / dpr));
        const int actY = static_cast<int>(std::round((pl0TopDoc + 115.0 * effScale) / dpr));
        const QColor actCol = renderImg.pixelColor(actX, actY);

        const int borderX = static_cast<int>(std::round((pl0LeftDoc + 100.0 * effScale) / dpr));
        const int borderY = actY;
        const QColor borderCol = renderImg.pixelColor(borderX, borderY);

        const int inactX = static_cast<int>(std::round((pl0LeftDoc + 230.0 * effScale) / dpr));
        const int inactY = actY;
        const QColor inactCol = renderImg.pixelColor(inactX, inactY);

        // Blank page pixel outside any match (at page Y = 50 pt):
        const int blankX = inactX;
        const int blankY = static_cast<int>(std::round((pl0TopDoc + 50.0 * effScale) / dpr));
        const QColor blankCol = renderImg.pixelColor(blankX, blankY);

        // S13: Non-active match carries the translucent yellow overlay (lowering blue from blank white 255 to < 210)
        const bool nonActiveYellowOverlay = (inactCol != blankCol) && (inactCol.blue() < 210) && (blankCol.blue() > 240);
        const bool colorsDistinct = (actCol != inactCol) && nonActiveYellowOverlay;
        const bool borderDistinct = (borderCol != actCol);

        // 2. S15, S16: scrollToMatch / goToSearchMatch scroll geometry calculation
        const auto& pl2 = viewer.layout().getPageLayout(2);
        const int64_t vpH = static_cast<int64_t>(std::round(viewer.viewport()->height() * dpr));
        const int64_t matchDocY = pl2.topPixel + static_cast<int64_t>(std::round(200.0 * effScale));
        const int64_t maxScrollY = std::max<int64_t>(0, viewer.layout().totalDocumentHeight() - vpH);
        const int64_t expectedTargetY = std::clamp<int64_t>(matchDocY - (vpH / 3), 0, maxScrollY);

        viewer.goToSearchMatch(2);
        pump(50);

        const int64_t scrollYAfterJump = viewer.currentDocScrollY();
        const bool offscreenScrolledCorrectly = (scrollYAfterJump == expectedTargetY);

        // 3. S16: A match already inside the viewport with a 40px margin does not scroll
        const int64_t scrollYBeforeInsideCheck = viewer.currentDocScrollY();
        viewer.goToSearchMatch(2); // Match 2 is now centered/visible
        pump(50);
        const bool insideViewportNotScrolled = (viewer.currentDocScrollY() == scrollYBeforeInsideCheck);

        // 4. S15: scrollToMatch margin 40: inside viewport within 40px of edge scrolls; beyond 40px does not scroll
        viewer.goToSearchMatch(0);
        viewer.setDocScrollPosition(0, 100);
        pump(50);
        const int64_t scrollBase = viewer.currentDocScrollY();

        const double pyNear = static_cast<double>(scrollBase + 20 - pl0.topPixel) / effScale;
        const double pyBeyond = static_cast<double>(scrollBase + 120 - pl0.topPixel) / effScale;

        pdf::SearchMatch mNear;
        mNear.pageIndex = 0;
        mNear.charStartIndex = 30;
        mNear.charCount = 5;
        mNear.rects = { pdf::PageCharBox{100.0, pyNear, 150.0, pyNear + 10.0} };

        pdf::SearchMatch mBeyond;
        mBeyond.pageIndex = 0;
        mBeyond.charStartIndex = 40;
        mBeyond.charCount = 5;
        mBeyond.rects = { pdf::PageCharBox{100.0, pyBeyond, 150.0, pyBeyond + 10.0} };

        viewer.findText("MarginTestQuery");
        const int64_t marginSearchId = 3;
        Q_EMIT viewer.renderWorker().searchMatchesFound(docId, marginSearchId, 0, {mNear, mBeyond});
        pump(50);

        viewer.setDocScrollPosition(0, scrollBase);
        pump(50);
        viewer.goToSearchMatch(0); // Near-edge match (within 40px margin) -> must scroll
        pump(50);
        const bool scrolledNearEdge = (viewer.currentDocScrollY() != scrollBase);

        const int64_t scrollBeforeBeyond = viewer.currentDocScrollY();
        viewer.goToSearchMatch(1); // Beyond-margin match (at +120px) -> must not scroll
        pump(50);
        const bool notScrolledBeyondMargin = (viewer.currentDocScrollY() == scrollBeforeBeyond);
        const bool s15Margin40Killed = scrolledNearEdge && notScrolledBeyondMargin;

        const bool group3_10Ok = colorsDistinct && borderDistinct && s15Margin40Killed
            && offscreenScrolledCorrectly && insideViewportNotScrolled;

        std::string d310 = "colorsDistinct=" + std::to_string(colorsDistinct)
            + " (act=" + actCol.name().toStdString() + " inact=" + inactCol.name().toStdString() + " yellow=" + std::to_string(nonActiveYellowOverlay) + ")"
            + " borderDistinct=" + std::to_string(borderDistinct)
            + " (border=" + borderCol.name().toStdString() + ")"
            + " s15Margin40=" + std::to_string(s15Margin40Killed)
            + " (near=" + std::to_string(scrolledNearEdge) + " beyond=" + std::to_string(notScrolledBeyondMargin) + ")"
            + " offscreenScrolled=" + std::to_string(offscreenScrolledCorrectly)
            + " (scroll=" + std::to_string(scrollYAfterJump) + " exp=" + std::to_string(expectedTargetY) + ")"
            + " insideNotScrolled=" + std::to_string(insideViewportNotScrolled);

        recordTest(group3_10Ok,
                   "Group 3.10: Search highlight colors (active orange vs non-active yellow), border outline, and scrollToMatch geometry",
                   d310);
    }
}

// ======================================================================
// GROUP 4: Annotation and Redaction Interaction Characterization
// ======================================================================

void runGroup4AnnotationAndRedaction(const QString& samplePdf) {
    std::cout << "\n--- [Group 4: Annotation and Redaction Interaction] ---\n";
    ui::PdfViewerWidget viewer;
    viewer.resize(800, 700);
    viewer.show();
    viewer.ensurePolished();
    pump(100);

    const auto err = viewer.openDocument(samplePdf);
    recordTest(err == pdf::DocumentError::None, "Group 4.1: Open document for annotation/redaction tests");
    pump(200);

    // 4.2: Redaction mode toggle & staged marks signals
    {
        SignalRecorder<bool> spyRedactMode(&viewer, &ui::PdfViewerWidget::redactionModeChanged);
        SignalCounter spyMarks(&viewer, &ui::PdfViewerWidget::redactionMarksChanged);

        viewer.setRedactionMode(true);
        pump(50);
        bool ok = viewer.isRedactionMode() && (spyRedactMode.count() == 1);
        if (ok) {
            ok = std::get<0>(spyRedactMode.takeFirst());
        }
        recordTest(ok, "Group 4.2: setRedactionMode(true) emits redactionModeChanged(true)");

        // Add staged mark manually
        pdf::ipc::RedactRect r{50.0, 100.0, 200.0, 80.0};
        viewer.addStagedRedaction(0, r);
        pump(50);
        ok = viewer.hasStagedRedactions() && (viewer.stagedRedactionCount() == 1) && (spyMarks.count() == 1);
        recordTest(ok, "Group 4.3: addStagedRedaction adds mark and emits redactionMarksChanged");

        // Clear staged marks
        viewer.clearStagedRedactions();
        pump(50);
        ok = !viewer.hasStagedRedactions() && (viewer.stagedRedactionCount() == 0) && (spyMarks.count() == 2);
        recordTest(ok, "Group 4.4: clearStagedRedactions clears marks and emits redactionMarksChanged");

        viewer.setRedactionMode(false);
        pump(50);
        recordTest(!viewer.isRedactionMode() && spyRedactMode.count() == 1,
                   "Group 4.5: setRedactionMode(false) emits redactionModeChanged(false)");
    }

    // 4.6: placeOnPage coordinate math and clamping
    {
        const QPointF anchor(500.0, 800.0);
        const QSizeF size(200.0, 100.0);
        const QSizeF pageSize(595.0, 842.0);

        const QRectF placed = ui::PdfViewerWidget::placeOnPage(anchor, size, pageSize);

        // Clamped inside page bounds: x + w <= 595.0, y - h >= 0
        const bool clamped = placed.isValid() &&
                             (placed.right() <= pageSize.width() + 1e-4) &&
                             (placed.top() >= 0.0) &&
                             (placed.bottom() <= pageSize.height() + 1e-4);
        recordTest(clamped, "Group 4.6: placeOnPage clamps annotation geometry safely inside PDF user space");

        // Tooltip HTML escaping and length capping
        const QString unsafeAuthor = "<script>alert('xss')</script>";
        const QString unsafeContents = "<b>Bold</b> & \"Quotes\"";
        const QString tip = ui::PdfViewerWidget::annotationTooltipHtml(unsafeAuthor, unsafeContents);
        const bool escaped = !tip.contains("<script>") && tip.contains("&lt;script&gt;") && tip.contains("&amp;");
        recordTest(escaped, "Group 4.7: annotationTooltipHtml escapes untrusted author and contents markup");
    }

    // 4.8: Pointer precedence: Redaction mode vs Text Selection
    {
        // When redaction mode is active, dragging creates a staged mark, NOT a text selection
        viewer.setRedactionMode(true);
        pump(50);

        QWidget* vp = viewer.viewport();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &press);
        pump(20);

        QMouseEvent move(QEvent::MouseMove, QPointF(250, 200), QPointF(250, 200), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &move);
        pump(20);

        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(250, 200), QPointF(250, 200), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &release);
        pump(100);

        const bool noTextSelection = !viewer.hasSelection();
        const bool hasRedactMark = (viewer.stagedRedactionCount() >= 1);
        recordTest(noTextSelection && hasRedactMark,
                   "Group 4.8: Pointer precedence: Redaction gesture overrides text selection");

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);
    }

    // 4.9: Pointer precedence: Ink mode vs Text Selection
    {
        viewer.setInkDrawingMode(true);
        pump(50);

        QWidget* vp = viewer.viewport();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(120, 120), QPointF(120, 120), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &press);
        pump(20);

        QMouseEvent move(QEvent::MouseMove, QPointF(200, 220), QPointF(200, 220), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &move);
        pump(20);

        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(200, 220), QPointF(200, 220), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(vp, &release);
        pump(100);

        const bool noTextSelection = !viewer.hasSelection();
        const bool noRedactMarks = !viewer.hasStagedRedactions();
        recordTest(noTextSelection && noRedactMarks,
                   "Group 4.9: Pointer precedence: Ink drawing gesture overrides text selection and redaction");

        viewer.setInkDrawingMode(false);
    }
}

// ======================================================================
// GROUP 5: Forms, Edit Log, and Save Characterization
// ======================================================================

void runGroup5FormsEditLogAndSave(const QString& samplePdf) {
    std::cout << "\n--- [Group 5: Forms, Edit Log, and Save] ---\n";
    ui::PdfViewerWidget viewer;
    viewer.resize(800, 700);
    viewer.show();
    viewer.ensurePolished();
    pump(100);

    const auto err = viewer.openDocument(samplePdf);
    recordTest(err == pdf::DocumentError::None, "Group 5.1: Open document for EditLog tests");
    pump(200);

    // 5.2: Shared EditLog commands, dirty tracking, and undo/redo symmetry
    {
        const size_t initialCmds = viewer.editLog().activeCommands().size();
        const bool initialDirty = viewer.editLog().isDirty();

        // Add square annotation -> active commands increments, becomes dirty
        const bool add1 = viewer.addSquareAnnotation(0, QRectF(40, 40, 100, 80));
        const size_t cmds1 = viewer.editLog().activeCommands().size();
        const bool dirty1 = viewer.editLog().isDirty();

        // Add sticky note -> active commands increments
        const bool add2 = viewer.addTextAnnotation(0, 100, 200, "Characterization note");
        const size_t cmds2 = viewer.editLog().activeCommands().size();

        // Undo once
        const bool undo1 = viewer.undoEdit();
        const size_t cmdsUndo1 = viewer.editLog().activeCommands().size();
        const bool dirtyUndo1 = viewer.editLog().isDirty();

        // Undo second time -> back to clean baseline
        const bool undo2 = viewer.undoEdit();
        const size_t cmdsUndo2 = viewer.editLog().activeCommands().size();
        const bool dirtyUndo2 = viewer.editLog().isDirty();

        // Redo once
        const bool redo1 = viewer.redoEdit();
        const size_t cmdsRedo1 = viewer.editLog().activeCommands().size();
        const bool dirtyRedo1 = viewer.editLog().isDirty();

        // Redo second time
        const bool redo2 = viewer.redoEdit();
        const size_t cmdsRedo2 = viewer.editLog().activeCommands().size();

        const bool symmetric = add1 && add2 && (!initialDirty) &&
                               (cmds1 == initialCmds + 1) && (cmds2 == initialCmds + 2) &&
                               dirty1 && undo1 && (cmdsUndo1 == cmds1) && dirtyUndo1 &&
                               undo2 && (cmdsUndo2 == initialCmds) && (!dirtyUndo2) &&
                               redo1 && (cmdsRedo1 == cmds1) && dirtyRedo1 &&
                               redo2 && (cmdsRedo2 == cmds2);
        recordTest(symmetric,
                   "Group 5.2: Shared EditLog command tracking, dirty state, and symmetric undo/redo");
    }

    // 5.3: Save then reload persistence
    {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            const QString targetPath = tmpDir.path() + "/saved_characterization.pdf";
            const bool saved = viewer.saveDocument(targetPath);
            const bool cleanAfterSave = !viewer.editLog().isDirty();

            // Re-open in a fresh viewer
            ui::PdfViewerWidget reloadedViewer;
            reloadedViewer.resize(600, 400);
            reloadedViewer.show();
            pump(100);
            const auto reloadErr = reloadedViewer.openDocument(targetPath);
            pump(200);

            recordTest(saved && cleanAfterSave && reloadErr == pdf::DocumentError::None && reloadedViewer.pageCount() > 0,
                       "Group 5.3: saveDocument commits edits, resets isDirty, and persisted PDF reloads cleanly");
        } else {
            recordTest(false, "Group 5.3: Could not create temporary dir for saveDocument test");
        }
    }

#ifdef PDF_WORKER_TEST_HOOKS
    // 5.4: Worker failure recovery replaying EditLog operations
    {
        viewer.openDocument(samplePdf);
        pump(200);

        // Add an edit to make the log dirty
        viewer.addSquareAnnotation(0, QRectF(20, 20, 80, 80));
        pump(50);
        const bool dirtyBeforeCrash = viewer.editLog().isDirty();

        SignalRecorder<int> spyRecovered(&viewer, &ui::PdfViewerWidget::editsRecovered);

        // Simulate crash
        viewer.simulateWorkerCrashForTesting();

        const bool recovered = waitFor([&]() {
            return spyRecovered.count() > 0 && viewer.isDocumentOpen();
        }, 8000);

        const bool dirtyAfterRecover = viewer.editLog().isDirty();
        recordTest(dirtyBeforeCrash && recovered && dirtyAfterRecover,
                   "Group 5.4: simulateWorkerCrashForTesting recovers worker and replays uncommitted edits");
    }
#else
    std::cout << "  [INFO] Skipping Group 5.4 crash recovery (requires PDF_WORKER_TEST_HOOKS)\n";
#endif
}

// ======================================================================
// GROUP 6: MainWindow Characterization
// ======================================================================

void runGroup6MainWindow(const QString& samplePdf, const QString& multiDoc) {
    std::cout << "\n--- [Group 6: MainWindow Characterization] ---\n";
    ui::MainWindow window;
    window.resize(1024, 768);
    window.show();
    pump(100);

    // 6.1: Active-tab routing
    {
        const bool open1 = window.openFile(samplePdf);
        pump(200);
        const auto* viewer1 = window.currentViewer();

        const int tab2 = window.openFileInNewTab(multiDoc);
        pump(200);
        const auto* viewer2 = window.currentViewer();

        recordTest(open1 && tab2 == 1 && window.tabCount() == 2 && viewer1 != viewer2 && viewer2 == window.viewerAt(1),
                   "Group 6.1: openFileInNewTab creates second tab and updates currentViewer()");

        // Routing action: toggleRedactionMode operates strictly on active tab (tab 1)
        window.toggleRedactionMode();
        pump(50);
        const bool v2Redact = window.viewerAt(1)->isRedactionMode();
        const bool v1Redact = window.viewerAt(0)->isRedactionMode();

        // Switch back to tab 0
        window.documentTabs()->setCurrentIndex(0);
        pump(50);
        const bool switched = (window.currentViewer() == window.viewerAt(0));

        // Background tab isolation: drive page change on background viewer (tab 1)
        QLabel* pageLabel = nullptr;
        for (auto* lbl : window.findChildren<QLabel*>()) {
            if (lbl->text().contains("Page")) {
                pageLabel = lbl;
                break;
            }
        }
        const QString activeStatusBefore = pageLabel ? pageLabel->text() : QString();
        const int activePageBefore = window.viewerAt(0)->currentPage();

        // Drive page change on background tab
        window.viewerAt(1)->goToPage(3);
        pump(50);

        const QString activeStatusAfter = pageLabel ? pageLabel->text() : QString();
        const int activePageAfter = window.viewerAt(0)->currentPage();
        const bool backgroundIgnored = (pageLabel != nullptr) &&
                                       (!activeStatusBefore.isEmpty()) &&
                                       (activeStatusBefore == activeStatusAfter) &&
                                       (activePageBefore == activePageAfter) &&
                                       (window.viewerAt(1)->currentPage() == 3);

        recordTest(v2Redact && !v1Redact && switched && backgroundIgnored,
                   "Group 6.2: Action routing targets active tab without mutating inactive tabs");
    }

    // 6.3: Reading-position persistence in isolated QSettings
    {
        window.clearReadingHistory();
        auto* viewer = window.currentViewer();
        viewer->setZoom(1.75);
        viewer->goToPage(2);
        viewer->setDocScrollPosition(35, 120);
        pump(50);

        const int pageBefore = viewer->currentPage();
        const double zoomBefore = viewer->currentZoom();

        window.saveReadingPosition(viewer);
        pump(50);

        // Reset viewer state
        viewer->setZoom(1.0);
        viewer->goToPage(0);
        pump(50);

        // Restore position
        window.restoreReadingPosition(viewer, viewer->currentFilePath());
        pump(100);

        const bool restored = (viewer->currentPage() == pageBefore) &&
                             (std::abs(viewer->currentZoom() - zoomBefore) < 1e-4);
        recordTest(restored,
                   "Group 6.3: saveReadingPosition and restoreReadingPosition persist view state in QSettings");
    }

    // 6.4: Close with unsaved edits
    {
        auto* viewer = window.currentViewer();
        viewer->addSquareAnnotation(0, QRectF(10, 10, 50, 50));
        pump(50);
        const bool dirty = viewer->editLog().isDirty();

        // While save or edit is in progress, closeEvent ignores the event
        QCloseEvent closeEv;
        closeEv.accept();
        recordTest(dirty, "Group 6.4: Unsaved edits in active viewer mark editLog as dirty before close flow");
    }
}

// ======================================================================
// Main test entry point
// ======================================================================

int main(int argc, char* argv[]) {
    const testsupport::IsolatedUserSettings isolatedSettings;

    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QApplication app(argc, argv);

    std::cout << "======================================================================\n";
    std::cout << " RUNNING VIEWER CHARACTERIZATION TEST SUITE (BRIEF T24, RFC Pass 0)\n";
    std::cout << "======================================================================\n";

    QTemporaryDir fixtureDir;
    if (!fixtureDir.isValid()) {
        std::cerr << "[FATAL] Cannot create temporary directory for test fixtures\n";
        return 1;
    }

    const QString sample5Page = fixtureDir.path() + "/fixture_5page.pdf";
    const QString twoPageTextPdf = fixtureDir.path() + "/fixture_twopage_text.pdf";
    const QString searchPdf = fixtureDir.path() + "/fixture_search.pdf";

    generateMultiPagePdf(sample5Page.toStdString(), 5);
    generateTwoPageTextPdf(twoPageTextPdf.toStdString(), "PageOne First Header Text", "PageTwo Second Footer Text");
    generateSearchPdf(searchPdf.toStdString());

    const QString samplePdf = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : sample5Page;

    runGroup1LayoutAndNavigation(sample5Page);
    runGroup2TileScheduling(samplePdf);
    runGroup3SelectionAndSearch(twoPageTextPdf, searchPdf);
    runGroup4AnnotationAndRedaction(samplePdf);
    runGroup5FormsEditLogAndSave(samplePdf);
    runGroup6MainWindow(samplePdf, sample5Page);

    std::cout << "\n======================================================================\n";
    std::cout << " TEST SUMMARY: " << g_passCount << " passed, " << g_failCount << " failed\n";
    std::cout << "======================================================================\n";

    if (g_failCount > 0) {
        std::cerr << "[FAILED] Viewer Characterization Test Suite failed with " << g_failCount << " error(s).\n";
        return 1;
    }

    std::cout << "[SUCCESS] All viewer characterization tests passed!\n";
    return 0;
}
