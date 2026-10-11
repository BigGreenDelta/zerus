#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

// The QLocalServer name a second hgs-tray uses to find the first. On Unix it is a
// socket in the user's own runtime directory. On Windows it is a named pipe in a
// namespace shared by every user of the machine, so the name carries a hash of the
// user's profile directory (case-insensitive, like Windows paths).
namespace SingleInstance {
inline QString key(const QString &userProfile = {})
{
    if (userProfile.isEmpty())
        return QStringLiteral("hgs-tray");
    const QByteArray digest = QCryptographicHash::hash(userProfile.toCaseFolded().toUtf8(),
                                                       QCryptographicHash::Sha256);
    return QStringLiteral("hgs-tray-") + QString::fromLatin1(digest.toHex().left(16));
}
} // namespace SingleInstance
