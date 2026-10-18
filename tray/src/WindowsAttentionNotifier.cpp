#include "AttentionNotifier.h"
#include "TrayIcon.h"

#include <QIcon>
#include <QPointer>
#include <QSet>
#include <QSystemTrayIcon>

namespace {
// The program's own colored icon (resources.qrc, the artwork of hgs-tray.exe's
// icon): a standard Information icon shows as a generic "i" in a Windows toast.
QIcon programIcon()
{
    QIcon icon;
    for (const int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addFile(QStringLiteral(":/hgs/icons/hgs-zerus-%1.png").arg(size), QSize(size, size));
    return icon;
}

// Windows: an attention notice is a message of the tray icon (a toast on Windows
// 10/11). Delivered messages cannot be retracted, so withdraw() only stops a later
// click from opening a session that no longer needs attention.
class WindowsAttentionNotifier final : public AttentionNotifier {
public:
    explicit WindowsAttentionNotifier(TrayIcon &tray) : m_tray(tray) {}

    void post(const QString &token, const QString &title, const QString &body) override
    {
        if (!QSystemTrayIcon::supportsMessages()) {
            emit failed(token, tr("This desktop does not show tray notifications."));
            return;
        }
        m_pending.insert(token);
        const QPointer<WindowsAttentionNotifier> self(this);
        m_tray.showClickableIconMessage(title, body, programIcon(), 15000, [self, token]() {
            if (self && self->m_pending.contains(token))
                emit self->activated(token, QString());
        });
    }

    void withdraw(const QString &token) override { m_pending.remove(token); }

private:
    TrayIcon &m_tray;
    QSet<QString> m_pending;
};
} // namespace

std::unique_ptr<AttentionNotifier> makeAttentionNotifier(TrayIcon &tray)
{
    return std::make_unique<WindowsAttentionNotifier>(tray);
}
