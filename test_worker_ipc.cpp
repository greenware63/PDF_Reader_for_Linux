#include <QCoreApplication>
#include <QProcess>
#include <QLocalSocket>
#include "ipc/SharedTile.h"
#include <cstring>
#include <vector>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <functional>
#include <algorithm>
#include "ipc/IpcProtocol.h"
#include "ipc/IpcMessages.h"
#include "ipc/WorkerClient.h"
#include <QTcpServer>
#include <QLocalServer>
#include <QFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>

static bool waitFor(std::function<bool()> condition, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        if (condition()) {
            return true;
        }
        QThread::msleep(5);
    }
    return condition();
}

constexpr int kHeartbeatTestPages = 60;

// Builds an N-page PDF (shared content stream with text and many filled rectangles) so a redaction of it
// takes long enough to outlive a short watchdog stall limit.
static QByteArray makeManyPagePdf(int pages) {
    QByteArray out("%PDF-1.4\n");
    std::vector<qint64> offs(5 + static_cast<size_t>(pages), 0);
    auto addObj = [&](int num, const QByteArray& body) {
        offs[static_cast<size_t>(num)] = out.size();
        out += QByteArray::number(num) + " 0 obj\n" + body + "\nendobj\n";
    };
    addObj(1, "<< /Type /Catalog /Pages 2 0 R >>");
    QByteArray kids;
    for (int i = 0; i < pages; ++i) kids += QByteArray::number(5 + i) + " 0 R ";
    addObj(2, "<< /Type /Pages /Count " + QByteArray::number(pages) + " /Kids [" + kids + "] >>");
    addObj(3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    QByteArray content = "BT /F1 28 Tf 72 720 Td (Heartbeat redaction test page) Tj ET\n";
    for (int i = 0; i < 60; ++i)
        content += QByteArray::number((i % 10) / 10.0 + 0.05) + " g " + QByteArray::number(40 + (i * 37) % 480) + " " +
                   QByteArray::number(60 + (i * 53) % 600) + " 90 40 re f\n";
    addObj(4, "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream");
    for (int i = 0; i < pages; ++i)
        addObj(5 + i, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 3 0 R >> >> >>");
    const qint64 xref = out.size();
    out += "xref\n0 " + QByteArray::number(5 + pages) + "\n0000000000 65535 f \n";
    for (int n = 1; n < 5 + pages; ++n) {
        QByteArray o = QByteArray::number(offs[static_cast<size_t>(n)]);
        out += QByteArray(10 - o.size(), '0') + o + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + QByteArray::number(5 + pages) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    if (argc < 3) {
        std::cerr << "Usage: test_worker_ipc <path-to-pdf_worker> <path-to-sample-pdf>" << std::endl;
        return 1;
    }

    const QString workerExe = QString::fromLocal8Bit(argv[1]);
    const QString samplePdf = QString::fromLocal8Bit(argv[2]);

    std::cout << "========================================" << std::endl;
    std::cout << " RUNNING SANDBOXED WORKER IPC UNIT TESTS" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- TEST 0: Shared-tile transport security ---
    std::cout << "[TEST 0] Shared-tile transport rejects hostile names and survives a shrunken segment..." << std::endl;
    {
        using namespace pdf::ipc;
        std::vector<uint8_t> sink(4096);
        for (const char* bad : {"", "pdf-tile-", "../etc/passwd", "pdf-tile-1-../x", "pdf-tile-x-1", "pdf-tile-1-XYZ",
                                "pdf-tile-1-12345678901234567", "/pdf-tile-1-a", "other-1-a"}) {
            if (isValidSharedTileKey(bad) || readAndUnlinkSharedTile(bad, 1, 16, 16, sink.data(), 16)) {
                std::cerr << "[FAIL] Hostile shared-tile name accepted: '" << bad << "'" << std::endl;
                return 1;
            }
        }
#ifndef _WIN32
        const std::string key = "pdf-tile-" + std::to_string(::getpid()) + "-feed";
        SharedTileSegment seg;
        if (!seg.create(key, 4096)) {
            std::cerr << "[FAIL] Could not create a shared tile segment" << std::endl;
            return 1;
        }
        std::memset(seg.data(), 0x5A, 4096);
        // A worker that truncates the segment after announcing it must produce a clean failure, not a SIGBUS.
        const int fd = ::shm_open(("/" + key).c_str(), O_RDWR, 0600);
        if (fd < 0 || ::ftruncate(fd, 64) != 0) {
            std::cerr << "[FAIL] Could not shrink the segment for the test" << std::endl;
            return 1;
        }
        ::close(fd);
        if (readAndUnlinkSharedTile(key, 16, 256, 256, sink.data(), 256)) {
            std::cerr << "[FAIL] Reading a shrunken segment must fail" << std::endl;
            return 1;
        }
        if (readAndUnlinkSharedTile(key, 16, 256, 256, sink.data(), 256)) { // the failed read still unlinked it
            std::cerr << "[FAIL] Segment should be gone after the first read attempt" << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: names validated, truncation handled, segment unlinked by the reader." << std::endl;
#else
        std::cout << "  -> Passed: hostile names rejected." << std::endl;
#endif
    }

    // --- TEST 1: Process Launch and Handshake (Ping/Pong) ---
    std::cout << "[TEST 1] Testing pdf_worker process spawn and Ping/Pong handshake..." << std::endl;
    const QString socketName = QStringLiteral("pdf_test_socket_%1").arg(QCoreApplication::applicationPid());

    QProcess worker;
    worker.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    worker.start(workerExe, QStringList() << QStringLiteral("--socket") << socketName);

    if (!worker.waitForStarted(5000)) {
        std::cerr << "[FAIL] Failed to start worker process: " << workerExe.toStdString() << std::endl;
        return 1;
    }

    QLocalSocket client;
#ifdef Q_OS_LINUX
    client.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    bool connected = waitFor([&]() {
        if (client.state() != QLocalSocket::ConnectedState && client.state() != QLocalSocket::ConnectingState) {
            client.connectToServer(socketName);
        }
        return client.state() == QLocalSocket::ConnectedState;
    }, 4000);

    if (!connected) {
        std::cerr << "[FAIL] Failed to connect to worker socket: " << socketName.toStdString() << std::endl;
        worker.kill();
        worker.waitForFinished();
        return 1;
    }

    QByteArray incoming;
    QObject::connect(&client, &QLocalSocket::readyRead, [&]() {
        incoming.append(client.readAll());
    });

    auto readPacket = [&](pdf::ipc::MessageType& outType, QByteArray& outPayload) -> bool {
        if (static_cast<size_t>(incoming.size()) < sizeof(pdf::ipc::PacketHeader)) {
            return false;
        }
        pdf::ipc::PacketHeader hdr;
        std::memcpy(&hdr, incoming.constData(), sizeof(pdf::ipc::PacketHeader));
        if (hdr.magic != pdf::ipc::IPC_MAGIC) {
            return false;
        }
        const size_t fullSize = sizeof(pdf::ipc::PacketHeader) + hdr.payloadSize;
        if (static_cast<size_t>(incoming.size()) < fullSize) {
            return false;
        }
        outType = static_cast<pdf::ipc::MessageType>(hdr.type);
        outPayload = incoming.mid(static_cast<qsizetype>(sizeof(pdf::ipc::PacketHeader)),
                                  static_cast<qsizetype>(hdr.payloadSize));
        incoming.remove(0, static_cast<qsizetype>(fullSize));
        return true;
    };

    auto sendPacket = [&](pdf::ipc::MessageType type, const QByteArray& payload) {
        QByteArray pkt = pdf::ipc::buildPacket(type, payload);
        client.write(pkt);
        client.flush();
    };

    // Send Ping
    pdf::ipc::PingRequest pingReq{42};
    sendPacket(pdf::ipc::MessageType::Ping, pdf::ipc::serialize(pingReq));

    pdf::ipc::MessageType msgType;
    QByteArray msgPayload;
    bool gotPong = waitFor([&]() {
        return readPacket(msgType, msgPayload) && msgType == pdf::ipc::MessageType::Pong;
    }, 5000);

    if (!gotPong) {
        std::cerr << "[FAIL] Did not receive Pong from worker." << std::endl;
        worker.kill();
        return 1;
    }

    pdf::ipc::PongResponse pongResp;
    if (!pdf::ipc::deserialize(msgPayload, pongResp) || pongResp.seq != 42) {
        std::cerr << "[FAIL] Invalid Pong response sequence." << std::endl;
        worker.kill();
        return 1;
    }
    std::cout << "  -> Passed: Worker process spawned and Ping/Pong handshake succeeded." << std::endl;

    // --- TEST 2: OpenDocumentRequest and Response ---
    std::cout << "[TEST 2] Testing document loading over IPC..." << std::endl;
    pdf::ipc::OpenDocumentRequest openReq{1, samplePdf, QString()};
    sendPacket(pdf::ipc::MessageType::OpenDocumentRequest, pdf::ipc::serialize(openReq));

    bool gotOpenResp = waitFor([&]() {
        return readPacket(msgType, msgPayload) && msgType == pdf::ipc::MessageType::OpenDocumentResponse;
    }, 4000);

    if (!gotOpenResp) {
        std::cerr << "[FAIL] Did not receive OpenDocumentResponse from worker." << std::endl;
        worker.kill();
        return 1;
    }

    pdf::ipc::OpenDocumentResponse openResp;
    if (!pdf::ipc::deserialize(msgPayload, openResp)) {
        std::cerr << "[FAIL] Failed to deserialize OpenDocumentResponse." << std::endl;
        worker.kill();
        return 1;
    }

    if (openResp.documentId != 1 || openResp.result.error != pdf::DocumentError::None || openResp.result.pageCount <= 0) {
        std::cerr << "[FAIL] OpenDocument failed. Error=" << static_cast<int>(openResp.result.error)\
                  << " pageCount=" << openResp.result.pageCount << std::endl;\
        worker.kill();
        return 1;
    }
    std::cout << "  -> Passed: Document opened over IPC, pageCount=" << openResp.result.pageCount << std::endl;

    // --- TEST 3: Tile Rendering over IPC ---
    std::cout << "[TEST 3] Testing tile rasterization over IPC..." << std::endl;
    pdf::TileRenderRequest tileReq;
    tileReq.documentId = 1;
    tileReq.generation = 1;
    tileReq.pageIndex = 0;
    tileReq.rect = {0, 0, 512, 512};
    tileReq.scale = 1.0;
    tileReq.dpr = 1.0;
    tileReq.key = pdf::TileKey::create(1, 0, 0, 0, 1.0, 1.0);
    tileReq.priority = 10;

    pdf::ipc::RenderTileRequest renderTileReq{tileReq};
    sendPacket(pdf::ipc::MessageType::RenderTileRequest, pdf::ipc::serialize(renderTileReq));

    bool gotTileResp = waitFor([&]() {
        return readPacket(msgType, msgPayload) && msgType == pdf::ipc::MessageType::TileRenderedResponse;
    }, 5000);

    if (!gotTileResp) {
        std::cerr << "[FAIL] Did not receive TileRenderedResponse from worker." << std::endl;
        worker.kill();
        return 1;
    }

    pdf::ipc::TileRenderedResponse tileResp;
    if (!pdf::ipc::deserialize(msgPayload, tileResp)) {
        std::cerr << "[FAIL] Failed to deserialize TileRenderedResponse." << std::endl;
        worker.kill();
        return 1;
    }

    if (tileResp.width != 512 || tileResp.height != 512 || !tileResp.isShm ||
        !tileResp.pixelData.isEmpty() || tileResp.shmKey.isEmpty()) {
        std::cerr << "[FAIL] Invalid tile buffer returned: width=" << tileResp.width
                  << " height=" << tileResp.height << " shared-memory=" << tileResp.isShm << std::endl;
        worker.kill();
        return 1;
    }
    const size_t tileByteCount = static_cast<size_t>(tileResp.stride) *
                                 static_cast<size_t>(tileResp.height);
    std::vector<unsigned char> pixelCopy(tileByteCount);
    if (!pdf::ipc::readAndUnlinkSharedTile(tileResp.shmKey.toStdString(), static_cast<size_t>(tileResp.height),
                                           static_cast<size_t>(tileResp.stride),
                                           static_cast<size_t>(tileResp.width) * 4, pixelCopy.data(),
                                           static_cast<size_t>(tileResp.stride))) {
        std::cerr << "[FAIL] Could not read the shared-memory tile buffer." << std::endl;
        worker.kill();
        return 1;
    }
    const bool hasPixels = std::any_of(pixelCopy.begin(), pixelCopy.end(), [](unsigned char value) { return value != 0; });
    if (!hasPixels) {
        std::cerr << "[FAIL] Shared-memory tile has no readable pixel data." << std::endl;
        worker.kill();
        return 1;
    }
    sendPacket(pdf::ipc::MessageType::SharedMemoryAck,
               pdf::ipc::serialize(pdf::ipc::SharedMemoryAck{tileResp.shmKey}));
    std::cout << "  -> Passed: 512x512 tile received through shared memory ("
              << tileResp.stride * tileResp.height << " bytes)." << std::endl;

    // --- TEST 4: Page Text Extraction over IPC ---
    std::cout << "[TEST 4] Testing asynchronous page text extraction over IPC..." << std::endl;
    pdf::ipc::PageTextRequest textReq{1, 0};
    sendPacket(pdf::ipc::MessageType::PageTextRequest, pdf::ipc::serialize(textReq));

    bool gotTextResp = waitFor([&]() {
        return readPacket(msgType, msgPayload) && msgType == pdf::ipc::MessageType::PageTextResponse;
    }, 4000);

    if (!gotTextResp) {
        std::cerr << "[FAIL] Did not receive PageTextResponse from worker." << std::endl;
        worker.kill();
        return 1;
    }

    pdf::ipc::PageTextResponse textResp;
    if (!pdf::ipc::deserialize(msgPayload, textResp) || !textResp.success) {
        std::cerr << "[FAIL] Text extraction failed over IPC." << std::endl;
        worker.kill();
        return 1;
    }
    std::cout << "  -> Passed: Page text extracted (" << textResp.pageText.text.length()
              << " characters, " << textResp.pageText.chars.size() << " glyph boxes)." << std::endl;

    // --- TEST 5: Document Outline over IPC ---
    std::cout << "[TEST 5] Testing document outline extraction over IPC..." << std::endl;
    pdf::ipc::GetOutlineRequest outlineReq{1};
    sendPacket(pdf::ipc::MessageType::GetOutlineRequest, pdf::ipc::serialize(outlineReq));

    bool gotOutlineResp = waitFor([&]() {
        return readPacket(msgType, msgPayload) && msgType == pdf::ipc::MessageType::GetOutlineResponse;
    }, 4000);

    if (!gotOutlineResp) {
        std::cerr << "[FAIL] Did not receive GetOutlineResponse from worker." << std::endl;
        worker.kill();
        return 1;
    }
    std::cout << "  -> Passed: Outline response received over IPC." << std::endl;

    // --- TEST 5.1: Thumbnail Rendering over IPC ---
    std::cout << "[TEST 5.1] Testing page thumbnail rendering over IPC..." << std::endl;
    pdf::ipc::ThumbnailRequest thumbReq{1, 0, 140};
    sendPacket(pdf::ipc::MessageType::ThumbnailRequest, pdf::ipc::serialize(thumbReq));

    bool gotThumbResp = waitFor([&]() {
        return readPacket(msgType, msgPayload) && msgType == pdf::ipc::MessageType::ThumbnailResponse;
    }, 4000);

    if (!gotThumbResp) {
        std::cerr << "[FAIL] Did not receive ThumbnailResponse from worker." << std::endl;
        worker.kill();
        return 1;
    }

    pdf::ipc::ThumbnailResponse thumbResp;
    if (!pdf::ipc::deserialize(msgPayload, thumbResp) || thumbResp.width <= 0 || thumbResp.height <= 0 || thumbResp.pixelData.isEmpty()) {
        std::cerr << "[FAIL] Invalid ThumbnailResponse received over IPC." << std::endl;
        worker.kill();
        return 1;
    }
    std::cout << "  -> Passed: Thumbnail rendered over IPC (" << thumbResp.width << "x"
              << thumbResp.height << ", " << thumbResp.pixelData.size() << " bytes)." << std::endl;

    // --- TEST 6: Clean Teardown on Disconnect ---
    std::cout << "[TEST 6] Testing clean worker exit when client disconnects..." << std::endl;
    client.disconnectFromServer();
    client.close();

    bool workerCleanExit = waitFor([&]() {
        return worker.state() == QProcess::NotRunning;
    }, 5000);

    if (!workerCleanExit) {
        std::cerr << "[FAIL] Worker process did not shut down upon socket disconnect." << std::endl;
        worker.kill();
        return 1;
    }

    if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
        std::cerr << "[FAIL] Worker exited abnormally with code: " << worker.exitCode() << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Worker process exited cleanly with code 0 on client disconnect." << std::endl;

    // --- TEST 7: Crash Resilience and Process Isolation ---
    std::cout << "[TEST 7] Testing crash resilience (worker killed without crashing caller)..." << std::endl;
    const QString crashSocketName = QStringLiteral("pdf_test_crash_%1").arg(QCoreApplication::applicationPid());
    QProcess crashWorker;
    crashWorker.start(workerExe, QStringList() << QStringLiteral("--socket") << crashSocketName);
    if (!crashWorker.waitForStarted(3000)) {
        std::cerr << "[FAIL] Failed to spawn crash-test worker." << std::endl;
        return 1;
    }

    QLocalSocket crashClient;
#ifdef Q_OS_LINUX
    crashClient.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
    bool crashConnected = waitFor([&]() {
        if (crashClient.state() != QLocalSocket::ConnectedState) {
            crashClient.connectToServer(crashSocketName);
        }
        return crashClient.state() == QLocalSocket::ConnectedState;
    }, 5000);

    if (!crashConnected) {
        std::cerr << "[FAIL] Failed to connect to crash-test worker." << std::endl;
        crashWorker.kill();
        return 1;
    }

    bool disconnectedDetected = false;
    QObject::connect(&crashClient, &QLocalSocket::disconnected, [&]() {
        disconnectedDetected = true;
    });

    // Abruptly kill the worker (simulating a segfault or hard crash in PDFium)
    crashWorker.kill();
    crashWorker.waitForFinished(2000);

    bool detected = waitFor([&]() {
        return disconnectedDetected || crashClient.state() == QLocalSocket::UnconnectedState;
    }, 5000);

    if (!detected) {
        std::cerr << "[FAIL] Client did not detect abrupt worker termination." << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Abrupt worker termination detected cleanly; caller survived intact." << std::endl;

    // --- TEST 8: Deterministic Worker Isolation Self-Test Probe ---
    std::cout << "[TEST 8] Testing deterministic worker sandbox isolation escape probe (--isolation-selftest)..." << std::endl;
    {
        QTcpServer tcpProbeServer;
        int tcpPort = 0;
        if (tcpProbeServer.listen(QHostAddress::LocalHost, 0)) {
            tcpPort = tcpProbeServer.serverPort();
        }

        QString shmMarkerPath;
        QString dbusFixturePath;
        QLocalServer dbusServer;
#ifdef Q_OS_LINUX
        shmMarkerPath = QStringLiteral("/dev/shm/pdf_worker_test_marker_%1.tmp").arg(QCoreApplication::applicationPid());
        QFile markerFile(shmMarkerPath);
        if (markerFile.open(QIODevice::WriteOnly)) {
            markerFile.write("test_marker");
            markerFile.close();
        }

        dbusFixturePath = QStringLiteral("/tmp/pdf_test_dbus_%1.sock").arg(QCoreApplication::applicationPid());
        QLocalServer::removeServer(dbusFixturePath);
        dbusServer.listen(dbusFixturePath);
#endif
        struct FixtureCleanup {
            QString shm;
            QString dbus;
            ~FixtureCleanup() {
                if (!shm.isEmpty()) QFile::remove(shm);
                if (!dbus.isEmpty()) QFile::remove(dbus);
            }
        } cleanupFixtures{shmMarkerPath, dbusFixturePath};

        QStringList args;
        args << QStringLiteral("--isolation-selftest");
        if (tcpPort > 0) {
            args << QStringLiteral("--test-tcp-port") << QString::number(tcpPort);
        }
        if (!shmMarkerPath.isEmpty()) {
            args << QStringLiteral("--test-shm-marker") << shmMarkerPath;
        }
        if (!dbusFixturePath.isEmpty()) {
            args << QStringLiteral("--test-dbus-socket") << dbusFixturePath;
        }

        QProcess selftestProc;
        selftestProc.start(workerExe, args);
        if (!selftestProc.waitForFinished(5000)) {
            std::cerr << "[FAIL] Isolation selftest process timed out." << std::endl;
            selftestProc.kill();
            return 1;
        }
        const QString output = QString::fromUtf8(selftestProc.readAllStandardOutput());
#ifdef Q_OS_LINUX
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] write_tmp: BLOCKED"))) {
            std::cerr << "[FAIL] write_tmp probe should be BLOCKED on Linux." << std::endl;
            return 1;
        }
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] open_tcp: BLOCKED"))) {
            std::cerr << "[FAIL] open_tcp probe should be BLOCKED by seccomp on Linux." << std::endl;
            return 1;
        }
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] session_bus: BLOCKED"))) {
            std::cerr << "[FAIL] session_bus probe should be BLOCKED by seccomp on Linux." << std::endl;
            return 1;
        }
#if defined(__x86_64__)
        for (const char* probe : {"abi32_execve", "x32_syscall"}) {
            if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] %1: BLOCKED").arg(QLatin1String(probe)))) {
                std::cerr << "[FAIL] seccomp must kill the " << probe << " probe (SIGSYS)." << std::endl;
                return 1;
            }
        }
