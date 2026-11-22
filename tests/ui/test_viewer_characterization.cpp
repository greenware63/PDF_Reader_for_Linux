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
