#include "AttentionNotifier.h"
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QCoreApplication>
#include <utility>
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

@interface HgsAttentionDelegate : NSObject <UNUserNotificationCenterDelegate>
@property(nonatomic, copy) void (^clicked)(NSString *);
@end
@implementation HgsAttentionDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
      willPresentNotification:(UNNotification *)notification
        withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completion {
    Q_UNUSED(center); Q_UNUSED(notification);
    completion(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList | UNNotificationPresentationOptionSound);
}
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
 didReceiveNotificationResponse:(UNNotificationResponse *)response
        withCompletionHandler:(void (^)(void))completion {
    Q_UNUSED(center);
    NSString *token = response.notification.request.content.userInfo[@"hgsAttentionToken"];
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier]
        && [token isKindOfClass:[NSString class]] && self.clicked) self.clicked(token);
    completion();
}
@end

class MacAttentionNotifier : public AttentionNotifier {
public:
    explicit MacAttentionNotifier(UNUserNotificationCenter *center = nil) {
        m_center = center ?: [UNUserNotificationCenter currentNotificationCenter];
        m_delegate = [HgsAttentionDelegate new];
        QPointer<MacAttentionNotifier> owner(this);
        m_delegate.clicked = ^(NSString *token) {
            const QString value = QString::fromNSString(token);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [owner, value] {
                if (!owner) return;
                [NSApp activateIgnoringOtherApps:YES];
                emit owner->activated(value, {});
            }, Qt::QueuedConnection);
        };
        m_center.delegate = m_delegate;
    }
    ~MacAttentionNotifier() override {
        m_delegate.clicked = nil;
        if (m_center.delegate == m_delegate) m_center.delegate = nil;
    }
    // Notification Center opens the app on a click; it shows no action button.
    void post(const QString &token, const QString &title, const QString &body, const QString &action = {}) override {
        Q_UNUSED(action);
        if (m_desired.contains(token)) return;
        m_desired.insert(token); m_pending[token] = {title, body};
        if (m_checking) return;
        m_checking = true;
        QPointer<MacAttentionNotifier> owner(this);
        UNUserNotificationCenter *center = m_center;
        [m_center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings *settings) {
            if (settings.authorizationStatus == UNAuthorizationStatusNotDetermined) {
                [center
                    requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                    completionHandler:^(BOOL granted, NSError *error) {
                        const QString detail = error ? QString::fromNSString(error.localizedDescription) : QString();
                        QMetaObject::invokeMethod(QCoreApplication::instance(), [owner, granted, detail] {
                            if (owner) owner->authorized(granted, detail);
                        }, Qt::QueuedConnection);
                    }];
            } else {
                const bool granted = settings.authorizationStatus == UNAuthorizationStatusAuthorized
                    || settings.authorizationStatus == UNAuthorizationStatusProvisional;
                QMetaObject::invokeMethod(QCoreApplication::instance(), [owner, granted] {
                    if (owner) owner->authorized(granted, {});
                }, Qt::QueuedConnection);
            }
        }];
    }
    void withdraw(const QString &token) override {
        m_desired.remove(token); m_pending.remove(token);
        NSArray *identifiers = @[(QStringLiteral("zerus.attention.") + token).toNSString()];
        [m_center removePendingNotificationRequestsWithIdentifiers:identifiers];
        [m_center removeDeliveredNotificationsWithIdentifiers:identifiers];
    }
private:
    void authorized(bool granted, const QString &error) {
        m_checking = false;
        const auto pending = std::exchange(m_pending, {});
        if (!granted) {
            for (const auto &token : pending.keys()) {
                m_desired.remove(token);
                emit failed(token, error.isEmpty() ? tr("Allow Zerus notifications in System Settings → Notifications.") : error);
            }
            return;
        }
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            const QString token = it.key();
            if (!m_desired.contains(token)) continue;
            UNMutableNotificationContent *content = [UNMutableNotificationContent new];
            content.title = it.value().first.toNSString(); content.body = it.value().second.toNSString();
            content.sound = UNNotificationSound.defaultSound;
            content.userInfo = @{@"hgsAttentionToken": token.toNSString()};
            UNNotificationRequest *request = [UNNotificationRequest
                requestWithIdentifier:(QStringLiteral("zerus.attention.") + token).toNSString()
                content:content trigger:nil];
            QPointer<MacAttentionNotifier> owner(this);
            [m_center addNotificationRequest:request withCompletionHandler:^(NSError *error) {
                const QString detail = error ? QString::fromNSString(error.localizedDescription) : QString();
                QMetaObject::invokeMethod(QCoreApplication::instance(), [owner, token, detail] {
                    if (!owner) return;
                    if (!owner->m_desired.contains(token)) owner->withdraw(token);
                    else if (!detail.isEmpty()) { owner->m_desired.remove(token); emit owner->failed(token, detail); }
                }, Qt::QueuedConnection);
            }];
        }
    }
    UNUserNotificationCenter *m_center;
    HgsAttentionDelegate *m_delegate;
    QHash<QString, QPair<QString, QString>> m_pending;
    QSet<QString> m_desired;
    bool m_checking = false;
};

std::unique_ptr<AttentionNotifier> makeAttentionNotifier() { return std::make_unique<MacAttentionNotifier>(); }
