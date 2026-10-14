#pragma once

#include "LocalSessionHost.h"

#include <QMessageBox>
#include <QObject>
#include <QPushButton>

namespace LocalSessionHost {
// Asks before a launch on host goes into a session host of another Windows session
// (otherWindowsSession). True when no warning applies or the user chose Launch
// anyway; Cancel changes nothing, so callers return and keep their drafts and
// dialogs as they were. Remote and Linux or macOS launches never ask.
inline bool confirmLaunch(QWidget *parent, const BoxState &local, const QString &host)
{
    const auto other = otherWindowsSession(local, host);
    if (!other) return true;
    QMessageBox box(QMessageBox::Warning, QObject::tr("Launch in another Windows session?"),
        QObject::tr("The session host on this computer runs in another Windows session (%1), for example one "
                    "started over SSH. Agents it starts cannot show windows on this desktop, such as a browser "
                    "sign-in. Launch anyway?").arg(*other),
        QMessageBox::NoButton, parent);
    box.setObjectName(QStringLiteral("otherWindowsSessionWarning"));
    box.setTextFormat(Qt::PlainText);
    auto *launch = box.addButton(QObject::tr("Launch anyway"), QMessageBox::AcceptRole);
    launch->setObjectName(QStringLiteral("launchAnyway"));
    auto *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel); box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == launch;
}
} // namespace LocalSessionHost
