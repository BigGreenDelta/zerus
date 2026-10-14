#pragma once
#include <QWidget>
#include <QJsonObject>
#include "FleetState.h"
class QComboBox; class IdentityBadge; class HgsClient; class QListWidget; class QLabel; class QLineEdit; class QSpinBox; class QCheckBox;
class QFormLayout; class QPushButton; class QPlainTextEdit;
class MachinesPage : public QWidget {
    Q_OBJECT
public:
    explicit MachinesPage(const QString &hgsPath, QWidget *parent = nullptr);
    void setFleet(const FleetState &fleet);
    void reload();
    void setTheme(bool dark);
    void selectMachine(const QString &host);
signals:
    void machinesChanged();
    void viewSessionsRequested(const QString &host);
    void sshTerminalRequested(const QString &alias);
    void appearanceChanged();
    void accountsRequested(const QString &host);
protected:
    void showEvent(QShowEvent *) override;
private:
    void renderRows();
    void loadSelection();
    void updateControls();
    void changed();
    void save();
    void check();
    void install();
    void enableLocalSessions();
    void report(const QString &text, bool error = false);
    QJsonObject connection() const;
    QString alias() const;
    bool aliasExists() const;
    bool discardChanges();
    void resolveConnection();
    void showEffectiveConnection(const QJsonObject &data);
    void updateAppearance();
    QString appearanceMachine() const;
    HgsClient *m_client;
    FleetState m_fleet;
    QJsonObject m_data;
    QListWidget *m_list;
    QLabel *m_summary, *m_title, *m_status, *m_origin, *m_effective, *m_colorHint, *m_localHost;
    QLineEdit *m_alias, *m_hostname, *m_user, *m_key;
    QSpinBox *m_port;
    QCheckBox *m_enabled;
    QFormLayout *m_form;
    QPushButton *m_save, *m_check, *m_ssh, *m_install, *m_remove, *m_reset, *m_sessions, *m_accounts, *m_add, *m_browse, *m_refresh;
    QPushButton *m_discardDraft, *m_enableLocal;
    QPushButton *m_color, *m_colorReset;
    QComboBox *m_colorStyle;
    IdentityBadge *m_colorPreview;
    QPlainTextEdit *m_log;
    QString m_selected = "@local", m_operation, m_pendingSelection;
    quint64 m_request = 0, m_resolveRequest = 0, m_hostRequest = 0;
    QString m_resolveAlias;
    // Shown for this computer once it enabled local sessions: the stable CLI
    // follows from the next start (spec D2).
    QString m_localNotice;
    QJsonObject m_effectiveData;
    bool m_loading = false, m_dirty = false, m_new = false, m_installing = false;
    bool m_discardPrompt = false;
};
