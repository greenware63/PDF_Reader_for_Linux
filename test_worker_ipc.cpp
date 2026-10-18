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
    }, 3000);

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
        std::cerr << "[FAIL] OpenDocument failed. Error=" << static_cast<int>(openResp.result.error)
                  << " pageCount=" << openResp.result.pageCount << std::endl;
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
    }, 3000);

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
    }, 3000);

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
    }, 3000);

    if (!detected) {
        std::cerr << "[FAIL] Client did not detect abrupt worker termination." << std::endl;
        return 1;
    }
    std::cout << "  -> Passed: Abrupt worker termination detected cleanly; caller survived intact." << std::endl;

    // --- TEST 8: Worker Isolation Self-Test Probe ---
    std::cout << "[TEST 8] Testing worker sandbox isolation escape probe (--isolation-selftest)..." << std::endl;
    {
        QProcess selftestProc;
        selftestProc.start(workerExe, QStringList() << QStringLiteral("--isolation-selftest"));
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
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] read_outside: PERMITTED")) ||
            !output.contains(QStringLiteral("[ISOLATION_SELFTEST] open_tcp: PERMITTED")) ||
            !output.contains(QStringLiteral("[ISOLATION_SELFTEST] session_bus: PERMITTED")) ||
            !output.contains(QStringLiteral("[ISOLATION_SELFTEST] dev_shm_foreign: PERMITTED"))) {
            std::cerr << "[FAIL] Baseline probes failed to report expected pre-hardening permissions." << std::endl;
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

    // --- TEST 9: Heartbeat Watchdog Terminating Unresponsive/Spinning Worker ---
    std::cout << "[TEST 9] Testing UI heartbeat watchdog terminating hung/spinning worker..." << std::endl;
    {
        pdf::ipc::WorkerClient client;
        // Fast watchdog parameters for the unit test: 80 ms interval, 3 missed pings = ~240 ms timeout
        client.setWatchdogParams(80, 3);
        const auto openRes = client.openDocumentSync(1, samplePdf.toStdString());
        if (openRes.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document for watchdog test." << std::endl;
            return 1;
        }

        bool failureSignalReceived = false;
        QString failureMsg;
        QObject::connect(&client, &pdf::ipc::WorkerClient::workerFailed, [&](const QString& msg) {
            failureSignalReceived = true;
            failureMsg = msg;
        });

        // Instruct the worker to spin forever, ignoring IPC and heartbeat pings
        client.spinWorkerForTesting();

        bool watchdogTriggered = waitFor([&]() {
            return failureSignalReceived;
        }, 5000);

        if (!watchdogTriggered) {
            std::cerr << "[FAIL] Watchdog failed to terminate spinning worker within timeout." << std::endl;
            client.killWorkerForTesting();
            return 1;
        }
        if (!failureMsg.contains(QStringLiteral("heartbeat timeout"))) {
            std::cerr << "[FAIL] Worker failure did not cite heartbeat timeout: " << failureMsg.toStdString() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Watchdog detected hung worker, terminated it, and reported: "
                  << failureMsg.toStdString() << std::endl;
        client.stop();
    }

    std::cout << "========================================" << std::endl;
    std::cout << " ALL WORKER IPC UNIT TESTS PASSED!     " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
