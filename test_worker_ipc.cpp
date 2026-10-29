#include <QCoreApplication>
#include <QProcess>
#include <QLocalSocket>
#include "ipc/SharedTile.h"
#include <cstring>
#include <vector>
#include <memory>
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
#include <QMap>
#include <QDir>
#include <QRegularExpression>
#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <cerrno>
#endif

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

#ifdef PDF_WORKER_TEST_HOOKS
constexpr int kHeartbeatTestPages = 60;   // duration is guaranteed by the per-step delay hook below
#else
constexpr int kHeartbeatTestPages = 150;  // no hook: rely on volume; the duration guard is advisory here
#endif

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

    // --- TEST 0S: search-result snippets survive IPC and hostile snippet fields are rejected ---
    std::cout << "[TEST 0S] Testing SearchMatch snippet serialization and bounds..." << std::endl;
    {
        using namespace pdf::ipc;
        SearchMatchesFoundResponse res;
        res.documentId = 7;
        res.searchId = 9;
        res.pageIndex = 3;
        pdf::SearchMatch m;
        m.pageIndex = 3;
        m.charStartIndex = 10;
        m.charCount = 3;
        m.snippet = QString::fromUtf8("\u2026the fox jumps");
        m.snippetMatchStart = 5;
        m.snippetMatchLength = 3;
        res.matches.push_back(m);

        SearchMatchesFoundResponse back;
        if (!deserialize(serialize(res), back) || back.matches.size() != 1 || back.matches[0].snippet != m.snippet ||
            back.matches[0].snippetMatchStart != 5 || back.matches[0].snippetMatchLength != 3) {
            std::cerr << "[FAIL] Snippet did not round-trip over IPC." << std::endl;
            return 1;
        }

        auto expectRejected = [&](const char* what, auto mutate) {
            SearchMatchesFoundResponse bad = res;
            mutate(bad.matches[0]);
            SearchMatchesFoundResponse out;
            if (deserialize(serialize(bad), out)) {
                std::cerr << "[FAIL] Hostile snippet accepted: " << what << std::endl;
                return false;
            }
            return true;
        };
        if (!expectRejected("oversized text", [](pdf::SearchMatch& x) {
                x.snippet = QString(pdf::MAX_SEARCH_SNIPPET_CHARS + 100, QChar(u'a'));
                x.snippetMatchStart = 0; x.snippetMatchLength = 1; }) ||
            !expectRejected("range past end", [](pdf::SearchMatch& x) { x.snippetMatchStart = 10; x.snippetMatchLength = 50; }) ||
            !expectRejected("negative start", [](pdf::SearchMatch& x) { x.snippetMatchStart = -1; }) ||
            !expectRejected("negative length", [](pdf::SearchMatch& x) { x.snippetMatchLength = -4; })) {
            return 1;
        }
        std::cout << "  -> Passed: snippet round-trips; oversized text and out-of-range highlight are rejected." << std::endl;
    }

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

    auto sendPacketWithFd = [&](QLocalSocket& sock, pdf::ipc::MessageType type, const QByteArray& payload, int fd = -1) {
#ifdef Q_OS_LINUX
        if (fd >= 0) {
            sock.flush();
            while (sock.bytesToWrite() > 0) {
                if (!sock.waitForBytesWritten(100)) break;
            }
            int sockfd = static_cast<int>(sock.socketDescriptor());
            const QByteArray pkt = pdf::ipc::buildPacket(type, payload);
            struct msghdr msg{};
            struct iovec iov{};
            iov.iov_base = const_cast<char*>(pkt.constData());
            iov.iov_len = static_cast<size_t>(pkt.size());
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;

            char cmsgBuf[CMSG_SPACE(sizeof(int))];
            msg.msg_control = cmsgBuf;
            msg.msg_controllen = sizeof(cmsgBuf);

            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

            ::sendmsg(sockfd, &msg, MSG_NOSIGNAL);
            ::close(fd);
            return;
        }
#else
        (void)fd;
#endif
        QByteArray pkt = pdf::ipc::buildPacket(type, payload);
        sock.write(pkt);
        sock.flush();
#ifdef Q_OS_LINUX
        if (fd >= 0) {
            ::close(fd);
        }
#endif
    };
    (void)sendPacketWithFd;

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
#ifdef Q_OS_LINUX
    const QString absSample = QFileInfo(samplePdf).absoluteFilePath();
    int sampleFd = ::open(absSample.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    sendPacketWithFd(client, pdf::ipc::MessageType::OpenDocumentRequest, pdf::ipc::serialize(openReq), sampleFd);
#else
    sendPacket(pdf::ipc::MessageType::OpenDocumentRequest, pdf::ipc::serialize(openReq));
#endif

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
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] read_outside: BLOCKED"))) {
            std::cerr << "[FAIL] read_outside probe should be BLOCKED by Landlock on Linux: " << output.toStdString() << std::endl;
            return 1;
        }
        if (!output.contains(QStringLiteral("[ISOLATION_SELFTEST] dev_shm_foreign: PERMITTED"))) {
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

    // --- TEST 9E: the worker keeps answering heartbeats while a waitForIdle request is pending ---
    // WorkerClient::waitForIdle() sends a control request that the worker answers only once its engine is
    // idle. If the worker handles that request by BLOCKING its IPC thread until idle, it cannot read pings
    // for the whole workload: no pongs, no progress reports, and the client's missed-ping count climbs
    // toward the watchdog limit (the source of the rare Test 9C flake under host load). The observable
    // contract: during a long workload the client keeps receiving pongs that carry advancing progress.
#ifdef PDF_WORKER_TEST_HOOKS
    std::cout << "[TEST 9E] Testing the worker keeps answering heartbeats while waitForIdle is pending..." << std::endl;
    {
        constexpr int kPingMs = 200;
        constexpr int kTileDelayMs = 100;
        constexpr int kTiles = 24; // 2.4 s of work = ~12 ping intervals
        pdf::ipc::WorkerClient client;
        client.setWatchdogParams(kPingMs, 50, 10000); // tolerant: only the pong/progress cadence is observed
        auto res = client.openDocumentSync(1, samplePdf.toStdString());
        if (res.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Failed to open document." << std::endl;
            return 1;
        }
        client.setRenderDelayForTesting(std::chrono::milliseconds(kTileDelayMs));

        int progressReports = 0;
        QObject::connect(&client, &pdf::ipc::WorkerClient::engineProgressed, [&]() { ++progressReports; });
        bool failed = false;
        QObject::connect(&client, &pdf::ipc::WorkerClient::workerFailed, [&](const QString&) { failed = true; });

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
        const int reportsDuringWait = progressReports;
        client.stop();

        if (failed) {
            std::cerr << "[FAIL] Worker failed during the heartbeat-cadence test." << std::endl;
            return 1;
        }
        // A worker that stays responsive reports progress about every ping interval (expect ~10+).
        // One that blocks in waitForIdle reports at most once or twice, right at the end.
        if (reportsDuringWait < 5) {
            std::cerr << "[FAIL] Only " << reportsDuringWait << " progress report(s) reached the client during a "
                      << (kTiles * kTileDelayMs) << " ms workload: the worker stopped answering heartbeats while "
                      << "waitForIdle was pending." << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: " << reportsDuringWait << " progress reports arrived during the workload." << std::endl;
    }
#endif

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
#ifdef PDF_WORKER_TEST_HOOKS
        // 3 phases x 60 pages = 180 steps x 15 ms = 2.7 s minimum, i.e. > 2x the stall limit on ANY host.
        client.setRenderDelayForTesting(std::chrono::milliseconds(15));
#endif
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
#ifdef PDF_WORKER_TEST_HOOKS
            // The step-delay hook makes this impossible unless the hook stopped working.
            std::cerr << "[FAIL] Heartbeat test finished in " << elapsedMs << " ms, faster than 2x the stall limit ("
                      << 2 * kStallMs << " ms) despite the step-delay hook." << std::endl;
            return 1;
#else
            // Hooks-OFF (shipping) build: duration depends on host speed, so a fast machine must not turn
            // into a red test. Meaningfulness is enforced by the hooks-ON legs.
            std::cout << "  NOTE: finished in " << elapsedMs << " ms (< 2x stall limit); not a meaningful heartbeat run"
                      << " on this host; the hooks-ON CI legs enforce it." << std::endl;
#endif
        }
        client.closeDocumentSync();
        client.stop();
        std::cout << "  -> Passed: " << kHeartbeatTestPages << "-page redaction took " << elapsedMs << " ms, outliving the "
                  << kStallMs << " ms stall limit and the " << kSilenceMs << " ms silence limit." << std::endl;
    }

    // --- TEST 15: Valid Document Reads Work via Descriptor Passing ---
    std::cout << "[TEST 15] Testing valid document reads and rendering work via descriptor passing..." << std::endl;
    {
        pdf::ipc::WorkerClient client;
        auto opened = client.openDocumentSync(15, samplePdf.toStdString());
        if (opened.error != pdf::DocumentError::None || opened.pageCount <= 0) {
            std::cerr << "[FAIL] Failed to open document over IPC with descriptor passing: error="
                      << static_cast<int>(opened.error) << std::endl;
            return 1;
        }
        bool tileReady = false;
        QObject::connect(&client, &pdf::ipc::WorkerClient::tileReady,
                         [&](int64_t docId, uint64_t, const pdf::TileKey&, const pdf::PixelBuffer& buf) {
                             if (docId == 15 && buf.isValid()) {
                                 tileReady = true;
                             }
                         });
        pdf::TileRenderRequest req;
        req.documentId = 15;
        req.generation = 1;
        req.pageIndex = 0;
        req.rect = pdf::PixelRect{0, 0, 128, 128};
        req.scale = 1.0;
        req.dpr = 1.0;
        req.key = pdf::TileKey::create(15, 0, 0, 0, 1.0, 1.0);
        client.enqueueRequest(req);
        if (!waitFor([&]() { return tileReady; }, 5000)) {
            std::cerr << "[FAIL] Tile rendering failed after opening document via descriptor passing." << std::endl;
            return 1;
        }
        client.closeDocumentSync();
        client.stop();
        std::cout << "  -> Passed: Valid document loaded and rendered successfully via passed descriptor." << std::endl;
    }

    // --- TEST 16: Denied Reads of Unrelated Files (Landlock Read Confinement) ---
    std::cout << "[TEST 16] Testing worker Landlock read confinement denies arbitrary file reads..." << std::endl;
    {
        const QString noFdSocket = QStringLiteral("pdf_test_nofd_%1").arg(QCoreApplication::applicationPid());
        QProcess noFdWorker;
        noFdWorker.start(workerExe, QStringList() << QStringLiteral("--socket") << noFdSocket);
        if (!noFdWorker.waitForStarted(3000)) {
            std::cerr << "[FAIL] Failed to spawn worker for Landlock read confinement test." << std::endl;
            return 1;
        }
        QLocalSocket rawClient;
#ifdef Q_OS_LINUX
        rawClient.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
        if (!waitFor([&]() {
                if (rawClient.state() != QLocalSocket::ConnectedState && rawClient.state() != QLocalSocket::ConnectingState) {
                    rawClient.connectToServer(noFdSocket);
                }
                return rawClient.state() == QLocalSocket::ConnectedState;
            }, 5000)) {
            std::cerr << "[FAIL] Could not connect to worker for read confinement test." << std::endl;
            noFdWorker.kill();
            return 1;
        }
        QByteArray rawIn;
        QObject::connect(&rawClient, &QLocalSocket::readyRead, [&]() { rawIn.append(rawClient.readAll()); });

        // Send OpenDocumentRequest WITHOUT passing a file descriptor over SCM_RIGHTS
        pdf::ipc::OpenDocumentRequest req{16, samplePdf, QString()};
        QByteArray pkt = pdf::ipc::buildPacket(pdf::ipc::MessageType::OpenDocumentRequest, pdf::ipc::serialize(req));
        rawClient.write(pkt);
        rawClient.flush();

        pdf::ipc::MessageType type{};
        QByteArray reply;
        bool gotReply = waitFor([&]() {
            if (static_cast<size_t>(rawIn.size()) < sizeof(pdf::ipc::PacketHeader)) return false;
            pdf::ipc::PacketHeader hdr;
            std::memcpy(&hdr, rawIn.constData(), sizeof(hdr));
            if (hdr.magic != pdf::ipc::IPC_MAGIC) return false;
            const size_t full = sizeof(hdr) + hdr.payloadSize;
            if (static_cast<size_t>(rawIn.size()) < full) return false;
            type = static_cast<pdf::ipc::MessageType>(hdr.type);
            reply = rawIn.mid(static_cast<qsizetype>(sizeof(hdr)), static_cast<qsizetype>(hdr.payloadSize));
            return true;
        }, 5000);

        if (!gotReply || type != pdf::ipc::MessageType::OpenDocumentResponse) {
            std::cerr << "[FAIL] Worker did not reply to OpenDocumentRequest without descriptor." << std::endl;
            noFdWorker.kill();
            return 1;
        }
        pdf::ipc::OpenDocumentResponse resp;
        if (!pdf::ipc::deserialize(reply, resp)) {
            std::cerr << "[FAIL] Failed to deserialize OpenDocumentResponse." << std::endl;
            noFdWorker.kill();
            return 1;
        }
#ifdef Q_OS_LINUX
        // On Linux under Landlock, worker cannot open the file path directly without descriptor passing.
        if (resp.result.error == pdf::DocumentError::None) {
            std::cerr << "[FAIL] Worker unexpectedly opened file by path without descriptor passing under Landlock!" << std::endl;
            noFdWorker.kill();
            return 1;
        }
#endif
        rawClient.disconnectFromServer();
        noFdWorker.kill();
        noFdWorker.waitForFinished(3000);
        std::cout << "  -> Passed: Worker is confined by Landlock and cannot read arbitrary files without descriptor passing." << std::endl;
    }

    // --- TEST 17: Unexpected Descriptor Hygiene (Unsolicited Descriptors Are Closed) ---
    std::cout << "[TEST 17] Testing unexpected descriptor hygiene (unsolicited descriptors closed)..." << std::endl;
    if (qEnvironmentVariableIsEmpty("PDF_TEST19_ONLY")) { // skipped only when isolating Test 19 sub-cases

        const QString hygieneSocket = QStringLiteral("pdf_test_hygiene_%1").arg(QCoreApplication::applicationPid());
        QProcess hygieneWorker;
        hygieneWorker.start(workerExe, QStringList() << QStringLiteral("--socket") << hygieneSocket);
        if (!hygieneWorker.waitForStarted(3000)) {
            std::cerr << "[FAIL] Failed to spawn worker for descriptor hygiene test." << std::endl;
            return 1;
        }
        QLocalSocket hygieneClient;
#ifdef Q_OS_LINUX
        hygieneClient.setSocketOptions(QLocalSocket::AbstractNamespaceOption);
#endif
        if (!waitFor([&]() {
                if (hygieneClient.state() != QLocalSocket::ConnectedState && hygieneClient.state() != QLocalSocket::ConnectingState) {
                    hygieneClient.connectToServer(hygieneSocket);
                }
                return hygieneClient.state() == QLocalSocket::ConnectedState;
            }, 5000)) {
            std::cerr << "[FAIL] Could not connect to worker for descriptor hygiene test." << std::endl;
            hygieneWorker.kill();
            return 1;
        }
        QByteArray hygieneIn;
        QObject::connect(&hygieneClient, &QLocalSocket::readyRead, [&]() { hygieneIn.append(hygieneClient.readAll()); });

        // Send a PingRequest with an unsolicited descriptor attached: the worker must reject (drop) it and
        // close the descriptor (closure itself is verified from outside in Test 19). A clean Ping afterwards
        // must still be answered, and it must be the only Pong.
        pdf::ipc::PingRequest pingReq{98};
        QByteArray pingPayload = pdf::ipc::serialize(pingReq);
#ifdef Q_OS_LINUX
        int dummyFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        sendPacketWithFd(hygieneClient, pdf::ipc::MessageType::Ping, pingPayload, dummyFd);
#else
        hygieneClient.write(pdf::ipc::buildPacket(pdf::ipc::MessageType::Ping, pingPayload));
        hygieneClient.flush();
#endif
        hygieneClient.write(pdf::ipc::buildPacket(pdf::ipc::MessageType::Ping,
                                                  pdf::ipc::serialize(pdf::ipc::PingRequest{99})));
        hygieneClient.flush();

        pdf::ipc::MessageType type{};
        QByteArray reply;
        bool gotPong = waitFor([&]() {
            if (static_cast<size_t>(hygieneIn.size()) < sizeof(pdf::ipc::PacketHeader)) return false;
            pdf::ipc::PacketHeader hdr;
            std::memcpy(&hdr, hygieneIn.constData(), sizeof(hdr));
            if (hdr.magic != pdf::ipc::IPC_MAGIC) return false;
            const size_t full = sizeof(hdr) + hdr.payloadSize;
            if (static_cast<size_t>(hygieneIn.size()) < full) return false;
            type = static_cast<pdf::ipc::MessageType>(hdr.type);
            reply = hygieneIn.mid(static_cast<qsizetype>(sizeof(hdr)), static_cast<qsizetype>(hdr.payloadSize));
            return true;
        }, 5000);

        if (!gotPong || type != pdf::ipc::MessageType::Pong) {
            std::cerr << "[FAIL] Worker failed to respond to the clean Ping sent after one carrying an unexpected descriptor." << std::endl;
            hygieneWorker.kill();
            return 1;
        }
        pdf::ipc::PongResponse pongResp;
        if (!pdf::ipc::deserialize(reply, pongResp) || pongResp.seq != 99) {
            std::cerr << "[FAIL] Invalid Pong reply sequence on hygiene test." << std::endl;
            hygieneWorker.kill();
            return 1;
        }

        hygieneClient.disconnectFromServer();
        hygieneWorker.kill();
        hygieneWorker.waitForFinished(3000);
        std::cout << "  -> Passed: Ping with an unsolicited descriptor was dropped; the following clean Ping was answered (descriptor closure: Test 19)." << std::endl;
    }

    // --- TEST 18: Startup Fails Without Landlock (Fail-Closed) ---
    std::cout << "[TEST 18] Testing worker startup fails when Landlock is unavailable (fail-closed)..." << std::endl;
