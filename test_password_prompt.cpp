#include "TestIsolation.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <iostream>

int main(int argc, char* argv[]) {
    const testsupport::IsolatedUserSettings isolatedSettings; // never touch the real ~/.config/PDFReader
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    if (argc < 3) {
        std::cerr << "Usage: test_password_prompt <sample.pdf> <encrypted.pdf>" << std::endl;
        return 1;
    }

    const QString samplePath = QString::fromLocal8Bit(argv[1]);
    const QString encryptedPath = QString::fromLocal8Bit(argv[2]);
    auto waitFor = [](const auto& condition, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < timeoutMs) {
            QThread::msleep(10);
            QCoreApplication::processEvents();
        }
        QCoreApplication::processEvents();
        return condition();
    };

    ui::MainWindow window;
    window.show();
    bool hasFindNextShortcut = false;
    bool hasFindPreviousShortcut = false;
    for (const auto* menu : window.findChildren<QMenu*>()) {
        for (const auto* action : menu->actions()) {
            hasFindNextShortcut |= action->shortcut() == QKeySequence(Qt::Key_F3);
            hasFindPreviousShortcut |= action->shortcut() == QKeySequence(Qt::SHIFT | Qt::Key_F3);
        }
    }
    if (!hasFindNextShortcut || !hasFindPreviousShortcut) {
        std::cerr << "[FAIL] Find Next/Previous F3 shortcuts are not installed." << std::endl;
        return 1;
    }

    auto* viewer = window.findChild<ui::PdfViewerWidget*>();
    if (!viewer || !window.openFile(samplePath) ||
        !waitFor([&]() { return !viewer->isLoading(); }, 5000) || !viewer->isDocumentOpen()) {
        std::cerr << "[FAIL] Could not load the active document for password prompt tests" << std::endl;
        return 1;
    }

    const int64_t activeDocumentId = viewer->currentDocumentId();
    const int activePageCount = viewer->pageCount();

    std::cout << "[TEST] Password prompt masking, cancel, bad password, and retry..." << std::endl;
    if (!window.openFile(encryptedPath) ||
        !waitFor([&]() { return window.findChild<QDialog*>("passwordDialog") != nullptr; }, 5000)) {
        std::cerr << "[FAIL] Password-protected PDF did not show a password dialog" << std::endl;
        return 1;
    }

    auto* dialog = window.findChild<QDialog*>("passwordDialog");
    auto* input = dialog->findChild<QLineEdit*>("passwordInput");
    auto* warning = dialog->findChild<QLabel*>("passwordWarning");
    if (!input || input->echoMode() != QLineEdit::Password || !warning || warning->isVisible()) {
        std::cerr << "[FAIL] Password prompt is not masked or has an unexpected initial warning" << std::endl;
        return 1;
    }
    if (viewer->currentDocumentId() != activeDocumentId || viewer->pageCount() != activePageCount) {
        std::cerr << "[FAIL] Password prompt replaced the active document before authentication" << std::endl;
        return 1;
    }

    dialog->reject();
    if (!waitFor([&]() { return window.findChild<QDialog*>("passwordDialog") == nullptr; }, 2000) ||
        viewer->currentDocumentId() != activeDocumentId || viewer->pageCount() != activePageCount) {
        std::cerr << "[FAIL] Cancelling the password prompt did not preserve the active document" << std::endl;
        return 1;
    }

    if (!window.openFile(encryptedPath) ||
        !waitFor([&]() { return window.findChild<QDialog*>("passwordDialog") != nullptr; }, 5000)) {
        std::cerr << "[FAIL] Password prompt did not reappear for a new open attempt" << std::endl;
        return 1;
    }
    dialog = window.findChild<QDialog*>("passwordDialog");
    input = dialog->findChild<QLineEdit*>("passwordInput");
    warning = dialog->findChild<QLabel*>("passwordWarning");
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    if (!input || !warning || !buttons || !buttons->button(QDialogButtonBox::Ok)) {
        std::cerr << "[FAIL] Password dialog controls are incomplete" << std::endl;
        return 1;
    }

    input->setText(QStringLiteral("incorrect-test-password"));
    buttons->button(QDialogButtonBox::Ok)->click();
    if (!waitFor([&]() { return warning->text() == QStringLiteral("Incorrect password") && !viewer->isLoading(); }, 5000)) {
        std::cerr << "[FAIL] Incorrect password did not produce an inline warning" << std::endl;
        return 1;
    }
    if (!dialog->isVisible() || viewer->currentDocumentId() != activeDocumentId ||
        viewer->pageCount() != activePageCount ||
        window.statusBar()->currentMessage().contains(QStringLiteral("incorrect-test-password"))) {
        std::cerr << "[FAIL] Bad password lost prompt/document state or exposed the password" << std::endl;
        return 1;
    }

    input->setText(QStringLiteral("fixture-password"));
    buttons->button(QDialogButtonBox::Ok)->click();
    if (!waitFor([&]() { return !viewer->isLoading() && viewer->currentDocumentId() != activeDocumentId; }, 5000) ||
        viewer->pageCount() != 1 ||
        !waitFor([&]() { return window.findChild<QDialog*>("passwordDialog") == nullptr; }, 2000)) {
        std::cerr << "[FAIL] Correct password did not open the protected document and close the prompt" << std::endl;
        return 1;
    }

    std::cout << "  -> Passed: cancel preserves the open document; bad password warns inline; correct password opens." << std::endl;
    return 0;
}
