#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <filesystem>

namespace ui::safety {

/// True when `dest` already exists and refers to the same file as `source`,
/// including through symlinks, relative paths, or hard links. A redacted copy
/// must never replace the original document.
inline bool isSameFile(const QString& source, const QString& dest) {
    const QFileInfo s(source);
    const QFileInfo d(dest);
    if (!s.exists() || !d.exists()) {
        return false;
    }
    if (!s.canonicalFilePath().isEmpty() && s.canonicalFilePath() == d.canonicalFilePath()) {
        return true;
    }

#ifdef Q_OS_WIN
    const std::filesystem::path sourcePath(source.toStdWString());
    const std::filesystem::path destPath(dest.toStdWString());
#else
    const std::filesystem::path sourcePath(QFile::encodeName(source).constData());
    const std::filesystem::path destPath(QFile::encodeName(dest).constData());
#endif
    std::error_code ec;
    return std::filesystem::equivalent(sourcePath, destPath, ec) && !ec;
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
