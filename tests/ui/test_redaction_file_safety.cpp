// Unit tests for the redaction "save" guards: never replace the source, never leave a partial file.
#include "ui/RedactionFileSafety.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <filesystem>
#include <iostream>

#ifdef __linux__
#include <sys/resource.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (!ok) ++g_failures;
}
bool writeRaw(const QString& p, const QByteArray& b) {
    QFile f(p);
    return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
}
QByteArray readRaw(const QString& p) {
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QString dir = tmp.path();
    const QString src = dir + "/doc.pdf";
    writeRaw(src, "ORIGINAL");

    std::cout << "isSameFile\n";
    check(ui::safety::isSameFile(src, src), "same path");
    check(ui::safety::isSameFile(src, dir + "/./doc.pdf"), "same path with ./ component");
    check(ui::safety::isSameFile(src, dir + "/../" + QDir(dir).dirName() + "/doc.pdf"), "same path via ..");

#ifdef Q_OS_UNIX
    const QString symlink = dir + "/symlink.pdf";
    const bool symlinkCreated = QFile::link(src, symlink);
    check(symlinkCreated, "creates symlink to the source");
    if (symlinkCreated) {
        check(ui::safety::isSameFile(src, symlink), "symlink to the source");
    }
#endif

    std::error_code hardEc;
#ifdef Q_OS_WIN
    std::filesystem::create_hard_link(src.toStdWString(), (dir + "/hard.pdf").toStdWString(), hardEc);
#else
    std::filesystem::create_hard_link(QFile::encodeName(src).constData(), QFile::encodeName(dir + "/hard.pdf").constData(), hardEc);
#endif
    check(!hardEc, "creates hard link to the source");
    if (!hardEc) {
        check(ui::safety::isSameFile(src, dir + "/hard.pdf"), "hard link to the source");
    }

    writeRaw(dir + "/other.pdf", "OTHER");
    check(!ui::safety::isSameFile(src, dir + "/other.pdf"), "different existing file");
    check(!ui::safety::isSameFile(src, dir + "/new_redacted.pdf"), "destination that does not exist yet");

    std::cout << "writeFileAtomically\n";
    const QString out = dir + "/out.pdf";
    QString err;
    check(ui::safety::writeFileAtomically(out, "NEWBYTES", &err) && readRaw(out) == "NEWBYTES", "creates the file with exact bytes");
    check(ui::safety::writeFileAtomically(out, "REPLACED-CONTENT", &err) && readRaw(out) == "REPLACED-CONTENT", "replaces existing content completely");
    check(!ui::safety::writeFileAtomically(dir + "/no_such_dir/x.pdf", "X", &err) && !err.isEmpty(), "fails with a message for an unwritable location");
    check(!QFileInfo::exists(dir + "/no_such_dir"), "  ...and creates nothing");

    // A write that fails part-way (file-size limit on Linux) must leave the previous destination intact and no temp file.
    writeRaw(out, "PREVIOUS-GOOD");
#ifdef __linux__
    signal(SIGXFSZ, SIG_IGN);
    rlimit old{};
    getrlimit(RLIMIT_FSIZE, &old);
    rlimit small = old;
    small.rlim_cur = 16;
    setrlimit(RLIMIT_FSIZE, &small);
    const bool failed = !ui::safety::writeFileAtomically(out, QByteArray(1 << 20, 'Z'), &err);
    setrlimit(RLIMIT_FSIZE, &old);
    check(failed, "reports failure when the write cannot complete");
    check(readRaw(out) == "PREVIOUS-GOOD", "  ...destination is untouched (no truncation)");
#endif
    int leftovers = 0;
    for (const auto& n : QDir(dir).entryList(QDir::Files | QDir::Hidden)) {
        if (n != "doc.pdf" && n != "hard.pdf" && n != "other.pdf" && n != "out.pdf" && n != "symlink.pdf") ++leftovers;
    }
    check(leftovers == 0, "  ...no temporary file left behind");

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "ALL REDACTION FILE SAFETY TESTS PASSED\n";
    return 0;
}