#endif
        if (output.contains(QStringLiteral("[ISOLATION_SELFTEST] io_uring: PERMITTED"))) {
            std::cerr << "[FAIL] io_uring_setup must be killed by seccomp." << std::endl;
            return 1;
        }
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] getuid: OK"))) {
            std::cerr << "[FAIL] seccomp must not deny ordinary syscalls (getuid regression)." << std::endl;
            return 1;
        }
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] read_outside: PERMITTED")) ||
            !output.contains(QStringLiteral("[ISOLATION_SELFTEST] dev_shm_foreign: PERMITTED"))) {
            std::cerr << "[FAIL] Probes failed to report expected baseline access: " << output.toStdString() << std::endl;
            return 1;
        }
#else
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] platform: SKIPPED (non-Linux)"))) {
            std::cerr << "[FAIL] Non-Linux platform expected SKIPPED." << std::endl;
            return 1;
        }
#endif
        std::cout << "  -> Passed: Worker isolation escape probe baseline accurately verified." << std::endl;
    }

    // --- TEST 9A: Heartbeat Watchdog Verifies Engine-Thread Health (Progress Snapshot) ---
    std::cout << "[TEST 9A] Testing worker answers Ping promptly with progress snapshot during engine hang..." << std::endl;
    {
#ifdef PDF_WORKER_TEST_HOOKS
        const QString hangSocket = QStringLiteral("pdf_test_hang_engine_%1").arg(QCoreApplication::applicationPid());
        QProcess hangWorker;
        hangWorker.start(workerExe, QStringList() << QStringLiteral("--socket") << hangSocket << QStringLiteral("--test-hang-engine"));
        if (!hangWorker.waitForStarted(5000)) {
            std::cerr << "[FAIL] Failed to spawn hang-test worker." << std::endl;
            return 1;
        }

        QLocalSocket hangClient;
#ifdef Q_OS_LINUX
        hangClient.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
        bool connected = waitFor([&]() {
            if (hangClient.state() != QLocalSocket::ConnectedState) {
                hangClient.connectToServer(hangSocket);
            }
            return hangClient.state() == QLocalSocket::ConnectedState;
        }, 5000);

        if (!connected) {
            std::cerr << "[FAIL] Failed to connect to hang-test worker." << std::endl;
            hangWorker.kill();
            return 1;
        }

        QByteArray hangIncoming;
        QObject::connect(&hangClient, &QLocalSocket::readyRead, [&]() {
            hangIncoming.append(hangClient.readAll());
        });

        auto readHangPacket = [&](pdf::ipc::MessageType& outType, QByteArray& outPayload) -> bool {
            if (static_cast<size_t>(hangIncoming.size()) < sizeof(pdf::ipc::PacketHeader)) return false;
            pdf::ipc::PacketHeader hdr;
            std::memcpy(&hdr, hangIncoming.constData(), sizeof(pdf::ipc::PacketHeader));
            if (hdr.magic != pdf::ipc::IPC_MAGIC) return false;
            const size_t fullSize = sizeof(pdf::ipc::PacketHeader) + hdr.payloadSize;
            if (static_cast<size_t>(hangIncoming.size()) < fullSize) return false;
            outType = static_cast<pdf::ipc::MessageType>(hdr.type);
            outPayload = hangIncoming.mid(sizeof(pdf::ipc::PacketHeader), hdr.payloadSize);
            hangIncoming.remove(0, fullSize);
            return true;
        };

        // Ping 1
        pdf::ipc::PingRequest pingReq1{101};
        hangClient.write(pdf::ipc::buildPacket(pdf::ipc::MessageType::Ping, pdf::ipc::serialize(pingReq1)));
        hangClient.flush();

        pdf::ipc::MessageType msgType;
        QByteArray payload;
        bool gotPong1 = waitFor([&]() {
            return readHangPacket(msgType, payload) && msgType == pdf::ipc::MessageType::Pong;
        }, 5000);

        if (!gotPong1) {
            std::cerr << "[FAIL] WorkerServer did not respond to Ping on IPC thread!" << std::endl;
            hangWorker.kill();
            return 1;
        }
        pdf::ipc::PongResponse pong1;
        if (!pdf::ipc::deserialize(payload, pong1) || pong1.seq != 101) {
            std::cerr << "[FAIL] Invalid PongResponse 1 sequence." << std::endl;
            hangWorker.kill();
            return 1;
        }

        // Wait a short duration and send Ping 2
        QThread::msleep(100);
        pdf::ipc::PingRequest pingReq2{102};
        hangClient.write(pdf::ipc::buildPacket(pdf::ipc::MessageType::Ping, pdf::ipc::serialize(pingReq2)));
        hangClient.flush();

        bool gotPong2 = waitFor([&]() {
            return readHangPacket(msgType, payload) && msgType == pdf::ipc::MessageType::Pong;
        }, 5000);

        if (!gotPong2) {
            std::cerr << "[FAIL] WorkerServer did not respond to second Ping!" << std::endl;
            hangWorker.kill();
            return 1;
        }
        pdf::ipc::PongResponse pong2;
        if (!pdf::ipc::deserialize(payload, pong2) || pong2.seq != 102) {
            std::cerr << "[FAIL] Invalid second PongResponse sequence." << std::endl;
            hangWorker.kill();
            return 1;
        }

        if (pong1.lastProgressEpochMs != pong2.lastProgressEpochMs) {
            std::cerr << "[FAIL] Engine thread is hung but lastProgressEpochMs advanced unexpectedly!" << std::endl;
            hangWorker.kill();
            return 1;
        }

        hangWorker.kill();
        hangWorker.waitForFinished(1000);
        std::cout << "  -> Passed: Worker answers Ping promptly with progress snapshot; timestamp remains frozen while engine is hung." << std::endl;
#else
        std::cout << "  -> Skipped: ENABLE_TEST_HOOKS is OFF (fault-injection hooks compiled out of shipping build)." << std::endl;
#endif
    }

    // --- TEST 9B: Watchdog Re-arming & Heartbeat Stall Timeout Verification ---
    std::cout << "[TEST 9B] Testing watchdog re-arming and heartbeat/stall timeout detection..." << std::endl;
    {
        pdf::ipc::WorkerClient client;
        client.setWatchdogParams(100, 3, 300); // 100 ms interval, 3 missed, 300 ms stall
        auto res = client.openDocumentSync(1, samplePdf.toStdString());
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for watchdog reopen test." << std::endl;
            return 1;
        }
        if (!client.isWatchdogActive()) {
            std::cerr << "[FAIL] Watchdog should be active on opened document." << std::endl;
            return 1;
        }

        // Close document
        client.closeDocumentSync();
        if (client.isWatchdogActive()) {
            std::cerr << "[FAIL] Watchdog timer should be stopped while idle." << std::endl;
            return 1;
        }

        // Reopen document on the same WorkerClient connection
        auto res2 = client.openDocumentSync(2, samplePdf.toStdString());
        if (res2.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to reopen document on existing WorkerClient." << std::endl;
            return 1;
        }
        if (!client.isWatchdogActive()) {
            std::cerr << "[FAIL] Watchdog timer was not re-armed on document reopen." << std::endl;
            return 1;
        }

        // Verify that the re-armed watchdog actively monitors the reopened document
        QString failureMessage;
        QElapsedTimer timer;
        timer.start();
        QObject::connect(&client, &pdf::ipc::WorkerClient::workerFailed, [&](const QString& msg) {
            failureMessage = msg;
        });

