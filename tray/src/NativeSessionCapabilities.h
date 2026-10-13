#pragma once
#include "HgsClient.h"

// Native API sessions do not acquire terminal or lifecycle capabilities merely
// by sharing the public session schema with a terminal-backed provider.
namespace NativeSessionCapabilities {
inline bool action(const SessionInfo &session, const QString &operation, const QJsonObject &details = {})
{
    if (details.value("allowed_actions").isArray()) return details.value("allowed_actions").toArray().contains(operation);
    if (session.actionsReported) return session.allowedActions.contains(operation);
    // Older terminal adapters keep their existing client behavior. OpenCode's
    // initial native API contract only permits these four lifecycle operations.
    return session.cmd != "opencode" || QStringList{"pause", "resume", "rename", "forget"}.contains(operation);
}
inline QString actionReason(const SessionInfo &session, const QString &operation, const QJsonObject &details = {})
{
    const auto reason=details.value("action_reasons").toObject().value(operation).toString(session.actionReasons.value(operation).toString());
    return reason.isEmpty() ? QObject::tr("This native adapter does not support this action in the current session state.") : reason;
}
inline bool terminal(const SessionInfo &session, const QJsonObject &details = {})
{
    if (session.cmd == "dsh") return false;
    if (details.value("terminal_supported").isBool()) return details.value("terminal_supported").toBool();
    if (session.cmd == "opencode" && !session.terminalReported) return false;
    return session.terminalSupported;
}
inline QString terminalReason(const SessionInfo &session, const QJsonObject &details = {})
{
    const auto reason=details.value("terminal_reason").toString(session.terminalReason);
    return reason.isEmpty() ? QObject::tr("This native adapter does not provide Terminal.") : reason;
}
}
