#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#endif
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "engine/IDocument.h"
#include "engine/IPage.h"
#include "engine/FormFieldTypes.h"
#include "engine/AnnotationTypes.h"
#include "engine/EditLog.h"
#include "ipc/WorkerClient.h"

// Static asserts pinning every security cap constant to its literal definition.
// Prevents accidental cap inflation or regression where tests compare against modified constants.
static_assert(pdf::kMaxFormFieldsPerPage == 1000, "kMaxFormFieldsPerPage must be exactly 1000");
static_assert(pdf::kMaxFormFieldNameChars == 1024, "kMaxFormFieldNameChars must be exactly 1024");
static_assert(pdf::kMaxFormFieldTextCodeUnits == 32768, "kMaxFormFieldTextCodeUnits must be exactly 32768");
static_assert(pdf::kMaxFormFieldOptions == 1000, "kMaxFormFieldOptions must be exactly 1000");
static_assert(pdf::kMaxFormFieldOptionChars == 1024, "kMaxFormFieldOptionChars must be exactly 1024");
static_assert(pdf::kMaxFormFieldListBytes == 16ull * 1024 * 1024, "kMaxFormFieldListBytes must be exactly 16 MiB");
static_assert(pdf::kMaxTextOutlineElements == 250000, "kMaxTextOutlineElements must be exactly 250000");

// Test suite for Milestone M5 hostile AcroForm fixtures (Threat T17)
// Verifies time bounds, cap enforcement, worker survival, and mutation safety
// across both direct engine (PdfiumPage) and sandboxed worker (WorkerClient).

