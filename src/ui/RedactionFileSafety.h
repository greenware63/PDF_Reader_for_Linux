#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#ifdef __linux__
#include <sys/stat.h>
#endif

namespace ui::safety {

/// True when `dest` already exists and is the same file as `source`: same canonical path (symlinks and
/// relative paths resolved) or, on Linux, the same device+inode (hard links, bind mounts). A redacted
/// copy must never replace the original document.
inline bool isSameFile(const QString& source, const QString& dest) {
    const QFileInfo s(source);
    const QFileInfo d(dest);
    if (!s.exists() || !d.exists()) {
        return false;
    }
    if (!s.canonicalFilePath().isEmpty() && s.canonicalFilePath() == d.canonicalFilePath()) {
        return true;
    }
#ifdef __linux__
    struct stat sa{};
    struct stat sb{};
    if (::stat(QFile::encodeName(source).constData(), &sa) == 0 &&
        ::stat(QFile::encodeName(dest).constData(), &sb) == 0) {
        return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    }
#endif
    return false;
}

/// Writes `data` to `path` all-or-nothing: the bytes go to a temporary file in the same directory that
/// is renamed over `path` only after every byte was written and flushed. On any failure the destination
/// is left exactly as it was and no temporary file remains.
inline bool writeFileAtomically(const QString& path, const QByteArray& data, QString* error = nullptr) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const qint64 written = file.write(data);
    if (written != data.size()) {
        if (error) *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

} // namespace ui::safety
