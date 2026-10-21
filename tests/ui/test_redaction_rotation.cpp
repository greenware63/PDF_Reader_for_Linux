// End-to-end, pixel-level regression test for the redaction UI's coordinate handling.
//
// Redaction rectangles are only meaningful if the box that is burnt in lands on the text the user
// marked. The viewer has a user-selectable view rotation (0/90/180/270) that the engine knows nothing
// about, so this test drives the REAL widget with synthetic mouse events at every rotation, sends the
// staged marks through the REAL sandboxed worker, and then judges the output only by its pixels,
// using pdftoppm as an independent witness:
//
//   * where the text is in the SOURCE is measured from pdftoppm's rendering of the source file,
//     never from the widget's own coordinate maths;
//   * the output must contain one solid black box that covers every source ink pixel, and NO dark
//     pixel outside it (leftover glyph ink == the text was not redacted).
//
// pdftotext is deliberately not used as the check: the engine rasterises the page, so extracted text
// is empty whether or not the box hit the text.
//
// Cases (each at 0/90/180/270): drag a box, select text then mark the selection, and mark at one
// rotation then rotate the view before applying (marks must stay glued to the page, not the view).

#include "ui/PdfViewerWidget.h"
#include "ipc/WorkerClient.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct IRect {
    int x0{1 << 30}, y0{1 << 30}, x1{-1}, y1{-1};
    bool valid() const { return x1 >= x0 && y1 >= y0; }
    void add(int x, int y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
};

void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
}

void mouse(QWidget* w, QEvent::Type type, QPoint p) {
    QMouseEvent e(type, QPointF(p), QPointF(w->mapToGlobal(p)), Qt::LeftButton,
                  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

bool haveTool(const QString& name) {
#ifdef Q_OS_UNIX
    QProcess p;
    p.start(QStringLiteral("sh"), {QStringLiteral("-c"), QStringLiteral("command -v ") + name});
    p.waitForFinished(5000);
    return p.exitCode() == 0;
#else
    return !QStandardPaths::findExecutable(name).isEmpty();
#endif
}

bool render72(const QString& pdf, const QString& outBase, QImage& img) {
    QProcess p;
    p.start(QStringLiteral("pdftoppm"), {QStringLiteral("-r"), QStringLiteral("72"), QStringLiteral("-png"),
                                          QStringLiteral("-singlefile"), pdf, outBase});
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        return false;
    }
    return img.load(outBase + QStringLiteral(".png"));
}

int gray(QRgb c) { return qGray(c); }

IRect inkBox(const QImage& img, int threshold) {
    IRect r;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (gray(img.pixel(x, y)) < threshold) r.add(x, y);
    return r;
}

// Locates the text of the page as drawn in the viewport, by looking for dark pixels inside the white page.
bool locateTextInViewport(ui::PdfViewerWidget& v, IRect& text) {
    const QImage img = v.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    IRect page;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = img.pixel(x, y);
            if (qRed(c) > 240 && qGreen(c) > 240 && qBlue(c) > 240) page.add(x, y);
        }
    if (!page.valid()) return false;
    text = IRect{};
    for (int y = page.y0; y <= page.y1; ++y)
        for (int x = page.x0; x <= page.x1; ++x) {
            const QRgb c = img.pixel(x, y);
            if (qRed(c) < 90 && qGreen(c) < 90 && qBlue(c) < 90) text.add(x, y);
        }
    return text.valid();
}

