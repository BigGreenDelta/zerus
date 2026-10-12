#include "SpeechRecorder.h"
#include <QAudioDevice>
#include <QAudioSource>
#include <QCoreApplication>
#include <QMediaDevices>
#ifdef Q_OS_MACOS
#include <QJsonObject>
#include <QPluginLoader>
#endif
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <memory>

SpeechPcmConverter::SpeechPcmConverter(QAudioFormat format) : m_format(format) { m_pcm.reserve(MaximumBytes); }
bool SpeechPcmConverter::valid() const
{
    return m_format.isValid() && m_format.sampleRate() >= 8000 && m_format.sampleRate() <= 192000
        && m_format.channelCount() > 0 && m_format.channelCount() <= 32;
}
void SpeechPcmConverter::append(const QByteArray &bytes)
{
    if (!valid() || full()) return;
    m_partial.append(bytes);
    const int frame = m_format.bytesPerFrame(), sampleBytes = m_format.bytesPerSample();
    qsizetype cursor = 0;
    while (m_partial.size() - cursor >= frame) {
        double sample = 0;
        for (int channel = 0; channel < m_format.channelCount(); ++channel) {
            const char *data = m_partial.constData() + cursor + channel * sampleBytes;
            switch (m_format.sampleFormat()) {
            case QAudioFormat::UInt8: sample += (quint8(*data) - 128) / 128.; break;
            case QAudioFormat::Int16: { qint16 value; std::memcpy(&value, data, 2); sample += value / 32768.; break; }
            case QAudioFormat::Int32: { qint32 value; std::memcpy(&value, data, 4); sample += value / 2147483648.; break; }
            case QAudioFormat::Float: { float value; std::memcpy(&value, data, 4); sample += std::isfinite(value) ? value : 0; break; }
            default: break;
            }
        }
        m_samples.append(float(qBound(-1., sample / m_format.channelCount(), 1.))); cursor += frame;
    }
    m_partial.remove(0, cursor); convert(false);
}
void SpeechPcmConverter::convert(bool finishing)
{
    // Windowed sinc removes frequencies above the target Nyquist limit. Retain
    // the kernel history between device chunks, including partial input frames.
    constexpr int radius = 32;
    constexpr double pi = 3.14159265358979323846;
    const double ratio = m_format.sampleRate() / 16000.;
    const double cutoff = qMin(1., 1. / ratio) * .94;
    const qint64 end = m_offset + m_samples.size();
    while (!full()) {
        const double position = m_outputFrames * ratio;
        if (position >= end || (!finishing && position + radius >= end)) break;
        double value = 0, weights = 0;
        const qint64 center = qint64(std::floor(position));
        for (qint64 i = center - radius + 1; i <= center + radius; ++i) {
            const double x = (i - position) * cutoff;
            const double sinc = std::abs(x) < 1e-10 ? 1 : std::sin(pi * x) / (pi * x);
            const double weight = cutoff * sinc * (.5 + .5 * std::cos(pi * (i - position) / radius));
            weights += weight;
            if (i >= m_offset && i < end) value += weight * m_samples[qsizetype(i - m_offset)];
        }
        const auto sample = qint16(std::lround(qBound(-1., value / weights, 1.) * 32767.));
        char encoded[2]; qToLittleEndian(sample, encoded); m_pcm.append(encoded, 2); ++m_outputFrames;
    }
    const auto discard = qBound(qint64(0), qint64(m_outputFrames * ratio) - radius - m_offset, qint64(m_samples.size()));
    m_samples.remove(0, qsizetype(discard)); m_offset += discard;
}
QByteArray SpeechPcmConverter::finish() { if (valid()) convert(true); return std::move(m_pcm); }

