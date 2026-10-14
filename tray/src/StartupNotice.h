#pragma once

#include <QLabel>
#include <QString>
#include <QTimer>

// A small frameless note while a Windows GUI picks its hgs before the tray icon
// exists (spec D2, stableHgs in main.cpp): `hgs session-host status` may take 15 s
// and the refresh `session-host install` 60 s. It appears only once the check has
// run for delayMs, so a quick start shows nothing, never takes the focus and goes
// with the object. main.cpp creates it on Windows only; the class itself is
// portable so the tests run everywhere.
class StartupNotice : public QLabel {
    Q_OBJECT
public:
    explicit StartupNotice(int delayMs = 500);
    // The `hgs session-host` action that starts now. The first one starts the
    // delay; later ones only change the text.
    void step(const QString &action);

private:
    void place();
    QTimer m_delay;
};
