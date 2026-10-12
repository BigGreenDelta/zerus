#pragma once
#include <QWidget>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <functional>
class SpeechClient;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QUrl;

class SpeechSettings : public QWidget {
    Q_OBJECT
public:
    explicit SpeechSettings(const QString &executable, QWidget *parent = nullptr,
                            std::function<bool(const QUrl &)> openDocumentation = {});
    void setPeers(const QStringList &peers);
    void refresh();
private:
    using OperationReply = std::function<void(const QJsonObject &, const QString &)>;
    void microphones();
    void controls();
    void editProfile(bool create);
    void removeProfile();
    void testProfile();
    void useDefault();
    QJsonObject selectedProfile() const;
    void operation(const QString &host, const QString &machine, QJsonObject payload, OperationReply done = {});
    void operationResult(const QJsonObject &reply, const QString &error, bool initial = false);
    void checkOperation();
    SpeechClient *m_client;
    std::function<bool(const QUrl &)> m_openDocumentation;
    QComboBox *m_host, *m_microphone;
    QListWidget *m_profiles;
    QLabel *m_status, *m_statistics, *m_default, *m_pendingStatus, *m_result, *m_profileDetails;
    QPushButton *m_refresh, *m_add, *m_edit, *m_remove, *m_test, *m_useDefault, *m_check;
    QString m_machine, m_selectAfterRefresh;
    qint64 m_revision = 0;
    QJsonArray m_adapters;
    QJsonObject m_pending, m_service;
    OperationReply m_operationDone;
    bool m_loaded = false, m_busy = false, m_waiting = false;
    int m_statusPolls = 0;
    QElapsedTimer m_statusClock;
    quint64 m_generation = 0;
};
