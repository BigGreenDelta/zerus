#include "AutomaticApprovals.h"
#include <QDateTime>
#include <QSettings>

namespace AutomaticApprovalPolicy {
bool enabled() { return QSettings().value("automaticApprovals/enabled", false).toBool(); }
void setEnabled(bool enabled) { QSettings().setValue("automaticApprovals/enabled", enabled); }
QString error() { return QSettings().value("automaticApprovals/error").toString(); }
}
namespace {
QString key(const QString &host, const QString &name) { return host + '\n' + name; }
bool permissionCandidate(const SessionInfo &session) {
    if (!session.tracked || session.state != "running" || session.processState == "exited"
        || session.runtimeState == "offline" || (session.cmd != "claude" && session.cmd != "opencode" && session.cmd != "dsh")) return false;
    if (session.phase == "approval") return true;
    // Native OpenCode and DeepSeek use input for both questions and tool
    // permissions. Inspection and the journal distinguish their exact kind.
    if ((session.cmd == "dsh" || session.cmd == "opencode") && session.phase == "input") return true;
    for (const auto &value : session.subagents) {
        const auto child = value.toObject();
        if (child.value("phase") == "approval" || child.value("state") == "approval"
            || child.value("display_state") == "approval" || child.value("status") == "Needs approval") return true;
    }
    return false;
}
}

AutomaticApprovals::AutomaticApprovals(const QString &executable, QObject *parent)
    : QObject(parent), m_client(executable, this), m_timer(this)
{
    m_timer.setInterval(250); connect(&m_timer, &QTimer::timeout, this, &AutomaticApprovals::tick); m_timer.start();
    connect(&m_client, &HgsClient::inspectionFailed, this,
        [this](const QString &host, const QString &name, const QString &, const QString &) {
            const auto target = m_inspections.take(key(host, name)); if (!target.key.isEmpty()) release(target);
        });
    connect(&m_client, &HgsClient::inspectionReady, this,
        [this](const QString &host, const QString &name, const QJsonObject &details, const QString &) {
            auto target = m_inspections.take(key(host, name)); if (target.key.isEmpty()) return;
            if (!current(target) || details.value("runtime_state") != "live" || details.value("process_state") != "running"
                || details.value("run_id") != target.run || details.value("conversation_id") != target.conversation) { release(target); return; }
            for (const auto &value : details.value("pending_questions").toArray()) {
                const auto question = value.toObject();
                if (!question.value("approval").toBool() || !question.value("can_answer").toBool() || question.value("trust_request").toBool()
                    || question.value("run_id") != target.run || question.value("conversation_id") != target.conversation) continue;
                target.question = question;
                const auto hostLabel = target.host.isEmpty() ? (m_fleet.local().host.isEmpty() ? QStringLiteral("local") : m_fleet.local().host) : target.host;
                const auto id = m_client.requestApprovalLog("begin", {{"host", hostLabel}, {"session", target.name}, {"provider", target.provider}, {"question", question}});
                m_reservations.insert(id, target); return;
            }
            release(target);
        });
    connect(&m_client, &HgsClient::approvalLogFinished, this,
        [this](quint64 id, const QString &, const QJsonObject &result, const QString &error) {
            if (m_finishes.contains(id)) {
                const auto target = m_finishes.take(id);
                if (!error.isEmpty()) failJournal(error);
                else if (QDateTime::currentMSecsSinceEpoch() >= m_pausedUntil) QSettings().remove("automaticApprovals/error");
                release(target); emit changed(target.host); return;
            }
            if (!m_reservations.contains(id)) return;
            auto target = m_reservations.take(id);
            if (!error.isEmpty()) { failJournal(error); release(target); return; }
            if (!result.value("eligible").toBool() || !result.value("fresh").toBool()) { release(target); return; }
            target.reservation = result;
            if (!current(target)) { finish(target, "cancelled", tr("Automatic approvals were disabled or the session changed before delivery.")); return; }
            if (result.value("id").toInteger() <= 0 || result.value("request_id").toString().isEmpty() || result.value("answers").toArray().isEmpty()) {
                failJournal(tr("The approval journal did not confirm a durable request. No answer was sent.")); release(target); return;
            }
            const auto answer = m_client.requestAnswerQuestion(target.host, target.name, target.question, result.value("answers").toArray(), result.value("request_id").toString());
            m_answers.insert(answer, target);
        });
    connect(&m_client, &HgsClient::questionAnswered, this,
        [this](quint64 id, const QString &, const QString &, const QJsonObject &) {
            if (m_answers.contains(id)) finish(m_answers.take(id), "approved");
        });
    connect(&m_client, &HgsClient::questionAnswerFailed, this,
        [this](quint64 id, const QString &, const QString &, const QString &detail, bool uncertain) {
            if (m_answers.contains(id)) finish(m_answers.take(id), uncertain ? "unknown" : "failed", detail.left(2000));
        });
}

