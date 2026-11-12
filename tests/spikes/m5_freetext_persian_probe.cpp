// Provenance of tests/fixtures/persian_freetext_production_sample.pdf: five single-line Persian FreeText boxes made through
// the real viewer (PdfViewerWidget::addDefaultAnnotationAt), whose appearances are therefore what production writes.
// The sample is the golden positive for tests/tools/check_persian_appearance.py. Developer tool, not part of the build.
//
// Build it as a temporary extra target that links like test_widget (same sources and libraries), for example by
// copying test_widget's CMake block and replacing test_widget.cpp with this file.
// usage: freetext_persian_probe <input.pdf> <output.pdf>        (the sample was made from sample.pdf at main 27f3a37)
// Creates single-line Persian (and mixed) FreeText boxes through the real viewer and saves the PDF.
// usage: freetext_persian_probe <input.pdf> <output.pdf>
#include "ui/PdfViewerWidget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ui::PdfViewerWidget viewer; viewer.resize(800, 600); viewer.show();
    viewer.openDocument(QString::fromUtf8(argv[1]));
    QElapsedTimer t; t.start();
    while (t.elapsed() < 15000 && !viewer.isDocumentOpen()) { QApplication::processEvents(); QThread::msleep(10); }
    if (!viewer.isDocumentOpen()) { std::cerr << "open failed\n"; return 1; }
    const QString texts[] = {
        QString::fromUtf8("سلام دنیا"),
        QString::fromUtf8("نام خانوادگی"),
        QString::fromUtf8("کتاب‌خانه ملی ایران"),
        QString::fromUtf8("ژاله و پژمان گچ‌بری می‌کنند"),
        QString::fromUtf8("۱۴۰۴/۰۸/۱۸"),
    };
    double y = 740;
    for (const auto& s : texts) {
        const bool ok = viewer.addDefaultAnnotationAt(0, pdf::AnnotationSubtype::FreeText, 60, y, s);
        std::cout << (ok ? "added   " : "FAILED  ") << s.left(30).toStdString() << "\n";
        y -= 70;
    }
    if (!viewer.saveDocument(QString::fromUtf8(argv[2]))) { std::cerr << "save failed\n"; return 1; }
    std::cout << "saved\n";
    return 0;
}
