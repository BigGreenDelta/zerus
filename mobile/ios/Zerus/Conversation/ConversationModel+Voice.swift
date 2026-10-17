import Foundation
import ZerusKit

/// Voice messages go to the exact live parent conversation on its own computer.
extension ConversationModel {
    func voiceHostReason(_ target: Target) -> String {
        if demo { return "Connect a computer to send voice messages." }
        if !app.connections.contains(where: { $0.id == target.connectionId }) { return "The voice workspace is disconnected." }
        if !app.machines.contains(where: { $0.connectionId == target.connectionId && $0.id == target.computerId && $0.online && !$0.lastKnown }) {
            return "The conversation computer is offline."
        }
        if !(app.relayOperations[target.connectionId] ?? []).isSuperset(of: VoiceProtocol.operations) { return "Update this gateway to send voice messages." }
        if !(app.machineOperations[target.machineKey] ?? []).isSuperset(of: VoiceProtocol.operations) {
            return "Update Zerus on the conversation computer to send voice messages."
        }
        return ""
    }

    func videoHostReason(_ target: Target) -> String {
        let voice = voiceHostReason(target)
        if !voice.isEmpty { return voice }
        if app.relayFeatures[target.connectionId]?.contains(VideoProtocol.feature) != true { return "Update this gateway to send video messages." }
        if app.machineFeatures[target.machineKey]?.contains(VideoProtocol.feature) != true { return "Update Zerus on this conversation's computer to send video messages." }
        return ""
    }

    func voiceSendReason(_ target: Target) -> String {
        if !app.storageReady { return "Private storage is not ready." }
        if !target.agentId.isEmpty || !target.archiveId.isEmpty { return "Voice messages are available in live parent conversations." }
        let host = voiceHostReason(target)
        if !host.isEmpty { return host }
        if !fresh(target) { return "Open and refresh the original conversation before sending." }
        let block = NativeEvents.messageBlockReason(activity)
        if !block.isEmpty { return block }
        if sendingBlocked(target) { return "Resolve the pending delivery before sending this recording." }
        return ""
    }

    func voiceColdCache() -> Bool { ContextPresentation.isCold(activity) }

    func voiceSent(_ target: Target, _ id: String) { lastOwnSend = (target, id) }
}
