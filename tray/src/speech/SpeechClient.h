#pragma once
#include <QObject>
#include <QJsonObject>
#include <QSet>
#include <functional>

// Every operation is bounded and launched on ProcessRunner's worker. Probe the
// exact host before calling speech: old hgs versions treat unknown verbs as sessions.
class SpeechClient : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(const QJsonObject &, const QString &)>;
    explicit SpeechClient(QString executable, QObject *parent = nullptr);
    void request(const QString &host, const QJsonObject &payload, Reply done);
private:
    void dispatch(const QString &host, const QJsonObject &payload, Reply done);
    QString m_executable;
    QSet<QString> m_supported;
};
