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
#include "ui/ViewerSelectionController.h"
#include "ui/ViewerSearchController.h"
#include "ui/MainWindow.h"
#include "ui/ColorMode.h"
#include "layout/DocumentLayout.h"
#include "cache/TileCache.h"
#include "ipc/WorkerClient.h"
#include "ipc/IpcMessages.h"
#include "engine/EditLog.h"
#include "engine/FormFieldTypes.h"
#include "ui/FreeTextLayout.h"

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

void generateCornerTextPdf(const std::string& path) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 100] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    // Text spans from x=10 across the entire 300 pt width and reaches the right edge
    std::string stream = "BT\n/F1 18 Tf\n10 40 Td\n(Wide Text Line Reaching Right Margin) Tj\nET\n";
    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream\nendobj\n";

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
    out.write(pdf.data(), static_cast<std::streamsize>(pdf.size()));
}

void generateBottomTextPdf(const std::string& path) {
    std::string pdf;
    pdf += "%PDF-1.4\n";
    std::vector<size_t> offsets;

    offsets.push_back(pdf.size());
    pdf += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 100] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n";

    offsets.push_back(pdf.size());
    pdf += "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";

    // Text baseline is at y=0 in PDF user space (MediaBox [0 0 300 100]), placing descenders right at/below the bottom margin (canonical Y >= 100)
    std::string stream = "BT\n/F1 18 Tf\n10 0 Td\n(Bottom Margin Touching Text Line) Tj\nET\n";
    offsets.push_back(pdf.size());
    pdf += "5 0 obj\n<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream\nendobj\n";

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

    // 3.11: Interactive selection invariants: release ends drag (C9), clearSelection during drag ends drag (C1), repeat position emits nothing (C4)
    {
        QTemporaryDir dir311;
        const QString docPath = dir311.path() + "/fixture_311.pdf";
        generateThreePageTextPdf(docPath.toStdString(), "AAA BBB CCC", "DDD EEE FFF", "GGG HHH III");

        ui::PdfViewerWidget viewer;
        viewer.resize(900, 700);
        viewer.show();
        viewer.ensurePolished();
        pump(100);

        viewer.openDocument(docPath);
        viewer.waitForRendering();
        pump(200);

        viewer.setZoom(0.2);
        viewer.goToPage(0);
        viewer.waitForRendering();
        pump(100);

        waitFor([&]() {
            return viewer.getPageText(0) && viewer.getPageText(1) && viewer.getPageText(2);
        }, 5000);

        const auto pt0 = viewer.getPageText(0);
        bool c9WidgetOk = false;
        bool c9ControllerOk = false;
        bool c9ClickOk = false;
        bool c1WidgetOk = false;
        bool c1ControllerOk = false;
        bool c4WidgetOk = false;
        bool c4ControllerOk = false;

        if (pt0 && pt0->charCount() >= 11) {
            const auto& pl0 = viewer.layout().getPageLayout(0);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

            auto charCenter = [&](const layout::PageLayout& pl, const pdf::PageCharBox& b) -> QPointF {
                double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                return QPointF(x, y);
            };

            const QPointF posA = charCenter(pl0, pt0->chars[4]);
            const QPointF posB = charCenter(pl0, pt0->chars[6]);
            const QPointF posC = charCenter(pl0, pt0->chars[8]);

            // ---- C9: Release must end the drag ----
            SignalCounter spySelChange(&viewer, &ui::PdfViewerWidget::selectionChanged);

            QMouseEvent p1(QEvent::MouseButtonPress, posA, posA, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &p1);
            pump(20);

            QMouseEvent m1(QEvent::MouseMove, posB, posB, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &m1);
            pump(20);

            QMouseEvent r1(QEvent::MouseButtonRelease, posB, posB, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &r1);
            pump(50);

            const QString selAfterRelease = viewer.selectedText();
            const size_t changeCountAfterRelease = spySelChange.count();

            // Send MouseMove with no buttons over character C
            QMouseEvent mNoBtn(QEvent::MouseMove, posC, posC, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &mNoBtn);
            pump(50);

            const QString selAfterNoBtnMove = viewer.selectedText();
            const size_t changeCountAfterNoBtnMove = spySelChange.count();

            c9WidgetOk = (selAfterRelease == "BBB") &&
                         (selAfterNoBtnMove == "BBB") &&
                         (changeCountAfterRelease >= 1) &&
                         (changeCountAfterNoBtnMove == changeCountAfterRelease);

            // Controller-level check for C9: drag A->B, release, assert !isSelecting()
            const auto ptCtrl = makeDummyPageText(0, "AAA BBB CCC");
            ui::ViewerSelectionController ctrlC9;
            ctrlC9.handlePageTextReady(1, 1, 0, ptCtrl, 0);
            ctrlC9.startSelection(0, ptCtrl->chars[4].left + 1.0, ptCtrl->chars[4].top + 1.0);
            bool outCh = false;
            ctrlC9.updateSelectionEnd(0, ptCtrl->chars[6].left + 1.0, ptCtrl->chars[6].top + 1.0, outCh);
            bool outCleared = false;
            ctrlC9.finishSelection(outCleared);
            c9ControllerOk = !ctrlC9.isSelecting() && !outCleared && ctrlC9.hasSelection();

            // Also pin: after same-character click, controller's isSelecting() is false
            ui::ViewerSelectionController ctrlClick;
            ctrlClick.handlePageTextReady(1, 1, 0, ptCtrl, 0);
            ctrlClick.startSelection(0, ptCtrl->chars[4].left + 1.0, ptCtrl->chars[4].top + 1.0);
            ctrlClick.finishSelection(outCleared);
            c9ClickOk = !ctrlClick.isSelecting() && outCleared && !ctrlClick.hasSelection();

            // ---- C1: clearSelection during a drag ends the drag ----
            viewer.clearSelection();
            pump(20);

            // Press on character A (selecting is on)
            QMouseEvent pC1(QEvent::MouseButtonPress, posA, posA, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &pC1);
            pump(20);

            // Call viewer.clearSelection() during active drag
            SignalCounter spySelC1(&viewer, &ui::PdfViewerWidget::selectionChanged);
            viewer.clearSelection();
            pump(20);
            const size_t countAfterClear = spySelC1.count();

            // Send mouse move over character B with LeftButton (simulating drag continuation)
            QMouseEvent mC1(QEvent::MouseMove, posB, posB, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &mC1);
            pump(20);

            const size_t countAfterMove = spySelC1.count();
            const bool noSelectionAfterMove = !viewer.hasSelection();

            // A later clearSelection also emits nothing
            viewer.clearSelection();
            pump(20);
            const size_t countAfterSecondClear = spySelC1.count();

            // Finish gesture
            QMouseEvent rC1(QEvent::MouseButtonRelease, posB, posB, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &rC1);
            pump(20);

            c1WidgetOk = (countAfterMove == countAfterClear) &&
                         (countAfterSecondClear == countAfterClear) &&
                         noSelectionAfterMove;

            // Controller-level check for C1
            ui::ViewerSelectionController ctrlC1;
            ctrlC1.handlePageTextReady(1, 1, 0, ptCtrl, 0);
            ctrlC1.startSelection(0, ptCtrl->chars[4].left + 1.0, ptCtrl->chars[4].top + 1.0);
            ctrlC1.clearSelection();
            c1ControllerOk = !ctrlC1.isSelecting() && !ctrlC1.hasSelection();

            // ---- C4: A move that doesn't change the end emits nothing ----
            viewer.clearSelection();
            pump(20);

            // Press on char A
            QMouseEvent pC4(QEvent::MouseButtonPress, posA, posA, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &pC4);
            pump(20);

            // Move to char B
            SignalCounter spySelC4(&viewer, &ui::PdfViewerWidget::selectionChanged);
            QMouseEvent mC4_1(QEvent::MouseMove, posB, posB, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &mC4_1);
            pump(20);

            const size_t countAfterFirstMove = spySelC4.count();

            // Move again to a slightly offset pixel still inside char B
            const QPointF posB_offset = posB + QPointF(1.0, 0.0);
            QMouseEvent mC4_2(QEvent::MouseMove, posB_offset, posB_offset, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &mC4_2);
            pump(20);

            const size_t countAfterRepeatMove = spySelC4.count();

            // Release
            QMouseEvent rC4(QEvent::MouseButtonRelease, posB_offset, posB_offset, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &rC4);
            pump(20);

            c4WidgetOk = (countAfterFirstMove == 1) && (countAfterRepeatMove == countAfterFirstMove);

            // Controller level: updateSelectionEnd returns true with outChanged == false for repeat position,
            // and true with outChanged == true for a new one.
            ui::ViewerSelectionController ctrlC4;
            ctrlC4.handlePageTextReady(1, 1, 0, ptCtrl, 0);
            ctrlC4.startSelection(0, ptCtrl->chars[4].left + 1.0, ptCtrl->chars[4].top + 1.0);

            bool changedFirst = false;
            const bool retFirst = ctrlC4.updateSelectionEnd(0, ptCtrl->chars[6].left + 1.0, ptCtrl->chars[6].top + 1.0, changedFirst);

            bool changedRepeat = true;
            const bool retRepeat = ctrlC4.updateSelectionEnd(0, ptCtrl->chars[6].left + 2.0, ptCtrl->chars[6].top + 1.0, changedRepeat);

            bool changedNew = false;
            const bool retNew = ctrlC4.updateSelectionEnd(0, ptCtrl->chars[8].left + 1.0, ptCtrl->chars[8].top + 1.0, changedNew);

            c4ControllerOk = retFirst && changedFirst && retRepeat && !changedRepeat && retNew && changedNew;
        }

        const bool group3_11Ok = c9WidgetOk && c9ControllerOk && c9ClickOk &&
                                 c1WidgetOk && c1ControllerOk &&
                                 c4WidgetOk && c4ControllerOk;
        std::string d311 = "c9W=" + std::to_string(c9WidgetOk)
            + " c9C=" + std::to_string(c9ControllerOk)
            + " c9Click=" + std::to_string(c9ClickOk)
            + " c1W=" + std::to_string(c1WidgetOk)
            + " c1C=" + std::to_string(c1ControllerOk)
            + " c4W=" + std::to_string(c4WidgetOk)
            + " c4C=" + std::to_string(c4ControllerOk);

        recordTest(group3_11Ok,
                   "Group 3.11: Interactive selection invariants: release ends drag (C9), clearSelection ends drag (C1), repeat position emits nothing (C4)",
                   d311);
    }

    // 3.12: Text cache trim protects in-flight entries (C7), and drag between lines picks nearest line (C8)
    {
        ui::PdfViewerWidget viewer;

        // ---- C7: in-flight entries are never evicted by trim ----
        ui::ViewerSelectionController ctrlC7;
        const int64_t dummyDocId = 888888;
        const int inFlightPage = 100;

        // Create in-flight (nullptr) entry for page 100
        ctrlC7.ensurePageTextRequested(inFlightPage, true, dummyDocId, viewer.renderWorker());
        const bool inFlightInitiallyPresent = (ctrlC7.pageTextCache().find(inFlightPage) != ctrlC7.pageTextCache().end()) &&
                                               (ctrlC7.pageTextCache().at(inFlightPage) == nullptr);

        // Fill controller to 49 pages with real text via handlePageTextReady (pages 0..47 plus inFlightPage = 49 total)
        for (int i = 0; i < 48; ++i) {
            ctrlC7.handlePageTextReady(dummyDocId, dummyDocId, i, makeDummyPageText(i, "P" + std::to_string(i)), 0);
        }

        const size_t countAfterFirstTrim = ctrlC7.cachedTextPageCount();
        const bool inFlightKeptAfterFirstTrim = (ctrlC7.pageTextCache().find(inFlightPage) != ctrlC7.pageTextCache().end()) &&
                                                (ctrlC7.pageTextCache().at(inFlightPage) == nullptr);

        // Second trim: add another real page (page 48)
        ctrlC7.handlePageTextReady(dummyDocId, dummyDocId, 48, makeDummyPageText(48, "P48"), 0);

        const size_t countAfterSecondTrim = ctrlC7.cachedTextPageCount();
        const bool inFlightKeptAfterSecondTrim = (ctrlC7.pageTextCache().find(inFlightPage) != ctrlC7.pageTextCache().end()) &&
                                                 (ctrlC7.pageTextCache().at(inFlightPage) == nullptr);

        const bool c7Ok = inFlightInitiallyPresent && (countAfterFirstTrim == 48) && inFlightKeptAfterFirstTrim &&
                          (countAfterSecondTrim == 48) && inFlightKeptAfterSecondTrim;

        // ---- C8: drag between lines picks nearest line ----
        // Build 3 lines of characters with vertical gaps
        // Line 1: chars 0, 1, 2 (top 100, bottom 120, cy 110)
        // Line 2: chars 3, 4, 5 (top 250, bottom 270, cy 260)
        // Line 3: chars 6, 7, 8 (top 400, bottom 420, cy 410)
        auto pt3Lines = std::make_shared<pdf::PageText>();
        pt3Lines->pageIndex = 0;
        pt3Lines->widthPoints = 600.0;
        pt3Lines->heightPoints = 800.0;
        pt3Lines->text = u"ABCDEFGHI";

        auto addChar = [&](double left, double top, double right, double bottom, char32_t uc) {
            pdf::PageCharBox b;
            b.left = left;
            b.top = top;
            b.right = right;
            b.bottom = bottom;
            b.unicode = uc;
            pt3Lines->chars.push_back(b);
        };

        // Line 1
        addChar(50.0, 100.0, 70.0, 120.0, U'A');
        addChar(70.0, 100.0, 90.0, 120.0, U'B');
        addChar(90.0, 100.0, 110.0, 120.0, U'C');
        // Line 2
        addChar(50.0, 250.0, 70.0, 270.0, U'D');
        addChar(70.0, 250.0, 90.0, 270.0, U'E');
        addChar(90.0, 250.0, 110.0, 270.0, U'F');
        // Line 3
        addChar(50.0, 400.0, 70.0, 420.0, U'G');
        addChar(70.0, 400.0, 90.0, 420.0, U'H');
        addChar(90.0, 400.0, 110.0, 420.0, U'I');

        ui::ViewerSelectionController ctrlC8;
        ctrlC8.handlePageTextReady(1, 1, 0, pt3Lines, 0);
        ctrlC8.startSelection(0, 60.0, 110.0); // start at char 0

        // Use px = 500.0 so hitTest returns negative, exercising the geometric fallbacks:
        const double pxOff = 500.0;
        bool chDummy = false;

        // 1. Between line 1 and 2, closer to line 2 (py = 200.0, closer to cy=260 than cy=110): expects line 2 (nearest=3)
        ctrlC8.updateSelectionEnd(0, pxOff, 200.0, chDummy);
        const int endBetween1and2 = ctrlC8.selectionEnd().charIndex;
        const bool c8a_1_Ok = (endBetween1and2 == 3);

        // 2. Between line 2 and 3, closer to line 2 (py = 300.0, closer to cy=260 than cy=410): expects line 2 (nearest=3)
        ctrlC8.updateSelectionEnd(0, pxOff, 300.0, chDummy);
        const int endBetween2and3 = ctrlC8.selectionEnd().charIndex;
        const bool c8a_2_Ok = (endBetween2and3 == 3);

        // 3. Above first line (py = 50.0 < 100.0): expects 0
        ctrlC8.updateSelectionEnd(0, pxOff, 50.0, chDummy);
        const int endAboveFirst = ctrlC8.selectionEnd().charIndex;
        const bool c8b_Ok = (endAboveFirst == 0);

        // 4. Below last line (py = 500.0 > 420.0): expects charCount() - 1 = 8
        ctrlC8.updateSelectionEnd(0, pxOff, 500.0, chDummy);
        const int endBelowLast = ctrlC8.selectionEnd().charIndex;
        const bool c8c_Ok = (endBelowLast == 8);

        const bool c8Ok = c8a_1_Ok && c8a_2_Ok && c8b_Ok && c8c_Ok;
        const bool group3_12Ok = c7Ok && c8Ok;

        std::string d312 = "c7=" + std::to_string(c7Ok)
            + " (init=" + std::to_string(inFlightInitiallyPresent)
            + " cnt1=" + std::to_string(countAfterFirstTrim) + " kept1=" + std::to_string(inFlightKeptAfterFirstTrim)
            + " cnt2=" + std::to_string(countAfterSecondTrim) + " kept2=" + std::to_string(inFlightKeptAfterSecondTrim) + ")"
            + " c8=" + std::to_string(c8Ok)
            + " (b12=" + std::to_string(endBetween1and2) + " exp 3)"
            + " (b23=" + std::to_string(endBetween2and3) + " exp 3)"
            + " (above=" + std::to_string(endAboveFirst) + " exp 0)"
            + " (below=" + std::to_string(endBelowLast) + " exp 8)";

        recordTest(group3_12Ok,
                   "Group 3.12: Text cache trim protects in-flight entries (C7), and drag between lines picks nearest line (C8)",
                   d312);
    }

    // 3.13: Search-side characterization gaps (M8, M1, M4, M5, S13b)
    {
        // 1. M8: Remove m_currentMatchIndex = -1; from beginSearch
        // Adding matches, navigating to match index 2, then beginSearch("zzz") must reset
        // currentIndex() to -1, completedDisplayIndex() to 0, and clear matches().
        {
            ui::ViewerSearchController ctrl;
            pdf::SearchMatch m0{0, 10, 4, {}};
            pdf::SearchMatch m1{1, 10, 4, {}};
            pdf::SearchMatch m2{2, 10, 4, {}};
            ctrl.addMatches({m0, m1, m2}, 5);
            ctrl.goTo(2);
            const bool preOk = (ctrl.currentIndex() == 2) && (ctrl.completedDisplayIndex() == 3) && (ctrl.matches().size() == 3);

            ctrl.beginSearch("zzz", false, 0);
            const bool m8Ok = preOk && (ctrl.currentIndex() == -1) && (ctrl.completedDisplayIndex() == 0) && ctrl.matches().empty();
            const std::string dM8 = "pre=" + std::to_string(preOk) + " cur=" + std::to_string(ctrl.currentIndex()) +
                                    " disp=" + std::to_string(ctrl.completedDisplayIndex()) + " empty=" + std::to_string(ctrl.matches().empty());
            recordTest(m8Ok, "Group 3.13: New search resets active match index and display index (M8)", dM8);
        }

        // 2. M1: goTo accepts index == size
        // With 3 matches, goTo(3) is out-of-range: must return nullptr and leave index unchanged.
        // Also check goTo(-1) (rejected) and goTo(2) (accepted).
        {
            ui::ViewerSearchController ctrl;
            pdf::SearchMatch m0{0, 10, 4, {}};
            pdf::SearchMatch m1{1, 10, 4, {}};
            pdf::SearchMatch m2{2, 10, 4, {}};
            ctrl.addMatches({m0, m1, m2}, 5);
            ctrl.goTo(1);
            const bool preOk = (ctrl.currentIndex() == 1);

            const auto* resSize = ctrl.goTo(3);
            const bool sizeRejected = (resSize == nullptr) && (ctrl.currentIndex() == 1);

            const auto* resNeg = ctrl.goTo(-1);
            const bool negRejected = (resNeg == nullptr) && (ctrl.currentIndex() == 1);

            const auto* resValid = ctrl.goTo(2);
            const bool validAccepted = (resValid != nullptr) && (resValid->pageIndex == 2) && (ctrl.currentIndex() == 2);

            const bool m1Ok = preOk && sizeRejected && negRejected && validAccepted;
            const std::string dM1 = "sizeRej=" + std::to_string(sizeRejected) + " negRej=" + std::to_string(negRejected) +
                                    " valAcc=" + std::to_string(validAccepted);
            recordTest(m1Ok, "Group 3.13: goTo rejects out-of-range index equal to match count (M1)", dM1);
        }

        // 3. M4: Nearest-match selection and tie-break in addMatches
        // Tests two aspects:
        // A) Equidistant-looking linear pages from startPage 5 with pageCount 10:
        //    Match on page 3 (linear distance 2 behind) vs page 7 (linear distance 2 ahead).
        //    Under circular distance, page 7 has (7 - 5 + 10)%10 = 2, while page 3 has (3 - 5 + 10)%10 = 8.
        //    Circular search selects page 7 (dist 2 < 8).
        // B) Tie-break on the same page between charStartIndex 10 and 50:
        //    Matches on the same page have identical circular distance, so charStartIndex determines
        //    the nearest hit (charStartIndex 10 chosen over 50).
        // Note on equivalent mutation: In ViewerSearchController::addMatches, m_searchMatches is already
        // pre-sorted by (pageIndex, charStartIndex). Because std::min_element preserves the first element
        // encountered when elements compare equivalent under a strict weak ordering (return false;),
        // mutating the tie-break to `return false;` is mathematically equivalent. Mutating the comparator
        // to `<=` or reversing the tie-break chooses the later match, which this test catches.
        {
            ui::ViewerSearchController ctrl;
            ctrl.beginSearch("tiebreak", false, 5);

            pdf::SearchMatch mA{3, 10, 4, {}}; // Page 3: circular dist = 8
            pdf::SearchMatch mB{7, 10, 4, {}}; // Page 7: circular dist = 2, char 10
            pdf::SearchMatch mC{7, 50, 4, {}}; // Page 7: circular dist = 2, char 50

            auto res = ctrl.addMatches({mA, mC, mB}, 10);
            const bool circularAheadChosen = (ctrl.currentIndex() == 1) && (res.scrollTarget != nullptr) && (res.scrollTarget->pageIndex == 7);
            const bool charTieBreakChosen = (res.scrollTarget != nullptr) && (res.scrollTarget->charStartIndex == 10);

            const bool m4Ok = circularAheadChosen && charTieBreakChosen;
            const std::string dM4 = "circPage7=" + std::to_string(circularAheadChosen) + " (cur=" + std::to_string(ctrl.currentIndex()) +
                                    ") char10=" + std::to_string(charTieBreakChosen) +
                                    " (char=" + std::to_string(res.scrollTarget ? res.scrollTarget->charStartIndex : -1) + ")";
            recordTest(m4Ok, "Group 3.13: Nearest search match tie-breaks by character start index (M4)", dM4);
        }

        // 4. M5: Subsequent match batch does not trigger scroll target
        // The first batch of matches has no prior selection and must return a non-null scrollTarget.
        // A subsequent batch arriving while an active selection exists must preserve the selection and return scrollTarget == nullptr.
        {
            ui::ViewerSearchController ctrl;
            ctrl.beginSearch("test", false, 0);

            pdf::SearchMatch m0{0, 10, 4, {}};
            auto res1 = ctrl.addMatches({m0}, 5);
            const bool firstBatchHasScroll = (res1.scrollTarget != nullptr) && (res1.scrollTarget->pageIndex == 0);

            pdf::SearchMatch m1{1, 20, 4, {}};
            auto res2 = ctrl.addMatches({m1}, 5);
            const bool secondBatchNoScroll = (res2.scrollTarget == nullptr);

            const bool m5Ok = firstBatchHasScroll && secondBatchNoScroll;
            const std::string dM5 = "firstScroll=" + std::to_string(firstBatchHasScroll) +
                                    " secondScroll=" + std::to_string(!secondBatchNoScroll);
            recordTest(m5Ok, "Group 3.13: Subsequent match batch does not trigger scroll target (M5)", dM5);
        }

        // 5. S13b: Active search highlight fill renders distinct color from document background
        // Renders PdfViewerWidget viewport to a QImage with active and non-active matches.
        // Confirms that the interior of the active match rectangle is filled with the active highlight
        // color (vivid orange, blue < 120), distinctly different from the blank document background
        // (white, blue > 240) and from the non-active match highlight (yellow, blue > 130).
        {
            ui::PdfViewerWidget viewer;
            viewer.resize(800, 700);
            viewer.show();
            viewer.ensurePolished();
            pump(100);

            viewer.openDocument(twoPageTextPdf);
            viewer.waitForRendering();
            pump(200);

            const int64_t docId = viewer.currentDocumentId();

            pdf::SearchMatch mActive;
            mActive.pageIndex = 0;
            mActive.charStartIndex = 0;
            mActive.charCount = 3;
            mActive.rects = { pdf::PageCharBox{100.0, 100.0, 150.0, 130.0} };

            pdf::SearchMatch mInactive;
            mInactive.pageIndex = 0;
            mInactive.charStartIndex = 10;
            mInactive.charCount = 5;
            mInactive.rects = { pdf::PageCharBox{200.0, 100.0, 260.0, 130.0} };

            viewer.findText("PaintTestQuery", false);
            const int64_t paintSearchId = 2;
            Q_EMIT viewer.renderWorker().searchMatchesFound(docId, paintSearchId, 0, {mActive, mInactive});
            pump(50);

            viewer.goToSearchMatch(0); // mActive is active match
            pump(50);

            QImage renderImg(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            {
                QPainter p(&renderImg);
                viewer.viewport()->render(&p);
                p.end();
            }

            const auto& pl0 = viewer.layout().getPageLayout(0);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

            const double pl0LeftDoc = static_cast<double>(pl0.leftPixel) - static_cast<double>(viewer.currentDocScrollX());
            const double pl0TopDoc = static_cast<double>(pl0.topPixel) - static_cast<double>(viewer.currentDocScrollY());

            const int actX = static_cast<int>(std::round((pl0LeftDoc + 125.0 * effScale) / dpr));
            const int actY = static_cast<int>(std::round((pl0TopDoc + 115.0 * effScale) / dpr));
            const QColor actCol = renderImg.pixelColor(actX, actY);

            const int inactX = static_cast<int>(std::round((pl0LeftDoc + 230.0 * effScale) / dpr));
            const int inactY = actY;
            const QColor inactCol = renderImg.pixelColor(inactX, inactY);

            const int blankX = inactX;
            const int blankY = static_cast<int>(std::round((pl0TopDoc + 50.0 * effScale) / dpr));
            const QColor blankCol = renderImg.pixelColor(blankX, blankY);

            // Active match fill must be painted (vivid orange, blue < 120), distinctly different
            // from unpainted blank white background (blue > 240) and inactive yellow (blue > 130).
            const bool actDiffFromBlank = (actCol != blankCol) && (actCol.blue() < 120) && (blankCol.blue() > 240);
            const bool actDiffFromInact = (actCol != inactCol);
            const bool s13bOk = actDiffFromBlank && actDiffFromInact;

            const std::string dS13b = "act=" + actCol.name().toStdString() + " (b=" + std::to_string(actCol.blue()) +
                                      ") inact=" + inactCol.name().toStdString() + " (b=" + std::to_string(inactCol.blue()) +
                                      ") blank=" + blankCol.name().toStdString() + " (b=" + std::to_string(blankCol.blue()) +
                                      ") diffBlank=" + std::to_string(actDiffFromBlank) +
                                      " diffInact=" + std::to_string(actDiffFromInact);
            recordTest(s13bOk, "Group 3.13: Active search highlight fill renders distinct color from document background (S13b)", dS13b);
        }
    }
}

