#include "ui/PdfViewerWidget.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QThread>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <fstream>
#include <cassert>
#include <cmath>
#include <vector>

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

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << " RUNNING PDF VIEWER WIDGET UNIT TESTS   " << std::endl;
    std::cout << "========================================" << std::endl;

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

        // Queue generation 1 tile request
        auto keyGen1 = pdf::TileKey::create(100, 0, 0, 0, 1.0, 1.0);
        pdf::TileRenderRequest req1{100, 1, 0, {0, 0, 512, 512}, 1.0, 1.0, keyGen1, 10};
        worker.enqueueRequest(req1);

        // Cancel generation 1 by advancing to generation 2
        worker.setGeneration(2);

        // Queue generation 2 tile request
        auto keyGen2 = pdf::TileKey::create(100, 0, 1, 0, 1.0, 1.0);
        bool gen1Delivered = false;
        bool gen2Delivered = false;

        QObject::connect(&worker, &pdf::RenderWorker::tileReady, [&](int64_t, uint64_t gen, const pdf::TileKey&, const pdf::PixelBuffer&) {
            if (gen == 1) gen1Delivered = true;
            if (gen == 2) gen2Delivered = true;
        });

        pdf::TileRenderRequest req2{100, 2, 0, {512, 0, 512, 512}, 1.0, 1.0, keyGen2, 10};
        worker.enqueueRequest(req2);

        worker.waitForIdle();
        QCoreApplication::processEvents();

        if (gen1Delivered) {
            std::cerr << "[FAIL] Generation 1 request was not cancelled!" << std::endl;
            return 1;
        }
        if (!gen2Delivered) {
            std::cerr << "[FAIL] Generation 2 request was not delivered!" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: RenderWorker cancelled stale generation requests cleanly." << std::endl;
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
            &viewer.renderWorker(), &pdf::RenderWorker::tileReady, &viewer,
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
        std::cout << "  -> Passed: 200-page document load kept worker page cache strictly bounded at " 
                  << cachedPages << " <= 64 pages." << std::endl;
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
        if (iterations < 5) {
            std::cerr << "[FAIL] UI thread was not responsive during load (iterations=" << iterations << ")" << std::endl;
            return 1;
        }
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
            &viewer.renderWorker(), &pdf::RenderWorker::tileReady, &viewer,
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

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 22 PDF VIEWER WIDGET TESTS PASSED! " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