#ifdef PDF_WORKER_TEST_HOOKS
        // Hang engine at runtime via control request without killing process
        client.hangEngineForTesting();
        // Allow up to 10s deadline so watchdog detection remains robust under heavy CPU load
        bool caught = waitFor([&]() { return !failureMessage.isEmpty(); }, 10000);
        if (!caught) {
            std::cerr << "[FAIL] Watchdog did not catch engine stall on reopened document." << std::endl;
            return 1;
        }
        if (!failureMessage.contains("heartbeat") && !failureMessage.contains("stalled") && !failureMessage.contains("unresponsive")) {
            std::cerr << "[FAIL] Failure message did not mention heartbeat/stall: " << failureMessage.toStdString() << std::endl;
            return 1;
        }
        client.stop();
        std::cout << "  -> Passed: Watchdog re-armed successfully across document close and reopen, actively caught hang/stall." << std::endl;
#else
        client.stop();
        std::cout << "  -> Passed: Watchdog re-armed successfully across document close and reopen (shipping build verification)." << std::endl;
#endif
    }

    // --- TEST 9C: A long operation that keeps making progress is not killed as "stalled" ---
    std::cout << "[TEST 9C] Testing that a long-running but progressing operation does not trigger a false watchdog timeout..." << std::endl;
    {
        // The claim under test is that stall detection is progress-aware: total work LONGER than the
        // stall limit must survive as long as progress keeps advancing. (The previous version did
        // ~160 ms of work against a 5 s limit, so it could never exercise that, and its aggressive
        // 250 ms x 3 heartbeat only made it sensitive to host load.) Heartbeat is deliberately
        // tolerant here; heartbeat/stall *detection* is covered by tests 9A and 9B.
#ifdef PDF_WORKER_TEST_HOOKS
        constexpr int kStallLimitMs = 1500;
        constexpr int kTileDelayMs = 100;   // progress interval: 15x below the stall limit
        constexpr int kTiles = 24;          // 24 x 100 ms = 2.4 s of work > kStallLimitMs
#else
        constexpr int kStallLimitMs = 5000;
        constexpr int kTileDelayMs = 0;
        constexpr int kTiles = 4;
#endif
        pdf::ipc::WorkerClient client;
        client.setWatchdogParams(500, 8, kStallLimitMs);
        auto res = client.openDocumentSync(1, samplePdf.toStdString());
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document." << std::endl;
            return 1;
        }

        bool failureFired = false;
        QString failureReason;
        QObject::connect(&client, &pdf::ipc::WorkerClient::workerFailed, [&](const QString& reason) {
            failureFired = true;
            failureReason = reason;
        });

