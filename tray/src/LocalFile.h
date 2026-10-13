#pragma once
#include <QFile>
#include <QFileInfo>

// Qt on Windows describes an existing *.lnk by its shortcut target (type, size,
// canonical path), while QFile opens and reads the .lnk file itself. Code that
// reads a user's file judges that file, not the place its shortcut points to.
namespace LocalFile {
inline bool readable(const QFileInfo &info) { return info.isFile() || info.isShortcut(); }
// The entry's own size; a shortcut is measured through an open handle, or -1.
inline qint64 size(const QFileInfo &info)
{
    if (!info.isShortcut()) return info.size();
    QFile file(info.filePath());
    return file.open(QIODevice::ReadOnly) ? file.size() : -1;
}
}
