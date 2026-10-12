#pragma once

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

inline Target parse(const QString &href)
{
    const QUrl url(href);
    if (!url.isValid() || !url.host().isEmpty() || !url.userInfo().isEmpty()
        || (!url.scheme().isEmpty() && url.scheme() != "file") || url.hasQuery()) return {};
    QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty() || path.startsWith("//") || path.size() > 8192) return {};
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
