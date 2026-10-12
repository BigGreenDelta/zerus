#pragma once
#include "MessageComposer.h"
#include <QElapsedTimer>
#include <QObject>
#include <QUrl>

class SpeechClient;
class SpeechRecorder;
class ToolbarChip;
class QTimer;

class SpeechComposer : public QObject {
    Q_OBJECT
public:
    explicit SpeechComposer(const QString &executable, MessageComposer *composer,
                            std::function<QString()> conversationIdentity, SpeechRecorder *recorder = nullptr,
                            std::function<bool(const QUrl &)> openSettings = {});
    static QUrl microphoneSettingsUrl();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    enum class State { Idle, Checking, Starting, Recording, Submitting, Recognizing, Uncertain, Saving };
    enum class Notice { General, MicrophonePermission };
    void start();
    void submitRecording(const QByteArray &base64);
    void cancel();
    void poll();
    void result(const QJsonObject &reply, const QString &error);
    void completed(const QString &text);
    void update();
    void showResult();
    void fail(const QString &error, Notice notice = Notice::General);
    void showPermissionRecovery();
    void clearPending();
    MessageComposer *m_composer;
    SpeechClient *m_client;
    SpeechRecorder *m_recorder;
    ToolbarChip *m_voice, *m_cancel, *m_result;
    QTimer *m_poll;
    std::function<QString()> m_identity;
    std::function<bool(const QUrl &)> m_openSettings;
    MessageComposer::VoiceTarget m_target;
    QString m_originalIdentity, m_host, m_profile, m_job, m_recoveryKey, m_text, m_notice;
    ComposerDraftStore m_store;
    QString m_pendingKey;
    bool m_loadingPending = true;
    State m_state = State::Idle;
    Notice m_noticeKind = Notice::General;
    quint64 m_generation = 0;
    int m_seconds = 0;
    QElapsedTimer m_deadline;
};
