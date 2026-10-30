#include "ipc/IpcProtocol.h"
#include "ipc/IpcMessages.h"

#include <QByteArray>
#include <QCoreApplication>
#include <iostream>
#include <vector>
#include <random>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace {

using namespace pdf::ipc;

void testPayloadWithAllTypes(const uint8_t* data, size_t size) {
    const QByteArray payload(reinterpret_cast<const char*>(data), static_cast<qsizetype>(size));

    // Try deserializing the raw payload into every known message type.
    // Invariant: arbitrary bytes must either deserialize cleanly or return false;
    // they must NEVER crash, read out-of-bounds, or throw unhandled exceptions.

    {
        OpenDocumentRequest msg;
        deserialize(payload, msg);
    }
    {
        OpenDocumentResponse msg;
        deserialize(payload, msg);
    }
    {
        CloseDocumentRequest msg;
        deserialize(payload, msg);
    }
    {
        GetOutlineRequest msg;
        deserialize(payload, msg);
    }
    {
        GetOutlineResponse msg;
        deserialize(payload, msg);
    }
    {
        RenderTileRequest msg;
        deserialize(payload, msg);
    }
    {
        TileRenderedResponse msg;
        deserialize(payload, msg);
    }
    {
        TileFailedResponse msg;
        deserialize(payload, msg);
    }
    {
        CancelRenderRequest msg;
        deserialize(payload, msg);
    }
    {
        SetGenerationRequest msg;
        deserialize(payload, msg);
    }
    {
        PageTextRequest msg;
        deserialize(payload, msg);
    }
    {
        PageTextResponse msg;
        deserialize(payload, msg);
    }
    {
        StartSearchRequest msg;
        deserialize(payload, msg);
    }
    {
        CancelSearchRequest msg;
        deserialize(payload, msg);
    }
    {
        SearchMatchesFoundResponse msg;
        deserialize(payload, msg);
    }
    {
        SearchProgressResponse msg;
        deserialize(payload, msg);
    }
    {
        SearchCompletedResponse msg;
        deserialize(payload, msg);
    }
    {
        PingRequest msg;
        deserialize(payload, msg);
    }
    {
        PongResponse msg;
        deserialize(payload, msg);
    }
    {
        WorkerControlRequest msg;
        deserialize(payload, msg);
    }
    {
        WorkerControlResponse msg;
        deserialize(payload, msg);
    }
    {
        ThumbnailRequest msg;
        deserialize(payload, msg);
    }
    {
        ThumbnailResponse msg;
        deserialize(payload, msg);
    }
    {
        ThumbnailFailedResponse msg;
        deserialize(payload, msg);
    }
    {
        RenderPageRequest msg;
        deserialize(payload, msg);
    }
    {
        RenderPageResponse msg;
        deserialize(payload, msg);
    }
    {
        RenderPageFailedResponse msg;
        deserialize(payload, msg);
    }
    {
        ExtractPagesRequest msg;
        deserialize(payload, msg);
    }
    {
        ExtractPagesResponse msg;
        deserialize(payload, msg);
    }
    {
        ExtractPagesFailedResponse msg;
        deserialize(payload, msg);
    }
    {
        RedactDocumentRequest msg;
        deserialize(payload, msg);
    }
    {
        RedactDocumentResponse msg;
        deserialize(payload, msg);
    }
    {
        RedactDocumentFailedResponse msg;
        deserialize(payload, msg);
    }
    {
        ListAnnotationsRequest msg;
        deserialize(payload, msg);
    }
    {
        ListAnnotationsResponse msg;
        deserialize(payload, msg);
    }
    {
        HitTestAnnotationRequest msg;
        deserialize(payload, msg);
    }
    {
        HitTestAnnotationResponse msg;
        deserialize(payload, msg);
    }
}

} // namespace

// Standard libFuzzer entry point when built with -fsanitize=fuzzer
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    testPayloadWithAllTypes(data, size);
    return 0;
}

