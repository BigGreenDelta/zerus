#pragma once
#include <QImage>
#include <QJsonObject>
#include <QThreadPool>
#include <QWidget>
#include <atomic>
#include <memory>
class QMediaPlayer;
void registerMessagePlayer(QMediaPlayer *player);
void pauseMessagePlayback(QMediaPlayer *except=nullptr);
class QVideoSink;
class QMediaCaptureSession;
class QCamera;
class QMediaRecorder;
class QAudioInput;
class QTemporaryDir;
class QTimer;

// One coalesced frame conversion, shared by the recorder and tap-to-play viewer.
class RoundVideoView : public QWidget {
    Q_OBJECT
public:
    explicit RoundVideoView(QWidget *parent = nullptr);
    ~RoundVideoView() override;
    QVideoSink *sink() const { return m_sink; }
    QImage image() const { return m_image; }
    void setImage(const QImage &image) { m_image=image; update(); }
    void setMirrored(bool value) { m_mirrored=value; update(); }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QVideoSink *m_sink;
    QThreadPool m_pool;
    QImage m_image;
    bool m_busy=false, m_mirrored=false;
};

class RoundVideoRecorder : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 MaximumBytes=10*1024*1024;
    explicit RoundVideoRecorder(RoundVideoView *view, QObject *parent=nullptr);
    ~RoundVideoRecorder() override;
    virtual void start(const QByteArray &microphone);
    virtual void stop();
    virtual void cancel();
    static QByteArray extractAudio(const QString &path, const std::shared_ptr<std::atomic_bool> &cancelled, QString &error);
signals:
    void started();
    void elapsed(int seconds);
    void preparing();
    void completed(const QByteArray &pcm, const QByteArray &mp4, const QJsonObject &metadata);
    void failed(const QString &error);
private:
    void finish();
    void releaseDevices();
    RoundVideoView *m_view;
    QMediaCaptureSession *m_session=nullptr;
    QCamera *m_camera=nullptr;
    QAudioInput *m_audio=nullptr;
    QMediaRecorder *m_recorder=nullptr;
    QTimer *m_timer=nullptr;
    std::shared_ptr<QTemporaryDir> m_directory;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    bool m_started=false, m_finishing=false;
    quint64 m_generation=0;
};
