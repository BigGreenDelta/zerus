#pragma once
#include <QDialog>
#include <QJsonObject>
#include "MachinePlatform.h"
#include "SessionOrganization.h"
class HgsClient;
class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QProgressBar;

// One fixed computer; native Git owns credentials. An uncertain mutation keeps
// its durable request ID across dialog/app restarts and offers receipt reads.
class CloneRepositoryDialog : public QDialog {
    Q_OBJECT
public:
    CloneRepositoryDialog(HgsClient *client,const QString &host,const QString &machine,
        MachinePlatform::Style style,const SessionOrganization &projects,const QString &project,
        bool startSession,QWidget *parent=nullptr);
    QString createdPath() const {return m_created;}
    QString projectId() const;
    QString newProjectName() const;
    void reject() override;
    static QString suggestedName(const QString &url);
private:
    void updateForm();
    void begin();
    void finish(quint64 request,bool ok,const QJsonObject &data,const QString &error);
    QString settingsKey() const;
    QString destination() const;
    HgsClient *m_client;
    QString m_host,m_created,m_token,m_action,m_expected;
    MachinePlatform::Style m_style;
    QComboBox *m_project;
    QLineEdit *m_url,*m_parent,*m_name,*m_projectName;
    QLabel *m_path,*m_error;
    QPushButton *m_browse,*m_accept,*m_cancel,*m_check,*m_another;
    QProgressBar *m_progress;
    quint64 m_request=0;
    bool m_customName=false,m_customProject=false,m_uncertain=false,m_startSession=false;
};