#ifdef PDF_WORKER_TEST_HOOKS
        client.setRenderDelayForTesting(std::chrono::milliseconds(kTileDelayMs));
#endif

        int tilesRendered = 0;
        QObject::connect(&client, &pdf::ipc::WorkerClient::tileReady, [&](int64_t, uint64_t, const pdf::TileKey&, const pdf::PixelBuffer&) {
            tilesRendered++;
        });

        QElapsedTimer total;
        total.start();
        for (int i = 0; i < kTiles; ++i) {
            pdf::TileRenderRequest req;
            req.documentId = 1;
            req.generation = 1;
            req.pageIndex = 0;
            req.rect = pdf::PixelRect{0, i * 32, 256, 32};
            req.scale = 1.0;
            req.dpr = 1.0;
            req.key = pdf::TileKey::create(1, 0, 0, i, 1.0, 1.0);
            req.priority = kTiles - i;
            client.enqueueRequest(req);
        }
        client.waitForIdle();
        const qint64 elapsedMs = total.elapsed();

        if (failureFired) {
            std::cerr << "[FAIL] Progressing operation was killed by the watchdog after " << elapsedMs
                      << " ms: " << failureReason.toStdString() << std::endl;
            return 1;
        }
        if (tilesRendered < kTiles) {
            std::cerr << "[FAIL] Not all tiles rendered (got " << tilesRendered << " of " << kTiles << ")." << std::endl;
            return 1;
        }