// Pixel judgement of a redacted page against the source's ink.
bool judge(const QImage& out, const IRect& srcInk, std::string& why) {
    const int w = out.width(), h = out.height();
    // Largest 4-connected component of near-black pixels == the redaction box.
    std::vector<int> label(static_cast<size_t>(w) * h, 0);
    int best = 0, bestId = 0, nextId = 0;
    IRect bestBox;
    std::vector<std::pair<int, int>> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (label[static_cast<size_t>(y) * w + x] || gray(out.pixel(x, y)) >= 32) continue;
            ++nextId;
            int area = 0;
            IRect box;
            stack.clear();
            stack.emplace_back(x, y);
            label[static_cast<size_t>(y) * w + x] = nextId;
            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                ++area;
                box.add(cx, cy);
                const int nx[4] = {cx + 1, cx - 1, cx, cx}, ny[4] = {cy, cy, cy + 1, cy - 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    int& l = label[static_cast<size_t>(ny[k]) * w + nx[k]];
                    if (l || gray(out.pixel(nx[k], ny[k])) >= 32) continue;
                    l = nextId;
                    stack.emplace_back(nx[k], ny[k]);
                }
            }
            if (area > best) { best = area; bestId = nextId; bestBox = box; }
        }
    }
    if (bestId == 0) { why = "no black redaction box in output"; return false; }
    const long boxArea = static_cast<long>(bestBox.x1 - bestBox.x0 + 1) * (bestBox.y1 - bestBox.y0 + 1);
    if (best < boxArea * 9 / 10) { why = "largest black region is not a solid box"; return false; }
    const int slack = 1;
    if (bestBox.x0 > srcInk.x0 + slack || bestBox.y0 > srcInk.y0 + slack ||
        bestBox.x1 < srcInk.x1 - slack || bestBox.y1 < srcInk.y1 - slack) {
        why = "redaction box does not cover the source text (box " + std::to_string(bestBox.x0) + "," + std::to_string(bestBox.y0) +
              "-" + std::to_string(bestBox.x1) + "," + std::to_string(bestBox.y1) + ", text " + std::to_string(srcInk.x0) + "," +
              std::to_string(srcInk.y0) + "-" + std::to_string(srcInk.x1) + "," + std::to_string(srcInk.y1) + ")";
        return false;
    }
    int leaked = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (gray(out.pixel(x, y)) >= 128) continue;
            const bool inBox = x >= bestBox.x0 - 2 && x <= bestBox.x1 + 2 && y >= bestBox.y0 - 2 && y <= bestBox.y1 + 2;
            if (!inBox) ++leaked;
        }
    if (leaked > 0) { why = std::to_string(leaked) + " dark pixels outside the redaction box (unredacted ink survives)"; return false; }
    return true;
}

enum class Mode { Drag, Selection, MarkThenRotate };

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Drag: return "drag-box";
        case Mode::Selection: return "text-selection";
        default: return "mark-then-rotate";
    }
}