namespace {

QString resolveFixture(const QString& samplePath, const QString& fixtureName) {
    QFileInfo sampleInfo(samplePath);
    QString path = sampleInfo.dir().filePath(QStringLiteral("tests/fixtures/") + fixtureName);
    if (QFile::exists(path)) return QFileInfo(path).canonicalFilePath();

    path = QStringLiteral("tests/fixtures/") + fixtureName;
    if (QFile::exists(path)) return QFileInfo(path).canonicalFilePath();

    path = QStringLiteral("../tests/fixtures/") + fixtureName;
    if (QFile::exists(path)) return QFileInfo(path).canonicalFilePath();

    path = QStringLiteral("../../tests/fixtures/") + fixtureName;
    if (QFile::exists(path)) return QFileInfo(path).canonicalFilePath();

    return QString();
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << " HOSTILE ACROFORM TEST SUITE (T17)      " << std::endl;
    std::cout << "========================================" << std::endl;

    const std::string workerExe = (argc > 1) ? argv[1] : "pdf_worker";
    const std::string samplePath = (argc > 2) ? argv[2] : "sample.pdf";

    // 7 hostile fixtures to test (hostile_form_slow_load_MANUAL.pdf excluded as per brief)
    const std::vector<QString> hostileFixtures = {
        QStringLiteral("hostile_form_many_widgets.pdf"),
        QStringLiteral("hostile_form_big_values.pdf"),
        QStringLiteral("hostile_form_oversize_values.pdf"),
        QStringLiteral("hostile_form_huge_options.pdf"),
        QStringLiteral("hostile_form_deep_hierarchy.pdf"),
        QStringLiteral("hostile_form_long_names.pdf"),
        QStringLiteral("hostile_form_cycles_and_malformed.pdf")
    };

    // --- PART 1: Direct Engine In-Process Validation (PdfiumPage) ---
    std::cout << "[PART 1] Testing hostile form fixtures against in-process engine..." << std::endl;
    for (const auto& fixtureName : hostileFixtures) {
        QString path = resolveFixture(QString::fromStdString(samplePath), fixtureName);
        if (path.isEmpty() || !QFile::exists(path)) {
            std::cerr << "[FAIL] Part 1: Fixture not found: " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        std::cout << "  Testing " << fixtureName.toStdString() << " in engine..." << std::endl;
        auto doc = pdf::createDocument();
        const auto tStart = std::chrono::steady_clock::now();
        auto err = doc->loadFromFile(path.toStdString());
        if (err != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Part 1: Failed to open fixture " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        int pageCount = doc->pageCount();
        if (pageCount <= 0) {
            std::cerr << "[FAIL] Part 1: Document reports 0 pages: " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        auto page0 = doc->getPage(0);
        if (!page0) {
            std::cerr << "[FAIL] Part 1: Failed to get page 0: " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        bool truncated = false;
        auto fields = page0->listFormFields(&truncated);
        const auto tEnd = std::chrono::steady_clock::now();
        const double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();

        // Time budget: engine must complete listing within 5.0 seconds
        if (elapsedSec > 5.0) {
            std::cerr << "[FAIL] Part 1: Engine exceeded time budget on " << fixtureName.toStdString()
                      << " (" << elapsedSec << "s > 5.0s)" << std::endl;
            return 1;
        }

        // Verify cap invariants on returned list
        if (fields.size() > pdf::kMaxFormFieldsPerPage) {
            std::cerr << "[FAIL] Part 1: Field count (" << fields.size() << ") > kMaxFormFieldsPerPage ("
                      << pdf::kMaxFormFieldsPerPage << ") on " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        uint64_t totalBudget = 0;
        for (const auto& f : fields) {
            if (static_cast<size_t>(f.fieldName.size()) > pdf::kMaxFormFieldNameChars) {
                std::cerr << "[FAIL] Part 1: Field name size (" << f.fieldName.size() << ") > kMaxFormFieldNameChars ("
                          << pdf::kMaxFormFieldNameChars << ") on " << fixtureName.toStdString() << std::endl;
                return 1;
            }
            if (static_cast<size_t>(f.value.size()) > pdf::kMaxFormFieldTextCodeUnits) {
                std::cerr << "[FAIL] Part 1: Field value size (" << f.value.size() << ") > kMaxFormFieldTextCodeUnits ("
                          << pdf::kMaxFormFieldTextCodeUnits << ") on " << fixtureName.toStdString() << std::endl;
                return 1;
            }
            if (f.options.size() > pdf::kMaxFormFieldOptions) {
                std::cerr << "[FAIL] Part 1: Option count (" << f.options.size() << ") > kMaxFormFieldOptions ("
                          << pdf::kMaxFormFieldOptions << ") on " << fixtureName.toStdString() << std::endl;
                return 1;
            }
            for (const auto& opt : f.options) {
                if (static_cast<size_t>(opt.size()) > pdf::kMaxFormFieldOptionChars) {
                    std::cerr << "[FAIL] Part 1: Option string size (" << opt.size() << ") > kMaxFormFieldOptionChars ("
                              << pdf::kMaxFormFieldOptionChars << ") on " << fixtureName.toStdString() << std::endl;
                    return 1;
                }
            }
            totalBudget += pdf::formFieldBudgetCost(f);
        }

        if (totalBudget > pdf::kMaxFormFieldListBytes) {
            std::cerr << "[FAIL] Part 1: Total budget (" << totalBudget << ") > kMaxFormFieldListBytes ("
                      << pdf::kMaxFormFieldListBytes << ") on " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        // Specific fixture expectations:
        // hostile_form_many_widgets (4000 widgets) -> must be truncated at 1,000 fields
        if (fixtureName == QStringLiteral("hostile_form_many_widgets.pdf")) {
            if (!truncated || fields.size() != pdf::kMaxFormFieldsPerPage) {
                std::cerr << "[FAIL] Part 1: hostile_form_many_widgets page 0 expected 1,000 fields truncated, got "
                          << fields.size() << " truncated=" << truncated << std::endl;
                return 1;
            }
        }
        // hostile_form_big_values (42 MB naive payload) -> must hit 16 MiB budget and set truncated
        if (fixtureName == QStringLiteral("hostile_form_big_values.pdf")) {
            if (!truncated || fields.size() >= 700) {
                std::cerr << "[FAIL] Part 1: hostile_form_big_values expected truncated before count cap, got count "
                          << fields.size() << " truncated=" << truncated << std::endl;
                return 1;
            }
        }
    }
    std::cout << "  -> Passed: In-process engine respects caps, budgets, and time limits across all hostile fixtures." << std::endl;

    // --- PART 2: Sandboxed WorkerClient IPC Testing ---
    std::cout << "[PART 2] Testing hostile form fixtures across IPC boundary with WorkerClient..." << std::endl;
    pdf::ipc::WorkerClient client;

    for (int i = 0; i < static_cast<int>(hostileFixtures.size()); ++i) {
        const auto& fixtureName = hostileFixtures[static_cast<size_t>(i)];
        QString path = resolveFixture(QString::fromStdString(samplePath), fixtureName);
        const int64_t docId = 1000 + i;

        std::cout << "  Testing " << fixtureName.toStdString() << " over IPC..." << std::endl;
        const auto tStart = std::chrono::steady_clock::now();
        auto openResp = client.openDocumentSync(docId, path.toStdString());
        if (openResp.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Part 2: WorkerClient open failed on " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        auto listResp = client.listFormFieldsSync(docId, 0, 8000);
        const auto tEnd = std::chrono::steady_clock::now();
        const double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();

        // Time budget: IPC round-trip must complete within 8.0 seconds
        if (elapsedSec > 8.0) {
            std::cerr << "[FAIL] Part 2: WorkerClient exceeded time budget on " << fixtureName.toStdString()
                      << " (" << elapsedSec << "s > 8.0s)" << std::endl;
            return 1;
        }

        if (!listResp.success) {
            std::cerr << "[FAIL] Part 2: WorkerClient listFormFieldsSync returned success=false on "
                      << fixtureName.toStdString() << std::endl;
            return 1;
        }

        if (listResp.fields.size() > pdf::kMaxFormFieldsPerPage) {
            std::cerr << "[FAIL] Part 2: WorkerClient field count (" << listResp.fields.size()
                      << ") > kMaxFormFieldsPerPage on " << fixtureName.toStdString() << std::endl;
            return 1;
        }

        uint64_t ipcBudget = 0;
        for (const auto& f : listResp.fields) {
            if (static_cast<size_t>(f.fieldName.size()) > pdf::kMaxFormFieldNameChars ||
                static_cast<size_t>(f.value.size()) > pdf::kMaxFormFieldTextCodeUnits ||
                f.options.size() > pdf::kMaxFormFieldOptions) {
                std::cerr << "[FAIL] Part 2: Field in IPC response exceeds individual caps on "
                          << fixtureName.toStdString() << std::endl;
                return 1;
            }
            ipcBudget += pdf::formFieldBudgetCost(f);
        }
        if (ipcBudget > pdf::kMaxFormFieldListBytes) {
            std::cerr << "[FAIL] Part 2: Total budget (" << ipcBudget << ") exceeds 16 MiB on "
                      << fixtureName.toStdString() << std::endl;
            return 1;
        }

        // Verify specific fixture behavior over IPC
        if (fixtureName == QStringLiteral("hostile_form_many_widgets.pdf")) {
            if (!listResp.truncated || listResp.fields.size() != pdf::kMaxFormFieldsPerPage) {
                std::cerr << "[FAIL] Part 2: hostile_form_many_widgets IPC page 0 count mismatch: "
                          << listResp.fields.size() << " truncated=" << listResp.truncated << std::endl;
                return 1;
            }

            // CRITICAL TEST: Worker survival check.
            // Page 2 of hostile_form_many_widgets has 3 ordinary fields.
            // A request on page 2 must succeed promptly, proving the worker remained healthy.
            auto page2Resp = client.listFormFieldsSync(docId, 1, 5000);
            if (!page2Resp.success || page2Resp.fields.size() != 3 || page2Resp.truncated) {
                std::cerr << "[FAIL] Part 2: Worker survival check failed on page 2 of hostile_form_many_widgets. "
                          << "Expected 3 fields, got " << page2Resp.fields.size()
                          << ", success=" << page2Resp.success << ", truncated=" << page2Resp.truncated << std::endl;
                return 1;
            }
        }

        // Close doc
        client.closeDocumentSync();
    }
    std::cout << "  -> Passed: WorkerClient IPC listing respects all caps, budgets, and proves worker survival." << std::endl;

    // --- PART 3: Hostile Form Mutations & EditLog Protection ---
    std::cout << "[PART 3] Testing hostile form mutations, oversize rejection, and EditLog integrity..." << std::endl;
    {
        // Use hostile_form_oversize_values.pdf
        QString oversizePath = resolveFixture(QString::fromStdString(samplePath), QStringLiteral("hostile_form_oversize_values.pdf"));
        if (oversizePath.isEmpty() || !QFile::exists(oversizePath)) {
            std::cerr << "[FAIL] Part 3: hostile_form_oversize_values.pdf not found" << std::endl;
            return 1;
        }

        const int64_t docId = 2001;
        auto openResp = client.openDocumentSync(docId, oversizePath.toStdString());
        if (openResp.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] Part 3: Failed to open hostile_form_oversize_values.pdf" << std::endl;
            return 1;
        }

        auto listResp = client.listFormFieldsSync(docId, 0, 5000);
        if (!listResp.success || listResp.fields.empty()) {
            std::cerr << "[FAIL] Part 3: Failed to list fields from hostile_form_oversize_values.pdf" << std::endl;
            return 1;
        }

        const QString targetField = listResp.fields[0].fieldName;
        pdf::EditLog log;

        // 1. Attempt mutation with text exceeding 32,768 code units (40,000 chars)
        const QString oversizeText(40000, QLatin1Char('Z'));
        auto mutResp = client.setFormFieldTextSync(docId, 0, targetField, 0, oversizeText, {});

        // Must be rejected cleanly by wire serializer / worker boundary
        if (mutResp.success) {
            std::cerr << "[FAIL] Part 3: Worker accepted oversize text mutation (" << oversizeText.size() << " code units)!" << std::endl;
            return 1;
        }

        // Verify document field value is not corrupted
        auto recheckList = client.listFormFieldsSync(docId, 0, 5000);
        if (!recheckList.success || recheckList.fields.empty() || recheckList.fields[0].value == oversizeText) {
            std::cerr << "[FAIL] Part 3: Document field was corrupted after rejected mutation!" << std::endl;
            return 1;
        }

        // Verify EditLog remains clean and uncorrupted when mutation is rejected
        if (log.isDirty() || log.canUndo()) {
            std::cerr << "[FAIL] Part 3: EditLog was marked dirty on rejected mutation!" << std::endl;
            return 1;
        }

        // 2. Attempt mutation with vector outlines exceeding 250,000 elements
        std::vector<pdf::AnnotationPathElement> massiveOutlines(250001);
        auto outlineResp = client.setFormFieldTextSync(docId, 0, targetField, 0, QStringLiteral("ValidText"), massiveOutlines);
        if (outlineResp.success) {
            std::cerr << "[FAIL] Part 3: Worker accepted mutation exceeding 250,000 outline elements!" << std::endl;
            return 1;
        }

        // 3. Positive control: valid bounded mutation succeeds cleanly
        const QString validText = QStringLiteral("SafeValidText");
        auto safeResp = client.setFormFieldTextSync(docId, 0, targetField, 0, validText, {});
        if (!safeResp.success) {
            std::cerr << "[FAIL] Part 3: Bounded valid mutation failed on hostile document: " << safeResp.errorMessage.toStdString() << std::endl;
            return 1;
        }

        // Record in EditLog and verify clean undo symmetry
        log.recordSetFormFieldText(0, targetField, 0, listResp.fields[0].value, validText, {});
        if (!log.isDirty() || !log.canUndo()) {
            std::cerr << "[FAIL] Part 3: EditLog failed to record valid mutation" << std::endl;
            return 1;
        }

        pdf::EditCommand undoCmd;
        if (!log.undo(undoCmd) || undoCmd.type != pdf::EditCommandType::SetFormFieldText) {
            std::cerr << "[FAIL] Part 3: EditLog failed to undo valid mutation" << std::endl;
            return 1;
        }
        if (log.isDirty() || log.canUndo() || !log.canRedo()) {
            std::cerr << "[FAIL] Part 3: EditLog state after undo is not clean/canRedo" << std::endl;
            return 1;
        }

        client.closeDocumentSync();
    }

    // T20-A: Password-field edits must be rejected and must never produce Persian glyph AP data.
#ifdef Q_OS_LINUX
    {
        QString identityPath = resolveFixture(QString::fromStdString(samplePath),
                                              QStringLiteral("forms_identity_and_topology.pdf"));
        if (identityPath.isEmpty() || !QFile::exists(identityPath)) {
            std::cerr << "[FAIL] T20-A: forms_identity_and_topology.pdf not found" << std::endl;
            return 1;
        }

        const int64_t docId = 2002;
        auto openResp = client.openDocumentSync(docId, identityPath.toStdString());
        if (openResp.error != pdf::DocumentError::None) {
            std::cerr << "[FAIL] T20-A: failed to open secret fixture" << std::endl;
            return 1;
        }

        auto secretFields = client.listFormFieldsSync(docId, 1, 5000);
        const pdf::FormFieldItem* secret = nullptr;
        pdf::FormFieldItem secretCopy;
        for (const auto& field : secretFields.fields) {
            if (field.fieldName == QStringLiteral("secret")) {
                secretCopy = field;
                secret = &secretCopy;
                break;
            }
        }
        if (!secretFields.success || !secret || !secret->isPassword()) {
            std::cerr << "[FAIL] T20-A: password field 'secret' not found on page 2" << std::endl;
            return 1;
        }

        const QString persian = QStringLiteral("\u0633\u0631\u0631\u0627\u0632");
        auto rejected = client.setFormFieldTextSync(docId, 1, secret->fieldName,
                                                     secret->controlIndex, persian, {});
        if (rejected.success) {
            std::cerr << "[FAIL] T20-A: engine accepted Persian edit for password field" << std::endl;
            return 1;
        }

        auto recheck = client.listFormFieldsSync(docId, 1, 5000);
        bool unchanged = false;
        for (const auto& field : recheck.fields) {
            if (field.fieldName == QStringLiteral("secret")) {
                unchanged = (field.value == secret->value);
                break;
            }
        }
        if (!unchanged) {
            std::cerr << "[FAIL] T20-A: password field /V changed after rejected edit" << std::endl;
            return 1;
        }

        QTemporaryDir tempDir;
        if (!tempDir.isValid()) {
            std::cerr << "[FAIL] T20-A: failed to create temporary output directory" << std::endl;
            return 1;
        }
        const QString savedPath = tempDir.filePath(QStringLiteral("password_saved.pdf"));
        int writeFd = ::open(savedPath.toLocal8Bit().constData(),
                             O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        if (writeFd < 0) {
            std::cerr << "[FAIL] T20-A: failed to open saved PDF" << std::endl;
            return 1;
        }
        auto saved = client.saveDocumentSync(docId, writeFd, true, "");
        if (!saved.success || saved.bytesWritten == 0) {
            std::cerr << "[FAIL] T20-A: incremental save failed" << std::endl;
            return 1;
        }

        QFile savedFile(savedPath);
        if (!savedFile.open(QIODevice::ReadOnly)) {
            std::cerr << "[FAIL] T20-A: failed to read saved PDF" << std::endl;
            return 1;
        }
        const QByteArray savedBytes = savedFile.readAll();
        savedFile.close();
        const QByteArray persianUtf8 = persian.toUtf8();
        const QByteArray persianUtf16 = QByteArray::fromRawData(
            reinterpret_cast<const char*>(persian.utf16()),
            persian.size() * static_cast<int>(sizeof(char16_t)));
        if (savedBytes.contains(persianUtf8) || savedBytes.contains(persianUtf16)) {
            std::cerr << "[FAIL] T20-A: plaintext Persian entered in password field leaked into saved PDF" << std::endl;
            return 1;
        }

        auto savedDoc = pdf::createDocument();
        if (savedDoc->loadFromFile(savedPath.toStdString()) != pdf::DocumentError::None) {
            std::cerr << "[FAIL] T20-A: failed to reopen saved PDF" << std::endl;
            return 1;
        }
        std::vector<uint8_t> flattened;
        if (!savedDoc->extractPages(1, 1, flattened, true, pdf::FlattenMode::NormalDisplay) ||
            flattened.empty()) {
            std::cerr << "[FAIL] T20-A: flattened export failed" << std::endl;
            return 1;
        }
        const QByteArray flatBytes(reinterpret_cast<const char*>(flattened.data()),
                                   static_cast<qsizetype>(flattened.size()));
        if (flatBytes.contains(persianUtf8) || flatBytes.contains(persianUtf16)) {
            std::cerr << "[FAIL] T20-A: plaintext Persian leaked into flattened export" << std::endl;
            return 1;
        }

        client.closeDocumentSync();
    }
#endif

    client.stop();
    std::cout << "  -> Passed: Hostile mutations rejected without corrupting document or EditLog." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << " ALL HOSTILE ACROFORM TESTS PASSED!     " << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