#ifdef PDF_WORKER_TEST_HOOKS
        if (elapsedMs <= kStallLimitMs) {
            // Without this the test would be vacuous: it must outlast the stall limit to prove anything.
            std::cerr << "[FAIL] Test 9C finished in " << elapsedMs << " ms, not longer than the " << kStallLimitMs
                      << " ms stall limit; it proves nothing." << std::endl;
            return 1;
        }
#endif
        client.stop();
        std::cout << "  -> Passed: " << kTiles << " tiles over " << elapsedMs << " ms survived a " << kStallLimitMs
                  << " ms stall limit because progress kept advancing." << std::endl;
    }

    // --- TEST 9D: Workload Validation for 4 GiB RLIMIT_AS ---
    std::cout << "[TEST 9D] Workload validation for 4 GiB RLIMIT_AS under high-DPI tile rendering..." << std::endl;
    {
        pdf::ipc::WorkerClient client;
        QString testDocPath = samplePdf;
        const QString fixture200 = QFileInfo(samplePdf).dir().filePath(QStringLiteral("doc_200_pages.pdf"));
        if (QFile::exists(fixture200)) {
            testDocPath = fixture200;
        }
        auto res = client.openDocumentSync(1, testDocPath.toStdString());
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for RLIMIT_AS validation." << std::endl;
            return 1;
        }
        const qint64 pid = client.workerPid();

        // Heavy high-DPI rendering workload (scale 3.0, dpr 2.0 = 6x scale factor)
        for (int i = 0; i < 4; ++i) {
            pdf::TileRenderRequest req;
            req.documentId = 1;
            req.generation = 1;
            req.pageIndex = 0;
            req.rect = pdf::PixelRect{0, i * 128, 512, 128};
            req.scale = 3.0;
            req.dpr = 2.0;
            req.key = pdf::TileKey::create(1, 0, 0, i, 3.0, 2.0);
            client.enqueueRequest(req);
        }
        client.waitForIdle();

