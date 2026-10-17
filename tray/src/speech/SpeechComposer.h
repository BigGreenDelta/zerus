#pragma once
#include "MessageComposer.h"
#include <QObject>
#include <QUrl>

class RoundVideoView;
class RoundVideoRecorder;
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
        // Why an unavailable target can never take voice: the voice button's tooltip.
        QString reason;
        bool sameConversation(const Target &other) const {
            return key == other.key && host == other.host && session == other.session
                && run == other.run && conversation == other.conversation;
        }
    };
    explicit SpeechComposer(const QString &executable, MessageComposer *composer,
                            std::function<Target()> target, SpeechRecorder *recorder = nullptr,
                            std::function<bool(const QUrl &)> openSettings = {},
                            RoundVideoRecorder *videoRecorder = nullptr);
    static QUrl microphoneSettingsUrl();
signals:
    void messageChanged(const QString &sessionKey, const QJsonObject &message);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    enum class State { Idle, Starting, Recording, Saving, Preview, Submitting, Uncertain };
    enum class Notice { General, MicrophonePermission };
    void start();
    void requestSendRecording();
    void stopRecording();
    void recorded(const QByteArray &base64);
    void send();
    void check();
    void result(const QJsonObject &reply, const QString &error);
    void discard();
    void syncTarget();
    void saveVoice(std::function<void(bool)> done, bool requireGeneration = true);
    void loadProfiles();
    QJsonObject selectedProfile() const;
    void saveProfileSelection();
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
    RoundVideoView *m_videoView;
    RoundVideoRecorder *m_videoRecorder;
    QPushButton *m_stopPreview;
    bool m_videoMode=false, m_captureVideo=false, m_videoSupported=false;
    bool m_sendAfterSaved=false;
    bool m_previewBeforeSend=false;
    QByteArray m_video;
    QJsonObject m_videoMeta;
    ToolbarChip *m_voice, *m_mode, *m_cancel, *m_result;
    QWidget *m_preview;
    QLabel *m_previewLabel, *m_recordingHint;
    QPushButton *m_play, *m_send, *m_discard, *m_refresh;
    QComboBox *m_provider;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_output = nullptr;
    std::function<Target()> m_currentTarget;
    std::function<bool(const QUrl &)> m_openSettings;
    Target m_target;
    ComposerDraftStore m_store;
    QByteArray m_pcm;
    QString m_id, m_notice, m_loadedKey;
    QString m_profileError;
    QJsonObject m_selectedProfile;
    bool m_loading = false, m_syncing = false;
    bool m_profilesLoading = false, m_profilesValidated = false, m_profileBound = false;
    State m_state = State::Idle;
    Notice m_noticeKind = Notice::General;
    quint64 m_generation = 0;
    quint64 m_profileGeneration = 0;
    int m_seconds = 0;
    QSet<int> m_stopKeys;
};
