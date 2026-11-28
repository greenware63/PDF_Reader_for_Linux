#include "TestIsolation.h"
#include "ui/MainWindow.h"
#include "ui/PageOrganizePlan.h"
#include "ui/OrganizePagesDialog.h"
#include "ipc/WorkerClient.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <sys/stat.h>

namespace {

bool waitForLoaded(ui::PdfViewerWidget* viewer, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (viewer->isDocumentOpen() && viewer->pageCount() > 0) {
            return true;
        }
    }
    return false;
}

QByteArray fileSha256(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

int testPlanModel() {
    std::cout << "[1] Testing PageOrganizePlan model & invariants..." << std::endl;
    ui::PageOrganizePlan plan(4);
    if (plan.count() != 4 || !plan.isUnchanged() || plan.empty()) {
        std::cerr << "[FAIL] Initial PageOrganizePlan state wrong" << std::endl;
        return 1;
    }

    // Rotate CW and CCW (mod 360)
    if (!plan.rotateClockwise(0) || plan.pageSpecs()[0].rotationDegrees != 90 ||
        !plan.rotateClockwise(0) || plan.pageSpecs()[0].rotationDegrees != 180 ||
        !plan.rotateCounterClockwise(0) || plan.pageSpecs()[0].rotationDegrees != 90 ||
        !plan.rotateCounterClockwise(0) || plan.pageSpecs()[0].rotationDegrees != 0 ||
        !plan.rotateCounterClockwise(0) || plan.pageSpecs()[0].rotationDegrees != 270) {
        std::cerr << "[FAIL] Rotation CW/CCW arithmetic failed" << std::endl;
        return 1;
    }
    if (plan.isUnchanged()) {
        std::cerr << "[FAIL] Plan should not report unchanged after rotation" << std::endl;
        return 1;
    }

    // Move
    if (!plan.move(2, 0) || plan.pageSpecs()[0].sourceIndex != 2) {
        std::cerr << "[FAIL] move(2, 0) failed" << std::endl;
        return 1;
    }

    // Delete & restore
    if (!plan.deleteAt(1) || plan.count() != 3 || plan.containsSourcePage(0)) {
        std::cerr << "[FAIL] deleteAt(1) failed" << std::endl;
        return 1;
    }
    if (!plan.restore(0) || plan.count() != 4 || !plan.containsSourcePage(0)) {
        std::cerr << "[FAIL] restore(0) failed" << std::endl;
        return 1;
    }

    // Never-empty invariant
    if (plan.deletePositions({0, 1, 2, 3}) || plan.count() != 4) {
        std::cerr << "[FAIL] deletePositions allowed emptying the plan" << std::endl;
        return 1;
    }
    if (!plan.deletePositions({1, 2, 3}) || plan.count() != 1) {
        std::cerr << "[FAIL] deletePositions(1,2,3) failed" << std::endl;
        return 1;
    }
    if (plan.deleteAt(0) || plan.count() != 1) {
        std::cerr << "[FAIL] deleteAt(0) allowed emptying the last page" << std::endl;
        return 1;
    }

    // Reset
    plan.reset();
    if (!plan.isUnchanged() || plan.count() != 4) {
        std::cerr << "[FAIL] reset() did not restore initial state" << std::endl;
        return 1;
    }

    // Combined edits PageSpec output
    plan.deleteAt(1);          // [0, 2, 3]
    plan.move(1, 0);           // [2, 0, 3]
    plan.rotateClockwise(0);   // 2 -> 90
    plan.rotateCounterClockwise(2); // 3 -> 270
    const auto& specs = plan.pageSpecs();
    if (specs.size() != 3 ||
        specs[0].sourceIndex != 2 || specs[0].rotationDegrees != 90 ||
        specs[1].sourceIndex != 0 || specs[1].rotationDegrees != 0 ||
        specs[2].sourceIndex != 3 || specs[2].rotationDegrees != 270) {
        std::cerr << "[FAIL] Combined edits produced wrong PageSpec output" << std::endl;
        return 1;
    }

    // Reject duplicate / bad permutation in reorderBySourceIndices
    if (plan.reorderBySourceIndices({2, 2, 3}) || plan.reorderBySourceIndices({2, 0})) {
        std::cerr << "[FAIL] reorderBySourceIndices accepted invalid permutation" << std::endl;
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const testsupport::IsolatedUserSettings isolation;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    if (testPlanModel() != 0) {
        return 1;
    }

    const QString samplePdf = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("sample.pdf");
    auto resolveFixture = [&](const QString& rel) -> QString {
        QString p = QFileInfo(samplePdf).dir().filePath(QStringLiteral("tests/fixtures/") + rel);
        if (!QFileInfo::exists(p)) {
            p = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../tests/fixtures/") + rel);
        }
        return p;
    };

    const QString richPdf = resolveFixture(QStringLiteral("page_org_rich.pdf"));
    const QString r2DeniedPdf = resolveFixture(QStringLiteral("page_org_perm_r2_denied.pdf"));
    const QString r4AllowedPdf = resolveFixture(QStringLiteral("page_org_perm_r4_allowed.pdf"));
    const QString signedPdf = resolveFixture(QStringLiteral("redaction/signed.pdf"));

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "[FAIL] Could not create tempDir" << std::endl;
        return 1;
    }

    std::cout << "[2] Testing MainWindow::organizeDocumentToFile end-to-end (reorder/delete/rotate + commit safety)..." << std::endl;
    {
        ui::MainWindow win;
        win.openFile(richPdf);
        auto* viewer = win.currentViewer();
        if (!waitForLoaded(viewer) || viewer->pageCount() < 3) {
            std::cerr << "[FAIL] Failed to open page_org_rich.pdf in MainWindow" << std::endl;
            return 1;
        }
        const QByteArray srcHash = fileSha256(richPdf);
        const auto srcSize0 = viewer->layout().getPageLayout(0);
        const auto srcSize2 = viewer->layout().getPageLayout(2);

        // Pre-create destination file to verify atomic commit replaces inode (never QFile::rename)
        const QString outPath = tempDir.filePath(QStringLiteral("organized.pdf"));
        {
            QFile placeholder(outPath);
            if (!placeholder.open(QIODevice::WriteOnly)) return 1;
            placeholder.write("placeholder");
        }
        struct stat beforeStat{};
        ::stat(outPath.toLocal8Bit().constData(), &beforeStat);

        const std::vector<pdf::PageSpec> specs = {{2, 90}, {0, 180}};
        ui::MainWindow::OrganizeOutcome outcome;
        if (!win.organizeDocumentToFile(outPath, specs, {}, &outcome) ||
            outcome.status != pdf::OrganizeStatus::Ok || outcome.bytesWritten == 0) {
            std::cerr << "[FAIL] organizeDocumentToFile failed: " << outcome.errorMessage.toStdString() << std::endl;
            return 1;
        }

        // Verify source untouched, inode changed, and no temp files left behind
        if (fileSha256(richPdf) != srcHash) {
            std::cerr << "[FAIL] Source file was modified" << std::endl;
            return 1;
        }
        struct stat afterStat{};
        ::stat(outPath.toLocal8Bit().constData(), &afterStat);
        if (afterStat.st_ino == beforeStat.st_ino) {
            std::cerr << "[FAIL] Output file inode was not replaced by atomic commit" << std::endl;
            return 1;
        }
        const QStringList leftover = QDir::temp().entryList({QStringLiteral("pdf_org_*")}, QDir::Files);
        if (!leftover.isEmpty()) {
            std::cerr << "[FAIL] Leftover pdf_org_* temp file(s): " << leftover.join(',').toStdString() << std::endl;
            return 1;
        }

        // Reload committed file in fresh worker and verify pages + rotated sizes
        pdf::ipc::WorkerClient verifyClient;
        const auto vOpen = verifyClient.openDocumentSync(1, outPath.toStdString());
        verifyClient.closeDocumentSync();
        verifyClient.stop();
        if (vOpen.error != pdf::DocumentError::None || vOpen.pageCount != 2 || vOpen.pageSizes.size() != 2) {
            std::cerr << "[FAIL] Reloaded organized PDF invalid (pages=" << vOpen.pageCount << ")" << std::endl;
            return 1;
        }
        if (std::abs(vOpen.pageSizes[0].first - srcSize2.heightPoints) >= 0.5 ||
            std::abs(vOpen.pageSizes[0].second - srcSize2.widthPoints) >= 0.5) {
            std::cerr << "[FAIL] Page 0 rotated 90 deg dimensions mismatch" << std::endl;
            return 1;
        }
        if (std::abs(vOpen.pageSizes[1].first - srcSize0.widthPoints) >= 0.5 ||
            std::abs(vOpen.pageSizes[1].second - srcSize0.heightPoints) >= 0.5) {
            std::cerr << "[FAIL] Page 1 rotated 180 deg dimensions mismatch" << std::endl;
            return 1;
        }

        // InvalidSpec through seam
        ui::MainWindow::OrganizeOutcome badSpecOut;
        if (win.organizeDocumentToFile(tempDir.filePath(QStringLiteral("badspec.pdf")), {{-1, 0}}, {}, &badSpecOut) ||
            badSpecOut.status != pdf::OrganizeStatus::InvalidSpec) {
            std::cerr << "[FAIL] InvalidSpec was not returned for negative sourceIndex" << std::endl;
            return 1;
        }

        // Dirty edit log refusal
        viewer->addSquareAnnotation(0, QRectF(72.0, 680.0, 68.0, 40.0));
        QCoreApplication::processEvents();
        if (!viewer->editLog().isDirty()) {
            std::cerr << "[FAIL] Expected dirty edit log after addAnnotation" << std::endl;
            return 1;
        }
        ui::MainWindow::OrganizeOutcome dirtyOut;
        if (win.organizeDocumentToFile(tempDir.filePath(QStringLiteral("dirty.pdf")), specs, {}, &dirtyOut) ||
            !dirtyOut.dirtyEditLogRefused) {
            std::cerr << "[FAIL] Expected dirtyEditLogRefused=true when edit log is dirty" << std::endl;
            return 1;
        }
        viewer->undoEdit();
        QCoreApplication::processEvents();
        viewer->closeDocument();
    }

    std::cout << "[3] Testing refusal statuses (PermissionDenied, SignedNotAcknowledged, EncryptedNotAcknowledged + retry)..." << std::endl;
    {
        // R2 permission denied
        ui::MainWindow win;
        win.currentViewer()->openDocument(r2DeniedPdf, QStringLiteral("userpw"));
        if (!waitForLoaded(win.currentViewer())) {
            std::cerr << "[FAIL] Failed to open r2DeniedPdf" << std::endl;
            return 1;
        }
        ui::MainWindow::OrganizeOutcome permOut;
        if (win.organizeDocumentToFile(tempDir.filePath(QStringLiteral("perm.pdf")), {{0, 0}}, {}, &permOut) ||
            permOut.status != pdf::OrganizeStatus::PermissionDenied) {
            std::cerr << "[FAIL] Expected PermissionDenied, got " << static_cast<int>(permOut.status) << std::endl;
            return 1;
        }
        win.currentViewer()->closeDocument();
    }
    {
        // Signed -> SignedNotAcknowledged -> retry with allowInvalidateSignatures
        ui::MainWindow win;
        win.openFile(signedPdf);
        if (!waitForLoaded(win.currentViewer())) {
            std::cerr << "[FAIL] Failed to open signedPdf" << std::endl;
            return 1;
        }
        const QString signedOutPath = tempDir.filePath(QStringLiteral("signed_org.pdf"));
        ui::MainWindow::OrganizeOutcome signedOut;
        if (win.organizeDocumentToFile(signedOutPath, {{0, 0}}, {}, &signedOut) ||
            signedOut.status != pdf::OrganizeStatus::SignedNotAcknowledged) {
            std::cerr << "[FAIL] Expected SignedNotAcknowledged, got " << static_cast<int>(signedOut.status) << std::endl;
            return 1;
        }
        pdf::OrganizeOptions ackOpts;
        ackOpts.allowInvalidateSignatures = true;
        if (!win.organizeDocumentToFile(signedOutPath, {{0, 0}}, ackOpts, &signedOut) ||
            signedOut.status != pdf::OrganizeStatus::Ok) {
            std::cerr << "[FAIL] Signed consent retry failed" << std::endl;
            return 1;
        }
        win.currentViewer()->closeDocument();
    }
    {
        // Encrypted -> EncryptedNotAcknowledged -> retry with allowDecrypt
        ui::MainWindow win;
        win.currentViewer()->openDocument(r4AllowedPdf, QStringLiteral("userpw"));
        if (!waitForLoaded(win.currentViewer())) {
            std::cerr << "[FAIL] Failed to open r4AllowedPdf" << std::endl;
            return 1;
        }
        const QString encOutPath = tempDir.filePath(QStringLiteral("enc_org.pdf"));
        ui::MainWindow::OrganizeOutcome encOut;
        if (win.organizeDocumentToFile(encOutPath, {{0, 0}}, {}, &encOut) ||
            encOut.status != pdf::OrganizeStatus::EncryptedNotAcknowledged) {
            std::cerr << "[FAIL] Expected EncryptedNotAcknowledged, got " << static_cast<int>(encOut.status) << std::endl;
            return 1;
        }
        pdf::OrganizeOptions decOpts;
        decOpts.allowDecrypt = true;
        if (!win.organizeDocumentToFile(encOutPath, {{0, 0}}, decOpts, &encOut) ||
            encOut.status != pdf::OrganizeStatus::Ok || !encOut.catalogEntriesDropped) {
            std::cerr << "[FAIL] Encrypted consent retry failed or catalogEntriesDropped=false" << std::endl;
            return 1;
        }
        win.currentViewer()->closeDocument();
    }

    std::cout << "[4] Testing busy guard (closeTab and closeEvent refused while busy)..." << std::endl;
    {
        // Generate a 10,000-page PDF so organizeDocumentToFile takes >50ms while nested event loop runs
        const QString bigPdf = tempDir.filePath(QStringLiteral("busy_10k.pdf"));
        {
            QByteArray pdfBytes("%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
            QList<qint64> offsets;
            offsets << 0 << 9;
            constexpr int kPages = 10000;
            QByteArray kids;
            for (int i = 0; i < kPages; ++i) kids += QByteArray::number(3 + i) + " 0 R ";
            offsets << pdfBytes.size();
            pdfBytes += "2 0 obj\n<< /Type /Pages /Count " + QByteArray::number(kPages) + " /Kids [ " + kids + "] >>\nendobj\n";
            for (int i = 0; i < kPages; ++i) {
                offsets << pdfBytes.size();
                pdfBytes += QByteArray::number(3 + i) + " 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>\nendobj\n";
            }
            const qint64 xrefPos = pdfBytes.size();
            pdfBytes += "xref\n0 " + QByteArray::number(3 + kPages) + "\n0000000000 65535 f \n";
            for (int i = 1; i < offsets.size(); ++i) pdfBytes += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
            pdfBytes += "trailer\n<< /Size " + QByteArray::number(3 + kPages) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefPos) + "\n%%EOF\n";
            QFile f(bigPdf);
            if (!f.open(QIODevice::WriteOnly)) return 1;
            f.write(pdfBytes);
        }

        ui::MainWindow win;
        win.openFile(bigPdf);
        if (!waitForLoaded(win.currentViewer())) {
            std::cerr << "[FAIL] Failed to open busy_10k.pdf" << std::endl;
            return 1;
        }
        std::vector<pdf::PageSpec> specs;
        specs.reserve(9999);
        for (int i = 1; i < 10000; ++i) specs.push_back({i, 90});

        bool checkedWhileBusy = false;
        bool closeRefused = false;
        bool tabCloseRefused = false;
        QTimer::singleShot(10, &win, [&]() {
            if (win.isWorkerBusy()) {
                checkedWhileBusy = true;
                const int beforeTabs = win.documentTabs()->count();
                win.closeTab(0);
                tabCloseRefused = (win.documentTabs()->count() == beforeTabs) && win.currentViewer()->isDocumentOpen();
                QCloseEvent ev;
                QCoreApplication::sendEvent(&win, &ev);
                closeRefused = !ev.isAccepted();
            }
        });

        ui::MainWindow::OrganizeOutcome busyOut;
        if (!win.organizeDocumentToFile(tempDir.filePath(QStringLiteral("busy_out.pdf")), specs, {}, &busyOut)) {
            std::cerr << "[FAIL] busy_10k organizeDocumentToFile failed: " << busyOut.errorMessage.toStdString() << std::endl;
            return 1;
        }
        if (!checkedWhileBusy || !closeRefused || !tabCloseRefused) {
            std::cerr << "[FAIL] Busy guard check failed (checked=" << checkedWhileBusy
                      << ", closeRefused=" << closeRefused
                      << ", tabCloseRefused=" << tabCloseRefused << ")" << std::endl;
            return 1;
        }
        win.currentViewer()->closeDocument();
    }

    std::cout << "[5] Testing validateOrganizedOutput rejects truncated/corrupt/wrong-page-count files..." << std::endl;
    {
        const QString badPdf = tempDir.filePath(QStringLiteral("corrupt.pdf"));
        {
            QFile f(badPdf);
            if (!f.open(QIODevice::WriteOnly)) return 1;
            f.write("%PDF-1.4\ngarbage\n%%EOF\n");
        }
        QByteArray bytes;
        QString err;
        if (ui::MainWindow::validateOrganizedOutput(badPdf, static_cast<uint64_t>(QFileInfo(badPdf).size()), 1, bytes, &err)) {
            std::cerr << "[FAIL] validateOrganizedOutput accepted corrupt PDF" << std::endl;
            return 1;
        }
    }

    std::cout << "ALL ORGANIZE PAGES TESTS PASSED!" << std::endl;
    return 0;
}
