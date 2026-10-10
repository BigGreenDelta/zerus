#pragma once

#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QStringList>

class FleetState;

// Quick access only. Session and project management live in Zerus.
class TrayMenu : public QObject {
    Q_OBJECT
public:
    explicit TrayMenu(const QStringList &openModes, const QString &currentMode,
                      bool terminalTemplateSet, QObject *parent = nullptr);
    ~TrayMenu() override;

    QMenu *menu() const { return m_menu.data(); }
    bool isOpen() const { return m_open; }
    void rebuild(const FleetState &state);

signals:
    void aboutToShow();
    void updatesRequested();
    // A remote tty belongs to another machine and must never raise a local window.
    void sessionActivated(const QString &peer, const QString &session, const QString &tty);
    void savedSessionRequested(const QString &peer, const QString &session);
    void openModeChosen(const QString &mode);
    void sessionsRequested();
    void refreshRequested();
    void quitRequested();

private:
    // SniTrayIcon owns and deletes the menu before us; QtTrayIcon leaves it to us.
    QPointer<QMenu> m_menu;
    // QMenu::clear() detaches submenus without deleting them. Keep one instance and
    // stable actions for the DBusMenu exporter; repeated rebuilding leaks otherwise.
    QMenu *m_openModeMenu;
    // Plasma's exporter forwards aboutToShow but never aboutToHide. After the first
    // opening, Linux refreshes synchronously on every aboutToShow and peer response.
    // AppKit forwards both, so background rebuilds resume when its menu closes.
    bool m_open = false;
};
