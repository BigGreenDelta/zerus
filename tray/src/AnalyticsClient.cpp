#include "AnalyticsClient.h"
#include "AnalyticsData.h"
#include "ProcessRunner.h"
#include <QJsonDocument>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

namespace {
ProcessRunner::Request command(const QString &executable, const QString &host, QStringList args) {
    ProcessRunner::Request request; request.program = executable.isEmpty() ? QStringLiteral("hgs") : executable;
    if (!host.isEmpty()) args.prepend('@' + host);
    request.arguments = args; request.input = QByteArray(); request.timeoutMs = 30000;
    request.maximumOutputBytes = AnalyticsData::MaximumBytes;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("HGS_SESSION"); environment.remove("HGS_RUN_ID"); request.environment = environment;
    return request;
}
bool success(const ProcessRunner::Result &result) {
    return result.outcome == ProcessRunner::Result::Finished && result.exitStatus == QProcess::NormalExit
        && result.exitCode == 0 && !result.outputLimitExceeded;
}
}

AnalyticsClient::AnalyticsClient(QString executable, QObject *parent) : QObject(parent), m_executable(std::move(executable)) {}

void AnalyticsClient::summary(const QString &host, QDate from, QDate through, Reply done) {
    request(host,"analytics",{{"schema",1},{"action","summary"},{"from",from.toString(Qt::ISODate)},{"through",through.toString(Qt::ISODate)}},from,through,std::move(done));
}
void AnalyticsClient::speech(const QString &host, QDate from, QDate through, Reply done) {
    request(host,"speech",{{"action","stats"}},from,through,std::move(done));
}
void AnalyticsClient::configure(const QString &epoch, bool enabled, int retentionDays, Reply done) {
    request({},"analytics",{{"schema",1},{"action","configure"},{"expected_epoch",epoch},{"enabled",enabled},{"retention_days",retentionDays}},{},{},std::move(done));
}
void AnalyticsClient::purge(const QString &epoch, Reply done) {
    request({},"analytics",{{"schema",1},{"action","purge"},{"expected_epoch",epoch},{"confirm",true}},{},{},std::move(done));
}

void AnalyticsClient::request(const QString &host, const QString &verb, const QJsonObject &payload, QDate from, QDate through, Reply done) {
    const bool mutation = payload.value("action") == "configure" || payload.value("action") == "purge";
    if (!mutation && !AnalyticsData::validRange(from,through)) { done({},tr("Choose an inclusive UTC range of at most 90 days.")); return; }
    // Probe the selected executable on every explicit read/mutation. Older hgs
    // treats an unknown verb as a session launch, so never discover by trying it.
    auto probe = command(m_executable,host,{"--help"}); probe.maximumOutputBytes = 128 * 1024;
    ProcessRunner::run(std::move(probe),this,[this,host,verb,payload,from,through,done,mutation](const ProcessRunner::Result &result) {
        if (!success(result)) { done({},tr("Computer unavailable. No current analytics read.")); return; }
        const auto marker = verb == "analytics" ? QByteArray("hgs [@host] analytics --json") : QByteArray("speech --json");
        if (!result.standardOutput.contains(marker)) { done({},tr("This computer does not support the requested analytics read.")); return; }
        auto operation = command(m_executable,host,{verb,"--json"}); operation.input = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        ProcessRunner::run(std::move(operation),this,[this,verb,from,through,done,mutation](const ProcessRunner::Result &result) {
            if (!success(result)) {
                done({},mutation ? tr("Change was not confirmed. Refresh and review before another action.") : tr("Analytics read failed or exceeded its response limit.")); return;
            }
            const auto bytes = result.standardOutput; const QPointer<AnalyticsClient> guard(this);
            QThreadPool::globalInstance()->start(QRunnable::create([guard,bytes,verb,from,through,done,mutation] {
                std::optional<QJsonObject> safe;
                if (mutation) {
                    // Mutations return metadata with the default range. Only a
                    // fresh explicit summary repopulates the controls/view.
                    QJsonParseError error; const auto json = QJsonDocument::fromJson(bytes,&error);
                    if (error.error == QJsonParseError::NoError && json.isObject() && json.object().value("schema") == 1
                        && json.object().value("source") == "agent" && !json.object().contains("error")) safe = QJsonObject{{"schema",1}};
                } else safe = verb == "speech" ? AnalyticsData::speech(bytes,from,through) : AnalyticsData::summary(bytes,from,through);
                if (guard) QMetaObject::invokeMethod(guard,[guard,safe,done,mutation] {
                    if (guard) done(safe.value_or(QJsonObject{}),safe ? QString{} : (mutation
                        ? tr("Change was not confirmed. Refresh and review before another action.")
                        : tr("Computer returned invalid analytics data.")));
                },Qt::QueuedConnection);
            }));
        });
    });
}
