#pragma once

#include "HgsClient.h"
#include "QuestionReply.h"
#include <QObject>
#include <QDateTime>

namespace SessionPresentation {
inline QString childState(const QJsonObject &child) {
    const auto display = child.value("display_state").toString();
    return display.isEmpty() ? child.value("state").toString() : display;
}
inline bool inactiveChild(const QJsonObject &child, bool countsComplete = false) {
    const auto state = childState(child);
    const bool inactive = state == "finished" || state == "idle" || state == "ready";
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

inline QString projectContext(const SessionInfo &s)
{
    QStringList parts{s.project.isEmpty() ? s.name.section('/', 1, 1) : s.project};
    const QString branch = branchContext(s);
    if (!branch.isEmpty()) parts << branch;
    if (s.gitWorktree) parts << (s.gitWorktreeName.isEmpty() ? QObject::tr("worktree") : QObject::tr("worktree: %1").arg(s.gitWorktreeName));
    return parts.join(QStringLiteral(" / "));
}

inline bool currentActivity(const SessionInfo &s, bool online = true)
{
    return online && s.state == "running" && s.processState != "exited"
        && s.activity != "unknown" && !(s.conversationState == "ended" && s.activity != "idle");
}

inline bool working(const SessionInfo &s, bool online = true)
{
    return currentActivity(s, online) && s.activity == "busy" && !s.needsAction();
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
    return false;
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
    if (s.unreadReply && s.activity != "busy" && !s.needsAction()) return status(s, online);
    if (!currentActivity(s, online)) return status(s, online);

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
        if (s.subagentSource == "hooks" && !s.subagentCountsComplete && s.subagentTotalCount > 0)
            return expanded ? QObject::tr("%1 confirmed active; some states unknown").arg(active) : QString("%1+").arg(active);
        if (active > 0) return expanded ? QObject::tr("%1 active").arg(active) : QString::number(active);
        return {};
    }
    if (s.subagentSource == "hooks" && !s.subagentCountsComplete && s.subagentTotalCount > 0)
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
