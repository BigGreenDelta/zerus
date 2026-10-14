#pragma once

#include "MachinePlatform.h"

#include <QDir>
#include <QRegularExpression>
#include <QUrl>

// A document reference is display data until the user explicitly opens it.
// Resolve relative paths on the session's machine, never against the GUI cwd.
namespace SessionFileReference {
struct Target {
    QString path;
    QString location;
    bool valid() const { return !path.isEmpty(); }
};

// A Windows GUI also reads drive paths: "C:\work\a.cpp:12", "C:/work/a.cpp" and
// "file:///C:/work/a.cpp", never a share (UNC). Elsewhere "C:\x" stays invalid.
inline Target parse(const QString &href, MachinePlatform::Style style = MachinePlatform::build())
{
    const bool windows = style == MachinePlatform::Style::Windows;
    const auto separator = [](QChar c) { return c == '/' || c == '\\'; };
    const auto drive = [&](const QString &path) {
        return MachinePlatform::absolute(path, MachinePlatform::Style::Windows) && !separator(path.at(0));
    };
    // A drive path reads as a file URL, which splits off and decodes the same parts
    // as any other reference; its backslashes stay as written.
    const QUrl url(windows && drive(href) ? QStringLiteral("file:///") + QString(href).replace('\\', "%5C") : href);
    if (!url.isValid() || !url.host().isEmpty() || !url.userInfo().isEmpty()
        || (!url.scheme().isEmpty() && url.scheme() != "file") || url.hasQuery()) return {};
    QString path = url.path(QUrl::FullyDecoded);
    if (windows && path.startsWith('/') && drive(path.mid(1))) path.remove(0, 1);
    if (path.isEmpty() || path.startsWith("//") || path.size() > 8192) return {};
    if (windows && path.size() > 1 && separator(path.at(0)) && separator(path.at(1))) return {};
    for (const auto ch : path) if (ch.isNull() || ch.category() == QChar::Other_Control) return {};
    QString location = url.fragment(QUrl::FullyDecoded);
    if (location.size() > 2048) return {};
    for (const auto ch : location) if (ch.isNull() || ch.category() == QChar::Other_Control) return {};
    if (!location.isEmpty()) location.prepend('#');
    static const QRegularExpression suffix(QStringLiteral("(:[1-9][0-9]*(?::[1-9][0-9]*)?)$"));
    const auto line = suffix.match(path);
    if (line.hasMatch() && location.isEmpty()) { location = line.captured(); path.chop(location.size()); }
    return {path, location};
}

inline QString resolve(const Target &target, const QString &cwd, const QString &localHome = {})
{
    if (!target.valid()) return {};
    if (QDir::isAbsolutePath(target.path)) return QDir::cleanPath(target.path);
    if (target.path.startsWith("~/")) return localHome.isEmpty() ? QString() : QDir::cleanPath(QDir(localHome).filePath(target.path.mid(2)));
    if (!QDir::isAbsolutePath(cwd)) return {};
    return QDir::cleanPath(QDir(cwd).filePath(target.path));
}
}
