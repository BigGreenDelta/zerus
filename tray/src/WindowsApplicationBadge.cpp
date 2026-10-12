#include "ApplicationBadge.h"

#include <QGuiApplication>

namespace {
// Windows: Qt draws the count as the taskbar overlay of Zerus' windows. The tray
// icon shows the same count while no window is open.
class WindowsApplicationBadge final : public ApplicationBadge {
public:
    void setCount(int count) override
    {
        count = qMax(0, count);
        if (count == m_count)
            return;
        m_count = count;
        qGuiApp->setBadgeNumber(count);
    }

private:
    int m_count = -1;
};
} // namespace

std::unique_ptr<ApplicationBadge> makeApplicationBadge()
{
    return std::make_unique<WindowsApplicationBadge>();
}