#ifndef LIBFUZZER_ENTRY_ONLY
// Standalone fuzz runner for CTest / deterministic regression testing
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::cout << "[IPC FUZZ TEST] Starting deterministic IPC message deserialization fuzz test..." << std::endl;

    // 1. Boundary & edge-case buffers
    std::vector<std::vector<uint8_t>> corpus = {
        {}, // empty
        {0}, // single zero
        {0xFF}, // single 0xFF
        {0, 0, 0, 0}, // 4 zero bytes
        {0x50, 0x44, 0x46, 0x57}, // magic header only
        std::vector<uint8_t>(8, 0xFF),
        std::vector<uint8_t>(16, 0x00),
        std::vector<uint8_t>(64, 0xAA),
        std::vector<uint8_t>(1024, 0x55),
        std::vector<uint8_t>(65536, 0x00) // 64 KiB zeroes
    };

    // 2. Valid serialized seed messages
    {
        OpenDocumentRequest req{101, QStringLiteral("sample.pdf"), QStringLiteral("secret123")};
        QByteArray b = serialize(req);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        TileRenderedResponse resp;
        resp.documentId = 42;
        resp.generation = 7;
        resp.key = pdf::TileKey::create(42, 0, 0, 0, 1.5, 1.0, 0);
        resp.width = 256;
        resp.height = 256;
        resp.stride = 1024;
        resp.format = 1;
        resp.pixelData = QByteArray(1024, 'X');
        QByteArray b = serialize(resp);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        StartSearchRequest req{999, 1, QString::fromUtf8("بحث فارسی"), true, 10, 0};
        QByteArray b = serialize(req);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        WorkerControlRequest req{123, 50, 100, true, true};
        QByteArray b = serialize(req);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        RedactDocumentRequest req;
        req.documentId = 55;
        req.dpi = 150.0;
        req.edgeGrowPx = 2;
        PageRedactionItem item;
        item.pageIndex = 0;
        item.rects.push_back(RedactRect{10.0, 20.0, 100.0, 50.0});
        req.redactions.push_back(item);
        QByteArray b = serialize(req);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        RedactDocumentResponse res;
        res.documentId = 55;
        res.success = true;
        res.pdfBytes = QByteArray(512, '%');
        res.pageCount = 1;
        res.warnings.push_back(QStringLiteral("Signatures invalidated"));
        res.verification.push_back(QStringLiteral("Checks passed"));
        QByteArray b = serialize(res);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        ListAnnotationsRequest req{42, 3};
        QByteArray b = serialize(req);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        ListAnnotationsResponse res;
        res.documentId = 42;
        res.pageIndex = 0;
        res.success = true;
        pdf::AnnotationItem item;
        item.pageIndex = 0;
        item.annotIndex = 0;
        item.subtype = pdf::AnnotationSubtype::Highlight;
        item.left = 72.0; item.top = 700.0; item.right = 200.0; item.bottom = 680.0;
        item.contents = QStringLiteral("Sample highlight note");
        item.author = QStringLiteral("Reviewer");
        item.name = QStringLiteral("annot-1");
        item.hasColor = true;
        item.colorArgb = 0xFFFFFF00;
        item.quadPoints.push_back(pdf::AnnotationQuad{72.0, 700.0, 200.0, 700.0, 72.0, 680.0, 200.0, 680.0});
        res.annotations.push_back(item);

        pdf::AnnotationItem link;
        link.pageIndex = 0;
        link.annotIndex = 1;
        link.subtype = pdf::AnnotationSubtype::Link;
        link.left = 72.0; link.top = 200.0; link.right = 250.0; link.bottom = 180.0;
        link.uri = QStringLiteral("https://example.org");
        res.annotations.push_back(link);

        QByteArray b = serialize(res);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        HitTestAnnotationRequest req{42, 0, 100.5, 690.0};
        QByteArray b = serialize(req);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }
    {
        HitTestAnnotationResponse res;
        res.documentId = 42;
        res.pageIndex = 0;
        res.hit = true;
        res.annotation.pageIndex = 0;
        res.annotation.subtype = pdf::AnnotationSubtype::Text;
        res.annotation.left = 50.0; res.annotation.top = 100.0;
        res.annotation.right = 70.0; res.annotation.bottom = 80.0;
        res.annotation.contents = QStringLiteral("Note contents");
        QByteArray b = serialize(res);
        corpus.push_back(std::vector<uint8_t>(b.begin(), b.end()));
    }

    // Run baseline corpus
    for (const auto& item : corpus) {
        testPayloadWithAllTypes(item.data(), item.size());
    }

    // 3. Deterministic pseudo-random mutation fuzzing (1000 iterations)
    std::mt19937 rng(1337);
    int iterations = 1000;
    for (int i = 0; i < iterations; ++i) {
        // Pick a base from corpus
        const auto& base = corpus[rng() % corpus.size()];
        std::vector<uint8_t> mutated = base;

        // Apply 1 to 8 random mutations
        int mutations = 1 + (rng() % 8);
        for (int m = 0; m < mutations; ++m) {
            int op = rng() % 5;
            switch (op) {
                case 0: // Bit flip
                    if (!mutated.empty()) {
                        size_t pos = rng() % mutated.size();
                        mutated[pos] ^= static_cast<uint8_t>(1 << (rng() % 8));
                    }
                    break;
                case 1: // Byte overwrite with extreme value
                    if (!mutated.empty()) {
                        size_t pos = rng() % mutated.size();
                        uint8_t extremes[] = {0x00, 0xFF, 0x7F, 0x80, 0x01};
                        mutated[pos] = extremes[rng() % 5];
                    }
                    break;
                case 2: // Truncation
                    if (!mutated.empty()) {
                        size_t newSize = rng() % mutated.size();
                        mutated.resize(newSize);
                    }
                    break;
                case 3: // Insertion of random bytes
                    {
                        size_t insertCount = 1 + (rng() % 16);
                        size_t pos = mutated.empty() ? 0 : (rng() % mutated.size());
                        std::vector<uint8_t> extra(insertCount);
                        for (auto& b : extra) b = static_cast<uint8_t>(rng() & 0xFF);
                        mutated.insert(mutated.begin() + static_cast<long>(pos), extra.begin(), extra.end());
                    }
                    break;
                case 4: // Insert large int32/int64 values (to attack length / collection headers)
                    {
                        uint32_t val = (rng() % 2 == 0) ? 0x7FFFFFFF : 0xFFFFFFFF;
                        uint8_t bytes[4];
                        std::memcpy(bytes, &val, 4);
                        size_t pos = mutated.empty() ? 0 : (rng() % mutated.size());
                        mutated.insert(mutated.begin() + static_cast<long>(pos), bytes, bytes + 4);
                    }
                    break;
            }
        }

        testPayloadWithAllTypes(mutated.data(), mutated.size());
    }

    std::cout << "  -> Passed: " << iterations << " mutated payloads survived without crash or memory corruption." << std::endl;
    return 0;
}
#endif
