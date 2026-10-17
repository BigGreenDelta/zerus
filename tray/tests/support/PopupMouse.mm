#include "PopupMouse.h"
#import <AppKit/AppKit.h>

bool PopupMouse::postCocoaClick(QWidget *widget, Qt::MouseButton button, QPoint position)
{
    // Post only to this fixture's window and process; no global input or
    // Accessibility permission is needed, and other applications are untouched.
    NSView *view = reinterpret_cast<NSView *>(widget->winId());
    NSWindow *window = view.window;
    if (!window || (button != Qt::LeftButton && button != Qt::RightButton)) return false;
    const NSPoint point = [view convertPoint:NSMakePoint(position.x(), position.y()) toView:nil];
    const auto down = button == Qt::LeftButton ? NSEventTypeLeftMouseDown : NSEventTypeRightMouseDown;
    const auto up = button == Qt::LeftButton ? NSEventTypeLeftMouseUp : NSEventTypeRightMouseUp;
    for (auto type : {down, up}) {
        NSEvent *event = [NSEvent mouseEventWithType:type location:point modifierFlags:0
            timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber
            context:nil eventNumber:0 clickCount:1 pressure:type == down ? 1 : 0];
        [NSApp postEvent:event atStart:NO];
    }
    return true;
}
