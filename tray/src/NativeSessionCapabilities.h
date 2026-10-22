#pragma once
#include "HgsClient.h"

// Native API sessions do not acquire terminal or lifecycle capabilities merely
// by sharing the public session schema with a terminal-backed provider.
namespace NativeSessionCapabilities {
inline bool action(const SessionInfo &session, const QString &operation, const QJsonObject &details = {})
{
    if (details.value("allowed_actions").isArray()) return details.value("allowed_actions").toArray().contains(operation);
    if (session.actionsReported) return session.allowedActions.contains(operation);
    // Older terminal adapters keep their existing client behavior. An OpenCode 2
    // session has no process of its own to pause or resume: without reported
    // actions only its Zerus binding can be renamed or forgotten.
    return session.cmd != "opencode" || QStringList{"rename", "forget"}.contains(operation);
}
// An OpenCode 2 session has no process for Zerus to stop. While it allows
// forget and not terminate, Forget removes only its Zerus binding, even while
// it runs; the conversation stays in OpenCode.
inline bool forgetsBindingOnly(const SessionInfo &session, const QJsonObject &details = {})
{
    return session.cmd == "opencode" && action(session, "forget", details) && !action(session, "terminate", details);
}
// The operation behind a session's Terminate / Forget entry: other running
// sessions terminate first.
inline QString endOperation(const SessionInfo &session, const QJsonObject &details = {})
{
    return session.state == "running" && !forgetsBindingOnly(session, details) ? QStringLiteral("terminate") : QStringLiteral("forget");
}
inline QString actionReason(const SessionInfo &session, const QString &operation, const QJsonObject &details = {})
{
    const auto reason=details.value("action_reasons").toObject().value(operation).toString(session.actionReasons.value(operation).toString());
    return reason.isEmpty() ? QObject::tr("This native adapter does not support this action in the current session state.") : reason;
}
inline bool terminal(const SessionInfo &session, const QJsonObject &details = {})
{
    if (session.cmd == "dsh") return false;
    if (session.cmd == "opencode") {
        if (details.value("terminal_attach_supported").isBool()) return details.value("terminal_attach_supported").toBool();
        if (session.terminalAttachReported) return session.terminalAttachSupported;
    }
    if (details.value("terminal_supported").isBool()) return details.value("terminal_supported").toBool();
    if (session.cmd == "opencode" && !session.terminalReported) return false;
    return session.terminalSupported;
}
inline QString terminalReason(const SessionInfo &session, const QJsonObject &details = {})
{
    if (session.cmd == "opencode" && (details.value("terminal_attach_supported").isBool() || session.terminalAttachReported)) {
        const auto reason = details.value("terminal_attach_reason").toString(session.terminalAttachReason);
        return reason.isEmpty() ? QObject::tr("Refresh this session after its OpenCode service reconnects.") : reason;
    }
    const auto reason=details.value("terminal_reason").toString(session.terminalReason);
    return reason.isEmpty() ? QObject::tr("This native adapter does not provide Terminal.") : reason;
}
}
