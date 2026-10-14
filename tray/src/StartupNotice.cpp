#include "StartupNotice.h"

#include <QScreen>

StartupNotice::StartupNotice(int delayMs)
    : QLabel(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setTextFormat(Qt::PlainText);
    setAlignment(Qt::AlignCenter);
    setFrameStyle(QFrame::Box | QFrame::Plain);
    setMargin(16);
    m_delay.setSingleShot(true);
    m_delay.setInterval(delayMs);
    connect(&m_delay, &QTimer::timeout, this, [this] {
        place();
        show();
    });
}

void StartupNotice::step(const QString &action)
{
    setText(action == QLatin1String("install")
                ? tr("Starting Zerus…\nUpdating the hgs command line for local sessions")
                : tr("Starting Zerus…"));
    if (isVisible())
        place();
    else if (!m_delay.isActive())
        m_delay.start();
}

// Centred on the screen, at the size of its text.
void StartupNotice::place()
{
    adjustSize();
    if (const QScreen *display = screen())
        move(display->availableGeometry().center() - rect().center());
}
