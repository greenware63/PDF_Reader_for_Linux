#pragma once

// Keeps automated tests away from the developer's real settings.
//
// MainWindow persists reading positions, recent files, colour mode, ... through QSettings under
// ~/.config/PDFReader. Tests that construct a MainWindow therefore used to read and write that real
// file: results depended on what the machine had stored, and concurrent runs corrupted each other.
//
// Construct one IsolatedUserSettings as the FIRST statement of main(), before any QSettings exists.
// It redirects every QSettings scope/format to a private temporary directory that is removed on exit.

#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

namespace testsupport {

class IsolatedUserSettings {
public:
    IsolatedUserSettings() {
        if (!m_dir.isValid()) {
            std::fprintf(stderr, "[FAIL] cannot create an isolated settings directory\n");
            std::abort();
        }
        // On Windows, NativeFormat uses the registry and ignores the redirected filesystem path.
        // Force INI settings so UI tests stay isolated and writable on every platform.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        for (const auto format : {QSettings::NativeFormat, QSettings::IniFormat}) {
            for (const auto scope : {QSettings::UserScope, QSettings::SystemScope}) {
                QSettings::setPath(format, scope, m_dir.path());
            }
        }
    }
    IsolatedUserSettings(const IsolatedUserSettings&) = delete;
    IsolatedUserSettings& operator=(const IsolatedUserSettings&) = delete;

private:
    QTemporaryDir m_dir;
};

} // namespace testsupport
