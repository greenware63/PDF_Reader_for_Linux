#include <QCoreApplication>
#include <QProcess>
#include <QLocalSocket>
#include <QSharedMemory>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <functional>
#include <algorithm>
#include "ipc/IpcProtocol.h"
#include "ipc/IpcMessages.h"

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
    QSharedMemory tileMemory(tileResp.shmKey);
    if (!tileMemory.attach(QSharedMemory::ReadOnly) ||
        tileMemory.size() < tileResp.stride * tileResp.height) {
        std::cerr << "[FAIL] Could not map the shared-memory tile buffer." << std::endl;
        worker.kill();
        return 1;
    }
    const size_t tileByteCount = static_cast<size_t>(tileResp.stride) *
                                 static_cast<size_t>(tileResp.height);
    const auto* pixels = static_cast<const unsigned char*>(tileMemory.constData());
    const bool hasPixels = pixels &&
        std::any_of(pixels, pixels + tileByteCount, [](unsigned char value) { return value != 0; });
    tileMemory.detach();
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

    std::cout << "========================================" << std::endl;
    std::cout << " ALL WORKER IPC UNIT TESTS PASSED!     " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
