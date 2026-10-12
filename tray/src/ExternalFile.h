#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <functional>

// Opens files that sessions produced, the way browsers treat downloads. Files
// from other machines carry the platform's downloaded-file marker (Windows Mark
// of the Web, macOS quarantine), so the system warns before they run. Types that
// run programs or scripts are shown in the file manager instead of being opened
// on every platform; Linux has no such marker.
namespace ExternalFile {
enum class Origin { ThisComputer, OtherMachine };
enum class Outcome { Opened, Revealed, OpenFailed, RevealFailed };

// Whether the system's default handler for this file can run programs or scripts.
bool runsPrograms(const QString &path);
// Adds the downloaded-file marker; false where the platform or file system has none.
bool markDownloaded(const QString &path);
// The raw marker (Zone.Identifier stream or quarantine attribute), or empty.
QByteArray downloadMarker(const QString &path);
// Shows the file selected in the system file manager.
bool reveal(const QString &path);
// `opener` replaces QDesktopServices::openUrl for files that are opened.
Outcome open(const QString &path, Origin origin, const std::function<bool(const QUrl &)> &opener = {});
QString revealedNotice(const QString &path);
QString revealFailedNotice(const QString &path);
// Tests observe reveals without starting a file manager.
void setRevealHandler(std::function<bool(const QString &)> handler);
}
