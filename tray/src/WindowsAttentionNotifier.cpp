#include "AttentionNotifier.h"
#include "TrayIcon.h"

#include <QPointer>
#include <QSet>
#include <QSystemTrayIcon>

namespace {
// Windows: an attention notice is a message of the tray icon (a toast on Windows
// 10/11). Delivered messages cannot be retracted, so withdraw() only stops a later
// click from opening a session that no longer needs attention.
class WindowsAttentionNotifier final : public AttentionNotifier {
public:
    explicit WindowsAttentionNotifier(TrayIcon &tray) : m_tray(tray) {}

    // A tray message has no action button; a click on it opens the target.
    void post(const QString &token, const QString &title, const QString &body, const QString &action = {}) override
    {
        Q_UNUSED(action);
        if (!QSystemTrayIcon::supportsMessages()) {
            emit failed(token, tr("This desktop does not show tray notifications."));
            return;
        }
        m_pending.insert(token);
        const QPointer<WindowsAttentionNotifier> self(this);
        m_tray.showClickableMessage(title, body, QSystemTrayIcon::Information, 15000, [self, token]() {
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
