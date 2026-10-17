import Foundation
import ZerusKit

/// The swarm-wide "send notifications to my phone" timing, stored by the gateway computer's hgs.
/// A change is a durable request with the usual receipt states; an unconfirmed one is never repeated.
extension ConversationModel {
    func mobileDeliveryGateway(_ connectionId: String) -> Machine? {
        MobileDelivery.gateway(app.machines.filter { $0.connectionId == connectionId }) {
            app.machineOperations[$0.key]?.contains(MobileDelivery.operation) == true
        }
    }

    func mobileDeliveryAction(_ connectionId: String) -> SessionAction? {
        sessionActions.last { $0.operation == MobileDelivery.operation && $0.target.connectionId == connectionId && $0.status != "reviewed" }
    }

    func mobileDeliveryReason(_ connectionId: String) -> String {
        if demo || !app.storageReady { return "Connect a workspace to change this setting." }
        let gateway = mobileDeliveryGateway(connectionId)
        return MobileDelivery.reason(gateway, gatewaySupports: gateway.map { app.machineOperations[$0.key]?.contains(MobileDelivery.operation) == true } ?? false,
                                     relaySupports: app.relayOperations[connectionId]?.contains(MobileDelivery.operation) == true,
                                     pending: sessionActions.contains { $0.operation == MobileDelivery.operation && $0.target.connectionId == connectionId && $0.blocksSending })
    }

    var mobileDeliveryError: String { MobileDelivery.error(mobileDeliveryMessage, requestId: mobileDeliveryRequest, actions: sessionActions) }

    func showMobileDeliveryError(_ message: String, requestId: String = "") {
        mobileDeliveryMessage = message
        mobileDeliveryRequest = requestId
    }

    func setMobileDelivery(_ connectionId: String, _ value: String) {
        let reason = mobileDeliveryReason(connectionId)
        guard reason.isEmpty else {
            showMobileDeliveryError(reason)
            return
        }
        guard let gateway = mobileDeliveryGateway(connectionId), let connection = app.connections.first(where: { $0.id == connectionId }) else { return }
        if MobileDelivery.shown(reported: gateway.mobileDelivery, action: mobileDeliveryAction(connectionId), now: Clock.milliseconds()) == value {
            showMobileDeliveryError("")
            return
        }
        let arguments: JSONObject
        do { arguments = try MobileDelivery.arguments(value) } catch {
            showMobileDeliveryError(AppModel.message(error, fallback: "Choose a supported notification timing."))
            return
        }
        let target = Target(computerId: gateway.id, session: "", run: "", conversation: "", connectionId: connectionId)
        let action = SessionAction(requestId: UUID().uuidString.lowercased(), target: target, operation: MobileDelivery.operation, arguments: arguments)
        showMobileDeliveryError("")
        actionFlights.insert(action.requestId)
        Task {
            var attempted = false, rejected = false
            defer { actionFlights.remove(action.requestId) }
            do {
                _ = try await app.durableMessages({ state in
                    guard !state.sessionActions.contains(where: { $0.operation == MobileDelivery.operation && $0.target.connectionId == connectionId && $0.blocksSending }) else {
                        throw ZerusError.invalid("Check the previous change before choosing again.")
                    }
                    return state.action(action)
                }, acknowledge: { current, written in
                    written.sessionActions.first { $0.requestId == action.requestId }.map(current.action) ?? current
                })
                attempted = true
                let initial: JSONObject
                do { initial = try await app.client.post(connection, computerId: gateway.id, operation: MobileDelivery.operation, payload: arguments, id: action.requestId) }
                catch let failure as RelayError {
                    rejected = Self.rejectedStatuses.contains(failure.status)
                    throw failure
                }
                await settleSessionAction(action, try await app.client.waitForReceipt(connection, initial: initial))
                let settled = messageState.sessionActions.first { $0.requestId == action.requestId }
                if settled?.status == "completed" { app.refresh() } else { showMobileDeliveryError(settled?.error ?? "", requestId: action.requestId) }
            } catch {
                let status = !attempted || rejected ? "failed" : "uncertain"
                let message = status == "failed" ? AppModel.message(error, fallback: "Not changed. Try again.") : MobileDelivery.unconfirmed
                do {
                    _ = try await app.durableMessages { state in
                        guard var current = state.sessionActions.first(where: { $0.requestId == action.requestId }), ["sending", "uncertain"].contains(current.status) else {
                            return state
                        }
                        current.status = status
                        current.error = message
                        return state.action(current)
                    }
                } catch {
                    showMobileDeliveryError("Could not save the change result. Its original request remains recoverable.", requestId: action.requestId)
                    return
                }
                showMobileDeliveryError(message, requestId: action.requestId)
            }
        }
    }
}
