#pragma once

#include <QSettings>
#include <QString>

namespace ui {

/// The application's persistent settings. Built with QSettings::defaultFormat() on purpose: the
/// QSettings(organization, application) constructor ignores setDefaultFormat() and always uses the native
/// format, which on Windows is the registry. Going through the default format lets the tests force an
/// isolated INI file; production keeps the native format (default) so existing users' settings are unchanged.
inline QSettings makeAppSettings() {
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                     QStringLiteral("PDFReader"), QStringLiteral("PDFReader"));
}

} // namespace ui
