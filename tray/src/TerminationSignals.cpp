#include "TerminationSignals.h"

#include <atomic>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace {
// The write end of the current instance's pipe, read by the async handler.
std::atomic<int> g_write{-1};
static_assert(std::atomic<int>::is_always_lock_free);

void forward(int signal)
{
    const int saved = errno;
    const unsigned char value = static_cast<unsigned char>(signal);
    // A full pipe already holds a pending request; nothing else is safe here.
    [[maybe_unused]] const auto written = ::write(g_write.load(), &value, 1);
    errno = saved;
}

bool prepare(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0
        && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
} // namespace

TerminationSignals::TerminationSignals(std::function<void(int)> onSignal)
    : m_onSignal(std::move(onSignal))
{
    int fds[2];
    if (::pipe(fds) != 0)
        return;
    if (!prepare(fds[0]) || !prepare(fds[1])) {
        ::close(fds[0]);
        ::close(fds[1]);
        return;
    }
    m_read = fds[0];
    m_write = fds[1];
    g_write.store(m_write);
    m_notifier = std::make_unique<QSocketNotifier>(m_read, QSocketNotifier::Read);
    QObject::connect(m_notifier.get(), &QSocketNotifier::activated, m_notifier.get(), [this] {
        unsigned char value = 0;
        while (::read(m_read, &value, 1) == 1)
            m_onSignal(value);
    });
    struct sigaction action = {};
    action.sa_handler = forward;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_RESETHAND;
    for (std::size_t i = 0; i < std::size(kSignals); ++i) {
        struct sigaction current = {};
        if (::sigaction(kSignals[i], nullptr, &current) != 0 || current.sa_handler == SIG_IGN)
            continue;
        m_installed[i] = ::sigaction(kSignals[i], &action, &m_previous[i]) == 0;
    }
}

TerminationSignals::~TerminationSignals()
{
    for (std::size_t i = 0; i < std::size(kSignals); ++i)
        if (m_installed[i])
            ::sigaction(kSignals[i], &m_previous[i], nullptr);
    m_notifier.reset();
    if (m_write >= 0) {
        int expected = m_write;
        g_write.compare_exchange_strong(expected, -1);
        ::close(m_write);
        ::close(m_read);
    }
}

bool TerminationSignals::handles(int signal) const
{
    for (std::size_t i = 0; i < std::size(kSignals); ++i) {
        if (kSignals[i] != signal || !m_installed[i])
            continue;
        struct sigaction current = {};
        return ::sigaction(signal, nullptr, &current) == 0 && current.sa_handler == forward;
    }
    return false;
}