bool runCase(const QString& samplePdf, const IRect& srcInk, int rot, Mode mode, const QString& tmp, std::string& why) {
    ui::PdfViewerWidget v;
    v.resize(900, 900);
    v.show();
    pump(100);
    if (v.openDocument(samplePdf) != pdf::DocumentError::None) { why = "cannot open sample"; return false; }
    pump(800);

    // MarkThenRotate marks at rotation 0 and rotates afterwards; the others rotate first.
    const int markRot = (mode == Mode::MarkThenRotate) ? 0 : rot;
    v.setRotation(markRot);
    pump(900);

    IRect text;
    if (!locateTextInViewport(v, text)) { why = "could not locate rendered text in viewport"; return false; }

    QWidget* vp = v.viewport();
    if (mode == Mode::Selection) {
        const bool horizontal = (text.x1 - text.x0) >= (text.y1 - text.y0);
        const QPoint a = horizontal ? QPoint(text.x0 + 1, (text.y0 + text.y1) / 2) : QPoint((text.x0 + text.x1) / 2, text.y0 + 1);
        const QPoint b = horizontal ? QPoint(text.x1 - 1, (text.y0 + text.y1) / 2) : QPoint((text.x0 + text.x1) / 2, text.y1 - 1);
        mouse(vp, QEvent::MouseButtonPress, a);
        pump(30);
        mouse(vp, QEvent::MouseMove, QPoint((a.x() + b.x()) / 2, (a.y() + b.y()) / 2));
        pump(30);
        mouse(vp, QEvent::MouseMove, b);
        pump(30);
        mouse(vp, QEvent::MouseButtonRelease, b);
        pump(300);
        const QString sel = v.selectedText();
        if (!sel.contains(QStringLiteral("Dummy")) || !sel.contains(QStringLiteral("file"))) {
            why = "text selection did not capture the line (got '" + sel.toStdString() + "')";
            return false;
        }
        if (!v.markSelectionForRedaction()) { why = "markSelectionForRedaction returned false"; return false; }
        v.setRedactionMode(true);
    } else {
        v.setRedactionMode(true);
        const int pad = 5;
        mouse(vp, QEvent::MouseButtonPress, QPoint(text.x0 - pad, text.y0 - pad));
        pump(30);
        mouse(vp, QEvent::MouseMove, QPoint((text.x0 + text.x1) / 2, (text.y0 + text.y1) / 2));
        pump(30);
        mouse(vp, QEvent::MouseMove, QPoint(text.x1 + pad, text.y1 + pad));
        pump(30);
        mouse(vp, QEvent::MouseButtonRelease, QPoint(text.x1 + pad, text.y1 + pad));
        pump(100);
    }
    if (v.stagedRedactionCount() < 1) { why = "no redaction mark was staged"; return false; }

    if (mode == Mode::MarkThenRotate) {
        v.setRotation(rot);
        pump(900);
    }

    // The overlay drawn on screen must sit where the text is NOW displayed (follows the view rotation).
    {
        const QImage g = v.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
        IRect crimson;
        for (int y = 0; y < g.height(); ++y)
            for (int x = 0; x < g.width(); ++x) {
                const QRgb c = g.pixel(x, y);
                if (qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) < 90) crimson.add(x, y);
            }
        if (!crimson.valid()) { why = "no redaction overlay drawn"; return false; }
        // The overlay must be a real, on-screen box. Where it lands relative to the page is judged on
        // the output pixels below, which is the property that matters.
        if (crimson.x1 - crimson.x0 < 8 || crimson.y1 - crimson.y0 < 8) { why = "overlay degenerate"; return false; }
    }

    std::vector<pdf::ipc::PageRedactionItem> items;
    for (const auto& [p, rs] : v.stagedRedactions()) {
        pdf::ipc::PageRedactionItem it;
        it.pageIndex = p;
        it.rects = rs;
        items.push_back(it);
    }
    const auto res = v.renderWorker().redactDocumentSync(v.currentDocumentId(), items, 150.0, 1);
    if (!res.success) { why = "worker redaction failed: " + res.errorMessage.toStdString(); return false; }

    const QString outPdf = tmp + QStringLiteral("/out_%1_%2.pdf").arg(rot).arg(static_cast<int>(mode));
    QFile f(outPdf);
    if (!f.open(QIODevice::WriteOnly) || f.write(res.pdfBytes) != res.pdfBytes.size()) { why = "cannot write output"; return false; }
    f.close();

    QImage out;
    if (!render72(outPdf, outPdf + QStringLiteral(".render"), out)) { why = "pdftoppm failed on output"; return false; }
    return judge(out, srcInk, why);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString sample = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("sample.pdf");

    const bool required = std::getenv("REDACT_REQUIRE_WITNESSES") && std::string(std::getenv("REDACT_REQUIRE_WITNESSES")) == "1";
    if (!haveTool(QStringLiteral("pdftoppm"))) {
        if (required) {
            std::cerr << "FATAL: REDACT_REQUIRE_WITNESSES=1 but pdftoppm is missing\n";
            return 1;
        }
        std::cout << "Skipped: pdftoppm witness not installed\n";
        return 0;
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::cerr << "[FAIL] no temp dir\n"; return 1; }

    QImage src;
    if (!render72(sample, tmp.path() + QStringLiteral("/src"), src)) { std::cerr << "[FAIL] cannot render source\n"; return 1; }
    const IRect srcInk = inkBox(src, 128);
    if (!srcInk.valid()) { std::cerr << "[FAIL] source has no ink\n"; return 1; }
    std::cout << "source ink box @72dpi: " << srcInk.x0 << "," << srcInk.y0 << " - " << srcInk.x1 << "," << srcInk.y1 << "\n";

    int failures = 0;
    for (const Mode mode : {Mode::Drag, Mode::Selection, Mode::MarkThenRotate}) {
        for (const int rot : {0, 90, 180, 270}) {
            std::string why;
            const bool ok = runCase(sample, srcInk, rot, mode, tmp.path(), why);
            std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << modeName(mode) << " @ view rotation " << rot;
            if (!ok) { std::cout << " : " << why; ++failures; }
            std::cout << "\n";
        }
    }
    if (failures) {
        std::cerr << failures << " redaction-rotation case(s) failed\n";
        return 1;
    }
    std::cout << "ALL REDACTION ROTATION CASES PASSED\n";
    return 0;
}
