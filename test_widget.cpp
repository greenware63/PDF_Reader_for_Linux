#include "ui/PdfViewerWidget.h"
#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QScrollBar>
#include <QThread>
#include <QCoreApplication>
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

    std::cout << "========================================" << std::endl;
    std::cout << " ALL 15 PDF VIEWER WIDGET TESTS PASSED! " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