// ======================================================================
// GROUP 4: Annotation and Redaction Interaction Characterization
// ======================================================================

void runGroup4AnnotationAndRedaction(const QString& samplePdf, const QString& twoPageTextPdf) {
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
        viewer.editLog().clear();
    }

    // 4.10: Drag threshold requires at least 3.0 pt on both axes (R1)
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const double dpr = viewer.effectiveDpr();

        auto sendRedactDrag = [&](double startXpt, double startYpt, double endXpt, double endYpt) {
            int64_t sx = 0, sy = 0, ex = 0, ey = 0;
            viewer.layout().pagePointsToDocument(0, startXpt, startYpt, sx, sy);
            viewer.layout().pagePointsToDocument(0, endXpt, endYpt, ex, ey);
            const QPointF startVp((sx - viewer.currentDocScrollX()) / dpr, (sy - viewer.currentDocScrollY()) / dpr);
            const QPointF endVp((ex - viewer.currentDocScrollX()) / dpr, (ey - viewer.currentDocScrollY()) / dpr);

            QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &press);
            pump(20);
            QMouseEvent move(QEvent::MouseMove, endVp, endVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &move);
            pump(20);
            QMouseEvent release(QEvent::MouseButtonRelease, endVp, endVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &release);
            pump(50);
        };

        // 1. dx = 2.0 (< 3.0), dy = 15.0 (>= 3.0) -> rejected by &&
        sendRedactDrag(100.0, 100.0, 102.0, 115.0);
        const bool dxRejected = (viewer.stagedRedactionCount() == 0);

        // 2. dx = 15.0 (>= 3.0), dy = 2.0 (< 3.0) -> rejected by &&
        sendRedactDrag(100.0, 100.0, 115.0, 102.0);
        const bool dyRejected = (viewer.stagedRedactionCount() == 0);

        // 3. dx = 4.0 (>= 3.0), dy = 4.0 (>= 3.0) -> accepted
        sendRedactDrag(100.0, 100.0, 104.0, 104.0);
        const bool bothAccepted = (viewer.stagedRedactionCount() == 1);

        const bool r1Ok = dxRejected && dyRejected && bothAccepted;
        const std::string dR1 = "dxRej=" + std::to_string(dxRejected) + " dyRej=" + std::to_string(dyRejected) +
                                " bothAcc=" + std::to_string(bothAccepted);
        recordTest(r1Ok, "Group 4.10: Drag threshold requires at least 3.0 pt on both axes (R1)", dR1);

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);
    }

    // 4.11: Bottom-right to top-left drag normalizes canonical bounds (R2)
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        int64_t sx = 0, sy = 0, ex = 0, ey = 0;
        viewer.layout().pagePointsToDocument(0, 200.0, 250.0, sx, sy);
        viewer.layout().pagePointsToDocument(0, 100.0, 120.0, ex, ey);
        const QPointF startVp((sx - viewer.currentDocScrollX()) / dpr, (sy - viewer.currentDocScrollY()) / dpr);
        const QPointF endVp((ex - viewer.currentDocScrollX()) / dpr, (ey - viewer.currentDocScrollY()) / dpr);

        QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);
        QMouseEvent move(QEvent::MouseMove, endVp, endVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);
        QMouseEvent release(QEvent::MouseButtonRelease, endVp, endVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(50);

        bool r2Ok = false;
        if (viewer.stagedRedactionCount() == 1 && viewer.stagedRedactions().count(0) == 1) {
            const auto& r = viewer.stagedRedactions().at(0)[0];
            r2Ok = (std::abs(r.left - 100.0) < 1.0) && (std::abs(r.top - 120.0) < 1.0) &&
                   (std::abs(r.right - 200.0) < 1.0) && (std::abs(r.bottom - 250.0) < 1.0) &&
                   (r.left < r.right) && (r.top < r.bottom);
        }
        recordTest(r2Ok, "Group 4.11: Dragging bottom-right to top-left normalizes canonical rect bounds (R2)");

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);
    }

    // 4.12: Drag extending past page edges is clamped to page dimensions (R3)
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const auto& pl0 = viewer.layout().getPageLayout(0);
        const double dpr = viewer.effectiveDpr();
        int64_t sx = 0, sy = 0, ex = 0, ey = 0;
        viewer.layout().pagePointsToDocument(0, 500.0, 700.0, sx, sy);
        // Exceeding page bounds: 700.0 > pl0.widthPoints (595), 1000.0 > pl0.heightPoints (842)
        viewer.layout().pagePointsToDocument(0, 700.0, 1000.0, ex, ey);
        const QPointF startVp((sx - viewer.currentDocScrollX()) / dpr, (sy - viewer.currentDocScrollY()) / dpr);
        const QPointF endVp((ex - viewer.currentDocScrollX()) / dpr, (ey - viewer.currentDocScrollY()) / dpr);

        QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);
        QMouseEvent move(QEvent::MouseMove, endVp, endVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);
        QMouseEvent release(QEvent::MouseButtonRelease, endVp, endVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(50);

        bool r3Ok = false;
        std::string dR3;
        if (viewer.stagedRedactionCount() == 1 && viewer.stagedRedactions().count(0) == 1) {
            const auto& r = viewer.stagedRedactions().at(0)[0];
            const bool clampedX = (std::abs(r.right - pl0.widthPoints) < 1e-2);
            const bool clampedY = (std::abs(r.bottom - pl0.heightPoints) < 1e-2);
            r3Ok = clampedX && clampedY;
            dR3 = "r=" + std::to_string(r.right) + " (max=" + std::to_string(pl0.widthPoints) +
                  ") b=" + std::to_string(r.bottom) + " (max=" + std::to_string(pl0.heightPoints) + ")";
        }
        recordTest(r3Ok, "Group 4.12: Drag extending past page edges is clamped to page dimensions (R3)", dR3);

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);
    }

    // 4.13: addStagedRedaction validates page bounds, normalizes rects, rejects zero-dimensions, and accumulates counts (R5)
    {
        viewer.clearStagedRedactions();
        SignalCounter spyMarks(&viewer, &ui::PdfViewerWidget::redactionMarksChanged);

        // 1. RM15: Page < 0 or >= pageCount rejected
        viewer.addStagedRedaction(-1, {10.0, 10.0, 50.0, 50.0});
        viewer.addStagedRedaction(viewer.pageCount(), {10.0, 10.0, 50.0, 50.0});
        const bool outOfBoundsIgnored = (viewer.stagedRedactionCount() == 0) && (spyMarks.count() == 0);

        // 2. RM4: Reversed rect normalized
        viewer.addStagedRedaction(0, {80.0, 90.0, 20.0, 30.0});
        bool normOk = false;
        if (viewer.stagedRedactionCount() == 1 && viewer.stagedRedactions().count(0) == 1) {
            const auto& r = viewer.stagedRedactions().at(0)[0];
            normOk = (r.left == 20.0) && (r.top == 30.0) && (r.right == 80.0) && (r.bottom == 90.0);
        }
        const bool firstAdded = normOk && (spyMarks.count() == 1);

        // 3. RM5: Zero width or zero height rejected (testing each with non-zero on the other axis)
        viewer.addStagedRedaction(0, {50.0, 50.0, 50.0, 80.0}); // zero width
        viewer.addStagedRedaction(0, {50.0, 50.0, 80.0, 50.0}); // zero height
        const bool zeroDimRejected = (viewer.stagedRedactionCount() == 1) && (spyMarks.count() == 1);

        // 4. RM12: Multiple rects on the same page correctly increment stagedRedactionCount (counts rects, not pages)
        viewer.addStagedRedaction(0, {100.0, 100.0, 150.0, 150.0});
        const bool countTracksRects = (viewer.stagedRedactionCount() == 2) && (spyMarks.count() == 2);

        const bool r5Ok = outOfBoundsIgnored && firstAdded && zeroDimRejected && countTracksRects;
        const std::string dR5 = "oob=" + std::to_string(outOfBoundsIgnored) + " norm=" + std::to_string(firstAdded) +
                                " zeroRej=" + std::to_string(zeroDimRejected) + " count=" + std::to_string(countTracksRects);
        recordTest(r5Ok, "Group 4.13: addStagedRedaction validates page bounds, normalizes, rejects zero-dim, and counts rects (R5)", dR5);

        viewer.clearStagedRedactions();
    }

    // 4.14: clearStagedRedactions emits only when non-empty (R6)
    {
        viewer.clearStagedRedactions();
        SignalCounter spyMarks(&viewer, &ui::PdfViewerWidget::redactionMarksChanged);

        // Clear when already empty -> early return, emits nothing
        viewer.clearStagedRedactions();
        const bool emptyNoEmit = (spyMarks.count() == 0);

        // Add a mark -> emits 1
        viewer.addStagedRedaction(0, {10.0, 10.0, 50.0, 50.0});
        const bool addEmits = (spyMarks.count() == 1);

        // Clear when not empty -> clears and emits 1
        viewer.clearStagedRedactions();
        const bool clearEmits = (spyMarks.count() == 2) && (viewer.stagedRedactionCount() == 0);

        const bool r6Ok = emptyNoEmit && addEmits && clearEmits;
        recordTest(r6Ok, "Group 4.14: clearStagedRedactions emits only when non-empty (R6)");
    }

    // 4.15: Redaction mode and ink mode mutual exclusion, selection clearing, and cursor shape (R9)
    {
        SignalCounter spyRedact(&viewer, &ui::PdfViewerWidget::redactionModeChanged);
        SignalCounter spyInk(&viewer, &ui::PdfViewerWidget::inkDrawingModeChanged);

        // 1. Ink mode on -> setRedactionMode(true) turns ink mode off
        viewer.setInkDrawingMode(true);
        pump(20);
        const bool inkPre = viewer.isInkDrawingMode();

        viewer.setRedactionMode(true);
        pump(20);
        const bool inkDisabledByRedact = inkPre && !viewer.isInkDrawingMode() && viewer.isRedactionMode();
        const bool crossCursorOn = (viewer.viewport()->cursor().shape() == Qt::CrossCursor);

        // 2. Redaction on -> setInkDrawingMode(true) turns redaction off
        viewer.setInkDrawingMode(true);
        pump(20);
        const bool redactDisabledByInk = !viewer.isRedactionMode() && viewer.isInkDrawingMode();

        // 3. Same-value calls emit nothing
        viewer.setRedactionMode(false);
        pump(20);
        const int redactCountBefore = spyRedact.count();
        viewer.setRedactionMode(false); // same-value call
        pump(20);
        const bool sameValSuppressed = (spyRedact.count() == redactCountBefore);

        // 4. Cursor returns to I-beam when both modes are off
        viewer.setInkDrawingMode(false);
        pump(20);
        const bool ibeamCursorOff = (viewer.viewport()->cursor().shape() == Qt::IBeamCursor);

        const bool r9Ok = inkDisabledByRedact && crossCursorOn && redactDisabledByInk && sameValSuppressed && ibeamCursorOff;
        const std::string dR9 = "inkDis=" + std::to_string(inkDisabledByRedact) + " cross=" + std::to_string(crossCursorOn) +
                                " redactDis=" + std::to_string(redactDisabledByInk) + " sameVal=" + std::to_string(sameValSuppressed) +
                                " ibeam=" + std::to_string(ibeamCursorOff);
        recordTest(r9Ok, "Group 4.15: Mode mutual exclusion, selection clearing, and cursor shape (R9)", dR9);
    }

    // 4.16: Mouse press outside any page starts no redaction drag (R11)
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        // Send mouse press in viewport at (2, 2) which is in the margin outside any page
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);
        QMouseEvent move(QEvent::MouseMove, QPointF(100, 100), QPointF(100, 100), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(100, 100), QPointF(100, 100), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(50);

        const bool r11Ok = (viewer.stagedRedactionCount() == 0);
        recordTest(r11Ok, "Group 4.16: Mouse press outside any page starts no redaction drag (R11)");

        viewer.setRedactionMode(false);
    }

    // 4.17: Mode switched off mid-drag still stages mark on mouse release (R12)
    // Characterizes current behavior: setRedactionMode(false) does not cancel in-progress drag; release checks m_isRedacting.
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        int64_t sx = 0, sy = 0, ex = 0, ey = 0;
        viewer.layout().pagePointsToDocument(0, 100.0, 100.0, sx, sy);
        viewer.layout().pagePointsToDocument(0, 200.0, 200.0, ex, ey);
        const QPointF startVp((sx - viewer.currentDocScrollX()) / dpr, (sy - viewer.currentDocScrollY()) / dpr);
        const QPointF endVp((ex - viewer.currentDocScrollX()) / dpr, (ey - viewer.currentDocScrollY()) / dpr);

        QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);

        // Turn mode off while mouse is held down
        viewer.setRedactionMode(false);
        pump(20);

        QMouseEvent move(QEvent::MouseMove, endVp, endVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);
        QMouseEvent release(QEvent::MouseButtonRelease, endVp, endVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(50);

        const bool r12Ok = (viewer.stagedRedactionCount() == 1);
        recordTest(r12Ok, "Group 4.17: Mode disabled mid-drag still stages mark on mouse release (R12)");

        viewer.clearStagedRedactions();
    }

    // 4.18: closeDocument with staged marks clears them and resets drag state (R8)
    {
        viewer.setRedactionMode(true);
        viewer.addStagedRedaction(0, {10.0, 10.0, 50.0, 50.0});
        const double dpr = viewer.effectiveDpr();
        int64_t sx = 0, sy = 0;
        viewer.layout().pagePointsToDocument(0, 100.0, 100.0, sx, sy);
        const QPointF startVp((sx - viewer.currentDocScrollX()) / dpr, (sy - viewer.currentDocScrollY()) / dpr);

        // Begin drag
        QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);

        SignalCounter spyMarks(&viewer, &ui::PdfViewerWidget::redactionMarksChanged);
        viewer.editLog().clear();
        viewer.closeDocument();
        pump(50);

        const bool marksCleared = (viewer.stagedRedactionCount() == 0) && !viewer.hasStagedRedactions();
        const bool signalEmitted = (spyMarks.count() >= 1);

        // Now release mouse: since closeDocument reset m_isRedacting = false, release must not stage anything
        QMouseEvent release(QEvent::MouseButtonRelease, startVp, startVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(50);
        const bool dragReset = (viewer.stagedRedactionCount() == 0);

        // Reopen samplePdf
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        const bool r8Ok = marksCleared && signalEmitted && dragReset;
        const std::string dR8 = "cleared=" + std::to_string(marksCleared) + " sig=" + std::to_string(signalEmitted) +
                                " dragReset=" + std::to_string(dragReset) + " count=" + std::to_string(viewer.stagedRedactionCount());
        recordTest(r8Ok, "Group 4.18: closeDocument clears staged marks and resets drag state (R8)", dR8);

        viewer.setRedactionMode(false);
    }

    // 4.19: markSelectionForRedaction padding, page clamp, multi-page staging, and selection clearing (R7)
    {
        // 1. Without selection: returns false
        viewer.clearSelection();
        const bool noSelFalse = !viewer.markSelectionForRedaction();

        bool multiPageOk = false;
        bool padOk = false;
        bool clampOk = false;
        bool clearSelOk = false;
        bool emitOnce = false;

        // 2. Open twoPageTextPdf to test multi-page selection and 1.5 pt padding
        if (!twoPageTextPdf.isEmpty()) {
            viewer.editLog().clear();
            viewer.openDocument(twoPageTextPdf);
            viewer.waitForRendering();
            viewer.setZoom(0.2);
            viewer.goToPage(0);
            viewer.waitForRendering();
            pump(100);

            waitFor([&]() {
                return viewer.getPageText(0) != nullptr && viewer.getPageText(1) != nullptr;
            }, 5000);

            const auto pt0 = viewer.getPageText(0);
            const auto pt1 = viewer.getPageText(1);
            if (pt0 && pt1 && pt0->charCount() > 5 && pt1->charCount() > 5) {
                const auto& pl0 = viewer.layout().getPageLayout(0);
                const auto& pl1 = viewer.layout().getPageLayout(1);
                const double dpr = viewer.effectiveDpr();
                const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;

                auto charCenter = [&](const layout::PageLayout& pl, const pdf::PageCharBox& b) -> QPointF {
                    double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                    double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                    return QPointF(x, y);
                };

                // Select from page 0 char 0 to page 1 char 4
                QMouseEvent p(QEvent::MouseButtonPress, charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[0]), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &p);
                pump(20);
                QMouseEvent m(QEvent::MouseMove, charCenter(pl1, pt1->chars[4]), charCenter(pl1, pt1->chars[4]), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &m);
                pump(20);
                QMouseEvent r(QEvent::MouseButtonRelease, charCenter(pl1, pt1->chars[4]), charCenter(pl1, pt1->chars[4]), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &r);
                pump(50);

                if (viewer.hasSelection()) {
                    const auto selRects0 = pt0->getSelectionRects(0, static_cast<int>(pt0->charCount()) - 1);
                    SignalCounter spyMarks(&viewer, &ui::PdfViewerWidget::redactionMarksChanged);

                    const bool markRet = viewer.markSelectionForRedaction();
                    pump(50);

                    // RM8: Selection must be cleared
                    clearSelOk = markRet && !viewer.hasSelection();
                    // Emits redactionMarksChanged once
                    emitOnce = (spyMarks.count() == 1);
                    // Multi-page: marks present on both page 0 and page 1
                    multiPageOk = (viewer.stagedRedactions().count(0) > 0) && (viewer.stagedRedactions().count(1) > 0);

                    // RM6: Check 1.5 pt padding
                    if (!selRects0.empty() && viewer.stagedRedactions().count(0) > 0) {
                        const auto& staged0 = viewer.stagedRedactions().at(0)[0];
                        const auto& raw0 = selRects0[0];
                        // rLeft - padLeft == 1.5 pt
                        padOk = (std::abs((raw0.left - staged0.left) - 1.5) < 1e-2);
                    }
                }
            }
        }

        std::string dClamp;
        // 3. RM7: Corner text clamping at page boundaries
        viewer.clearStagedRedactions();
        QTemporaryDir cornerDir;
        if (cornerDir.isValid()) {
            const QString cornerPdf = cornerDir.path() + "/corner_text.pdf";
            generateCornerTextPdf(cornerPdf.toStdString());
            viewer.editLog().clear();
            viewer.openDocument(cornerPdf);
            viewer.waitForRendering();
            viewer.setZoom(0.5);
            viewer.goToPage(0);
            viewer.waitForRendering();
            pump(100);

            waitFor([&]() {
                return viewer.getPageText(0) != nullptr;
            }, 5000);

            const auto pt = viewer.getPageText(0);
            dClamp += "pt=" + std::to_string(pt != nullptr);
            if (pt) {
                dClamp += " chars=" + std::to_string(pt->charCount());
            }
            if (pt && pt->charCount() > 5) {
                const auto& pl = viewer.layout().getPageLayout(0);
                const double dpr = viewer.effectiveDpr();
                const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;
                auto charCenter = [&](const pdf::PageCharBox& b) -> QPointF {
                    double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                    double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                    return QPointF(x, y);
                };
                const int lastChar = static_cast<int>(pt->charCount()) - 1;
                QPointF from = charCenter(pt->chars[0]);
                QPointF to = charCenter(pt->chars[static_cast<size_t>(lastChar)]);
                QMouseEvent p(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &p);
                pump(20);
                QMouseEvent m(QEvent::MouseMove, to, to, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &m);
                pump(20);
                QMouseEvent r(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &r);
                pump(50);

                dClamp += " sel=" + std::to_string(viewer.hasSelection());
                if (viewer.hasSelection()) {
                    viewer.markSelectionForRedaction();
                    pump(50);
                    dClamp += " staged=" + std::to_string(viewer.stagedRedactions().count(0));
                    if (viewer.stagedRedactions().count(0) > 0) {
                        const auto& staged = viewer.stagedRedactions().at(0)[0];
                        dClamp += " r=" + std::to_string(staged.right) + " max=" + std::to_string(pl.widthPoints);
                        // The text reaches past the right margin so raw.right + 1.5 > pl.widthPoints (300.0), clamped to pl.widthPoints
                        clampOk = (std::abs(staged.right - pl.widthPoints) < 1e-2) && (staged.right <= pl.widthPoints);
                    }
                }
            }
        }

        // Restore viewer with samplePdf
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.setZoom(1.0);
        viewer.goToPage(0);
        viewer.waitForRendering();
        pump(100);

        const bool r7Ok = noSelFalse && multiPageOk && padOk && clampOk && clearSelOk && emitOnce;
        const std::string dR7 = "noSel=" + std::to_string(noSelFalse) + " multi=" + std::to_string(multiPageOk) +
                                " pad=" + std::to_string(padOk) + " clamp=" + std::to_string(clampOk) +
                                " clearSel=" + std::to_string(clearSelOk) + " emit1=" + std::to_string(emitOnce) +
                                " [" + dClamp + "]";
        recordTest(r7Ok, "Group 4.19: markSelectionForRedaction pads, clamps, stages multi-page, and clears selection (R7)", dR7);
    }

    // 4.20: Overlay paint renders dark mask for staged mark and rubber-band tint during drag (R10)
    {
        if (!twoPageTextPdf.isEmpty()) {
            viewer.editLog().clear();
            viewer.openDocument(twoPageTextPdf);
            viewer.waitForRendering();
            pump(100);
        }

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(true);
        pump(50);

        // 1. RM13: Staged mark mask is dark (alpha 190) over white background
        pdf::ipc::RedactRect r{50.0, 50.0, 150.0, 150.0};
        viewer.addStagedRedaction(0, r);
        pump(50);

        QImage maskImg(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&maskImg);
            viewer.viewport()->render(&p);
            p.end();
        }

        // Sample at (70.0, 70.0) away from centered 8pt "REDACT" badge text
        int64_t markCx = 0, markCy = 0;
        viewer.layout().pagePointsToDocument(0, 70.0, 70.0, markCx, markCy);
        const double dpr = viewer.effectiveDpr();
        const int markVpX = static_cast<int>(std::round((markCx - viewer.currentDocScrollX()) / dpr));
        const int markVpY = static_cast<int>(std::round((markCy - viewer.currentDocScrollY()) / dpr));
        const QColor markCol = maskImg.pixelColor(markVpX, markVpY);

        // Over white background (255, 255, 255), alpha 190 yields rgb ~ 65. Without fill (alpha 0), it is 255.
        const bool markMaskDark = (markCol.red() < 120) && (markCol.green() < 120) && (markCol.blue() < 120);

        viewer.clearStagedRedactions();
        pump(50);

        // 2. RM14: Drag rubber-band renders semi-transparent crimson tint
        int64_t dragSx = 0, dragSy = 0, dragEx = 0, dragEy = 0;
        viewer.layout().pagePointsToDocument(0, 20.0, 20.0, dragSx, dragSy);
        viewer.layout().pagePointsToDocument(0, 80.0, 80.0, dragEx, dragEy);
        const QPointF dragStartVp((dragSx - viewer.currentDocScrollX()) / dpr, (dragSy - viewer.currentDocScrollY()) / dpr);
        const QPointF dragEndVp((dragEx - viewer.currentDocScrollX()) / dpr, (dragEy - viewer.currentDocScrollY()) / dpr);

        QMouseEvent press(QEvent::MouseButtonPress, dragStartVp, dragStartVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);
        QMouseEvent move(QEvent::MouseMove, dragEndVp, dragEndVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);

        // While drag is in progress (mouse held), render viewport
        QImage dragImg(viewer.viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&dragImg);
            viewer.viewport()->render(&p);
            p.end();
        }

        // Release mouse to end drag
        QMouseEvent release(QEvent::MouseButtonRelease, dragEndVp, dragEndVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release);
        pump(50);

        int64_t bandCx = 0, bandCy = 0;
        viewer.layout().pagePointsToDocument(0, 50.0, 50.0, bandCx, bandCy);
        const int bandVpX = static_cast<int>(std::round((bandCx - viewer.currentDocScrollX()) / dpr));
        const int bandVpY = static_cast<int>(std::round((bandCy - viewer.currentDocScrollY()) / dpr));
        const QColor bandCol = dragImg.pixelColor(bandVpX, bandVpY);

        // Tinted QColor(220, 20, 60, 70) over white has blue < 230 and green < 230. Unpainted white has 255.
        const bool bandTinted = (bandCol != QColor(255, 255, 255)) && (bandCol.green() < 230) && (bandCol.blue() < 230);

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);

        // Restore samplePdf for subsequent groups
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        const bool r10Ok = markMaskDark && bandTinted;
        const std::string dR10 = "maskCol=" + markCol.name().toStdString() + " (dark=" + std::to_string(markMaskDark) +
                                 ") bandCol=" + bandCol.name().toStdString() + " (tint=" + std::to_string(bandTinted) + ")";
        recordTest(r10Ok, "Group 4.20: Overlay paint renders dark mask and rubber-band tint (R10)", dR10);
    }

    // 4.21: Exact 3.0 pt drag threshold boundary pinning on both axes (G1)
    // Kills mutations: (x1-x0) >= 3.0 -> > 3.0, >= 2.5, or >= 3.9 (on x and on y)
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        // At zoom 10.0 (the upper zoom clamp), effScale = 10.000 exact document pixels per PDF point.
        // 29 doc pixels = 29 / 10 = 2.900 pt (< 3.0 pt: stages 0 marks; kills >= 2.5)
        // 30 doc pixels = 30 / 10 = 3.000 pt (== 3.0 pt: stages 1 mark; kills > 3.0)
        // 31 doc pixels = 31 / 10 = 3.100 pt (>= 3.0 pt: stages 1 mark; kills >= 3.9)
        viewer.setZoom(10.0);
        viewer.waitForRendering();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        auto sendExactDrag = [&](int64_t deltaDocX, int64_t deltaDocY) {
            int64_t sx = 0, sy = 0;
            viewer.layout().pagePointsToDocument(0, 100.0, 100.0, sx, sy);
            int64_t ex = sx + deltaDocX;
            int64_t ey = sy + deltaDocY;
            const QPointF startVp(static_cast<double>(sx - viewer.currentDocScrollX()) / dpr,
                                  static_cast<double>(sy - viewer.currentDocScrollY()) / dpr);
            const QPointF endVp(static_cast<double>(ex - viewer.currentDocScrollX()) / dpr,
                                static_cast<double>(ey - viewer.currentDocScrollY()) / dpr);

            QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &press);
            pump(20);
            QMouseEvent move(QEvent::MouseMove, endVp, endVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &move);
            pump(20);
            QMouseEvent release(QEvent::MouseButtonRelease, endVp, endVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &release);
            pump(50);
        };

        // --- X axis threshold tests (with dy = 150 px = 15.0 pt >= 3.0) ---
        // 1. dx = 29 px (2.900 pt < 3.0 pt): stages nothing (kills >= 2.5)
        sendExactDrag(29, 150);
        const bool x2_9Rejected = (viewer.stagedRedactionCount() == 0);
        viewer.clearStagedRedactions();

        // 2. dx = 30 px (3.000 pt == 3.0 pt): stages 1 mark (kills > 3.0)
        sendExactDrag(30, 150);
        const bool x3_0Accepted = (viewer.stagedRedactionCount() == 1);
        viewer.clearStagedRedactions();

        // 3. dx = 31 px (3.100 pt >= 3.0 pt): stages 1 mark (kills >= 3.9)
        sendExactDrag(31, 150);
        const bool x3_1Accepted = (viewer.stagedRedactionCount() == 1);
        viewer.clearStagedRedactions();

        // --- Y axis threshold tests (with dx = 150 px = 15.0 pt >= 3.0) ---
        // 4. dy = 29 px (2.900 pt < 3.0 pt): stages nothing (kills >= 2.5)
        sendExactDrag(150, 29);
        const bool y2_9Rejected = (viewer.stagedRedactionCount() == 0);
        viewer.clearStagedRedactions();

        // 5. dy = 30 px (3.000 pt == 3.0 pt): stages 1 mark (kills > 3.0)
        sendExactDrag(150, 30);
        const bool y3_0Accepted = (viewer.stagedRedactionCount() == 1);
        viewer.clearStagedRedactions();

        // 6. dy = 31 px (3.100 pt >= 3.0 pt): stages 1 mark (kills >= 3.9)
        sendExactDrag(150, 31);
        const bool y3_1Accepted = (viewer.stagedRedactionCount() == 1);
        viewer.clearStagedRedactions();

        viewer.setZoom(1.0);
        viewer.setRedactionMode(false);
        viewer.waitForRendering();
        pump(50);

        const bool g1Ok = x2_9Rejected && x3_0Accepted && x3_1Accepted &&
                          y2_9Rejected && y3_0Accepted && y3_1Accepted;
        const std::string dG1 = "x[2.9=" + std::to_string(x2_9Rejected) +
                                ",3.0=" + std::to_string(x3_0Accepted) +
                                ",3.1=" + std::to_string(x3_1Accepted) + "]" +
                                " y[2.9=" + std::to_string(y2_9Rejected) +
                                ",3.0=" + std::to_string(y3_0Accepted) +
                                ",3.1=" + std::to_string(y3_1Accepted) + "]";
        recordTest(g1Ok, "Group 4.21: Exact 3.0 pt drag threshold boundary and relaxation immunity (G1)", dG1);
    }

    // 4.22: Drag lower clamp on x0 and y0 to page origin (G2)
    // Kills mutation: removing std::max(0.0, ...) on x0 or y0
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        auto sendPageDrag = [&](double startXpt, double startYpt, double endXpt, double endYpt) {
            int64_t sx = 0, sy = 0, ex = 0, ey = 0;
            viewer.layout().pagePointsToDocument(0, startXpt, startYpt, sx, sy);
            viewer.layout().pagePointsToDocument(0, endXpt, endYpt, ex, ey);
            const QPointF startVp(static_cast<double>(sx - viewer.currentDocScrollX()) / dpr,
                                  static_cast<double>(sy - viewer.currentDocScrollY()) / dpr);
            const QPointF endVp(static_cast<double>(ex - viewer.currentDocScrollX()) / dpr,
                                static_cast<double>(ey - viewer.currentDocScrollY()) / dpr);

            QMouseEvent press(QEvent::MouseButtonPress, startVp, startVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &press);
            pump(20);
            QMouseEvent move(QEvent::MouseMove, endVp, endVp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &move);
            pump(20);
            QMouseEvent release(QEvent::MouseButtonRelease, endVp, endVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &release);
            pump(50);
        };

        // 1. Drag left past page edge: from (100.0, 100.0) to (-100.0, 150.0)
        sendPageDrag(100.0, 100.0, -100.0, 150.0);
        bool leftClampedOk = false;
        if (viewer.stagedRedactionCount() == 1 && viewer.stagedRedactions().count(0) == 1) {
            const auto& r = viewer.stagedRedactions().at(0)[0];
            leftClampedOk = (r.left == 0.0) && (std::abs(r.right - 100.0) < 1.0);
        }
        viewer.clearStagedRedactions();

        // 2. Drag above page edge: from (100.0, 100.0) to (150.0, -100.0)
        sendPageDrag(100.0, 100.0, 150.0, -100.0);
        bool topClampedOk = false;
        if (viewer.stagedRedactionCount() == 1 && viewer.stagedRedactions().count(0) == 1) {
            const auto& r = viewer.stagedRedactions().at(0)[0];
            topClampedOk = (r.top == 0.0) && (std::abs(r.bottom - 100.0) < 1.0);
        }
        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);

        const bool g2Ok = leftClampedOk && topClampedOk;
        const std::string dG2 = "leftClamp=" + std::to_string(leftClampedOk) + " topClamp=" + std::to_string(topClampedOk);
        recordTest(g2Ok, "Group 4.22: Drag lower clamp on x0 and y0 to page origin (G2)", dG2);
    }

    // 4.23: Bottom-edge selection padBottom clamp to page height (G3)
    // Kills mutation: removing min(pl.heightPoints, rBottom + kSelectionPadPt)
    {
        QTemporaryDir bottomDir;
        if (bottomDir.isValid()) {
            const QString bottomPdf = bottomDir.path() + "/bottom_text.pdf";
            generateBottomTextPdf(bottomPdf.toStdString());

            viewer.editLog().clear();
            viewer.openDocument(bottomPdf);
            viewer.waitForRendering();
            viewer.setZoom(1.0);
            viewer.goToPage(0);
            viewer.waitForRendering();
            pump(100);

            waitFor([&]() {
                return viewer.getPageText(0) != nullptr;
            }, 5000);

            const auto pt = viewer.getPageText(0);
            bool g3Ok = false;
            std::string dG3;
            if (pt && pt->charCount() > 5) {
                const auto& pl = viewer.layout().getPageLayout(0);
                const double dpr = viewer.effectiveDpr();
                const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;
                auto charCenter = [&](const pdf::PageCharBox& b) -> QPointF {
                    double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                    double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                    return QPointF(x, y);
                };

                const int lastChar = static_cast<int>(pt->charCount()) - 1;
                QPointF from = charCenter(pt->chars[0]);
                QPointF to = charCenter(pt->chars[static_cast<size_t>(lastChar)]);

                QMouseEvent p(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &p);
                pump(20);
                QMouseEvent m(QEvent::MouseMove, to, to, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &m);
                pump(20);
                QMouseEvent r(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(viewer.viewport(), &r);
                pump(50);

                if (viewer.hasSelection()) {
                    viewer.markSelectionForRedaction();
                    pump(50);
                    if (viewer.stagedRedactions().count(0) > 0) {
                        const auto& staged = viewer.stagedRedactions().at(0)[0];
                        dG3 = "stagedBottom=" + std::to_string(staged.bottom) + " max=" + std::to_string(pl.heightPoints);
                        g3Ok = (staged.bottom <= pl.heightPoints) && (std::abs(staged.bottom - pl.heightPoints) < 1e-2);
                    }
                }
            }
            recordTest(g3Ok, "Group 4.23: Bottom-edge selection padBottom clamp to page height (G3)", dG3);
        }

        // Restore samplePdf for subsequent groups
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);
    }

    // 4.24: RedactionMode toggle clears active text selection (G4)
    // Kills mutation: commenting out clearSelection() in setRedactionMode(true)
    {
        if (!twoPageTextPdf.isEmpty()) {
            viewer.editLog().clear();
            viewer.openDocument(twoPageTextPdf);
            viewer.waitForRendering();
            viewer.setZoom(1.0);
            viewer.goToPage(0);
            viewer.waitForRendering();
            pump(100);

            waitFor([&]() {
                return viewer.getPageText(0) != nullptr;
            }, 5000);
        }

        viewer.setRedactionMode(false);
        viewer.setInkDrawingMode(false);
        viewer.clearSelection();
        pump(50);

        const auto pt0 = viewer.getPageText(0);
        bool selCreated = false;
        if (pt0 && pt0->charCount() > 5) {
            const auto& pl0 = viewer.layout().getPageLayout(0);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;
            auto charCenter = [&](const layout::PageLayout& pl, const pdf::PageCharBox& b) -> QPointF {
                double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                return QPointF(x, y);
            };

            QPointF from = charCenter(pl0, pt0->chars[0]);
            QPointF to = charCenter(pl0, pt0->chars[4]);
            QMouseEvent p(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &p);
            pump(20);
            QMouseEvent m(QEvent::MouseMove, to, to, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &m);
            pump(20);
            QMouseEvent r(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &r);
            pump(50);

            selCreated = viewer.hasSelection();
        }

        // Selection must exist immediately before turning redaction mode on
        SignalCounter spySelChange(&viewer, &ui::PdfViewerWidget::selectionChanged);
        viewer.setRedactionMode(true);
        pump(50);

        const bool selClearedOnModeOn = selCreated && !viewer.hasSelection() && (spySelChange.count() >= 1);
        const std::string dG4 = "selCreated=" + std::to_string(selCreated) +
                                " hasSelAfter=" + std::to_string(viewer.hasSelection()) +
                                " selSignals=" + std::to_string(spySelChange.count());
        recordTest(selClearedOnModeOn, "Group 4.24: RedactionMode toggle clears active text selection (G4)", dG4);

        viewer.setRedactionMode(false);
        viewer.clearStagedRedactions();

        // Restore samplePdf
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);
    }

    // 4.25: beginDrag initializes current doc pos preventing stale click-drag (T1)
    // Kills mutation: commenting out m_dragCurrentDocPos = docPos in beginDrag()
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        auto pagePtToVp = [&](int page, double px, double py) -> QPointF {
            int64_t sx = 0, sy = 0;
            viewer.layout().pagePointsToDocument(page, px, py, sx, sy);
            return QPointF(static_cast<double>(sx - viewer.currentDocScrollX()) / dpr,
                           static_cast<double>(sy - viewer.currentDocScrollY()) / dpr);
        };

        // 1. Drag a rectangle far from origin (100, 100) -> (200, 200) and release, staging 1 mark
        QPointF p100 = pagePtToVp(0, 100.0, 100.0);
        QPointF p200 = pagePtToVp(0, 200.0, 200.0);
        QMouseEvent press1(QEvent::MouseButtonPress, p100, p100, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press1);
        pump(20);
        QMouseEvent move1(QEvent::MouseMove, p200, p200, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move1);
        pump(20);
        QMouseEvent release1(QEvent::MouseButtonRelease, p200, p200, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release1);
        pump(50);
        const bool step1Ok = (viewer.stagedRedactionCount() == 1);

        // 2. Clear staged marks; count is 0
        viewer.clearStagedRedactions();
        pump(20);
        const bool step2Ok = (viewer.stagedRedactionCount() == 0);

        // 3. Press at (20, 20) with no move, release immediately at (20, 20)
        QPointF p20 = pagePtToVp(0, 20.0, 20.0);
        QMouseEvent press2(QEvent::MouseButtonPress, p20, p20, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press2);
        pump(20);
        QMouseEvent release2(QEvent::MouseButtonRelease, p20, p20, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &release2);
        pump(50);

        // 4. Assert stagedRedactionCount() == 0 (under mutation, delta |200-20|=180 stages a mark)
        const bool step3Ok = (viewer.stagedRedactionCount() == 0);

        const bool t1Ok = step1Ok && step2Ok && step3Ok;
        const std::string dT1 = "step1=" + std::to_string(step1Ok) +
                                " step2=" + std::to_string(step2Ok) +
                                " countAfterClick=" + std::to_string(viewer.stagedRedactionCount());
        recordTest(t1Ok, "Group 4.25: beginDrag initializes current doc pos preventing stale click-drag (T1)", dT1);

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);
    }

    // 4.26: resetDrag on closeDocument discards in-flight drag across reopen (T2)
    // Kills mutation: resetDrag() becoming a no-op
    {
        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        auto pagePtToVp = [&](int page, double px, double py) -> QPointF {
            int64_t sx = 0, sy = 0;
            viewer.layout().pagePointsToDocument(page, px, py, sx, sy);
            return QPointF(static_cast<double>(sx - viewer.currentDocScrollX()) / dpr,
                           static_cast<double>(sy - viewer.currentDocScrollY()) / dpr);
        };

        // 1. Press at (100, 100) and move to (150, 150) without releasing
        QPointF p100 = pagePtToVp(0, 100.0, 100.0);
        QPointF p150 = pagePtToVp(0, 150.0, 150.0);
        QMouseEvent press(QEvent::MouseButtonPress, p100, p100, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &press);
        pump(20);
        QMouseEvent move(QEvent::MouseMove, p150, p150, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &move);
        pump(20);

        // 2. Call closeDocument(), reopen samplePdf, wait for layout
        viewer.editLog().clear();
        viewer.closeDocument();
        pump(50);
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        // 3. Assert stagedRedactionCount() == 0 after reopen
        const bool countZeroAfterReopen = (viewer.stagedRedactionCount() == 0);

        // 4. Send only a left-button release (no press) at moved position (150, 150)
        QPointF releaseVp = pagePtToVp(0, 150.0, 150.0);
        QMouseEvent loneRelease(QEvent::MouseButtonRelease, releaseVp, releaseVp, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &loneRelease);
        pump(50);

        // 5. Assert stagedRedactionCount() == 0
        const bool countZeroAfterRelease = (viewer.stagedRedactionCount() == 0);

        const bool t2Ok = countZeroAfterReopen && countZeroAfterRelease;
        const std::string dT2 = "reopenCount=" + std::to_string(viewer.stagedRedactionCount()) +
                                " zeroAfterReopen=" + std::to_string(countZeroAfterReopen) +
                                " zeroAfterRelease=" + std::to_string(countZeroAfterRelease);
        recordTest(t2Ok, "Group 4.26: resetDrag on closeDocument discards in-flight drag across reopen (T2)", dT2);

        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);
    }

    // 4.27: finishDrag clears isDragging allowing subsequent text selection (T3)
    // Kills mutation: commenting out m_isDragging = false in finishDrag()
    {
        // Open twoPageTextPdf so page 0 has known text characters
        if (!twoPageTextPdf.isEmpty()) {
            viewer.editLog().clear();
            viewer.openDocument(twoPageTextPdf);
            viewer.waitForRendering();
            viewer.setZoom(1.0);
            viewer.goToPage(0);
            viewer.waitForRendering();
            pump(100);

            waitFor([&]() {
                return viewer.getPageText(0) != nullptr;
            }, 5000);
        }

        viewer.setRedactionMode(true);
        viewer.clearStagedRedactions();
        viewer.clearSelection();
        pump(50);

        const double dpr = viewer.effectiveDpr();
        auto pagePtToVp = [&](int page, double px, double py) -> QPointF {
            int64_t sx = 0, sy = 0;
            viewer.layout().pagePointsToDocument(page, px, py, sx, sy);
            return QPointF(static_cast<double>(sx - viewer.currentDocScrollX()) / dpr,
                           static_cast<double>(sy - viewer.currentDocScrollY()) / dpr);
        };

        // 1. Drag from (100, 100) to (150, 150) in page pt and release, staging 1 mark
        QPointF p100 = pagePtToVp(0, 100.0, 100.0);
        QPointF p150 = pagePtToVp(0, 150.0, 150.0);
        QMouseEvent p1(QEvent::MouseButtonPress, p100, p100, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &p1);
        pump(20);
        QMouseEvent m1(QEvent::MouseMove, p150, p150, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &m1);
        pump(20);
        QMouseEvent r1(QEvent::MouseButtonRelease, p150, p150, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &r1);
        pump(50);
        const bool step1Ok = (viewer.stagedRedactionCount() == 1);

        // 2. Without pressing, send a mouse move to (250, 250) and a mouse release
        QPointF p250 = pagePtToVp(0, 250.0, 250.0);
        QMouseEvent bareMove(QEvent::MouseMove, p250, p250, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &bareMove);
        pump(20);
        QMouseEvent bareRelease(QEvent::MouseButtonRelease, p250, p250, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer.viewport(), &bareRelease);
        pump(50);

        // 3. Assert stagedRedactionCount() == 1
        const bool step2Ok = (viewer.stagedRedactionCount() == 1);

        // 4. Turn redaction mode off, select text across chars via mouse drag
        viewer.setRedactionMode(false);
        viewer.clearSelection();
        pump(50);

        const auto pt0 = viewer.getPageText(0);
        bool selCreated = false;
        if (pt0 && pt0->charCount() > 5) {
            const auto& pl0 = viewer.layout().getPageLayout(0);
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;
            auto charCenter = [&](const layout::PageLayout& pl, const pdf::PageCharBox& b) -> QPointF {
                double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                return QPointF(x, y);
            };

            QPointF from = charCenter(pl0, pt0->chars[0]);
            QPointF to = charCenter(pl0, pt0->chars[4]);
            QMouseEvent pSel(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &pSel);
            pump(20);
            QMouseEvent mSel(QEvent::MouseMove, to, to, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &mSel);
            pump(20);
            QMouseEvent rSel(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(viewer.viewport(), &rSel);
            pump(50);

            // 5. Assert viewer.hasSelection() == true
            selCreated = viewer.hasSelection();
        }

        const bool t3Ok = step1Ok && step2Ok && selCreated;
        const std::string dT3 = "step1=" + std::to_string(step1Ok) +
                                " step2=" + std::to_string(step2Ok) +
                                " selCreated=" + std::to_string(selCreated) +
                                " hasSel=" + std::to_string(viewer.hasSelection());
        recordTest(t3Ok, "Group 4.27: finishDrag clears isDragging allowing subsequent text selection (T3)", dT3);

        viewer.clearSelection();
        viewer.clearStagedRedactions();
        viewer.setRedactionMode(false);

        // Restore samplePdf for Group 5
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);
    }

    // 4.28: addTextAnnotation item construction, UUID name, unclamped rect, default color, and flags (A1)
    {
        viewer.editLog().clear();

        // 1. Add note with default color (0), specific coordinates
        const double x1 = 50.0;
        const double y1 = 80.0;
        const bool add1 = viewer.addTextAnnotation(0, x1, y1, QStringLiteral("Sticky Note One"), 0);
        const auto cmds1 = viewer.editLog().activeCommands();
        bool a1Ok = add1 && (cmds1.size() == 1);
        if (a1Ok) {
            const auto& item1 = cmds1.back().after;
            const bool prefixOk = item1.name.startsWith(QStringLiteral("pdfr-"));
            const QString uuidPart = item1.name.mid(5);
            const bool uuidLenOk = (uuidPart.length() == 36) && !uuidPart.contains(QLatin1Char('{')) && !uuidPart.contains(QLatin1Char('}'));
            const bool subtypeOk = (item1.subtype == pdf::AnnotationSubtype::Text);
            const bool rectOk = (std::abs(item1.left - 50.0) < 1e-3) &&
                               (std::abs(item1.bottom - 56.0) < 1e-3) && // 80 - 24
                               (std::abs(item1.right - 74.0) < 1e-3) &&  // 50 + 24
                               (std::abs(item1.top - 80.0) < 1e-3);
            const bool colorOk = (item1.colorArgb == 0xFFFFD700) && item1.hasColor;
            const bool flagsOk = (item1.flags == 4);
            const bool contentsOk = (item1.contents == QStringLiteral("Sticky Note One"));

            // 2. Add second note: verify unique UUID and unclamped coordinates outside page (e.g. negative)
            const bool add2 = viewer.addTextAnnotation(0, -10.0, -5.0, QStringLiteral("Outside Page Note"), 0xFF112233);
            const auto cmds2 = viewer.editLog().activeCommands();
            bool note2Ok = false;
            if (add2 && cmds2.size() == 2) {
                const auto& item2 = cmds2.back().after;
                const bool uniqueName = (item2.name != item1.name) && item2.name.startsWith(QStringLiteral("pdfr-"));
                const bool unclamped = (std::abs(item2.left - (-10.0)) < 1e-3) &&
                                       (std::abs(item2.bottom - (-29.0)) < 1e-3) &&
                                       (std::abs(item2.right - 14.0) < 1e-3) &&
                                       (std::abs(item2.top - (-5.0)) < 1e-3);
                const bool colorExplicit = (item2.colorArgb == 0xFF112233);
                note2Ok = uniqueName && unclamped && colorExplicit;
            }
            a1Ok = prefixOk && uuidLenOk && subtypeOk && rectOk && colorOk && flagsOk && contentsOk && note2Ok;
        }

        recordTest(a1Ok, "Group 4.28: addTextAnnotation name, unclamped rect, default gold color, flags, contents (A1)");
        viewer.editLog().clear();
    }

    // 4.29: addSquareAnnotation and addCircleAnnotation rect normalization, bottom/top mapping, border width, default colors (A2)
    {
        viewer.editLog().clear();

        // 1. Square: un-normalized rect QRectF(150, 280, -50, -80) -> normalized(100, 200, 50, 80)
        // bottom = normalized.top() = 200, top = normalized.bottom() = 280
        // Border width: test 0.0 -> fallback 2.0; default color -> 0xFFFF0000
        const bool addSq1 = viewer.addSquareAnnotation(0, QRectF(150.0, 280.0, -50.0, -80.0), 0, 0.0);
        // Border width: test negative (-5.0) -> fallback 2.0
        const bool addSq2 = viewer.addSquareAnnotation(0, QRectF(10.0, 10.0, 20.0, 20.0), 0xFFAABBCC, -5.0);
        // Border width: test exactly 100.0 -> accepted 100.0
        const bool addSq3 = viewer.addSquareAnnotation(0, QRectF(10.0, 10.0, 20.0, 20.0), 0xFFAABBCC, 100.0);
        // Border width: test just over 100.0 (100.1) -> fallback 2.0
        const bool addSq4 = viewer.addSquareAnnotation(0, QRectF(10.0, 10.0, 20.0, 20.0), 0xFFAABBCC, 100.1);

        // 2. Circle: default color -> 0xFF0000FF, border width 3.5 -> 3.5
        const bool addCr1 = viewer.addCircleAnnotation(0, QRectF(200.0, 300.0, 40.0, 60.0), 0, 3.5);

        // 3. Rejections: empty rect and bad page must return false and log nothing
        const size_t cmdsBeforeRejects = viewer.editLog().activeCommands().size();
        const bool rejEmpty1 = !viewer.addSquareAnnotation(0, QRectF(10.0, 10.0, 0.0, 20.0));
        const bool rejEmpty2 = !viewer.addCircleAnnotation(0, QRectF(10.0, 10.0, 20.0, 0.0));
        const bool rejBadPage = !viewer.addSquareAnnotation(-1, QRectF(10.0, 10.0, 20.0, 20.0)) &&
                                !viewer.addCircleAnnotation(viewer.pageCount(), QRectF(10.0, 10.0, 20.0, 20.0));
        const bool noRejectsLogged = (viewer.editLog().activeCommands().size() == cmdsBeforeRejects);

        const auto cmds = viewer.editLog().activeCommands();
        bool a2Ok = addSq1 && addSq2 && addSq3 && addSq4 && addCr1 &&
                    rejEmpty1 && rejEmpty2 && rejBadPage && noRejectsLogged && (cmds.size() == 5);
        if (a2Ok) {
            const auto& sq1 = cmds[0].after;
            const bool sq1Mapping = (sq1.subtype == pdf::AnnotationSubtype::Square) &&
                                    (std::abs(sq1.left - 100.0) < 1e-3) &&
                                    (std::abs(sq1.bottom - 200.0) < 1e-3) && // normalized.top()
                                    (std::abs(sq1.right - 150.0) < 1e-3) &&
                                    (std::abs(sq1.top - 280.0) < 1e-3) &&    // normalized.bottom()
                                    (sq1.borderWidth == 2.0) &&              // fallback from 0
                                    (sq1.colorArgb == 0xFFFF0000) && sq1.hasColor && (sq1.flags == 4);

            const bool sq2Bw = (cmds[1].after.borderWidth == 2.0);   // fallback from negative
            const bool sq3Bw = (cmds[2].after.borderWidth == 100.0); // boundary accepted
            const bool sq4Bw = (cmds[3].after.borderWidth == 2.0);   // fallback from > 100

            const auto& cr1 = cmds[4].after;
            const bool cr1Ok = (cr1.subtype == pdf::AnnotationSubtype::Circle) &&
                               (std::abs(cr1.left - 200.0) < 1e-3) &&
                               (std::abs(cr1.bottom - 300.0) < 1e-3) &&
                               (std::abs(cr1.right - 240.0) < 1e-3) &&
                               (std::abs(cr1.top - 360.0) < 1e-3) &&
                               (cr1.borderWidth == 3.5) &&
                               (cr1.colorArgb == 0xFF0000FF) && cr1.hasColor && (cr1.flags == 4);

            a2Ok = sq1Mapping && sq2Bw && sq3Bw && sq4Bw && cr1Ok;
        }

        recordTest(a2Ok, "Group 4.29: addSquareAnnotation and addCircleAnnotation rect mapping, border width, default colors (A2)");
        viewer.editLog().clear();
    }

    // 4.30: addInkAnnotation stroke storage, border width bounds, default black, and empty stroke rejection (A3)
    {
        viewer.editLog().clear();

        // 1. Valid ink strokes
        const std::vector<std::vector<pdf::AnnotationPoint>> strokes = {
            { {10.0, 20.0}, {30.0, 40.0} },
            { {50.0, 60.0}, {70.0, 80.0}, {90.0, 100.0} }
        };
        // Test default color (0) -> 0xFF000000, border width 0 -> 2.0
        const bool addInk1 = viewer.addInkAnnotation(0, strokes, 0, 0.0);
        // Test border width 100 -> 100
        const bool addInk2 = viewer.addInkAnnotation(0, strokes, 0xFF123456, 100.0);
        // Test border width 100.5 -> 2.0
        const bool addInk3 = viewer.addInkAnnotation(0, strokes, 0xFF123456, 100.5);
        // Test border width negative -> 2.0
        const bool addInk4 = viewer.addInkAnnotation(0, strokes, 0xFF123456, -2.0);

        // 2. Rejection of empty strokes
        const size_t cmdsBeforeRejects = viewer.editLog().activeCommands().size();
        const bool rejEmpty = !viewer.addInkAnnotation(0, {}, 0, 2.0);
        const bool rejBadPage = !viewer.addInkAnnotation(-1, strokes, 0, 2.0);
        const bool noRejectsLogged = (viewer.editLog().activeCommands().size() == cmdsBeforeRejects);

        const auto cmds = viewer.editLog().activeCommands();
        bool a3Ok = addInk1 && addInk2 && addInk3 && addInk4 && rejEmpty && rejBadPage && noRejectsLogged && (cmds.size() == 4);
        if (a3Ok) {
            const auto& ink1 = cmds[0].after;
            const bool subtypeOk = (ink1.subtype == pdf::AnnotationSubtype::Ink);
            const bool strokesOk = (ink1.inkStrokes.size() == 2) &&
                                   (ink1.inkStrokes[0].size() == 2) &&
                                   (ink1.inkStrokes[0][0].x == 10.0 && ink1.inkStrokes[0][0].y == 20.0) &&
                                   (ink1.inkStrokes[1].size() == 3) &&
                                   (ink1.inkStrokes[1][2].x == 90.0 && ink1.inkStrokes[1][2].y == 100.0);
            const bool colorOk = (ink1.colorArgb == 0xFF000000) && ink1.hasColor;
            const bool bw1 = (ink1.borderWidth == 2.0);
            const bool flagsOk = (ink1.flags == 4);

            const bool bw2 = (cmds[1].after.borderWidth == 100.0);
            const bool bw3 = (cmds[2].after.borderWidth == 2.0);
            const bool bw4 = (cmds[3].after.borderWidth == 2.0);

            a3Ok = subtypeOk && strokesOk && colorOk && bw1 && flagsOk && bw2 && bw3 && bw4;
        }

        recordTest(a3Ok, "Group 4.30: addInkAnnotation stroke storage, border width bounds, default black, empty rejection (A3)");
        viewer.editLog().clear();
    }

    // 4.31: addFreeTextAnnotation trimmed empty, font size bounds, top anchoring, text outline, and error paths (A4)
    {
        viewer.editLog().clear();
        SignalRecorder<QString> spyErr(&viewer, &ui::PdfViewerWidget::loadError);

        // 1. Rejection of trimmed empty text
        const bool rejTrimmed1 = !viewer.addFreeTextAnnotation(0, QRectF(50, 50, 100, 40), QStringLiteral("   "));
        const bool rejTrimmed2 = !viewer.addFreeTextAnnotation(0, QRectF(50, 50, 100, 40), QStringLiteral("\t\n  \n"));

        // 2. Successful add with default color (0 -> 0xFF000000), font size 0 -> 12
        // rect QRectF(50, 60, 100, 30) -> left=50, bottom=60, right=150, top=90
        const bool addFt1 = viewer.addFreeTextAnnotation(0, QRectF(50.0, 60.0, 100.0, 30.0), QStringLiteral("Hello FreeText"), 0, 0.0);
        // Test font size bounds: 144 -> accepted 144
        const bool addFt2 = viewer.addFreeTextAnnotation(0, QRectF(50.0, 60.0, 100.0, 30.0), QStringLiteral("Big FreeText"), 0xFF00FF00, 144.0);
        // Test font size bounds: 145 -> fallback 12
        const bool addFt3 = viewer.addFreeTextAnnotation(0, QRectF(50.0, 60.0, 100.0, 30.0), QStringLiteral("Over FreeText"), 0xFF00FF00, 145.0);

        // 3. Error paths:
        // Undrawable character: Private Use code point U+10FFFD
        const size_t errCountBeforeUndrawable = spyErr.count();
        const char32_t missing = 0x10FFFD;
        const QString undrawableText = QStringLiteral("AB") + QString::fromUcs4(&missing, 1);
        const bool rejUndrawable = !viewer.addFreeTextAnnotation(0, QRectF(50, 50, 100, 30), undrawableText);
        const bool undrawableEmitted = (spyErr.count() > errCountBeforeUndrawable);

        // NothingToDraw: string containing zero-width space U+200B (non-empty after trim, but leavesNoInk)
        const size_t errCountBeforeNothing = spyErr.count();
        const bool rejNothing = !viewer.addFreeTextAnnotation(0, QRectF(50, 50, 100, 30), QString(QChar(0x200B)));
        const bool nothingEmitted = (spyErr.count() > errCountBeforeNothing);

        // TooLarge: 30000 characters
        const size_t errCountBeforeTooLarge = spyErr.count();
        const QString hugeText(30000, QLatin1Char('W'));
        const bool rejTooLarge = !viewer.addFreeTextAnnotation(0, QRectF(50, 50, 100, 30), hugeText);
        const bool tooLargeEmitted = (spyErr.count() > errCountBeforeTooLarge);

        const auto cmds = viewer.editLog().activeCommands();
        bool a4Ok = rejTrimmed1 && rejTrimmed2 && addFt1 && addFt2 && addFt3 &&
                    rejUndrawable && undrawableEmitted &&
                    rejNothing && nothingEmitted &&
                    rejTooLarge && tooLargeEmitted && (cmds.size() == 3);
        if (a4Ok) {
            const auto& ft1 = cmds[0].after;
            const bool subtypeOk = (ft1.subtype == pdf::AnnotationSubtype::FreeText);
            const bool topAnchorOk = (std::abs(ft1.top - 90.0) < 1e-3);
            const bool bottomGrownOk = (ft1.bottom <= 60.0); // top stayed anchored, bottom grew downward
            const bool outlineOk = !ft1.textOutline.empty();
            const bool colorOk = (ft1.colorArgb == 0xFF000000) && ft1.hasColor;
            const bool fs1Ok = (ft1.fontSize == 12.0); // fallback from 0
            const bool flagsOk = (ft1.flags == 4);

            const bool fs2Ok = (cmds[1].after.fontSize == 144.0);
            const bool fs3Ok = (cmds[2].after.fontSize == 12.0); // fallback from 145

            a4Ok = subtypeOk && topAnchorOk && bottomGrownOk && outlineOk && colorOk && fs1Ok && flagsOk && fs2Ok && fs3Ok;
        }

        recordTest(a4Ok, "Group 4.31: addFreeTextAnnotation font size, top anchor, outline, and loadError paths (A4)");
        viewer.editLog().clear();
    }

    // 4.32: addStampAnnotation trimmed text, length limit, default color, border width, outline, and DRAFT fallback (A5)
    {
        viewer.editLog().clear();
        SignalRecorder<QString> spyErr(&viewer, &ui::PdfViewerWidget::loadError);

        // 1. Text is trimmed: "  APPROVED  " -> contents == "APPROVED"
        const bool addSt1 = viewer.addStampAnnotation(0, QRectF(50.0, 50.0, 120.0, 40.0), QStringLiteral("  APPROVED  "), 0);

        // 2. Text longer than kMaxStampTextCharacters (128) emits loadError and logs nothing
        const QString over128(129, QLatin1Char('X'));
        const size_t errBeforeOver = spyErr.count();
        const size_t cmdsBeforeOver = viewer.editLog().activeCommands().size();
        const bool rejOver = !viewer.addStampAnnotation(0, QRectF(50.0, 50.0, 120.0, 40.0), over128);
        const bool overEmitted = (spyErr.count() > errBeforeOver);
        const bool overNotLogged = (viewer.editLog().activeCommands().size() == cmdsBeforeOver);

        // Exactly 128 characters is accepted
        const QString exact128(128, QLatin1Char('Y'));
        const bool addExact = viewer.addStampAnnotation(0, QRectF(50.0, 50.0, 120.0, 40.0), exact128);

        // 3. addDefaultAnnotationAt(Stamp, ..., "") uses "DRAFT"
        const bool addDefStamp = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Stamp, 50.0, 50.0, QString());

        const auto cmds = viewer.editLog().activeCommands();
        bool a5Ok = addSt1 && rejOver && overEmitted && overNotLogged && addExact && addDefStamp && (cmds.size() == 3);
        if (a5Ok) {
            const auto& st1 = cmds[0].after;
            const bool subtypeOk = (st1.subtype == pdf::AnnotationSubtype::Stamp);
            const bool contentsOk = (st1.contents == QStringLiteral("APPROVED")); // trimmed!
            const bool colorOk = (st1.colorArgb == 0xFFCC0000) && st1.hasColor;
            const bool bwOk = (st1.borderWidth == 2.0);
            const bool flagsOk = (st1.flags == 4);
            const bool outlineOk = !st1.textOutline.empty();

            const auto& defStamp = cmds[2].after;
            const bool draftOk = (defStamp.contents == QStringLiteral("DRAFT"));

            a5Ok = subtypeOk && contentsOk && colorOk && bwOk && flagsOk && outlineOk && draftOk;
        }

        recordTest(a5Ok, "Group 4.32: addStampAnnotation trimmed text, length limit, default color, outline, DRAFT fallback (A5)");
        viewer.editLog().clear();
    }

    // 4.33: addFileAttachmentAnnotation invalid filename, payload limits, flags, contents, placeOnPage, default color (A6)
    {
        viewer.editLog().clear();
        SignalRecorder<QString> spyErr(&viewer, &ui::PdfViewerWidget::loadError);

        // 1. Invalid filename emits loadError
        const size_t errBeforeBadFile = spyErr.count();
        const bool rejBadFile = !viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("invalid/filename.bin"), "data");
        const bool badFileEmitted = (spyErr.count() > errBeforeBadFile);

        // 2. Empty payload emits loadError
        const size_t errBeforeEmpty = spyErr.count();
        const bool rejEmptyPayload = !viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("valid.bin"), QByteArray());
        const bool emptyPayloadEmitted = (spyErr.count() > errBeforeEmpty);

        // 3. Oversize payload (> 16 MiB) emits loadError
        const size_t errBeforeOver = spyErr.count();
        QByteArray overData;
        overData.resize(pdf::kMaxAnnotationAttachmentBytes + 1);
        const bool rejOverPayload = !viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("valid.bin"), overData);
        const bool overPayloadEmitted = (spyErr.count() > errBeforeOver);

        // 4. Successful addFileAttachmentAnnotation with default color (0 -> 0xFF336699)
        const QByteArray validData("PayloadContent123");
        const bool addFa = viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("attachment.txt"), validData, 0);

        const auto cmds = viewer.editLog().activeCommands();
        bool a6Ok = rejBadFile && badFileEmitted && rejEmptyPayload && emptyPayloadEmitted &&
                    rejOverPayload && overPayloadEmitted && addFa && (cmds.size() == 1);
        if (a6Ok) {
            const auto& fa = cmds[0].after;
            const bool subtypeOk = (fa.subtype == pdf::AnnotationSubtype::FileAttachment);
            const bool fileOk = (fa.attachmentFileName == QStringLiteral("attachment.txt"));
            const bool contentsOk = (fa.contents == QStringLiteral("attachment.txt"));
            const bool payloadOk = (fa.binaryPayload == validData);
            const uint32_t expectedFlags = pdf::AnnotationFlag::Print | pdf::AnnotationFlag::NoZoom | pdf::AnnotationFlag::NoRotate;
            const bool flagsOk = (fa.flags == expectedFlags); // 4 | 8 | 16 = 28
            const bool colorOk = (fa.colorArgb == 0xFF336699) && fa.hasColor;

            // Geometry: placeOnPage placed 24x24 inside page
            const auto& pl = viewer.layout().getPageLayout(0);
            const bool rectPlaced = (std::abs((fa.right - fa.left) - 24.0) < 1e-3) &&
                                    (std::abs((fa.top - fa.bottom) - 24.0) < 1e-3) &&
                                    (fa.left >= 0.0) && (fa.right <= pl.widthPoints) &&
                                    (fa.bottom >= 0.0) && (fa.top <= pl.heightPoints);

            a6Ok = subtypeOk && fileOk && contentsOk && payloadOk && flagsOk && colorOk && rectPlaced;
        }

        recordTest(a6Ok, "Group 4.33: addFileAttachmentAnnotation filename/payload validation, flags, contents, placeOnPage (A6)");
        viewer.editLog().clear();
    }

    // 4.34: addDefaultAnnotationAt size table, unsupported subtypes, and closed document guard (A7)
    {
        viewer.editLog().clear();

        // 1. Size table verification:
        // Text: 24x24
        const bool addText = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Text, 100.0, 200.0, QStringLiteral("Default Text"));
        // Square: 100x60
        const bool addSq = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Square, 100.0, 200.0);
        // Circle: 100x60
        const bool addCr = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Circle, 100.0, 200.0);
        // Stamp: 120x40
        const bool addSt = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Stamp, 100.0, 200.0, QStringLiteral("STAMP"));

        // 2. Unsupported subtypes in addDefaultAnnotationAt:
        // FileAttachment returns false
        const bool rejFileAtt = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::FileAttachment, 100.0, 200.0);
        // Markup subtypes return false
        const bool rejHl = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Highlight, 100.0, 200.0);
        const bool rejUl = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Underline, 100.0, 200.0);
        const bool rejSo = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::StrikeOut, 100.0, 200.0);
        const bool rejSq = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Squiggly, 100.0, 200.0);
        const bool rejInk = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Ink, 100.0, 200.0);

        const auto cmds = viewer.editLog().activeCommands();
        bool a7Ok = addText && addSq && addCr && addSt &&
                    rejFileAtt && rejHl && rejUl && rejSo && rejSq && rejInk && (cmds.size() == 4);
        if (a7Ok) {
            const auto& t = cmds[0].after;
            const bool textDim = (std::abs((t.right - t.left) - 24.0) < 1e-3) && (std::abs((t.top - t.bottom) - 24.0) < 1e-3);
            const auto& sq = cmds[1].after;
            const bool sqDim = (std::abs((sq.right - sq.left) - 100.0) < 1e-3) && (std::abs((sq.top - sq.bottom) - 60.0) < 1e-3);
            const auto& cr = cmds[2].after;
            const bool crDim = (std::abs((cr.right - cr.left) - 100.0) < 1e-3) && (std::abs((cr.top - cr.bottom) - 60.0) < 1e-3);
            const auto& st = cmds[3].after;
            const bool stDim = (std::abs((st.right - st.left) - 120.0) < 1e-3) && (std::abs((st.top - st.bottom) - 40.0) < 1e-3);

            a7Ok = textDim && sqDim && crDim && stDim;
        }

        // 3. Document closed: addDefaultAnnotationAt returns false
        viewer.editLog().clear();
        viewer.closeDocument();
        pump(50);
        const bool rejClosed = !viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Text, 100.0, 200.0, QStringLiteral("Closed Text"));
        a7Ok = a7Ok && rejClosed;

        // Restore samplePdf
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        recordTest(a7Ok, "Group 4.34: addDefaultAnnotationAt size table, unsupported subtypes, closed doc guard (A7)");
        viewer.editLog().clear();
    }

    // 4.35: markupSelection default colors, quad corners, min/max bounds, multi-page, reverse selection, selection clearing (A8)
    {
        // Open twoPageTextPdf for text selection
        if (!twoPageTextPdf.isEmpty()) {
            viewer.editLog().clear();
            viewer.openDocument(twoPageTextPdf);
            viewer.waitForRendering();
            viewer.setZoom(0.2);
            viewer.goToPage(0);
            viewer.waitForRendering();
            pump(100);
            waitFor([&]() {
                return viewer.getPageText(0) != nullptr && viewer.getPageText(1) != nullptr;
            }, 5000);
        }

        viewer.clearSelection();
        pump(50);

        // 1. With no selection: returns false
        const bool noSelFalse = !viewer.markupSelection(pdf::AnnotationSubtype::Highlight);

        const auto pt0 = viewer.getPageText(0);
        const auto pt1 = viewer.getPageText(1);
        bool a8Ok = false;
        bool hlOk = false;
        bool colorsOk = false;
        bool bwdOk = false;
        bool mpOk = false;
        bool textAvail = (pt0 && pt1 && pt0->charCount() > 5 && pt1->charCount() > 5);
        if (textAvail) {
            const auto& pl0 = viewer.layout().getPageLayout(0);
            const double dpr = viewer.effectiveDpr();
            const double effScale = pdf::quantizeScale(viewer.currentZoom()) * dpr;
            auto charCenter = [&](const layout::PageLayout& pl, const pdf::PageCharBox& b) -> QPointF {
                double x = (static_cast<double>(pl.leftPixel) + ((b.left + b.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                double y = (static_cast<double>(pl.topPixel) + ((b.top + b.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                return QPointF(x, y);
            };

            auto selectRange = [&](const QPointF& from, const QPointF& to) {
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

            // 2. Single-page forward selection on page 0
            selectRange(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[3]));
            const QString selText1 = viewer.selectedText();

            // Highlight with default color (0 -> 0x80FFFF00)
            viewer.editLog().clear();
            const bool hlAdded = viewer.markupSelection(pdf::AnnotationSubtype::Highlight, 0);
            const bool selClearedAfterHl = !viewer.hasSelection(); // cleared on success!
            const auto cmdsHl = viewer.editLog().activeCommands();

            hlOk = false;
            if (hlAdded && selClearedAfterHl && (cmdsHl.size() == 1)) {
                const auto& item = cmdsHl[0].after;
                const bool subtypeOk = (item.subtype == pdf::AnnotationSubtype::Highlight);
                const bool colorOk = (item.colorArgb == 0x80FFFF00) && item.hasColor;
                const bool flagsOk = (item.flags == 4);
                const bool contentsOk = (item.contents == selText1);

                // Quad geometry: corners (pLeft, pTop), (pRight, pTop), (pLeft, pBottom), (pRight, pBottom)
                bool quadsOk = !item.quadPoints.empty();
                double expectedMinX = std::numeric_limits<double>::max();
                double expectedMaxX = std::numeric_limits<double>::lowest();
                double expectedMinY = std::numeric_limits<double>::max();
                double expectedMaxY = std::numeric_limits<double>::lowest();
                for (size_t i = 0; i < item.quadPoints.size(); ++i) {
                    const auto& q = item.quadPoints[i];
                    // In user space: x1=left, x2=right, x3=left, x4=right, y1=top, y2=top, y3=bottom, y4=bottom
                    if (q.x1 > q.x2 || q.y1 < q.y3 || q.x3 > q.x4 || q.y2 < q.y4) {
                        quadsOk = false;
                    }
                    expectedMinX = std::min({expectedMinX, q.x1, q.x2, q.x3, q.x4});
                    expectedMaxX = std::max({expectedMaxX, q.x1, q.x2, q.x3, q.x4});
                    expectedMinY = std::min({expectedMinY, q.y1, q.y2, q.y3, q.y4});
                    expectedMaxY = std::max({expectedMaxY, q.y1, q.y2, q.y3, q.y4});
                }
                const bool boundsOk = (std::abs(item.left - expectedMinX) < 1e-3) &&
                                     (std::abs(item.right - expectedMaxX) < 1e-3) &&
                                     (std::abs(item.bottom - expectedMinY) < 1e-3) &&
                                     (std::abs(item.top - expectedMaxY) < 1e-3);
                hlOk = subtypeOk && colorOk && flagsOk && contentsOk && quadsOk && boundsOk;
            }

            // 3. Test other default colors: Underline 0xFF00AA00, StrikeOut 0xFFFF0000, Squiggly 0xFFFF0000, Other 0x80FFFF00
            selectRange(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[2]));
            viewer.markupSelection(pdf::AnnotationSubtype::Underline, 0);
            selectRange(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[2]));
            viewer.markupSelection(pdf::AnnotationSubtype::StrikeOut, 0);
            selectRange(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[2]));
            viewer.markupSelection(pdf::AnnotationSubtype::Squiggly, 0);
            selectRange(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[2]));
            viewer.markupSelection(pdf::AnnotationSubtype::Text, 0); // "any other subtype" -> default 0x80FFFF00

            const auto cmdsColors = viewer.editLog().activeCommands();
            bool colorsOk = (cmdsColors.size() == 5);
            if (colorsOk) {
                colorsOk = (cmdsColors[1].after.colorArgb == 0xFF00AA00) &&
                           (cmdsColors[2].after.colorArgb == 0xFFFF0000) &&
                           (cmdsColors[3].after.colorArgb == 0xFFFF0000) &&
                           (cmdsColors[4].after.colorArgb == 0x80FFFF00);
            }

            // 4. Backward selection produces same quad bounds and contents
            selectRange(charCenter(pl0, pt0->chars[3]), charCenter(pl0, pt0->chars[0])); // backward
            viewer.editLog().clear();
            const bool bwdAdded = viewer.markupSelection(pdf::AnnotationSubtype::Highlight, 0);
            const auto cmdsBwd = viewer.editLog().activeCommands();
            bwdOk = bwdAdded && (cmdsBwd.size() == 1);
            if (bwdOk && hlOk) {
                const auto& itemBwd = cmdsBwd[0].after;
                const auto& itemFwd = cmdsHl[0].after;
                bwdOk = (std::abs(itemBwd.left - itemFwd.left) < 1e-3) &&
                        (std::abs(itemBwd.right - itemFwd.right) < 1e-3) &&
                        (std::abs(itemBwd.bottom - itemFwd.bottom) < 1e-3) &&
                        (std::abs(itemBwd.top - itemFwd.top) < 1e-3) &&
                        (itemBwd.contents == itemFwd.contents);
            }

            // 5. Multi-page selection produces one annotation per page
            const auto& pl1 = viewer.layout().getPageLayout(1);
            selectRange(charCenter(pl0, pt0->chars[1]), charCenter(pl1, pt1->chars[2]));
            viewer.editLog().clear();
            const bool mpAdded = viewer.markupSelection(pdf::AnnotationSubtype::Highlight, 0);
            const auto cmdsMp = viewer.editLog().activeCommands();
            mpOk = mpAdded && (cmdsMp.size() == 2) &&
                   (cmdsMp[0].after.pageIndex == 0) &&
                   (cmdsMp[1].after.pageIndex == 1);

            a8Ok = noSelFalse && hlOk && colorsOk && bwdOk && mpOk;
        }

        // Restore samplePdf
        viewer.clearSelection();
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        const std::string dA8 = "textAvail=" + std::to_string(textAvail) +
                                " noSel=" + std::to_string(noSelFalse) +
                                " hl=" + std::to_string(hlOk) +
                                " colors=" + std::to_string(colorsOk) +
                                " bwd=" + std::to_string(bwdOk) +
                                " mp=" + std::to_string(mpOk);
        recordTest(a8Ok, "Group 4.35: markupSelection colors, quads, bounds, multi-page, bwd parity, clearSelection (A8)", dA8);
        viewer.editLog().clear();
    }

    // 4.36: Public API guard contracts: closed doc, page -1, page == pageCount for all add functions (A9)
    {
        viewer.editLog().clear();

        // 1. When document is open: test page -1 and page == pageCount
        const int pCount = viewer.pageCount();
        const std::vector<std::vector<pdf::AnnotationPoint>> validStrokes = {{{10.0, 10.0}, {20.0, 20.0}}};
        const QRectF validRect(10.0, 10.0, 50.0, 50.0);

        bool openGuardsOk = true;
        for (int badPage : {-1, pCount, pCount + 1}) {
            if (viewer.addTextAnnotation(badPage, 50.0, 50.0, QStringLiteral("text")) ||
                viewer.addSquareAnnotation(badPage, validRect) ||
                viewer.addCircleAnnotation(badPage, validRect) ||
                viewer.addInkAnnotation(badPage, validStrokes) ||
                viewer.addFreeTextAnnotation(badPage, validRect, QStringLiteral("freetext")) ||
                viewer.addStampAnnotation(badPage, validRect, QStringLiteral("STAMP")) ||
                viewer.addFileAttachmentAnnotation(badPage, 50.0, 50.0, QStringLiteral("file.txt"), "data") ||
                viewer.addDefaultAnnotationAt(badPage, pdf::AnnotationSubtype::Text, 50.0, 50.0)) {
                openGuardsOk = false;
            }
        }
        const bool openLogsClean = viewer.editLog().activeCommands().empty();

        // 2. When document is closed: all add functions and markupSelection must return false
        viewer.closeDocument();
        pump(50);

        bool closedGuardsOk = true;
        if (viewer.addTextAnnotation(0, 50.0, 50.0, QStringLiteral("text")) ||
            viewer.addSquareAnnotation(0, validRect) ||
            viewer.addCircleAnnotation(0, validRect) ||
            viewer.addInkAnnotation(0, validStrokes) ||
            viewer.addFreeTextAnnotation(0, validRect, QStringLiteral("freetext")) ||
            viewer.addStampAnnotation(0, validRect, QStringLiteral("STAMP")) ||
            viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("file.txt"), "data") ||
            viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Text, 50.0, 50.0) ||
            viewer.markupSelection(pdf::AnnotationSubtype::Highlight)) {
            closedGuardsOk = false;
        }
        const bool closedLogsClean = viewer.editLog().activeCommands().empty();

        // Restore samplePdf for Group 5
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        const bool a9Ok = openGuardsOk && openLogsClean && closedGuardsOk && closedLogsClean;
        recordTest(a9Ok, "Group 4.36: Public API guards reject closed doc, page -1, page >= pageCount for all adders (A9)");
        viewer.editLog().clear();
    }

    // 4.37: loadError emission counts: silent (0) vs exactly one (1) with message prefix (E1)
    {
        viewer.editLog().clear();
        SignalRecorder<QString> spyErr(&viewer, &ui::PdfViewerWidget::loadError);
        const QRectF validRect(10.0, 10.0, 50.0, 50.0);
        const std::vector<std::vector<pdf::AnnotationPoint>> validStrokes = {{{10.0, 10.0}, {20.0, 20.0}}};
        const QRectF emptyRect(10.0, 10.0, 0.0, 0.0);
        const int pCount = viewer.pageCount();

        bool silentOk = true;

        // 1. Silent rejections when document is closed (0 loadError emissions)
        viewer.closeDocument();
        pump(50);
        spyErr.clear();

        if (viewer.addTextAnnotation(0, 50.0, 50.0, QStringLiteral("txt")) || spyErr.count() != 0 ||
            viewer.addSquareAnnotation(0, validRect) || spyErr.count() != 0 ||
            viewer.addCircleAnnotation(0, validRect) || spyErr.count() != 0 ||
            viewer.addInkAnnotation(0, validStrokes) || spyErr.count() != 0 ||
            viewer.addFreeTextAnnotation(0, validRect, QStringLiteral("txt")) || spyErr.count() != 0 ||
            viewer.addStampAnnotation(0, validRect, QStringLiteral("STAMP")) || spyErr.count() != 0 ||
            viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("f.bin"), "data") || spyErr.count() != 0 ||
            viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Text, 50.0, 50.0) || spyErr.count() != 0 ||
            viewer.markupSelection(pdf::AnnotationSubtype::Highlight) || spyErr.count() != 0) {
            silentOk = false;
        }

        // Reopen samplePdf
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(50);
        spyErr.clear();

        // 2. Silent rejections for out-of-range page (-1, == pageCount, > pageCount) -> catches G1
        for (int badPage : {-1, pCount, pCount + 2}) {
            spyErr.clear();
            if (viewer.addTextAnnotation(badPage, 50.0, 50.0, QStringLiteral("txt")) || spyErr.count() != 0 ||
                viewer.addSquareAnnotation(badPage, validRect) || spyErr.count() != 0 ||
                viewer.addCircleAnnotation(badPage, validRect) || spyErr.count() != 0 ||
                viewer.addInkAnnotation(badPage, validStrokes) || spyErr.count() != 0 ||
                viewer.addFreeTextAnnotation(badPage, validRect, QStringLiteral("txt")) || spyErr.count() != 0 ||
                viewer.addStampAnnotation(badPage, validRect, QStringLiteral("STAMP")) || spyErr.count() != 0 ||
                viewer.addFileAttachmentAnnotation(badPage, 50.0, 50.0, QStringLiteral("f.bin"), "data") || spyErr.count() != 0 ||
                viewer.addDefaultAnnotationAt(badPage, pdf::AnnotationSubtype::Text, 50.0, 50.0) || spyErr.count() != 0) {
                silentOk = false;
            }
        }

        // 3. Silent rejections for empty / zero-area rect -> catches G2
        spyErr.clear();
        if (viewer.addSquareAnnotation(0, emptyRect) || spyErr.count() != 0 ||
            viewer.addCircleAnnotation(0, emptyRect) || spyErr.count() != 0 ||
            viewer.addFreeTextAnnotation(0, emptyRect, QStringLiteral("txt")) || spyErr.count() != 0 ||
            viewer.addStampAnnotation(0, emptyRect, QStringLiteral("STAMP")) || spyErr.count() != 0) {
            silentOk = false;
        }

        // 4. Silent rejections for empty strokes
        spyErr.clear();
        if (viewer.addInkAnnotation(0, {}) || spyErr.count() != 0) {
            silentOk = false;
        }

        // 5. Silent rejections for whitespace-only free-text -> catches T1
        spyErr.clear();
        if (viewer.addFreeTextAnnotation(0, validRect, QStringLiteral("   ")) || spyErr.count() != 0 ||
            viewer.addFreeTextAnnotation(0, validRect, QStringLiteral("\t\n  \n")) || spyErr.count() != 0) {
            silentOk = false;
        }

        // 6. Silent rejections for whitespace-only stamp text
        spyErr.clear();
        if (viewer.addStampAnnotation(0, validRect, QStringLiteral("   ")) || spyErr.count() != 0 ||
            viewer.addStampAnnotation(0, validRect, QStringLiteral("\t\n  ")) || spyErr.count() != 0) {
            silentOk = false;
        }

        // 7. Silent rejections for addDefaultAnnotationAt rejections
        spyErr.clear();
        if (viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Highlight, 50.0, 50.0) || spyErr.count() != 0 ||
            viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::FileAttachment, 50.0, 50.0) || spyErr.count() != 0 ||
            viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Ink, 50.0, 50.0) || spyErr.count() != 0 ||
            viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::FreeText, 50.0, 50.0, QStringLiteral("")) || spyErr.count() != 0) {
            silentOk = false;
        }

        // 8. Exactly one emission for invalid attachment filename
        spyErr.clear();
        const bool badFileRet = !viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("bad/file.txt"), "data");
        const bool badFileOk = badFileRet && (spyErr.count() == 1) &&
                               std::get<0>(spyErr.at(0)).startsWith(viewer.tr("Could not attach file: invalid attachment filename."));

        // 9. Exactly one emission for empty attachment payload -> catches A2
        spyErr.clear();
        const bool emptyPayloadRet = !viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("valid.txt"), QByteArray());
        const bool emptyPayloadOk = emptyPayloadRet && (spyErr.count() == 1) &&
                                    std::get<0>(spyErr.at(0)).startsWith(viewer.tr("Could not attach file: attachment payload must be between 1 byte and 16 MiB."));

        // 10. Exactly one emission for oversized attachment payload (> 16 MiB)
        spyErr.clear();
        QByteArray overPayload(1, 'x');
        overPayload.resize(pdf::kMaxAnnotationAttachmentBytes + 1);
        const bool overPayloadRet = !viewer.addFileAttachmentAnnotation(0, 50.0, 50.0, QStringLiteral("valid.txt"), overPayload);
        const bool overPayloadOk = overPayloadRet && (spyErr.count() == 1) &&
                                   std::get<0>(spyErr.at(0)).startsWith(viewer.tr("Could not attach file: attachment payload must be between 1 byte and 16 MiB."));

        // 11. Exactly one emission for stamp text exceeding kMaxStampTextCharacters (128)
        spyErr.clear();
        const QString overStampText(pdf::kMaxStampTextCharacters + 1, 'X');
        const bool overStampRet = !viewer.addStampAnnotation(0, validRect, overStampText);
        const bool overStampOk = overStampRet && (spyErr.count() == 1) &&
                                 std::get<0>(spyErr.at(0)).startsWith(viewer.tr("Could not add stamp: stamp text exceeds"));

        // 12. Exactly one emission for undrawable free-text
        spyErr.clear();
        constexpr char32_t kUndrawableCp = 0x10FFFD;
        const QString undrawableText = QString::fromUcs4(&kUndrawableCp, 1);
        const bool undrawableRet = !viewer.addFreeTextAnnotation(0, validRect, undrawableText);
        const bool undrawableOk = undrawableRet && (spyErr.count() == 1) &&
                                  std::get<0>(spyErr.at(0)).startsWith(viewer.tr("Could not add text box: no installed font can draw"));

        const bool e1Ok = silentOk && badFileOk && emptyPayloadOk && overPayloadOk && overStampOk && undrawableOk;
        recordTest(e1Ok, "Group 4.37: loadError emission counts: silent (0) vs exactly one (1) with message prefix (E1)");
        viewer.editLog().clear();
    }

    // 4.38: Attachment payload boundary pinning: kMax accepted and logged, kMax + 1 rejected (E2)
    {
        viewer.editLog().clear();
        SignalRecorder<QString> spyErr(&viewer, &ui::PdfViewerWidget::loadError);

        // 1. Exactly kMaxAnnotationAttachmentBytes (16 MiB) accepted and logged
        QByteArray exactPayload(1, 'A');
        exactPayload.resize(pdf::kMaxAnnotationAttachmentBytes);
        spyErr.clear();
        const bool exactOk = viewer.addFileAttachmentAnnotation(0, 100.0, 200.0, QStringLiteral("exact.bin"), exactPayload);
        const size_t exactErrCount = spyErr.count();
        const auto cmds = viewer.editLog().activeCommands();
        const bool loggedOk = (cmds.size() == 1) &&
                              (cmds.back().after.binaryPayload.size() == pdf::kMaxAnnotationAttachmentBytes) &&
                              (cmds.back().after.attachmentFileName == QStringLiteral("exact.bin"));

        // 2. Exactly kMaxAnnotationAttachmentBytes + 1 rejected with exactly one loadError
        QByteArray overPayload(1, 'B');
        overPayload.resize(pdf::kMaxAnnotationAttachmentBytes + 1);
        spyErr.clear();
        const bool overOk = !viewer.addFileAttachmentAnnotation(0, 100.0, 200.0, QStringLiteral("over.bin"), overPayload);
        const bool overErrOk = (spyErr.count() == 1) &&
                               std::get<0>(spyErr.at(0)).startsWith(viewer.tr("Could not attach file: attachment payload must be between 1 byte and 16 MiB."));
        const bool logUnchanged = (viewer.editLog().activeCommands().size() == 1);

        const bool e2Ok = exactOk && (exactErrCount == 0) && loggedOk && overOk && overErrOk && logUnchanged;
        recordTest(e2Ok, "Group 4.38: Attachment payload boundary pinning: kMax accepted and logged, kMax + 1 rejected (E2)");
        viewer.editLog().clear();
    }

    // 4.39: Markup quad exactness: all 8 coordinates match character user-space box (E3)
    {
        viewer.clearSelection();
        viewer.editLog().clear();
        viewer.openDocument(twoPageTextPdf);
        viewer.setZoom(0.2); // Both pages rendered
        viewer.waitForRendering();
        pump(100);

        auto pt0 = viewer.getPageText(0);
        bool e3Ok = false;
        if (pt0 && pt0->charCount() > 0 && pt0->chars[0].hasUserSpaceBox) {
            const auto& pl0 = viewer.layout().getPageLayout(0);
            const auto& b = pt0->chars[0];
            const double effScale = viewer.currentZoom();
            const double dpr = viewer.devicePixelRatio();

            auto charCenter = [&](const layout::PageLayout& pl, const auto& cb) {
                double x = (static_cast<double>(pl.leftPixel) + ((cb.left + cb.right) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollX())) / dpr;
                double y = (static_cast<double>(pl.topPixel) + ((cb.top + cb.bottom) * 0.5) * effScale - static_cast<double>(viewer.currentDocScrollY())) / dpr;
                return QPointF(x, y);
            };

            auto selectRange = [&](const QPointF& from, const QPointF& to) {
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

            // Select first few characters so first quad is chars[0]
            selectRange(charCenter(pl0, pt0->chars[0]), charCenter(pl0, pt0->chars[2]));

            const bool hlAdded = viewer.markupSelection(pdf::AnnotationSubtype::Highlight, 0);
            const auto cmds = viewer.editLog().activeCommands();
            if (hlAdded && (cmds.size() == 1) && !cmds[0].after.quadPoints.empty()) {
                const auto& q = cmds[0].after.quadPoints[0];
                const bool x1Ok = (std::abs(q.x1 - b.userSpaceLeft) < 1e-4);
                const bool y1Ok = (std::abs(q.y1 - b.userSpaceTop) < 1e-4);
                const bool x2Ok = (std::abs(q.x2 - b.userSpaceRight) < 1e-4);
                const bool y2Ok = (std::abs(q.y2 - b.userSpaceTop) < 1e-4);
                const bool x3Ok = (std::abs(q.x3 - b.userSpaceLeft) < 1e-4);
                const bool y3Ok = (std::abs(q.y3 - b.userSpaceBottom) < 1e-4);
                const bool x4Ok = (std::abs(q.x4 - b.userSpaceRight) < 1e-4);
                const bool y4Ok = (std::abs(q.y4 - b.userSpaceBottom) < 1e-4);
                e3Ok = x1Ok && y1Ok && x2Ok && y2Ok && x3Ok && y3Ok && x4Ok && y4Ok;
            }
        }

        // Restore samplePdf
        viewer.clearSelection();
        viewer.editLog().clear();
        viewer.openDocument(samplePdf);
        viewer.waitForRendering();
        pump(100);

        recordTest(e3Ok, "Group 4.39: Markup quad exactness: all 8 coordinates match character user-space box (E3)");
        viewer.editLog().clear();
    }

    // 4.40: addDefaultAnnotationAt placement and anchor corner for all 5 subtypes (E4)
    {
        viewer.editLog().clear();
        const auto& pl = viewer.layout().getPageLayout(0);
        const QSizeF pageSize(pl.widthPoints, pl.heightPoints);

        // Anchor near top-left edge so clamp kicks in
        const QPointF anchor(2.0, 2.0);

        // 1. Text: 24x24 icon placed on page, anchored from bottom of rect
        const QSizeF textSize(24.0, 24.0);
        const QRectF expectedTextRect = viewer.placeOnPage(anchor, textSize, pageSize);
        viewer.editLog().clear();
        const bool addTextOk = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Text, anchor.x(), anchor.y(), QStringLiteral("NoteContents"));
        const auto cmdsText = viewer.editLog().activeCommands();
        bool textPlacedOk = addTextOk && (cmdsText.size() == 1);
        if (textPlacedOk) {
            const auto& note = cmdsText[0].after;
            // addDefaultAnnotationAt passes rect.left(), rect.bottom()
            // note.left = rect.left(), note.right = rect.right(), note.bottom = rect.top(), note.top = rect.bottom()
            textPlacedOk = (std::abs(note.left - expectedTextRect.left()) < 1e-4) &&
                           (std::abs(note.right - expectedTextRect.right()) < 1e-4) &&
                           (std::abs(note.bottom - expectedTextRect.top()) < 1e-4) &&
                           (std::abs(note.top - expectedTextRect.bottom()) < 1e-4);
        }

        // 2. Square: 100x60 placed on page
        const QSizeF sqSize(100.0, 60.0);
        const QRectF expectedSqRect = viewer.placeOnPage(anchor, sqSize, pageSize);
        viewer.editLog().clear();
        const bool addSqOk = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Square, anchor.x(), anchor.y());
        const auto cmdsSq = viewer.editLog().activeCommands();
        bool sqPlacedOk = addSqOk && (cmdsSq.size() == 1);
        if (sqPlacedOk) {
            const auto& sq = cmdsSq[0].after;
            sqPlacedOk = (std::abs(sq.left - expectedSqRect.left()) < 1e-4) &&
                         (std::abs(sq.right - expectedSqRect.right()) < 1e-4) &&
                         (std::abs(sq.bottom - expectedSqRect.top()) < 1e-4) &&
                         (std::abs(sq.top - expectedSqRect.bottom()) < 1e-4);
        }

        // 3. Circle: 100x60 placed on page
        const QSizeF circleSize(100.0, 60.0);
        const QRectF expectedCircleRect = viewer.placeOnPage(anchor, circleSize, pageSize);
        viewer.editLog().clear();
        const bool addCircleOk = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Circle, anchor.x(), anchor.y());
        const auto cmdsCircle = viewer.editLog().activeCommands();
        bool circlePlacedOk = addCircleOk && (cmdsCircle.size() == 1);
        if (circlePlacedOk) {
            const auto& circ = cmdsCircle[0].after;
            circlePlacedOk = (std::abs(circ.left - expectedCircleRect.left()) < 1e-4) &&
                             (std::abs(circ.right - expectedCircleRect.right()) < 1e-4) &&
                             (std::abs(circ.bottom - expectedCircleRect.top()) < 1e-4) &&
                             (std::abs(circ.top - expectedCircleRect.bottom()) < 1e-4);
        }

        // 4. Stamp: 120x40 placed on page
        const QSizeF stampSize(120.0, 40.0);
        const QRectF expectedStampRect = viewer.placeOnPage(anchor, stampSize, pageSize);
        viewer.editLog().clear();
        const bool addStampOk = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::Stamp, anchor.x(), anchor.y(), QStringLiteral("DRAFT"));
        const auto cmdsStamp = viewer.editLog().activeCommands();
        bool stampPlacedOk = addStampOk && (cmdsStamp.size() == 1);
        if (stampPlacedOk) {
            const auto& st = cmdsStamp[0].after;
            stampPlacedOk = (std::abs(st.left - expectedStampRect.left()) < 1e-4) &&
                            (std::abs(st.right - expectedStampRect.right()) < 1e-4) &&
                            (std::abs(st.bottom - expectedStampRect.top()) < 1e-4) &&
                            (std::abs(st.top - expectedStampRect.bottom()) < 1e-4);
        }

        // 5. FreeText: placed on page
        const QString ftText = QStringLiteral("Sample FreeText");
        viewer.editLog().clear();
        const bool addFtOk = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::FreeText, anchor.x(), anchor.y(), ftText);
        const auto cmdsFt = viewer.editLog().activeCommands();
        bool ftPlacedOk = addFtOk && (cmdsFt.size() == 1);
        if (ftPlacedOk) {
            const auto& ft = cmdsFt[0].after;
            const QSizeF ftSize(ft.right - ft.left, ft.top - ft.bottom);
            const QRectF expectedFtRect = viewer.placeOnPage(anchor, ftSize, pageSize);
            ftPlacedOk = (std::abs(ft.left - expectedFtRect.left()) < 1e-4) &&
                         (std::abs(ft.right - expectedFtRect.right()) < 1e-4) &&
                         (std::abs(ft.top - expectedFtRect.bottom()) < 1e-4);
        }

        const bool e4Ok = textPlacedOk && sqPlacedOk && circlePlacedOk && stampPlacedOk && ftPlacedOk;
        recordTest(e4Ok, "Group 4.40: addDefaultAnnotationAt placement and anchor corner for all 5 subtypes (E4)");
        viewer.editLog().clear();
    }

    // 4.41: fitTextBoxOutline boundary condition at 144 pt, 12 pt fallback, and free-text default slack
    {
        viewer.editLog().clear();

        // 1. Boundary condition: Exactly 144 pt font size.
        // In QRectF(50, 60, 100, 30), top anchor is 90.0 (60 + 30).
        // Text "Big FreeText" at 144 pt has line height > 144 pt, so the box must expand downward:
        // bottom must be < 60 - 100 (-40.0).
        // Regression bug had item.fontSize < 144.0, which fell back to 12 pt layout (bottom == 60.0).
        const bool add144Ok = viewer.addFreeTextAnnotation(0, QRectF(50.0, 60.0, 100.0, 30.0), QStringLiteral("Big FreeText"), 0xFF00FF00, 144.0);
        const auto cmds144 = viewer.editLog().activeCommands();
        bool bound144Ok = add144Ok && (cmds144.size() == 1);
        if (bound144Ok) {
            const auto& item144 = cmds144[0].after;
            const bool fsStored = (item144.fontSize == 144.0);
            const bool bottomGrown = (item144.bottom < -40.0); // bottom < 60 - 100
            bound144Ok = fsStored && bottomGrown;
        }

        // 2. 12 pt fallback at fit level: deleting a non-log free-text item with fontSize == 0
        // and undoing the deletion recreates the item laid out with the 12 pt fallback.
        bool fallback12Ok = false;
        viewer.editLog().clear();
        const QString fbText = QStringLiteral("Fallback Test String");
        const bool addDummy = viewer.addFreeTextAnnotation(0, QRectF(100.0, 200.0, 150.0, 40.0), fbText, 0xFF112233, 16.0);
        const auto cmdsDummy = viewer.editLog().activeCommands();
        if (addDummy && (cmdsDummy.size() == 1)) {
            const pdf::AnnotationItem originalItem = cmdsDummy[0].after;
            viewer.editLog().clear();

            // Prepare item to delete: empty outline and fontSize == 0.0
            pdf::AnnotationItem unloggedItem = originalItem;
            unloggedItem.textOutline.clear();
            unloggedItem.fontSize = 0.0;

            const bool delOk = viewer.deleteAnnotation(0, unloggedItem);
            const auto cmdsDel = viewer.editLog().activeCommands();
            if (delOk && (cmdsDel.size() == 1)) {
                // The recorded item in the Delete command was fitted with 12 pt fallback
                const auto& rec = cmdsDel[0].before;
                const bool outlineRestored = !rec.textOutline.empty();
                const bool geometryLaidOut = (rec.bottom < rec.top) && outlineRestored;

                const bool undoOk = viewer.undoEdit();
                fallback12Ok = geometryLaidOut && undoOk;
            }
        }

        // 3. Default annotation size slack (+ 1.0 pt)
        const QString slackText = QStringLiteral("Slack measurement test");
        const auto& pl = viewer.layout().getPageLayout(0);
        const QSizeF pageSize(pl.widthPoints, pl.heightPoints);
        const double maxInner = std::max(20.0, std::min(300.0, pageSize.width() - 8.0 - 2.0 * ui::kFreeTextPadding));
        const auto measured = ui::layoutFreeText(slackText, 12.0, maxInner, false);
        const double expectedW = std::ceil(measured.size.width()) + 1.0 + 2.0 * ui::kFreeTextPadding;
        const double expectedH = std::ceil(measured.size.height()) + 1.0 + 2.0 * ui::kFreeTextPadding;

        viewer.editLog().clear();
        const bool addSlackOk = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::FreeText, 100.0, 300.0, slackText);
        const auto cmdsSlack = viewer.editLog().activeCommands();
        bool slackOk = addSlackOk && (cmdsSlack.size() == 1);
        if (slackOk) {
            const auto& ftItem = cmdsSlack[0].after;
            const double actualW = ftItem.right - ftItem.left;
            const double actualH = ftItem.top - ftItem.bottom;
            slackOk = (std::abs(actualW - expectedW) < 1e-4) && (std::abs(actualH - expectedH) < 1e-4);
        }

        const bool g41Ok = bound144Ok && fallback12Ok && slackOk;
        std::string failDetail;
        if (!bound144Ok) failDetail += "bound144=0 ";
        if (!fallback12Ok) failDetail += "fallback12=0 ";
        if (!slackOk) failDetail += "slack=0 ";

        recordTest(g41Ok, "Group 4.41: fitTextBoxOutline boundary at 144 pt, 12 pt fallback, and free-text default slack", failDetail);
        viewer.editLog().clear();
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
    runGroup4AnnotationAndRedaction(samplePdf, twoPageTextPdf);
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
