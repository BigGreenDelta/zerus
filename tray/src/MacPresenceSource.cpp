#include "DesktopPresence.h"

#include <CoreGraphics/CoreGraphics.h>
#include <cmath>

namespace {
bool flag(CFDictionaryRef dictionary, CFStringRef key, bool fallback)
{
    const void *value = CFDictionaryGetValue(dictionary, key);
    if (!value || CFGetTypeID(value) != CFBooleanGetTypeID()) return fallback;
    return CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

// Neither call needs Accessibility or Input Monitoring permission.
class MacPresenceSource : public PresenceSource {
public:
    void sample() override
    {
        PresenceSample result;
        const double idle = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
        if (std::isfinite(idle) && idle >= 0) { result.available = true; result.idleSeconds = idle; }
        if (CFDictionaryRef session = CGSessionCopyCurrentDictionary()) {
            // A locked screen, or a session switched away by fast user switching.
            result.locked = flag(session, CFSTR("CGSSessionScreenIsLocked"), false)
                || !flag(session, kCGSessionOnConsoleKey, true);
            CFRelease(session);
        }
        emit sampled(result);
    }
};
}

std::unique_ptr<PresenceSource> makePresenceSource()
{
    return std::make_unique<MacPresenceSource>();
}
