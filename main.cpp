#include <QApplication>
#include <QCommandLineParser>
#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("PDF Reader Workstation");
    app.setApplicationVersion("0.1.0-spike");
    app.setOrganizationName("PDFReader");

    QCommandLineParser parser;
    parser.setApplicationDescription("PDF Reader Workstation - Native Linux PDF Viewer");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF file to open on startup", "[file]");
    parser.process(app);

    ui::MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        window.openFile(args.first());
    }

    return app.exec();
}
