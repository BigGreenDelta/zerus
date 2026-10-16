#include "SpeechClient.h"
#include "ProcessRunner.h"
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>
#include <QRunnable>

namespace {
ProcessRunner::Request command(const QString &executable, const QString &host, QStringList args)
{
    ProcessRunner::Request request;
    request.program = executable.isEmpty() ? QStringLiteral("hgs") : executable;
    if (!host.isEmpty()) args.prepend('@' + host);
    request.arguments = args; request.timeoutMs = 30000; request.input = QByteArray();
    return request;
}
bool management(const QJsonObject &payload)
{
    const auto action = payload.value("action").toString();
    return action.startsWith("config_") || action.startsWith("profile_");
}
QString failure(const ProcessRunner::Result &result, bool redact = false)
{
    if (result.outcome == ProcessRunner::Result::TimedOut) return QObject::tr("Speech computer did not respond in time.");
    if (result.outcome == ProcessRunner::Result::FailedToStart) return QObject::tr("Could not start hgs: %1").arg(result.errorString);
    if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0)
        return redact ? QObject::tr("The provider request could not be completed. Check its status before trying again.")
                      : QObject::tr("Speech request failed: %1").arg(QString::fromUtf8(result.standardError).trimmed().left(1000));
    return {};
}
}

SpeechClient::SpeechClient(QString executable, QObject *parent) : QObject(parent), m_executable(std::move(executable)) {}

SpeechClient::Reply SpeechClient::reportCompletion(Reply done)
{
    return [guard = QPointer<SpeechClient>(this), done = std::move(done)](const QJsonObject &reply, const QString &error) {
        done(reply, error);
        if (guard) emit guard->requestFinished();
    };
}

void SpeechClient::request(const QString &host, const QJsonObject &payload, Reply done)
{
    done = reportCompletion(std::move(done));
    const bool manage = management(payload);
    if (manage ? m_managementSupported.contains(host) : m_supported.contains(host)) { dispatch(host, payload, std::move(done)); return; }
    ProcessRunner::run(command(m_executable, host, {"--help"}), this,
        [this, host, payload, done, manage](const ProcessRunner::Result &result) {
            const auto error = failure(result, manage);
            if (!error.isEmpty()) { done(manage ? QJsonObject{{"client_dispatch_started", false}} : QJsonObject{}, error); return; }
            if (!QRegularExpression(QStringLiteral("\\bspeech\\s+--json\\b")).match(QString::fromUtf8(result.standardOutput)).hasMatch()) {
                done(manage ? QJsonObject{{"client_dispatch_started", false}} : QJsonObject{}, tr("Voice input needs an updated hgs CLI on the selected speech computer.")); return;
            }
            if (manage && !result.standardOutput.contains("Speech profile management v1")) {
                done({{"client_dispatch_started", false}}, tr("Update hgs on this computer to manage speech providers.")); return;
            }
            if (manage) m_managementSupported.insert(host);
            m_supported.insert(host); dispatch(host, payload, done);
        });
}

void SpeechClient::requestVoice(const QString &host, const QString &session, const QJsonObject &payload, Reply done)
{
    done = reportCompletion(std::move(done));
    if (session.isEmpty()) { done({}, tr("Choose a conversation before sending voice.")); return; }
    if (m_voiceSupported.contains(host)) { dispatch(host, payload, std::move(done), session); return; }
    ProcessRunner::run(command(m_executable, host, {"--help"}), this,
        [this, host, session, payload, done](const ProcessRunner::Result &result) {
            const auto error = failure(result);
            if (!error.isEmpty()) { done({}, error); return; }
            if (!QRegularExpression(QStringLiteral("\\bvoice\\s+[^\\n]*--json\\b")).match(QString::fromUtf8(result.standardOutput)).hasMatch()) {
                done({}, tr("Voice messages need an updated hgs CLI on the conversation computer.")); return;
            }
            m_voiceSupported.insert(host); dispatch(host, payload, done, session);
        });
}

void SpeechClient::dispatch(const QString &host, const QJsonObject &payload, Reply done, const QString &session)
{
    auto request = command(m_executable, host, session.isEmpty() ? QStringList{"speech", "--json"} : QStringList{"voice", session, "--json"});
    auto start = [this, request, done, redact=management(payload)](QByteArray input) mutable {
        auto command=request;command.input=std::move(input);
        ProcessRunner::run(std::move(command), this, [done, redact](const ProcessRunner::Result &result) {
            const auto error = failure(result, redact);
            if (!error.isEmpty()) { done({}, error); return; }
            QJsonParseError parse;
            const auto json = QJsonDocument::fromJson(result.standardOutput, &parse);
            if (result.standardOutput.size() > 1024 * 1024 || parse.error != QJsonParseError::NoError || !json.isObject()
                || json.object().value("schema").toInt() != 1) { done({}, tr("The speech computer returned an invalid response.")); return; }
            auto reply=json.object();reply.remove("client_dispatch_started");done(reply, {});
        });
    };
    if(!payload.contains("video")){start(QJsonDocument(payload).toJson(QJsonDocument::Compact));return;}
    const QPointer<SpeechClient> guard(this);
    QThreadPool::globalInstance()->start(QRunnable::create([guard,payload,start]() mutable {
        const auto input=QJsonDocument(payload).toJson(QJsonDocument::Compact);
        if(guard)QMetaObject::invokeMethod(guard,[guard,input,start]() mutable {if(guard)start(input);},Qt::QueuedConnection);
    }));
}
