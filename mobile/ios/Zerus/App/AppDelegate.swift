import BackgroundTasks
import UIKit
import UserNotifications
import ZerusKit

/// Owns the app model so system callbacks (push tokens, wakes, notification taps and background
/// refresh) reach it even when no window has been created.
@MainActor
final class AppDelegate: NSObject, UIApplicationDelegate, UNUserNotificationCenterDelegate {
    let model = AppModel.live()

    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        UNUserNotificationCenter.current().delegate = self
        BGTaskScheduler.shared.register(forTaskWithIdentifier: SessionNotifications.refreshTask, using: .main) { [weak self] task in
            guard let task = task as? BGAppRefreshTask else { return }
            MainActor.assumeIsolated { self?.refresh(task) }
        }
        return true
    }

    func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
        model.notifications.registered(deviceToken: deviceToken)
    }

    func application(_ application: UIApplication, didFailToRegisterForRemoteNotificationsWithError error: Error) {
        model.notifications.registrationFailed()
    }

    /// A wake push: read authenticated state once and update the cards it implies.
    func application(_ application: UIApplication, didReceiveRemoteNotification userInfo: [AnyHashable: Any]) async -> UIBackgroundFetchResult {
        await model.start()
        await model.notifications.syncAll()
        return .newData
    }

    private func refresh(_ task: BGAppRefreshTask) {
        Self.scheduleRefresh()
        let work = Task {
            await model.start()
            await model.notifications.syncAll()
            task.setTaskCompleted(success: !Task.isCancelled)
        }
        task.expirationHandler = { work.cancel() }
    }

    /// The opportunistic fallback when no push arrives; iOS decides when it runs.
    static func scheduleRefresh() {
        let request = BGAppRefreshTaskRequest(identifier: SessionNotifications.refreshTask)
        request.earliestBeginDate = Date(timeIntervalSinceNow: 15 * 60)
        try? BGTaskScheduler.shared.submit(request)
    }

    // MARK: UNUserNotificationCenterDelegate

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification) async -> UNNotificationPresentationOptions {
        let identifier = notification.request.identifier
        let info = notification.request.content.userInfo
        let connection = info["zerus_connection"] as? String, computer = info["zerus_computer"] as? String, session = info["zerus_session"] as? String
        return await MainActor.run {
            if identifier == SessionNotifications.wakeIdentifier {
                // In the foreground the wake only starts a read; cards decide what appears.
                Task { await self.model.notifications.syncAll() }
                return []
            }
            if let selected = model.conversation.selected?.target, selected.connectionId == connection, selected.computerId == computer, selected.session == session {
                return []
            }
            return [.banner, .list, .sound]
        }
    }

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        let info = response.notification.request.content.userInfo
        let connection = info["zerus_connection"] as? String, computer = info["zerus_computer"] as? String, session = info["zerus_session"] as? String
        await MainActor.run {
            if let connection, let computer, let session {
                model.openNotification(connection: connection, computer: computer, session: session)
            } else {
                model.refresh(explicit: true)
            }
        }
    }
}