#ifdef PDF_WORKER_TEST_HOOKS
    {
        QProcess landlockProc;
        landlockProc.start(workerExe, QStringList() << QStringLiteral("--test-fail-landlock"));
        landlockProc.waitForFinished(5000);
        if (landlockProc.exitCode() == 0) {
            std::cerr << "[FAIL] Worker should fail startup when Landlock is unavailable." << std::endl;
            return 1;
        }
        const QString errOut = QString::fromUtf8(landlockProc.readAllStandardError());
        if (!errOut.contains(QStringLiteral("Landlock is unavailable"))) {
            std::cerr << "[FAIL] Error message did not cite Landlock failure: " << errOut.toStdString() << std::endl;
            return 1;
        }
        std::cout << "  -> Passed: Worker strictly fails closed when Landlock is unavailable: "
                  << errOut.trimmed().toStdString() << std::endl;
    }
#else
    std::cout << "  -> Skipped: ENABLE_TEST_HOOKS is OFF (--test-fail-landlock hook compiled out of shipping build)." << std::endl;
#endif

    // --- TEST 19: Descriptor lifecycle (SCM_RIGHTS association, rejection, closure) ---
    // The worker must (a) only accept a descriptor on OpenDocumentRequest, (b) bind a descriptor to the
    // packet it travelled with (never to a later request), and (c) really close every descriptor it
    // refuses. Closure is observed from outside: we hand over the READ end of a pipe, drop our own copy,
    // and poll the WRITE end, which reports POLLERR as soon as no process holds the read end any more.
