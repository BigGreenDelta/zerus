#pragma once
#include <QAudioFormat>
#include <QByteArray>
#include <QObject>
#include <QThread>
#include <QVector>

// Streaming, bounded conversion; samples and final wire encoding stay off GUI.
class SpeechPcmConverter {
public:
    static constexpr qsizetype MaximumBytes = 16000 * 2 * 120;
    explicit SpeechPcmConverter(QAudioFormat format);
    bool valid() const;
    void append(const QByteArray &bytes);
    QByteArray finish();
    bool full() const { return m_pcm.size() >= MaximumBytes; }
private:
    void convert(bool finishing);
    QAudioFormat m_format;
    QByteArray m_partial, m_pcm;
    QVector<float> m_samples;
    qint64 m_offset = 0, m_outputFrames = 0;
};

class SpeechRecorderWorker;
class SpeechRecorder : public QObject {
    Q_OBJECT
public:
    enum class Failure { Recording, PermissionDenied, Setup };
    Q_ENUM(Failure)
    explicit SpeechRecorder(QObject *parent = nullptr);
    ~SpeechRecorder() override;
    // Read-only: never checks consent, requests permission or opens a device.
    static bool microphonePermissionBackendAvailable();
    virtual void start(const QByteArray &deviceId);
    virtual void stop();
    virtual void cancel();
signals:
    void started();
    void elapsed(int seconds);
    void recorded(const QByteArray &base64);
    void failed(SpeechRecorder::Failure reason, const QString &error);
private:
    QThread m_thread;
    SpeechRecorderWorker *m_worker;
    quint64 m_generation = 0;
};