class SpeechRecorderWorker : public QObject {
    Q_OBJECT
public:
    void start(const QByteArray &deviceId) {
        cancel();
        QAudioDevice device = QMediaDevices::defaultAudioInput();
        if (!deviceId.isEmpty()) {
            device = {};
            for (const auto &candidate : QMediaDevices::audioInputs()) if (candidate.id() == deviceId) device = candidate;
        }
        if (device.isNull()) { emit failed(SpeechRecorder::Failure::Recording, tr("The selected microphone is unavailable. Choose one in Settings > Voice input.")); return; }
        QAudioFormat format; format.setSampleRate(16000); format.setChannelCount(1); format.setSampleFormat(QAudioFormat::Int16);
        if (!device.isFormatSupported(format)) format = device.preferredFormat();
        converter = std::make_unique<SpeechPcmConverter>(format);
        if (!converter->valid()) { converter.reset(); emit failed(SpeechRecorder::Failure::Recording, tr("This microphone's audio format is unsupported.")); return; }
        source = new QAudioSource(device, format, this);
        connect(source, &QAudioSource::stateChanged, this, [this](QtAudio::State state) {
            if (source && state == QtAudio::StoppedState && source->error() != QtAudio::NoError) {
                cancel(); emit failed(SpeechRecorder::Failure::Recording, tr("Microphone recording failed. Check microphone permissions and device availability."));
            }
        }, Qt::QueuedConnection);
        input = source->start();
        if (!source || !input) { cancel(); emit failed(SpeechRecorder::Failure::Recording, tr("Could not open the microphone.")); return; }
        connect(input, &QIODevice::readyRead, this, &SpeechRecorderWorker::read);
        timer = new QTimer(this); timer->setInterval(1000); seconds = 0;
        connect(timer, &QTimer::timeout, this, [this] { emit elapsed(++seconds); if (seconds >= 120) stop(); });
        timer->start(); emit started();
    }
    void read() {
        if (!input || !converter) return;
        while (input->bytesAvailable() > 0 && !converter->full()) converter->append(input->read(16384));
        if (converter->full()) QTimer::singleShot(0, this, &SpeechRecorderWorker::stop);
    }
    void stop() {
        if (!converter) return;
        read();
        const auto pcm = converter->finish(); cancel();
        if (pcm.size() < 3200) { emit failed(SpeechRecorder::Failure::Recording, tr("Recording was too short. Speak for at least a tenth of a second.")); return; }
        emit recorded(pcm.toBase64());
    }
    void cancel() {
        if (timer) { delete timer; timer = nullptr; }
        if (source) { auto *previous = source; source = nullptr; previous->disconnect(this); previous->stop(); delete previous; }
        input = nullptr; converter.reset();
    }
signals:
    void started();
    void elapsed(int seconds);
    void recorded(const QByteArray &base64);
    void failed(SpeechRecorder::Failure reason, const QString &error);
private:
    QAudioSource *source = nullptr;
    QIODevice *input = nullptr;
    QTimer *timer = nullptr;
    std::unique_ptr<SpeechPcmConverter> converter;
    int seconds = 0;
};

SpeechRecorder::SpeechRecorder(QObject *parent) : QObject(parent), m_worker(new SpeechRecorderWorker)
{
    m_thread.setObjectName("speechCapture"); m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SpeechRecorderWorker::started, this, &SpeechRecorder::started);
    connect(m_worker, &SpeechRecorderWorker::elapsed, this, &SpeechRecorder::elapsed);
    connect(m_worker, &SpeechRecorderWorker::recorded, this, &SpeechRecorder::recorded);
    connect(m_worker, &SpeechRecorderWorker::failed, this, &SpeechRecorder::failed);
    m_thread.start();
}
SpeechRecorder::~SpeechRecorder()
{
    QMetaObject::invokeMethod(m_worker, [this] { m_worker->cancel(); }, Qt::BlockingQueuedConnection);
    m_thread.quit(); m_thread.wait();
}
bool SpeechRecorder::microphonePermissionBackendAvailable()
{
#ifdef Q_OS_MACOS
    // Darwin permission backends are static plugins even with shared Qt. Without
    // this import Qt reports Denied without ever consulting the system's consent.
    for (const auto &plugin : QPluginLoader::staticPlugins()) {
        if (plugin.metaData().value("className").toString() == QStringLiteral("QDarwinMicrophonePermissionPlugin")) return true;
    }
    return false;
#else
    return true;
#endif
}
void SpeechRecorder::start(const QByteArray &deviceId)
{
    const auto generation = ++m_generation;
    if (!microphonePermissionBackendAvailable()) {
        emit failed(Failure::Setup, tr("This Zerus installation cannot request microphone access. Update or reinstall Zerus, then press Voice again."));
        return;
    }
#if QT_CONFIG(permissions)
    QMicrophonePermission permission;
    const auto status = qApp->checkPermission(permission);
    if (status == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, deviceId, generation](const QPermission &result) {
            if (generation != m_generation) return;
            if (result.status() == Qt::PermissionStatus::Granted) start(deviceId);
            else emit failed(SpeechRecorder::Failure::PermissionDenied, tr("Microphone permission was denied."));
        }); return;
    }
    if (status != Qt::PermissionStatus::Granted) { emit failed(SpeechRecorder::Failure::PermissionDenied, tr("Microphone permission was denied.")); return; }
#else
    Q_UNUSED(generation);
#endif
    QMetaObject::invokeMethod(m_worker, [this, deviceId] { m_worker->start(deviceId); }, Qt::QueuedConnection);
}
void SpeechRecorder::stop() { QMetaObject::invokeMethod(m_worker, &SpeechRecorderWorker::stop, Qt::QueuedConnection); }
void SpeechRecorder::cancel() { ++m_generation; QMetaObject::invokeMethod(m_worker, &SpeechRecorderWorker::cancel, Qt::QueuedConnection); }

#include "SpeechRecorder.moc"
