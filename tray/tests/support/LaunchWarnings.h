#pragma once

#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <utility>

// Answers each warning about a session host of another Windows session
// (LocalLaunchWarning.h) under parent once it is shown, with answer:
// "launchAnyway" or "cancel". The warning is modal, so its launch waits for this
// answer however slowly the window appears; shown counts the warnings and texts
// keeps what they said.
class LaunchWarnings {
public:
    explicit LaunchWarnings(QWidget &parent, QString initial = QStringLiteral("cancel")) : answer(std::move(initial))
    {
        m_timer.setInterval(10);
        QObject::connect(&m_timer, &QTimer::timeout, &parent, [this, &parent] {
            for (auto *box : parent.findChildren<QMessageBox *>(QStringLiteral("otherWindowsSessionWarning"))) {
                if (!box->isVisible() || box->property("answered").toBool()) continue;
                box->setProperty("answered", true); ++shown; texts << box->text();
                auto *button = answer == QLatin1String("cancel") ? box->button(QMessageBox::Cancel)
                                                                 : box->findChild<QPushButton *>(answer);
                // An unknown answer cancels rather than leave the launch waiting.
                (button ? button : box->button(QMessageBox::Cancel))->click();
            }
        });
        m_timer.start();
    }
    QString answer;
    int shown = 0;
    QStringList texts;
private:
    QTimer m_timer;
};
