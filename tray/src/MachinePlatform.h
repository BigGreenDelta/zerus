#pragma once

#include <QDir>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>

// Folder paths in the form of the machine they belong to. The local computer may
// be Windows (local sessions, D15 and D16 of the Windows native host design);
// peers are POSIX in Phase 2, and the swarm catalog accepts nothing else. A Style
// parameter rather than an #ifdef, so both forms are tested on every platform.
// Posix answers are exactly the expressions the GUI used before Windows: a leading
// '/', QDir::cleanPath, ==, section('/', -1) and withinFolder (NewSessionDialog).
// Windows stores the Qt form ("C:/work", "//server/share/dir"), compares with either
// separator and ignoring case, and shows native separators.
namespace MachinePlatform {
enum class Style { Posix, Windows };

// The platform this GUI was built for.
inline Style build()
{
#ifdef Q_OS_WIN
    return Style::Windows;
#else
    return Style::Posix;
#endif
}

// A peer is POSIX. The local machine follows metrics.platform of its box ("windows",
// "linux", "macos"); before it reports one (the first poll, an older CLI) it is the
// platform this GUI was built for.
inline Style of(const QJsonObject &metrics, bool local)
{
    if (!local) return Style::Posix;
    const auto platform = metrics.value(QLatin1String("platform")).toString();
    if (platform.isEmpty()) return build();
    return platform == QLatin1String("windows") ? Style::Windows : Style::Posix;
}

namespace Detail {
inline bool separator(QChar c) { return c == QLatin1Char('/') || c == QLatin1Char('\\'); }
// "X:\" or "X:/".
inline bool drive(QStringView path)
{
    if (path.size() < 3 || path[1] != QLatin1Char(':') || !separator(path[2])) return false;
    const auto letter = path[0].unicode();
    return (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
}
// The end of "\\server" (two separators, then a name), or 0.
inline qsizetype server(QStringView path)
{
    if (path.size() < 3 || !separator(path[0]) || !separator(path[1]) || separator(path[2])) return 0;
    qsizetype end = 3;
    while (end < path.size() && !separator(path[end])) ++end;
    return end;
}
// The end of "\\server\share", or 0 without a share name.
inline qsizetype share(QStringView path)
{
    const auto from = server(path) + 1;
    if (from == 1 || from >= path.size() || separator(path[from])) return 0;
    auto end = from;
    while (end < path.size() && !separator(path[end])) ++end;
    return end;
}
// "\\?\" and "\\.\" name verbatim paths and devices, not a share.
inline bool device(QStringView path)
{
    return server(path) == 3 && (path[2] == QLatin1Char('?') || path[2] == QLatin1Char('.'));
}
}

// A drive or UNC path in either separator form (also "\\?\" and "\\.\" paths):
// a Windows folder, which the swarm catalog must never receive.
inline bool windowsShaped(const QString &path)
{
    return Detail::drive(path) || Detail::share(path) > 0;
}

// Windows: a drive root ("C:\", "c:/") or a share ("\\server\share", "//server/share");
// "\work", "C:work" and "\\?\" paths are not folders of a machine.
inline bool absolute(const QString &path, Style style)
{
    if (style == Style::Posix) return path.startsWith(QLatin1Char('/'));
    return Detail::drive(path) || (Detail::share(path) && !Detail::device(path));
}

// The stored form: QDir::cleanPath for Posix; for Windows what QDir::cleanPath gives
// in a Windows build ('/' separators, no trailing one except in "X:/",
// "//server/share" keeps its two), except that ".." never leaves the root.
inline QString clean(const QString &path, Style style)
{
    if (style == Style::Posix) return QDir::cleanPath(path);
    if (path.isEmpty()) return path;
    QString text = path;
    text.replace(QLatin1Char('\\'), QLatin1Char('/'));
    // Kept as typed: "X:/", "//server/share" (or "//server" without a share), "/".
    qsizetype root = Detail::drive(text) ? 3 : Detail::share(text);
    if (!root) root = Detail::server(text);
    if (!root && text.startsWith(QLatin1Char('/'))) root = 1;
    QStringList names;
    for (const auto name : QStringView(text).mid(root).split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (name == QLatin1String(".")) continue;
        if (name != QLatin1String("..")) names.append(name.toString());
        else if (!names.isEmpty() && names.last() != QLatin1String("..")) names.removeLast();
        else if (!root) names.append(name.toString());
    }
    const auto rest = names.join(QLatin1Char('/'));
    if (!root) return rest.isEmpty() ? QStringLiteral(".") : rest;
    const auto prefix = text.left(root);
    return rest.isEmpty() || prefix.endsWith(QLatin1Char('/')) ? prefix + rest : prefix + QLatin1Char('/') + rest;
}

// Windows compares with either separator and ignoring case; Posix compares exactly.
inline bool same(const QString &a, const QString &b, Style style)
{
    if (style == Style::Posix) return a == b;
    return QString(a).replace(QLatin1Char('\\'), QLatin1Char('/'))
               .compare(QString(b).replace(QLatin1Char('\\'), QLatin1Char('/')), Qt::CaseInsensitive) == 0;
}

// Whether path is folder or inside it, by whole names: C:/workshop is not in C:/work.
inline bool within(const QString &path, const QString &folder, Style style)
{
    if (style == Style::Posix)
        return QDir::isAbsolutePath(folder)
            && (path == folder || path.startsWith(folder == QLatin1String("/") ? folder : folder + QLatin1Char('/')));
    if (!absolute(folder, style)) return false;
    const auto target = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const auto base = QString(folder).replace(QLatin1Char('\\'), QLatin1Char('/'));
    return target.compare(base, Qt::CaseInsensitive) == 0
        || target.startsWith(base.endsWith(QLatin1Char('/')) ? base : base + QLatin1Char('/'), Qt::CaseInsensitive);
}

// The last name; empty for a root ("/", "C:/", "//server/share") and after a
// trailing separator, as section('/', -1) gives on POSIX.
inline QString leaf(const QString &path, Style style)
{
    if (style == Style::Posix) return path.section(QLatin1Char('/'), -1);
    const auto text = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (const auto end = Detail::share(text); end && end == text.size()) return {};
    return text.section(QLatin1Char('/'), -1);
}

// The form shown to the user: backslashes on Windows, whatever the GUI's platform.
inline QString native(const QString &path, Style style)
{
    if (style == Style::Posix) return path;
    return QString(path).replace(QLatin1Char('/'), QLatin1Char('\\'));
}
} // namespace MachinePlatform
