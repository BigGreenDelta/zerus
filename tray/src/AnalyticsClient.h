#pragma once
#include <QObject>
#include <QDate>
#include <QJsonObject>
#include <functional>

class AnalyticsClient : public QObject {
public:
    using Reply = std::function<void(const QJsonObject &, const QString &)>;
    explicit AnalyticsClient(QString executable, QObject *parent = nullptr);
    void summary(const QString &host, QDate from, QDate through, Reply done);
    void speech(const QString &host, QDate from, QDate through, Reply done);
    // Configuration and purge are deliberately local-only. Both pin the epoch
    // the person reviewed; a failed/uncertain mutation is never retried.
    void configure(const QString &epoch, bool enabled, int retentionDays, Reply done);
    void purge(const QString &epoch, Reply done);
private:
    void request(const QString &host, const QString &verb, const QJsonObject &payload, QDate from, QDate through, Reply done);
    QString m_executable;
};
