#pragma once

#include "HgsClient.h"
#include "QuestionReply.h"
#include "SessionElapsed.h"
#include <QObject>
#include <QDateTime>
#include <QRegularExpression>

namespace SessionPresentation {
inline QString childState(const QJsonObject &child) {
    const auto display = child.value("display_state").toString();
    return display.isEmpty() ? child.value("state").toString() : display;
}
inline bool inactiveChild(const QJsonObject &child, bool countsComplete = false) {
    const auto state = childState(child);
    // A failed turn has ended too; its error belongs to the parent's result.
    const bool inactive = state == "finished" || state == "idle" || state == "ready" || state == "error";
    // Explicit display evidence wins over an older raw turn state or count.
    if (!state.isEmpty() && !inactive) return false;
    if (child.value("group").toBool())
        return countsComplete && child.value("active_count").isDouble()
            && child.value("active_count").toDouble() == 0;
    return inactive;
}
inline QString childStatus(const QJsonObject &child) {
    const auto state = childState(child);
    if (state == "working") return QObject::tr("Working");
    if (state == "finished" || state == "ready" || state == "idle") return QObject::tr("Ready");
    if (state == "error") return QObject::tr("Error");
    if (state == "paused") return QObject::tr("Paused");
    if (state == "approval" || state == "input" || state == "attention") return QObject::tr("Needs input");
    return QObject::tr("Unknown");
}
inline QString gitStatusPath(const SessionInfo &s) {
    if (s.state == "archived" || s.gitMetadataState == "not_repo") return {};
    return s.gitRoot.isEmpty() ? (s.canonicalCwd.isEmpty() ? s.cwd : s.canonicalCwd) : s.gitRoot;
}
inline QJsonObject gitStatus(const SessionInfo &s, QJsonObject snapshot, bool online) {
    if (gitStatusPath(s).isEmpty()) return {};
    if (snapshot.isEmpty()) snapshot = {{"state", "checking"}, {"root", s.gitRoot}, {"branch", s.gitBranch}};
    if (snapshot.value("state") == "ok" && ((!s.gitRoot.isEmpty() && snapshot.value("root") != s.gitRoot)
        || snapshot.value("branch").toString() != s.gitBranch || snapshot.value("detached").toBool() != s.gitDetached))
        snapshot["state"] = "scope_changed";
    snapshot["offline"] = !online;
    return snapshot;
}
inline SessionInfo inspected(SessionInfo s, const QJsonObject &details)
{
    const auto wait = details.value("coordination_wait").toObject();
    if (details.value("run_id") == s.runId && details.value("conversation_id") == s.conversationId
        && wait.value("status") == "ok" && wait.value("run_id") == s.runId
        && wait.value("conversation_id") == s.conversationId
        && wait.value("revision").toDouble() >= s.coordinationWait.value("revision").toDouble())
        s.coordinationWait = wait;
    const auto work = details.value("process_work").toObject();
    if (s.state == "running" && !s.runId.isEmpty() && details.value("tracked").toBool()
        && details.value("run_id").toString() == s.runId && details.value("conversation_id").toString() == s.conversationId
        && work.value("sampled_at").toDouble() > s.backgroundObservedAt) {
        s.backgroundProcesses = work.value("count").toInt(-1);
        s.backgroundStarted = work.value("started_at").toDouble();
        s.backgroundObservedAt = work.value("sampled_at").toDouble();
        s.backgroundCommand = work.value("command").toString();
    }
    // A selected-session inspection can discover failure before the fleet poll.
    // Never apply another run/conversation or older hook evidence to its row.
    if (s.state != "running" || s.runId.isEmpty() || !details.value("tracked").toBool()
        || details.value("run_id").toString() != s.runId
        || details.value("conversation_id").toString() != s.conversationId
        || details.value("last_event_at").toDouble() < s.lastEventAt
        || details.value("provider_status_at").toDouble() < s.providerStatusAt
        || (details.value("last_event_at").toDouble() <= s.lastEventAt
            && details.value("provider_status_at").toDouble() <= s.providerStatusAt)) return s;
    s.phase=details.value("phase").toString(s.phase);s.activity=details.value("activity").toString(s.activity);
    s.processState=details.value("process_state").toString(s.processState);
    s.providerError=details.value("provider_error").toObject();s.recovery=details.value("recovery").toObject();
    s.providerStatusAt=details.value("provider_status_at").toDouble();
    s.activitySummary=details.value("activity_summary").toString(s.activitySummary);
    s.activityDetail=details.value("activity_detail").toString(s.activityDetail);
    s.currentTool=details.value("current_tool").toString(s.currentTool);s.toolDetail=details.value("tool_detail").toString(s.toolDetail);
    const auto attention=details.value("attention_id").toString(s.attentionId);
    if(attention != s.attentionId)s.attentionAcknowledged=false;
    s.attentionId=attention;
    return s;
}
inline QString providerFailure(const QJsonObject &error)
{
    const auto kind = error.value("error_kind").toString();
    if (kind == "session_limit") return QObject::tr("Session limit reached");
    if (kind == "quota") return QObject::tr("Usage limit reached");
    if (kind == "rate_limit") return QObject::tr("Rate limit reached");
    if (kind == "capacity") return QObject::tr("Model at capacity");
    if (kind == "authentication") return QObject::tr("Sign-in failed");
    if (kind == "provider_policy") return QObject::tr("Request blocked by provider");
    if (kind == "context_limit") return QObject::tr("Context limit reached");
    if (kind == "model_unavailable") return QObject::tr("Model unavailable");
    return QObject::tr("Provider error");
}
// Jobs the agent scheduled for itself will start its next turns: the session
// is neither waiting for the person nor working right now.
inline bool scheduledIdle(const SessionInfo &s)
{
    return s.activity == "idle" && s.phase == "idle" && s.scheduledJobs > 0;
}
inline bool backgroundIdle(const SessionInfo &s)
{
    return s.activity == "idle" && s.phase == "idle" && s.backgroundProcesses > 0;
}
inline bool waitingIdle(const SessionInfo &s)
{
    return s.state == "running" && s.processState != "exited" && s.activity == "idle" && s.phase == "idle"
        && s.backgroundProcesses <= 0 && s.subagentActiveCount <= 0 && s.hasCoordinationWait();
}
inline QString waitingLabel(const SessionInfo &s, bool detail = false)
{
    const auto items = s.coordinationWait.value("items").toArray();
    const auto item = items.isEmpty() ? QJsonObject() : items.first().toObject();
    const auto producer = item.value("producer").toObject();
    const auto label = item.value("state") == "awaiting_result" ? QObject::tr("Waiting for result") : QObject::tr("Waiting for acceptance");
    const auto name = producer.value("name").toString().section('/',2);
    const auto host = producer.value("host").toString();
    QString result = name.isEmpty() ? label : QObject::tr("%1 from %2").arg(label,name);
    if (!host.isEmpty()) result += QObject::tr(" on %1").arg(host);
    if (detail && !item.value("label").toString().isEmpty()) result += ": " + item.value("label").toString();
    const auto count = s.coordinationWait.value("count").toInt();
    if (count > 1) result += QObject::tr(" (+%1)").arg(count-1);
    return SessionElapsed::status(result,item.value("created_at").toDouble());
}
inline QString waitingCaption(const SessionInfo &s)
{
    const auto items=s.coordinationWait.value("items").toArray();
    const auto item=items.isEmpty()?QJsonObject():items.first().toObject();
    return SessionElapsed::status(item.value("state")=="awaiting_result"?QObject::tr("Waiting for result"):QObject::tr("Waiting for acceptance"),item.value("created_at").toDouble());
}
// The next run as a time of day, "due" once it has passed, or empty if unknown.
inline QString nextScheduledRun(const SessionInfo &s, const QDateTime &now = QDateTime::currentDateTime())
{
    if (s.nextScheduledAt <= 0) return {};
    const auto next = QDateTime::fromSecsSinceEpoch(qint64(s.nextScheduledAt)).toLocalTime();
    if (next <= now) return QObject::tr("due");
    return next.date() == now.date() ? next.toString("HH:mm") : next.toString("d MMM HH:mm");
}
inline QString status(const SessionInfo &s, bool reachable = true)
{
    if (!reachable) return QObject::tr("Offline");
    if (s.state == "archived") return QObject::tr("Archived");
    if (s.state == "paused") return QObject::tr("Paused");
    if (s.state == "stopped") return QObject::tr("Stopped");
    if (s.processState == "exited") return QObject::tr("Agent exited");
    if (!s.tracked && s.activity.isEmpty()) return QObject::tr("Not tracked");
    if (s.phase == "approval") return QObject::tr("Needs approval");
    if (s.phase == "input" && s.cmd == "dsh" && s.activitySummary == "Sign in required") return QObject::tr("Sign in");
    if (s.phase == "input") return QObject::tr("Needs input");
    if (s.phase == "error" && (s.providerError.value("error_kind")=="quota"
        || s.providerError.value("error_kind")=="provider_policy")) return providerFailure(s.providerError);
    if (s.recovery.value("state") == "waiting") {
        if (s.recovery.value("class") == "session_limit" && s.recovery.value("session_limit_mode") == "reset")
            return QObject::tr("Reset at %1").arg(QDateTime::fromMSecsSinceEpoch(qint64(s.recovery.value("due_at").toDouble()*1000)).toLocalTime().toString("HH:mm"));
        const auto seconds = qMax(0, int(s.recovery.value("due_at").toDouble()-QDateTime::currentMSecsSinceEpoch()/1000.0));
        return seconds ? QObject::tr("Retry in %1 s").arg(seconds) : QObject::tr("Waiting to retry");
    }
    if (s.recovery.value("state") == "dispatching") return QObject::tr("Retrying");
    if (s.phase == "error") return s.providerError.isEmpty() ? QObject::tr("Error") : providerFailure(s.providerError);
    if (s.phase == "interrupted") return QObject::tr("Interrupted");
    if (s.phase == "compacting") return QObject::tr("Compacting");
    if (s.phase == "starting") return QObject::tr("Starting");
    if (s.phase == "tool") return QObject::tr("Using a tool");
    if (s.activity == "busy") return QObject::tr("Working");
    if (backgroundIdle(s)) return QObject::tr("Background");
    if (waitingIdle(s)) return waitingLabel(s);
    if (scheduledIdle(s)) {
        const auto next = nextScheduledRun(s);
        return next.isEmpty() ? QObject::tr("Scheduled") : QObject::tr("Scheduled · %1").arg(next);
    }
    if (s.activity == "idle") return QObject::tr("Ready");
    // A conversation can end while the CLI and its tmux terminal remain alive.
    if (s.activity == "ended" || s.activity == "unknown") return QObject::tr("Status unknown");
    return QObject::tr("Connecting");
}

inline QString displayTitle(const SessionInfo &s)
{
    const QString project = s.project.isEmpty() ? s.name.section('/', 1, 1) : s.project;
    const QString tag = s.tag.isEmpty() ? s.name.section('/', 2) : s.tag;
    if (project.isEmpty()) return s.name;
    return tag.isEmpty() ? project : project + QStringLiteral(" / ") + tag;
}

inline QString sessionLabel(const SessionInfo &s)
{
    const QString tag = s.tag.isEmpty() ? s.name.section('/', 2) : s.tag;
    return tag.isEmpty() ? (s.project.isEmpty() ? s.name : s.project) : tag;
}

inline QString branchContext(const SessionInfo &s)
{
    if (!s.gitBranch.isEmpty()) return s.gitBranch;
    return s.gitDetached ? QObject::tr("detached HEAD") : QString();
}

// Path components of a local or remote path, whichever separator its machine uses.
inline QStringList pathParts(const QString &path)
{
    static const QRegularExpression separators(QStringLiteral("[/\\\\]+"));
    return path.split(separators, Qt::SkipEmptyParts);
}

// The folder people know a checkout by. A linked worktree names its main
// checkout (`zerus` rather than `zerus-work-1a2b`); a bare `repo.git` names itself.
inline QString checkoutFolder(const SessionInfo &s)
{
    if (s.gitWorktree && !s.gitCommonDir.isEmpty()) {
        auto parts = pathParts(s.gitCommonDir);
        if (parts.size() > 1 && parts.last().startsWith('.')) parts.removeLast();
        QString name = parts.isEmpty() ? QString() : parts.last();
        if (name.size() > 4 && name.endsWith(QStringLiteral(".git"))) name.chop(4);
        if (!name.isEmpty()) return name;
    }
    const auto parts = pathParts(!s.gitRoot.isEmpty() ? s.gitRoot : !s.cwd.isEmpty() ? s.cwd : s.canonicalCwd);
    return parts.isEmpty() ? QString() : parts.last();
}

// The card's checkout line: folder / branch (or detached HEAD); an ordinary folder shows its name.
inline QString checkoutLine(const SessionInfo &s)
{
    const QString folder = checkoutFolder(s), branch = branchContext(s);
    if (folder.isEmpty() || branch.isEmpty()) return folder.isEmpty() ? branch : folder;
    return folder + QStringLiteral(" / ") + branch;
}

// The workspace icon beside checkoutLine.
inline QString checkoutIcon(const SessionInfo &s)
{
    if (!branchContext(s).isEmpty()) return QStringLiteral("git-branch");
    return checkoutFolder(s).isEmpty() ? QString() : QStringLiteral("folder");
}

// The opened header and Details: the card line plus a linked-worktree mark.
// The worktree folder itself stays in the path shown beside it.
inline QString projectContext(const SessionInfo &s)
{
    QString context = checkoutLine(s);
    if (context.isEmpty()) context = s.project.isEmpty() ? s.name.section('/', 1, 1) : s.project;
    if (s.gitWorktree) context += QObject::tr(" · worktree");
    return context;
}

inline bool currentActivity(const SessionInfo &s, bool online = true)
{
    return online && s.state == "running" && s.processState != "exited"
        && s.activity != "unknown" && !(s.conversationState == "ended" && s.activity != "idle");
}

inline bool working(const SessionInfo &s, bool online = true)
{
    return currentActivity(s, online) && (s.activity == "busy" || backgroundIdle(s)) && !s.needsAction();
}

inline bool scheduled(const SessionInfo &s, bool online = true)
{
    return currentActivity(s, online) && !s.needsAction() && !backgroundIdle(s) && !waitingIdle(s) && scheduledIdle(s);
}

inline bool needsAttention(const SessionInfo &s, bool online = true)
{
    if (s.state == "archived") return false;
    if (s.reviewLater || (online && s.needsAttention())) return true;
    if (!currentActivity(s, online) || s.attentionAcknowledged || s.subagentSource == "hook_profiles") return false;
    for (const auto &value : s.subagents) {
        const auto state = childState(value.toObject());
        if (state == "approval" || state == "input" || state == "attention") return true;
    }
    for (const auto &value : s.subagentPreviews) {
        const auto state = childState(value.toObject());
        if (state == "approval" || state == "input" || state == "attention") return true;
    }
    return false;
}

// Pending optional Codex questions shown on the card; 0 hides the chip. Only a
// running agent on a reachable machine with a known count shows one. The agent
// continues without an answer, so these never count as attention or activity.
inline int optionalQuestionCount(const SessionInfo &s, bool online = true)
{
    if (!online || s.state != "running" || s.processState == "exited" || s.optionalQuestions < 0) return 0;
    return s.optionalQuestions;
}
inline QString optionalQuestionsLabel(int count)
{
    return count == 1 ? QObject::tr("1 optional question waiting") : QObject::tr("%1 optional questions waiting").arg(count);
}

inline bool active(const SessionInfo &s, bool online = true, bool hasDraft = false)
{
    return s.state != "archived" && (hasDraft || needsAttention(s, online) || working(s, online) || scheduled(s, online)
        || (online && s.replyInProgress() && s.activity == "idle" && s.phase == "idle"));
}

inline QString promptPreview(const QString &text)
{
    const auto replies = QuestionReply::parse(text);
    return replies.isEmpty() ? text : QuestionReply::preview(replies);
}

inline QString activityDetailPreview(const SessionInfo &s)
{
    const auto replies = QuestionReply::parse(s.activityDetail);
    if (!replies.isEmpty()) return QuestionReply::preview(replies);
    // Telemetry clips the detail at 240 characters, often inside the JSON.
    // Use the complete prompt only when this detail is its actual excerpt;
    // an older answer must never replace the current tool or approval text.
    QString excerpt = s.activityDetail;
    if (excerpt.endsWith(QChar(0x2026))) excerpt.chop(1);
    if (!excerpt.isEmpty() && s.prompt.startsWith(excerpt)) {
        const auto promptReplies = QuestionReply::parse(s.prompt);
        if (!promptReplies.isEmpty()) return QuestionReply::preview(promptReplies);
    }
    return s.activityDetail;
}

inline QString currentAction(const SessionInfo &s, bool online = true)
{
    // A tool excerpt must not hide the fact that the agent is waiting for us.
    if (online && s.needsAction()) {
        const QString detail = !s.activityDetail.isEmpty() ? activityDetailPreview(s) : !s.toolDetail.isEmpty() ? s.toolDetail : promptPreview(s.prompt);
        return status(s) + (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
    }
    if (s.visibleUnreadReply(online) && !s.needsAction()) return status(s, online);
    if (!currentActivity(s, online)) return status(s, online);
    if (backgroundIdle(s)) return (s.backgroundProcesses == 1 ? QObject::tr("1 running command") : QObject::tr("%1 running commands").arg(s.backgroundProcesses))
        + (s.backgroundCommand.isEmpty() ? QString() : QStringLiteral(": ") + s.backgroundCommand);
    if (waitingIdle(s)) return waitingLabel(s,true);
    if (scheduled(s, online)) {
        const auto now = QDateTime::currentDateTime();
        if (s.nextScheduledAt <= 0) return QObject::tr("Waiting for its next scheduled run");
        return s.nextScheduledAt <= now.toSecsSinceEpoch() ? QObject::tr("Next run due")
            : QObject::tr("Next run at %1").arg(nextScheduledRun(s, now));
    }

    if (!s.activitySummary.isEmpty()) return s.activitySummary + (s.activityDetail.isEmpty() ? QString() : QStringLiteral(": ") + activityDetailPreview(s));
    if (!s.currentTool.isEmpty()) return s.currentTool + (s.toolDetail.isEmpty() ? QString() : QStringLiteral(": ") + s.toolDetail);
    if (!s.prompt.isEmpty()) return status(s) + QStringLiteral(": ") + promptPreview(s.prompt);
    return s.tracked ? status(s) : QObject::tr("Open terminal to view activity");
}

inline QString childCount(const SessionInfo &s, bool expanded = false, bool online = true, bool includeCompleted = true)
{
    if (!currentActivity(s, online) || s.subagentSource == "unavailable") return {};
    const int active = qMax(s.subagentActiveCount, s.subagentCount);
    if (s.subagentSource == "hook_profiles" && !s.subagentCountsComplete)
        return expanded ? QObject::tr("Observed runs: incomplete counts") : QStringLiteral("?");
    if (!includeCompleted) {
        if (s.subagentSource != "hook_profiles" && !s.subagentCountsComplete && s.subagentTotalCount > 0)
            return expanded ? QObject::tr("%1 confirmed active; some states unknown").arg(active) : QString("%1+").arg(active);
        if (active > 0) return expanded ? QObject::tr("%1 active").arg(active) : QString::number(active);
        return {};
    }
    if (s.subagentSource != "hook_profiles" && !s.subagentCountsComplete && s.subagentTotalCount > 0)
        return expanded ? QObject::tr("%1 confirmed active / %2 observed; some states unknown").arg(active).arg(s.subagentTotalCount)
                        : QString("%1+/%2").arg(active).arg(s.subagentTotalCount);
    if (s.subagentTotalCount > 0)
        return expanded ? (s.subagentSource == "hook_profiles" ? QObject::tr("%1 active / %2 observed runs") : QObject::tr("%1 active / %2 total")).arg(active).arg(s.subagentTotalCount) : QString("%1/%2").arg(active).arg(s.subagentTotalCount);
    if (s.subagentTotalCount < 0 && s.subagentSource == "hook_profiles")
        return expanded ? QObject::tr("Observed runs: count unavailable") : QStringLiteral("?");
    if (s.subagentTotalCount < 0 && active > 0)
        return expanded ? QObject::tr("%1 active / total unknown").arg(active) : QString("%1/?").arg(active);
    return {};
}

}