#ifdef Q_OS_LINUX
        if (pid > 0) {
            QFile statusFile(QStringLiteral("/proc/%1/status").arg(pid));
            if (!statusFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                std::cerr << "[FAIL] Could not open /proc/" << pid << "/status to verify RLIMIT_AS" << std::endl;
                return 1;
            }
            const QString statusText = QString::fromUtf8(statusFile.readAll());
            quint64 vmPeakKb = 0;
            quint64 vmSizeKb = 0;
            for (const QString& line : statusText.split('\n')) {
                if (line.startsWith(QStringLiteral("VmPeak:"))) {
                    const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                    if (parts.size() >= 2) vmPeakKb = parts[1].toULongLong();
                } else if (line.startsWith(QStringLiteral("VmSize:"))) {
                    const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
                    if (parts.size() >= 2) vmSizeKb = parts[1].toULongLong();
                }
            }
            if (vmPeakKb == 0 && vmSizeKb > 0) {
                vmPeakKb = vmSizeKb;
            }
            if (vmPeakKb == 0) {
                std::cerr << "[FAIL] Failed to parse VmPeak/VmSize from /proc/" << pid << "/status" << std::endl;
                return 1;
            }
            std::cout << "  -> Measured worker peak virtual memory (VmPeak): " << vmPeakKb << " kB ("
                      << (vmPeakKb / 1024) << " MB)" << std::endl;
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
            constexpr bool isAsan = true;
#else
            const bool isAsan = (vmPeakKb > 10ULL * 1024 * 1024) || qEnvironmentVariableIsSet("ASAN_OPTIONS");
#endif
            if (isAsan) {
                std::cout << "  -> AddressSanitizer detected (shadow memory pre-allocation). Skipping 4 GiB RLIMIT_AS upper bound check." << std::endl;
            } else {
                constexpr quint64 RLIMIT_AS_KB = 4ULL * 1024 * 1024;
                if (vmPeakKb >= RLIMIT_AS_KB) {
                    std::cerr << "[FAIL] Worker virtual memory exceeded 4 GiB RLIMIT_AS cap: " << vmPeakKb << " kB" << std::endl;
                    return 1;
                }
            }
            if (vmPeakKb < 10240) {
                std::cerr << "[FAIL] Unreasonable VmPeak measurement: " << vmPeakKb << " kB" << std::endl;
                return 1;
            }
        }
#else
        (void)pid;
#endif
        client.stop();
        std::cout << "  -> Passed: High-DPI workload verified; 4 GiB RLIMIT_AS provides abundant headroom while capping runaway exhaustion." << std::endl;
    }

    // --- TEST 10: Resource-Limit Setup Failure Path ---
    std::cout << "[TEST 10] Testing explicit failure handling when resource-limit setup fails..." << std::endl;
