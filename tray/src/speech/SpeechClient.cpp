#include "SpeechClient.h"
#include "ProcessRunner.h"
#include <QJsonDocument>
#include <QRegularExpression>

namespace {
ProcessRunner::Request command(const QString &executable, const QString &host, QStringList args)
{
    ProcessRunner::Request request;
    request.program = executable.isEmpty() ? QStringLiteral("hgs") : executable;
    if (!host.isEmpty()) args.prepend('@' + host);
    request.arguments = args; request.timeoutMs = 30000; request.input = QByteArray();
    return request;
}
QString failure(const ProcessRunner::Result &result)
{
    if (result.outcome == ProcessRunner::Result::TimedOut) return QObject::tr("Speech computer did not respond in time.");
    if (result.outcome == ProcessRunner::Result::FailedToStart) return QObject::tr("Could not start hgs: %1").arg(result.errorString);
    if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0)
        return QObject::tr("Speech request failed: %1").arg(QString::fromUtf8(result.standardError).trimmed().left(1000));
    return {};
}
}

SpeechClient::SpeechClient(QString executable, QObject *parent) : QObject(parent), m_executable(std::move(executable)) {}

void SpeechClient::request(const QString &host, const QJsonObject &payload, Reply done)
{
    if (m_supported.contains(host)) { dispatch(host, payload, std::move(done)); return; }
    ProcessRunner::run(command(m_executable, host, {"--help"}), this,
        [this, host, payload, done](const ProcessRunner::Result &result) {
            const auto error = failure(result);
            if (!error.isEmpty()) { done({}, error); return; }
            if (!QRegularExpression(QStringLiteral("\\bspeech\\s+--json\\b")).match(QString::fromUtf8(result.standardOutput)).hasMatch()) {
                done({}, tr("Voice input needs an updated hgs CLI on the selected speech computer.")); return;
            }
            m_supported.insert(host); dispatch(host, payload, done);
        });
}

void SpeechClient::dispatch(const QString &host, const QJsonObject &payload, Reply done)
{
    auto request = command(m_executable, host, {"speech", "--json"});
    request.input = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    ProcessRunner::run(std::move(request), this, [done](const ProcessRunner::Result &result) {
        const auto error = failure(result);
        if (!error.isEmpty()) { done({}, error); return; }
        QJsonParseError parse;
        const auto json = QJsonDocument::fromJson(result.standardOutput, &parse);
        if (result.standardOutput.size() > 1024 * 1024 || parse.error != QJsonParseError::NoError || !json.isObject()
            || json.object().value("schema").toInt() != 1) { done({}, tr("The speech computer returned an invalid response.")); return; }
        done(json.object(), {});
    });
}