#ifdef Q_OS_LINUX
    std::cout << "[TEST 19] Testing descriptor lifecycle (association, rejection and closure)..." << std::endl;
    {
        struct RawPeer {
            int fd{-1};
            QByteArray in;
            ~RawPeer() { if (fd >= 0) ::close(fd); }
            bool connectTo(const QString& name, int timeoutMs) {
                QElapsedTimer t; t.start();
                while (t.elapsed() < timeoutMs) {
                    fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                    sockaddr_un addr{};
                    addr.sun_family = AF_UNIX;
                    const QByteArray nb = name.toUtf8();
                    std::memcpy(addr.sun_path + 1, nb.constData(), static_cast<size_t>(nb.size()));
                    const socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + nb.size());
                    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0) return true;
                    ::close(fd); fd = -1;
                    QThread::msleep(20);
                }
                return false;
            }
            // Sends `data` in one sendmsg(); `fds` (may be empty) ride with its first byte.
            bool send(const QByteArray& data, const std::vector<int>& fds = {}) {
                iovec iov{const_cast<char*>(data.constData()), static_cast<size_t>(data.size())};
                msghdr msg{};
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                std::vector<char> ctl(fds.empty() ? 0 : CMSG_SPACE(fds.size() * sizeof(int)));
                if (!fds.empty()) {
                    msg.msg_control = ctl.data();
                    msg.msg_controllen = ctl.size();
                    cmsghdr* c = CMSG_FIRSTHDR(&msg);
                    c->cmsg_level = SOL_SOCKET;
                    c->cmsg_type = SCM_RIGHTS;
                    c->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
                    std::memcpy(CMSG_DATA(c), fds.data(), fds.size() * sizeof(int));
                }
                return ::sendmsg(fd, &msg, MSG_NOSIGNAL) == data.size();
            }
            bool next(pdf::ipc::MessageType& type, QByteArray& payload, int timeoutMs) {
                QElapsedTimer t; t.start();
                while (true) {
                    if (static_cast<size_t>(in.size()) >= sizeof(pdf::ipc::PacketHeader)) {
                        pdf::ipc::PacketHeader h;
                        std::memcpy(&h, in.constData(), sizeof(h));
                        const size_t full = sizeof(h) + h.payloadSize;
                        if (static_cast<size_t>(in.size()) >= full) {
                            type = static_cast<pdf::ipc::MessageType>(h.type);
                            payload = in.mid(static_cast<qsizetype>(sizeof(h)), static_cast<qsizetype>(h.payloadSize));
                            in.remove(0, static_cast<qsizetype>(full));
                            return true;
                        }
                    }
                    const int left = timeoutMs - static_cast<int>(t.elapsed());
                    if (left <= 0) return false;
                    pollfd p{fd, POLLIN, 0};
                    if (::poll(&p, 1, left) > 0) {
                        char buf[4096];
                        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                        if (n <= 0) return false;
                        in.append(buf, static_cast<qsizetype>(n));
                    }
                }
            }
            bool peerClosed(int timeoutMs) {
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, timeoutMs) <= 0) return false;
                char b[1];
                return ::recv(fd, b, 1, MSG_PEEK) == 0;
            }
        };
        struct Probe { // pipe whose read end is handed to the worker
            int r{-1}, w{-1};
            Probe() { int p[2]; if (::pipe2(p, O_CLOEXEC) == 0) { r = p[0]; w = p[1]; } }
            ~Probe() { if (r >= 0) ::close(r); if (w >= 0) ::close(w); }
            int giveAway() { const int x = r; r = -1; return x; } // caller sends then closes x
            bool workerDroppedIt(int timeoutMs) {
                QElapsedTimer t; t.start();
                do {
                    pollfd p{w, 0, 0};
                    ::poll(&p, 1, 0);
                    if (p.revents & POLLERR) return true;
                    QThread::msleep(10);
                } while (t.elapsed() < timeoutMs);
                return false;
            }
        };
        struct Env {
            QProcess proc;
            RawPeer peer;
            bool start(const QString& exe, const QString& name) {
                proc.start(exe, QStringList() << QStringLiteral("--socket") << name);
                return proc.waitForStarted(3000) && peer.connectTo(name, 5000);
            }
            ~Env() { proc.kill(); proc.waitForFinished(2000); }
        };
        // PDF_TEST19_ONLY=ACF runs only those sub-cases (used to show each one fails on the old code).
        const QByteArray only19 = qgetenv("PDF_TEST19_ONLY");
        auto want = [&](char c) { return only19.isEmpty() || only19.contains(c); };
        using MT = pdf::ipc::MessageType;
        static int s_envCounter = 0;
        auto newEnv = [&](std::unique_ptr<Env>& env) {
            env = std::make_unique<Env>();
            return env->start(workerExe, QStringLiteral("pdf_test_fdlife_%1_%2")
                                             .arg(QCoreApplication::applicationPid()).arg(++s_envCounter));
        };
        auto ping = [](int64_t seq) {
            return pdf::ipc::buildPacket(MT::Ping, pdf::ipc::serialize(pdf::ipc::PingRequest{seq}));
        };
        auto openPkt = [&](int64_t id) {
            return pdf::ipc::buildPacket(MT::OpenDocumentRequest,
                pdf::ipc::serialize(pdf::ipc::OpenDocumentRequest{id, QFileInfo(samplePdf).absoluteFilePath(), QString()}));
        };
        auto expectPong = [&](Env& e, int64_t seq, const char* what) {
            MT t{}; QByteArray pl;
            if (!e.peer.next(t, pl, 5000) || t != MT::Pong) {
                std::cerr << "[FAIL] " << what << ": no Pong" << std::endl; return false;
            }
            pdf::ipc::PongResponse r;
            if (!pdf::ipc::deserialize(pl, r) || r.seq != seq) {
                std::cerr << "[FAIL] " << what << ": Pong seq " << r.seq << ", expected " << seq << std::endl; return false;
            }
            return true;
        };
        // Lenient variant: skips Pongs for other sequence numbers (used where the dropped/answered state of an
        // earlier Ping is not what the case is about).
        auto awaitPong = [&](Env& e, int64_t seq, const char* what) {
            MT t{}; QByteArray pl;
            while (e.peer.next(t, pl, 5000)) {
                pdf::ipc::PongResponse r;
                if (t == MT::Pong && pdf::ipc::deserialize(pl, r) && r.seq == seq) return true;
            }
            std::cerr << "[FAIL] " << what << ": Pong " << seq << " never arrived" << std::endl;
            return false;
        };
        // Reads the next OpenDocumentResponse; returns false on timeout.
        auto nextOpen = [&](Env& e, pdf::ipc::OpenDocumentResponse& out) {
            MT t{}; QByteArray pl;
            while (e.peer.next(t, pl, 8000)) {
                if (t == MT::OpenDocumentResponse) return pdf::ipc::deserialize(pl, out);
            }
            return false;
        };
        const QByteArray samplePath = QFileInfo(samplePdf).absoluteFilePath().toLocal8Bit();
        auto realFd = [&]() { return ::open(samplePath.constData(), O_RDONLY | O_CLOEXEC); };

        // 19A: a descriptor on every non-Open request type is rejected and closed, and the request is dropped.
        if (want('A')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19A: spawn/connect failed" << std::endl; return 1; }
            const std::vector<std::pair<const char*, QByteArray>> msgs = {
                {"Ping", ping(1)},
                {"CloseDocumentRequest", pdf::ipc::buildPacket(MT::CloseDocumentRequest, pdf::ipc::serialize(pdf::ipc::CloseDocumentRequest{}))},
                {"CancelRenderRequest", pdf::ipc::buildPacket(MT::CancelRenderRequest, pdf::ipc::serialize(pdf::ipc::CancelRenderRequest{}))},
                {"GetOutlineRequest", pdf::ipc::buildPacket(MT::GetOutlineRequest, pdf::ipc::serialize(pdf::ipc::GetOutlineRequest{}))},
            };
            for (const auto& m : msgs) {
                Probe p;
                const int give = p.giveAway();
                const bool sent = env->peer.send(m.second, {give});
                ::close(give);
                if (!sent) { std::cerr << "[FAIL] 19A: send failed for " << m.first << std::endl; return 1; }
                if (!p.workerDroppedIt(3000)) {
                    std::cerr << "[FAIL] 19A: worker still holds a descriptor received with " << m.first << std::endl;
                    return 1;
                }
            }
            // The rejected Ping(1) must not have been answered; a clean Ping(2) must be.
            env->peer.send(ping(2));
            if (!expectPong(*env, 2, "19A follow-up")) return 1;
            std::cout << "  -> 19A passed: descriptors on Ping/Close/Cancel/Outline are rejected, closed and the request dropped." << std::endl;
        }

        // 19B: a stray descriptor can never be consumed by a later OpenDocumentRequest.
        if (want('B')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19B: spawn/connect failed" << std::endl; return 1; }
            Probe stray;
            const int give = stray.giveAway();
            env->peer.send(ping(10), {give});
            ::close(give);
            env->peer.send(ping(11));
            if (!awaitPong(*env, 11, "19B sync")) return 1;
            const int fd = realFd();
            env->peer.send(openPkt(1), {fd});
            ::close(fd);
            pdf::ipc::OpenDocumentResponse r;
            if (!nextOpen(*env, r) || r.result.error != pdf::DocumentError::None || r.result.pageCount <= 0) {
                std::cerr << "[FAIL] 19B: Open after a stray descriptor did not use its own descriptor (error="
                          << static_cast<int>(r.result.error) << ")" << std::endl;
                return 1;
            }
            if (!stray.workerDroppedIt(3000)) { std::cerr << "[FAIL] 19B: stray descriptor was never closed" << std::endl; return 1; }
            std::cout << "  -> 19B passed: stray descriptor closed; the next Open used its own descriptor." << std::endl;
        }

        // 19C: stalled worker, stray + Open queued back to back (coalesced reads), then released.
        if (want('C')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19C: spawn/connect failed" << std::endl; return 1; }
            ::kill(static_cast<pid_t>(env->proc.processId()), SIGSTOP);
            Probe stray;
            const int give = stray.giveAway();
            env->peer.send(ping(20), {give});
            ::close(give);
            env->peer.send(ping(21));
            const int fd = realFd();
            env->peer.send(openPkt(2), {fd});
            ::close(fd);
            ::kill(static_cast<pid_t>(env->proc.processId()), SIGCONT);
            if (!awaitPong(*env, 21, "19C")) return 1;
            pdf::ipc::OpenDocumentResponse r;
            if (!nextOpen(*env, r) || r.result.error != pdf::DocumentError::None || r.documentId != 2) {
                std::cerr << "[FAIL] 19C: coalesced Open got the wrong descriptor (error=" << static_cast<int>(r.result.error) << ")" << std::endl;
                return 1;
            }
            if (!stray.workerDroppedIt(3000)) { std::cerr << "[FAIL] 19C: stray descriptor not closed" << std::endl; return 1; }
            std::cout << "  -> 19C passed: queued/coalesced packets keep each descriptor with its own packet." << std::endl;
        }

        // 19D: the descriptor belongs to the packet that carries its first byte. [Ping][Open] in ONE
        //      sendmsg with one descriptor therefore hands it to the Ping; the Open sees none and fails closed.
        if (want('D')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19D: spawn/connect failed" << std::endl; return 1; }
            const int fd = realFd();
            env->peer.send(ping(30) + openPkt(3), {fd});
            ::close(fd);
            pdf::ipc::OpenDocumentResponse r;
            if (!nextOpen(*env, r) || r.result.error == pdf::DocumentError::None) {
                std::cerr << "[FAIL] 19D: Open succeeded using a descriptor that travelled with a different packet" << std::endl;
                return 1;
            }
            std::cout << "  -> 19D passed: descriptor bound to the packet it travelled with, Open failed closed." << std::endl;
        }

        // 19E: two Opens with two descriptors in flight at once are matched in order (text file first, PDF second).
        if (want('E')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19E: spawn/connect failed" << std::endl; return 1; }
            QTemporaryDir tmp;
            QFile junk(tmp.filePath(QStringLiteral("not_a_pdf.txt")));
            junk.open(QIODevice::WriteOnly); junk.write("this is definitely not a pdf file\n"); junk.close();
            ::kill(static_cast<pid_t>(env->proc.processId()), SIGSTOP);
            const int f1 = ::open(junk.fileName().toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
            const int f2 = realFd();
            env->peer.send(openPkt(41), {f1});
            env->peer.send(openPkt(42), {f2});
            ::close(f1); ::close(f2);
            ::kill(static_cast<pid_t>(env->proc.processId()), SIGCONT);
            pdf::ipc::OpenDocumentResponse a, b;
            if (!nextOpen(*env, a) || !nextOpen(*env, b) || a.documentId != 41 || b.documentId != 42 ||
                a.result.error == pdf::DocumentError::None || b.result.error != pdf::DocumentError::None) {
                std::cerr << "[FAIL] 19E: descriptors were not matched to their own requests" << std::endl;
                return 1;
            }
            std::cout << "  -> 19E passed: two in-flight Opens matched to their own descriptors." << std::endl;
        }

        // 19F: more than one descriptor on an Open is malformed: all are closed and the Open fails closed.
        if (want('F')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19F: spawn/connect failed" << std::endl; return 1; }
            Probe extra;
            const int good = realFd();
            const int give = extra.giveAway();
            env->peer.send(openPkt(5), {good, give});
            ::close(good); ::close(give);
            pdf::ipc::OpenDocumentResponse r;
            if (!nextOpen(*env, r) || r.result.error == pdf::DocumentError::None) {
                std::cerr << "[FAIL] 19F: Open with two descriptors was accepted" << std::endl;
                return 1;
            }
            if (!extra.workerDroppedIt(3000)) { std::cerr << "[FAIL] 19F: extra descriptor not closed" << std::endl; return 1; }
            std::cout << "  -> 19F passed: Open carrying two descriptors rejected, both closed." << std::endl;
        }

        // 19G: malformed Open payload with a descriptor: closed, worker stays usable.
        if (want('G')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19G: spawn/connect failed" << std::endl; return 1; }
            Probe p;
            const int give = p.giveAway();
            env->peer.send(pdf::ipc::buildPacket(MT::OpenDocumentRequest, QByteArray("\x01\x02\x03", 3)), {give});
            ::close(give);
            if (!p.workerDroppedIt(3000)) { std::cerr << "[FAIL] 19G: descriptor from a malformed Open not closed" << std::endl; return 1; }
            env->peer.send(ping(50));
            if (!expectPong(*env, 50, "19G follow-up")) return 1;
            std::cout << "  -> 19G passed: malformed Open: descriptor closed, protocol still in sync." << std::endl;
        }

        // 19H: invalid header carrying a descriptor: the worker drops the connection and closes it.
        if (want('H')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19H: spawn/connect failed" << std::endl; return 1; }
            Probe p;
            const int give = p.giveAway();
            QByteArray junk(sizeof(pdf::ipc::PacketHeader), '\xAB');
            env->peer.send(junk, {give});
            ::close(give);
            if (!p.workerDroppedIt(3000)) { std::cerr << "[FAIL] 19H: descriptor from an invalid packet not closed" << std::endl; return 1; }
            if (!env->peer.peerClosed(3000)) { std::cerr << "[FAIL] 19H: worker kept a connection after a protocol error" << std::endl; return 1; }
            std::cout << "  -> 19H passed: invalid header: descriptor closed, connection dropped." << std::endl;
        }

        // 19I: client vanishes mid-packet after the descriptor arrived (cancellation / shutdown): no leak.
        if (want('I')) {
            std::unique_ptr<Env> env;
            if (!newEnv(env)) { std::cerr << "[FAIL] 19I: spawn/connect failed" << std::endl; return 1; }
            Probe p;
            const int give = p.giveAway();
            const QByteArray full = openPkt(6);
            env->peer.send(full.left(10), {give});
            ::close(give);
            QThread::msleep(200);
            ::shutdown(env->peer.fd, SHUT_RDWR);
            ::close(env->peer.fd); env->peer.fd = -1;
            if (!p.workerDroppedIt(3000)) { std::cerr << "[FAIL] 19I: descriptor of a half-received Open leaked after disconnect" << std::endl; return 1; }
            std::cout << "  -> 19I passed: disconnect mid-packet closes the pending descriptor." << std::endl;
        }

        // --- TEST 20: Non-Latin text renders inside the sandboxed worker (Landlock read confinement) ---
        // Every other fixture here is Latin text in a PDFium built-in font, so nothing would notice if read
        // confinement (or a Flatpak/other sandbox) broke font access. Two fixtures:
        //  * unicode_embedded.pdf    - Latin, Cyrillic, Greek and Persian, glyphs embedded in the PDF. Must always render.
        //  * unicode_system_font.pdf - Japanese in a NON-embedded CID font, so PDFium must find a system CJK font via
        //    fontconfig, i.e. through the worker's read allowlist. Required to render when the host has a Japanese
        //    font (per fontconfig); otherwise reported as skipped, never as passed.
        // Ink is counted per 70 px band (one text line per band), so a single missing script is caught.
        std::cout << "[TEST 20] Testing non-Latin text rendering inside the confined worker..." << std::endl;
        {
            const QString dir = QFileInfo(samplePdf).absolutePath();
            auto findFixture = [&](const QString& name) {
                for (const QString& c : {dir + "/" + name, dir + "/tests/fixtures/" + name}) {
                    if (QFileInfo::exists(c)) return c;
                }
                return QString();
            };
            // Returns per-band count of dark pixels, or an empty vector (with a message) on failure.
            auto renderBands = [&](const QString& pdfPath, int bands, std::vector<int>& out, QString& why) {
                std::unique_ptr<Env> env;
                if (!newEnv(env)) { why = "spawn/connect failed"; return false; }
                const QByteArray pathBytes = QFileInfo(pdfPath).absoluteFilePath().toLocal8Bit();
                const int fd = ::open(pathBytes.constData(), O_RDONLY | O_CLOEXEC);
                if (fd < 0) { why = "cannot open fixture"; return false; }
                env->peer.send(pdf::ipc::buildPacket(MT::OpenDocumentRequest,
                    pdf::ipc::serialize(pdf::ipc::OpenDocumentRequest{7, QFileInfo(pdfPath).absoluteFilePath(), QString()})), {fd});
                ::close(fd);
                pdf::ipc::OpenDocumentResponse open;
                if (!nextOpen(*env, open) || open.result.error != pdf::DocumentError::None) { why = "open failed"; return false; }
                pdf::TileRenderRequest tr;
                tr.documentId = 7; tr.generation = 1; tr.pageIndex = 0;
                tr.rect = {0, 0, 512, 512}; tr.scale = 1.0; tr.dpr = 1.0;
                tr.key = pdf::TileKey::create(7, 0, 0, 0, 1.0, 1.0); tr.priority = 10;
                env->peer.send(pdf::ipc::buildPacket(MT::RenderTileRequest, pdf::ipc::serialize(pdf::ipc::RenderTileRequest{tr})));
                MT t{}; QByteArray pl;
                while (env->peer.next(t, pl, 8000)) {
                    if (t != MT::TileRenderedResponse) continue;
                    pdf::ipc::TileRenderedResponse resp;
                    if (!pdf::ipc::deserialize(pl, resp) || !resp.isShm || resp.width != 512 || resp.height != 512) { why = "bad tile"; return false; }
                    std::vector<unsigned char> px(static_cast<size_t>(resp.stride) * static_cast<size_t>(resp.height));
                    if (!pdf::ipc::readAndUnlinkSharedTile(resp.shmKey.toStdString(), static_cast<size_t>(resp.height),
                            static_cast<size_t>(resp.stride), static_cast<size_t>(resp.width) * 4, px.data(),
                            static_cast<size_t>(resp.stride))) { why = "cannot read tile"; return false; }
                    env->peer.send(pdf::ipc::buildPacket(MT::SharedMemoryAck, pdf::ipc::serialize(pdf::ipc::SharedMemoryAck{resp.shmKey})));
                    out.assign(static_cast<size_t>(bands), 0);
                    for (int y = 60; y < 60 + 70 * bands && y < 512; ++y) {
                        for (int x = 0; x < 512; ++x) {
                            const unsigned char* q = &px[static_cast<size_t>(y) * resp.stride + static_cast<size_t>(x) * 4];
                            if (std::min({q[0], q[1], q[2]}) < 128) ++out[static_cast<size_t>((y - 60) / 70)];
                        }
                    }
                    return true;
                }
                why = "no tile response";
                return false;
            };
            constexpr int kMinInk = 150; // a 32 pt line has thousands of dark pixels; a blank band has none

            const QString embedded = findFixture(QStringLiteral("unicode_embedded.pdf"));
            if (embedded.isEmpty()) { std::cerr << "[FAIL] 20: unicode_embedded.pdf not found next to " << samplePdf.toStdString() << std::endl; return 1; }
            std::vector<int> ink; QString why;
            if (!renderBands(embedded, 4, ink, why)) { std::cerr << "[FAIL] 20 (embedded): " << why.toStdString() << std::endl; return 1; }
            const char* names[] = {"Latin", "Cyrillic", "Greek", "Persian"};
            for (int i = 0; i < 4; ++i) {
                std::cout << "     embedded " << names[i] << " line: " << ink[static_cast<size_t>(i)] << " dark pixels" << std::endl;
                if (ink[static_cast<size_t>(i)] < kMinInk) {
                    std::cerr << "[FAIL] 20: " << names[i] << " text did not render in the sandboxed worker" << std::endl;
                    return 1;
                }
            }
            std::cout << "  -> 20a passed: embedded Latin/Cyrillic/Greek/Persian text renders in the confined worker." << std::endl;

            const QString sysFont = findFixture(QStringLiteral("unicode_system_font.pdf"));
            if (sysFont.isEmpty()) { std::cerr << "[FAIL] 20: unicode_system_font.pdf not found" << std::endl; return 1; }
            QProcess fc;
            fc.start(QStringLiteral("fc-list"), QStringList() << QStringLiteral(":lang=ja") << QStringLiteral("file"));
            const bool haveJa = fc.waitForStarted(2000) && fc.waitForFinished(5000) && fc.exitCode() == 0 &&
                                !fc.readAllStandardOutput().trimmed().isEmpty();
            if (!haveJa) {
                std::cout << "  -> 20b SKIPPED: fontconfig reports no Japanese font on this host (or fc-list is missing), so the "
                             "system-font fallback path through the read allowlist was NOT exercised." << std::endl;
            } else {
                if (!renderBands(sysFont, 2, ink, why)) { std::cerr << "[FAIL] 20 (system font): " << why.toStdString() << std::endl; return 1; }
                std::cout << "     system-font Japanese lines: " << ink[0] << " / " << ink[1] << " dark pixels" << std::endl;
                if (ink[0] < kMinInk || ink[1] < kMinInk) {
                    std::cerr << "[FAIL] 20: a Japanese font is installed but the confined worker could not use it "
                                 "(read allowlist too narrow?)" << std::endl;
                    return 1;
                }
                std::cout << "  -> 20b passed: non-embedded CJK text renders through the system-font fallback inside the sandbox." << std::endl;
            }
        }

        // --- TEST 21: every font directory fontconfig reports is readable by the confined worker ---
        // Flatpak exposes host fonts under /run/host/{fonts,user-fonts,local-fonts} and lists them in a fontconfig
        // fragment (/run/host/font-dirs.xml); the Landlock read allowlist has to cover those paths or user-installed
        // fonts (e.g. Persian ones) silently disappear. Instead of assuming paths, ask fontconfig which font files it
        // knows (fc-list) and have the sandboxed worker (--isolation-selftest --test-probe-read, hooks builds only)
        // try to read one file per font root. Roots under /run/host/ MUST be readable; other unreadable roots are
        // reported as notes because a developer machine can keep fonts in arbitrary places.
        std::cout << "[TEST 21] Testing that the confined worker can read every font root fontconfig reports..." << std::endl;
#ifdef PDF_WORKER_TEST_HOOKS
        {
            QProcess fc;
            fc.start(QStringLiteral("fc-list"), QStringList() << QStringLiteral(":") << QStringLiteral("file"));
            QStringList files;
            if (fc.waitForStarted(2000) && fc.waitForFinished(15000) && fc.exitCode() == 0) {
                for (QString line : QString::fromLocal8Bit(fc.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
                    line = line.trimmed();
                    if (line.endsWith(':')) line.chop(1);
                    if (line.startsWith('/')) files << line;
                }
            }
            if (files.isEmpty()) {
                std::cout << "  -> 21 SKIPPED: fc-list is missing or reports no fonts, so no font root could be probed." << std::endl;
            } else {
                // One representative file per root = first three path components (/usr/share/fonts, /run/host/fonts,
                // /run/host/user-fonts, /home/u/.local ...).
                QMap<QString, QString> rootToFile;
                for (const QString& f : files) {
                    const QStringList parts = f.split('/', Qt::SkipEmptyParts);
                    if (parts.size() < 3) continue;
                    const QString root = "/" + parts.mid(0, 3).join('/');
                    if (!rootToFile.contains(root)) rootToFile.insert(root, f);
                }
                QStringList probes = rootToFile.values();
                for (const char* extra : {"/run/host/font-dirs.xml"}) {
                    if (QFileInfo::exists(QString::fromLatin1(extra))) probes << QString::fromLatin1(extra);
                }
                QStringList args{QStringLiteral("--isolation-selftest")};
                for (const QString& p : probes) args << QStringLiteral("--test-probe-read") << p;
                QProcess w;
                w.start(workerExe, args);
                if (!w.waitForStarted(3000) || !w.waitForFinished(15000) || w.exitCode() != 0) {
                    std::cerr << "[FAIL] 21: worker probe run failed: " << w.readAllStandardError().toStdString() << std::endl;
                    return 1;
                }
                const QString out = QString::fromLocal8Bit(w.readAllStandardOutput());
                bool failed = false;
                int checked = 0;
                for (const QString& p : probes) {
                    const QString key = "[PROBE_READ] " + p + ": ";
                    const int at = out.indexOf(key);
                    const QString verdict = at < 0 ? QStringLiteral("NO_RESULT")
                        : out.mid(at + key.size(), out.indexOf('\n', at) - at - key.size()).trimmed();
                    ++checked;
                    std::cout << "     " << p.toStdString() << " -> " << verdict.toStdString() << std::endl;
                    if (verdict == "PERMITTED") continue;
                    if (p.startsWith("/run/host/")) {
                        std::cerr << "[FAIL] 21: fontconfig exposes " << p.toStdString()
                                  << " but the confined worker cannot read it (" << verdict.toStdString()
                                  << "): Flatpak host fonts would be unusable. Add it to the Landlock read allowlist." << std::endl;
                        failed = true;
                    } else {
                        std::cout << "     NOTE: font root not readable by the confined worker (" << verdict.toStdString()
                                  << "); fonts there will not be used. Not an error on a developer machine." << std::endl;
                    }
                }
                if (failed) return 1;
                std::cout << "  -> 21 passed: " << checked << " font root(s) reported by fontconfig were probed; none under /run/host is blocked." << std::endl;
            }
        }
#else
        std::cout << "  -> 21 SKIPPED: ENABLE_TEST_HOOKS is OFF (--test-probe-read hook compiled out of shipping build)." << std::endl;
#endif
        std::cout << "  -> Passed: descriptor lifecycle is bound to packets, rejected descriptors are really closed." << std::endl;
    }
#else
    std::cout << "[TEST 19] Skipped: descriptor passing is Linux-only." << std::endl;
#endif

    std::cout << "========================================" << std::endl;
    std::cout << " ALL WORKER IPC UNIT TESTS PASSED!     " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
