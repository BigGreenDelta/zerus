#pragma once
#include <QWidget>
#include <QStringList>
class SpeechClient;
class QComboBox;
class QLabel;
class QPushButton;

class SpeechSettings : public QWidget {
    Q_OBJECT
public:
    explicit SpeechSettings(const QString &executable, QWidget *parent = nullptr);
    void setPeers(const QStringList &peers);
    void refresh();
private:
    void microphones();
    SpeechClient *m_client;
    QComboBox *m_host, *m_profile, *m_microphone;
    QLabel *m_status;
    QLabel *m_statistics;
    QPushButton *m_refresh;
    quint64 m_generation = 0;
};
