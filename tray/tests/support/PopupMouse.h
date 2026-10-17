#pragma once
#include <QGuiApplication>
#include <QTest>
#include <QWidget>

namespace PopupMouse {
#ifdef Q_OS_MACOS
bool postCocoaClick(QWidget *window, Qt::MouseButton button, QPoint position);
#endif

// Cocoa's modal session can discard an outside click before it reaches Qt.
// Exercise that native dispatch path instead of injecting a Qt window event.
inline bool click(QWidget *window, Qt::MouseButton button, QPoint position)
{
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() == "cocoa")
        return postCocoaClick(window, button, position);
#endif
    QTest::mouseClick(window->windowHandle(), button, Qt::NoModifier, position);
    return true;
}
}
