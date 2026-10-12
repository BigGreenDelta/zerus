#pragma once
#include "MessageComposer.h"
#include <QObject>
#include <QUrl>

class SpeechClient;
class SpeechRecorder;
class ToolbarChip;
class QComboBox;
class QLabel;
class QMediaPlayer;
class QAudioOutput;

class SpeechComposer : public QObject {
    Q_OBJECT
public:
    struct Target {
        QString key, host, session, run, conversation, warning;
        bool available = false;
        bool sameConversation(const Target &other) const {
            return key == other.key && host == other.host && session == other.session
                && run == other.run && conversation == other.conversation;
        }
    };
    explicit SpeechComposer(const QString &executable, MessageComposer *composer,
                            std::function<Target()> target, SpeechRecorder *recorder = nullptr,
                            std::function<bool(const QUrl &)> openSettings = {});
    static QUrl microphoneSettingsUrl();
signals:
    void messageChanged(const QString &sessionKey, const QJsonObject &message);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    enum class State { Idle, Checking, Starting, Recording, Saving, Preview, Submitting, Uncertain };
    enum class Notice { General, MicrophonePermission };
    void start();
    void recorded(const QByteArray &base64);
    void send();
    void check();
    void result(const QJsonObject &reply, const QString &error);
    void discard();
    void syncTarget();
    void saveVoice(std::function<void(bool)> done, bool requireGeneration = true);
    void loadProfiles(std::function<void(bool)> done);
    void update();
    void play();
    void stopPlayback();
    void showResult();
    void fail(const QString &error, Notice notice = Notice::General);
    void showPermissionRecovery();
    QJsonObject identityPayload(const QString &action) const;
    QString storageKey() const;
    MessageComposer *m_composer;
    SpeechClient *m_client;
    SpeechRecorder *m_recorder;
    ToolbarChip *m_voice, *m_cancel, *m_result;
    QWidget *m_preview;
    QLabel *m_previewLabel;
    QPushButton *m_play, *m_send, *m_discard;
    QComboBox *m_provider;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    std::function<Target()> m_currentTarget;
    std::function<bool(const QUrl &)> m_openSettings;
    Target m_target;
    ComposerDraftStore m_store;
    QByteArray m_pcm;
    QString m_id, m_notice, m_loadedKey;
    bool m_loading = false, m_syncing = false;
    State m_state = State::Idle;
    Notice m_noticeKind = Notice::General;
    quint64 m_generation = 0;
    int m_seconds = 0;
};
