#pragma once
#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <vector>

class HgsClient; class QComboBox; class QLabel; class QPushButton; class QTimer; class QTreeWidget;

// Phones paired with the workspaces this computer reaches: through its own
// mobile connection, or through one enabled peer that has one (hgs @peer
// mobile, one hop). Pairing shows a single-use QR code; revoking needs
// confirmation. See docs/superpowers/specs/2026-10-17-desktop-phone-management-design.md.
namespace PhonePairing {

// A workspace reached through a host ("" is this computer).
struct Target {
    QString host, workspaceId, workspaceName, serverUrl;
};

// The QR modules of `text` (row-major, true = dark), with the side length.
// Empty for text that does not fit a QR code.
std::vector<bool> encode(const QString &text, int *size);

class QrCodeView : public QWidget {
public:
    explicit QrCodeView(QWidget *parent = nullptr);
    void setText(const QString &text);
    void setFaded(bool faded);
    QString text() const { return m_text; }
    int modules() const { return m_size; }
    QSize sizeHint() const override { return {264, 264}; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QString m_text;
    std::vector<bool> m_modules;
    int m_size = 0;
    bool m_faded = false;
};

class Dialog : public QDialog {
public:
    // `now` (seconds since the epoch) is injectable for tests.
    Dialog(const QString &executable, const QList<Target> &targets, int selected, QWidget *parent = nullptr,
           std::function<double()> now = {});
    void request();
private:
    void showInvitation(const QJsonObject &invitation);
    void tick();
    HgsClient *m_client;
    QList<Target> m_targets;
    std::function<double()> m_now;
    QComboBox *m_workspace = nullptr;
    QrCodeView *m_code;
    QLabel *m_server, *m_pairCode, *m_expiry, *m_status;
    QPushButton *m_copyCode, *m_copyLink, *m_again;
    QTimer *m_timer;
    QString m_link, m_codeText;
    double m_expires = 0;
    quint64 m_request = 0;
};

class Panel : public QWidget {
public:
    // `confirm` replaces the revoke confirmation in tests.
    explicit Panel(const QString &executable, QWidget *parent = nullptr,
                   std::function<bool(const QString &phone, const QString &workspace)> confirm = {});
    void refresh();
    QList<Target> targets() const { return m_targets; }
protected:
    void showEvent(QShowEvent *) override;
private:
    void finish();
    void render();
    void revoke();
    void pair();
    QString m_executable;
    HgsClient *m_client;
    std::function<bool(const QString &, const QString &)> m_confirm;
    QTreeWidget *m_tree;
    QLabel *m_status;
    QPushButton *m_pair, *m_revoke, *m_refresh;
    QList<Target> m_targets;
    QList<QJsonObject> m_lists;
    QStringList m_hostOrder;
    QStringList m_problems;
    QSet<quint64> m_pending;
    quint64 m_machines = 0, m_revoking = 0;
    bool m_loaded = false;
};

} // namespace PhonePairing