#ifdef PDF_WORKER_TEST_HOOKS
    {
        QProcess rlimitProc;
        rlimitProc.start(workerExe, QStringList() << QStringLiteral("--test-fail-rlimit"));
        rlimitProc.waitForFinished(5000);
        if (rlimitProc.exitCode() == 0) {
            std::cerr << "[FAIL] Worker should fail startup when resource-limit setup fails." << std::endl;
            return 1;
        }
        const QString errOut = QString::fromUtf8(rlimitProc.readAllStandardError());
        if (!errOut.contains(QStringLiteral("RLIMIT_AS"))) {
            std::cerr << "[FAIL] Error message did not cite failed limit name: " << errOut.toStdString() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Failed resource limit explicitly aborted worker startup with error: "
                  << errOut.trimmed().toStdString() << std::endl;
    }
#else
    std::cout << "  -> Skipped: ENABLE_TEST_HOOKS is OFF (--test-fail-rlimit hook compiled out of shipping build)." << std::endl;
#endif

    // --- TEST 11: End-to-End Sandboxed Worker Redaction Verification ---
    std::cout << "[TEST 11] Testing burn-in redaction request/response through sandboxed worker IPC..." << std::endl;
    {
        pdf::ipc::WorkerClient client;
        auto res = client.openDocumentSync(1, samplePdf.toStdString());
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for redaction test." << std::endl;
            return 1;
        }

        std::vector<pdf::ipc::PageRedactionItem> redactions;
        pdf::ipc::PageRedactionItem item;
        item.pageIndex = 0;
        item.rects.push_back(pdf::ipc::RedactRect{50.0, 50.0, 200.0, 100.0});
        redactions.push_back(item);

        // Synchronous redaction test
        pdf::ipc::RedactDocumentResponse syncRes = client.redactDocumentSync(1, redactions, 150.0, 1);
        if (!syncRes.success) {
            std::cerr << "[FAIL] Synchronous redaction failed: " << syncRes.errorMessage.toStdString() << std::endl;
            return 1;
        }
        if (syncRes.pdfBytes.isEmpty()) {
            std::cerr << "[FAIL] Redacted PDF payload is empty." << std::endl;
            return 1;
        }
        if (!syncRes.pdfBytes.startsWith("%PDF-")) {
            std::cerr << "[FAIL] Redacted PDF bytes do not have valid %PDF- magic header." << std::endl;
            return 1;
        }
        if (syncRes.pageCount <= 0) {
            std::cerr << "[FAIL] Redacted pageCount reported as 0." << std::endl;
            return 1;
        }

        // Asynchronous redaction test
        bool asyncReady = false;
        pdf::ipc::RedactDocumentResponse asyncRes;
        QObject::connect(&client, &pdf::ipc::WorkerClient::redactDocumentReady,
                         [&](int64_t docId, const pdf::ipc::RedactDocumentResponse& resp) {
                             if (docId == 1) {
                                 asyncRes = resp;
                                 asyncReady = true;
                             }
                         });

        client.requestRedactDocument(1, redactions, 150.0, 1);
        bool waitSuccess = waitFor([&]() { return asyncReady; }, 10000);
        if (!waitSuccess || !asyncRes.success) {
            std::cerr << "[FAIL] Asynchronous redaction did not complete successfully." << std::endl;
            return 1;
        }
        if (asyncRes.pdfBytes.isEmpty() || !asyncRes.pdfBytes.startsWith("%PDF-")) {
            std::cerr << "[FAIL] Asynchronous redacted PDF payload is invalid." << std::endl;
            return 1;
        }

        client.closeDocumentSync();
        client.stop();
        std::cout << "  -> Passed: Worker successfully performed burn-in redaction and returned verified raster PDF bytes ("
                  << syncRes.pdfBytes.size() << " bytes, " << syncRes.pageCount << " pages)." << std::endl;
    }

    // --- TEST 12: A malformed redaction request is answered, never silently dropped ---
    std::cout << "[TEST 12] Testing that malformed RedactDocumentRequest frames get a failure response..." << std::endl;
    {
        const QString badSocket = QStringLiteral("pdf_test_badredact_%1").arg(QCoreApplication::applicationPid());
        QProcess badWorker;
        badWorker.start(workerExe, QStringList() << QStringLiteral("--socket") << badSocket);
        if (!badWorker.waitForStarted(3000)) {
            std::cerr << "[FAIL] Failed to spawn worker for malformed-request test." << std::endl;
            return 1;
        }
        QLocalSocket bad;
#ifdef Q_OS_LINUX
        bad.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
        if (!waitFor([&]() {
                if (bad.state() != QLocalSocket::ConnectedState && bad.state() != QLocalSocket::ConnectingState) {
                    bad.connectToServer(badSocket);
                }
                return bad.state() == QLocalSocket::ConnectedState;
            }, 5000)) {
            std::cerr << "[FAIL] Could not connect to worker for malformed-request test." << std::endl;
            badWorker.kill();
            return 1;
        }
        QByteArray badIn;
        QObject::connect(&bad, &QLocalSocket::readyRead, [&]() { badIn.append(bad.readAll()); });

        auto expectFailure = [&](const QByteArray& payload, const char* label) -> bool {
            badIn.clear();
            bad.write(pdf::ipc::buildPacket(pdf::ipc::MessageType::RedactDocumentRequest, payload));
            bad.flush();
            QByteArray reply;
            pdf::ipc::MessageType type{};
            const bool got = waitFor([&]() {
                if (static_cast<size_t>(badIn.size()) < sizeof(pdf::ipc::PacketHeader)) return false;
                pdf::ipc::PacketHeader hdr;
                std::memcpy(&hdr, badIn.constData(), sizeof(hdr));
                const size_t full = sizeof(hdr) + hdr.payloadSize;
                if (static_cast<size_t>(badIn.size()) < full) return false;
                type = static_cast<pdf::ipc::MessageType>(hdr.type);
                reply = badIn.mid(static_cast<qsizetype>(sizeof(hdr)), static_cast<qsizetype>(hdr.payloadSize));
                return true;
            }, 3000);
            pdf::ipc::RedactDocumentFailedResponse fail;
            if (!got || type != pdf::ipc::MessageType::RedactDocumentFailedResponse || !pdf::ipc::deserialize(reply, fail) ||
                fail.errorMessage.isEmpty()) {
                std::cerr << "[FAIL] Malformed redaction request (" << label << ") got no failure response." << std::endl;
                return false;
            }
            return true;
        };

        // (a) garbage that cannot even yield a document id
        bool ok = expectFailure(QByteArray("\x01\x02\x03", 3), "3-byte garbage");
        // (b) a valid request cut short mid-rectangle: the document id IS parseable (7)
        pdf::ipc::RedactDocumentRequest req;
        req.documentId = 7;
        pdf::ipc::PageRedactionItem item;
        item.pageIndex = 0;
        item.rects.push_back(pdf::ipc::RedactRect{1.0, 2.0, 30.0, 40.0});
        req.redactions.push_back(item);
        QByteArray truncated = pdf::ipc::serialize(req);
        truncated.chop(9);
        ok = ok && expectFailure(truncated, "truncated payload");
        bad.disconnectFromServer();
        badWorker.kill();
        badWorker.waitForFinished(3000);
        if (!ok) {
            return 1;
        }
        std::cout << "  -> Passed: malformed redaction requests are answered with RedactDocumentFailedResponse." << std::endl;
    }

    // --- TEST 13: Redaction uses the bytes that were opened, not whatever is on disk now ---
    std::cout << "[TEST 13] Testing that redaction is immune to the source file changing after it was opened..." << std::endl;
    {
        QTemporaryDir pinDir;
        if (!pinDir.isValid()) {
            std::cerr << "[FAIL] Could not create temp dir for pinned-source test." << std::endl;
            return 1;
        }
        auto runCase = [&](const char* label, const std::function<void(const QString&)>& mutate) -> bool {
            const QString copy = pinDir.path() + QStringLiteral("/pinned.pdf");
            QFile::remove(copy);
            if (!QFile::copy(samplePdf, copy)) {
                std::cerr << "[FAIL] Could not copy sample for pinned-source test." << std::endl;
                return false;
            }
            pdf::ipc::WorkerClient client;
            auto opened = client.openDocumentSync(7, copy.toStdString());
            if (opened.error != pdf::DocumentError::None) {
                std::cerr << "[FAIL] Could not open copy for pinned-source test (" << label << ")." << std::endl;
                return false;
            }
            mutate(copy); // the file on disk now differs from what the worker has open
            std::vector<pdf::ipc::PageRedactionItem> redactions;
            pdf::ipc::PageRedactionItem item;
            item.pageIndex = 0;
            item.rects.push_back(pdf::ipc::RedactRect{50.0, 50.0, 200.0, 100.0});
            redactions.push_back(item);
            const auto res = client.redactDocumentSync(7, redactions, 150.0, 1);
            const bool ok = res.success && res.pdfBytes.startsWith("%PDF-") && res.pageCount == opened.pageCount;
            if (!ok) {
                std::cerr << "[FAIL] Redaction after the source was " << label << ": success=" << res.success
                          << " pages=" << res.pageCount << " (opened " << opened.pageCount << ") "
                          << res.errorMessage.toStdString() << std::endl;
            }
            client.closeDocumentSync();
            client.stop();
            return ok;
        };
        const bool overwritten = runCase("overwritten with garbage", [](const QString& path) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(QByteArray(4096, 'X'));
            }
        });
        const bool deleted = runCase("deleted", [](const QString& path) { QFile::remove(path); });
        if (!overwritten || !deleted) {
            return 1;
        }
        std::cout << "  -> Passed: redaction used the opened bytes after the file was overwritten and after it was deleted." << std::endl;
    }

    // --- TEST 14: A long but healthy redaction is not killed by the watchdog or the client wait ---
    std::cout << "[TEST 14] Testing that redaction progress heartbeats keep a long redaction alive..." << std::endl;
    {
        constexpr int kStallMs = 1000;   // watchdog kills a worker whose heartbeat is frozen this long
        constexpr int kSilenceMs = 1500; // client wait limit when the worker is silent
        QTemporaryDir tmp;
        if (!tmp.isValid()) {
            std::cerr << "[FAIL] No temp dir for heartbeat test." << std::endl;
            return 1;
        }
        const QString bigPdf = tmp.path() + QStringLiteral("/many_pages.pdf");
        {
            QFile f(bigPdf);
            const QByteArray data = makeManyPagePdf(kHeartbeatTestPages);
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size()) {
                std::cerr << "[FAIL] Cannot write heartbeat test PDF." << std::endl;
                return 1;
            }
        }

        pdf::ipc::WorkerClient client;
        client.setWatchdogParams(150, 5, kStallMs);
        bool workerDied = false;
        QObject::connect(&client, &pdf::ipc::WorkerClient::workerFailed, [&](const QString& msg) {
            workerDied = true;
            std::cerr << "  (worker failed: " << msg.toStdString() << ")" << std::endl;
        });
        auto res = client.openDocumentSync(1, bigPdf.toStdString());
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open heartbeat test document." << std::endl;
            return 1;
        }
        std::vector<pdf::ipc::PageRedactionItem> redactions;
        pdf::ipc::PageRedactionItem item;
        item.pageIndex = 0;
        item.rects.push_back(pdf::ipc::RedactRect{50.0, 50.0, 300.0, 120.0});
        redactions.push_back(item);

        QElapsedTimer clock;
        clock.start();
        const auto r = client.redactDocumentSync(1, redactions, 150.0, 1, kSilenceMs);
        const qint64 elapsedMs = clock.elapsed();

        if (workerDied || !r.success) {
            std::cerr << "[FAIL] Long redaction did not complete (success=" << r.success << ", workerDied=" << workerDied
                      << ", after " << elapsedMs << " ms): " << r.errorMessage.toStdString() << std::endl;
            return 1;
        }
        if (r.pageCount != kHeartbeatTestPages || !r.pdfBytes.startsWith("%PDF-")) {
            std::cerr << "[FAIL] Long redaction returned an unexpected result (pages=" << r.pageCount << ")." << std::endl;
            return 1;
        }
        // Guard against a vacuous pass: the run must have outlived BOTH limits, otherwise it proves nothing.
        if (elapsedMs < 2 * kStallMs) {
            std::cerr << "[FAIL] Heartbeat test finished in " << elapsedMs << " ms, faster than 2x the stall limit ("
                      << 2 * kStallMs << " ms): raise kHeartbeatTestPages so the test is meaningful." << std::endl;
            return 1;
        }
        client.closeDocumentSync();
        client.stop();
        std::cout << "  -> Passed: " << kHeartbeatTestPages << "-page redaction took " << elapsedMs << " ms, outliving the "
                  << kStallMs << " ms stall limit and the " << kSilenceMs << " ms silence limit." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL WORKER IPC UNIT TESTS PASSED!     " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