bool AutomaticApprovals::current(const Target &target) const
{
    if (!AutomaticApprovalPolicy::enabled()) return false;
    const auto *box = target.host.isEmpty() ? &m_fleet.local() : m_fleet.peer(target.host);
    if (!box || !box->ok || (!target.host.isEmpty() && QDateTime::currentMSecsSinceEpoch() - m_fleet.peerPolledAt(target.host) > 30000)) return false;
    for (const auto &session : box->sessions) if (session.name == target.name)
        return session.state == "running" && session.runId == target.run && session.conversationId == target.conversation && session.created == target.created;
    return false;
}

void AutomaticApprovals::release(const Target &target)
{
    m_active.remove(target.key); m_recent.insert(target.key, new qint64(QDateTime::currentMSecsSinceEpoch() + 10000));
}
void AutomaticApprovals::failJournal(const QString &error)
{
    m_pausedUntil = QDateTime::currentMSecsSinceEpoch() + 60000;
    QSettings().setValue("automaticApprovals/error", error);
}
void AutomaticApprovals::finish(const Target &target, const QString &status, const QString &detail)
{
    const auto id = m_client.requestApprovalLog("finish", {{"id", target.reservation.value("id")}, {"request_id", target.reservation.value("request_id")}, {"status", status}, {"detail", detail}});
    m_finishes.insert(id, target);
}

void AutomaticApprovals::tick()
{
    if (!AutomaticApprovalPolicy::enabled() || m_active.size() >= 2 || QDateTime::currentMSecsSinceEpoch() < m_pausedUntil) return;
    auto hosts = m_fleet.peerNames(); hosts.prepend(QString());
    const auto now = QDateTime::currentMSecsSinceEpoch();
    // Fixed scan budget, independent of total sessions. Retain the cursor so
    // busy or unsupported sessions at the front cannot starve later machines.
    for (int scanned = 0; scanned < 512 && m_active.size() < 2; ++scanned) {
        if (m_box >= hosts.size()) m_box = 0;
        const auto host = hosts[m_box]; const auto *box = host.isEmpty() ? &m_fleet.local() : m_fleet.peer(host);
        if (!box || !box->ok || (!host.isEmpty() && now - m_fleet.peerPolledAt(host) > 30000) || m_session >= box->sessions.size()) {
            m_session = 0; m_box = (m_box + 1) % hosts.size(); continue;
        }
        const auto &session = box->sessions[m_session++];
        if (!permissionCandidate(session) || session.runId.isEmpty() || session.conversationId.isEmpty()) continue;
        const auto identity = key(host, session.name); const auto *until = m_recent.object(identity);
        if (m_active.contains(identity) || (until && *until > now)) continue;
        Target target{host, session.name, session.cmd, identity, session.runId, session.conversationId, session.created, {}, {}};
        if (!m_client.requestInspection(host, session.name, 0, {}, false)) continue;
        m_active.insert(identity); m_inspections.insert(identity, target);
    }
}
