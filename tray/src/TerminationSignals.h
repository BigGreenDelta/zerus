#pragma once

#include <QSocketNotifier>

#include <csignal>
#include <functional>
#include <iterator>
#include <memory>

// SIGTERM, SIGINT and SIGHUP end the tray the way its Quit action does. Service
// managers stop it with SIGTERM (systemctl stop/restart, launchctl bootout/unload);
// without a handler the process died before aboutToQuit, and queued draft writes
// (ComposerDraftStore) were lost.
//
// The handler only writes the signal number to a self-pipe; onSignal runs on the
// thread that owns this object, from its event loop. Each handler resets to the
// default action when it fires (SA_RESETHAND): a second signal still ends a tray
// that cannot finish quitting, for example while a swarm change refuses to close.
// Signals ignored at startup stay ignored (nohup keeps working). Destruction
// restores the previous actions. Windows delivers none of these to a GUI program.
class TerminationSignals {
public:
    static constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};

    explicit TerminationSignals(std::function<void(int)> onSignal);
    ~TerminationSignals();

    TerminationSignals(const TerminationSignals &) = delete;
    TerminationSignals &operator=(const TerminationSignals &) = delete;

    // Whether this signal now reaches onSignal.
    bool handles(int signal) const;

private:
    std::function<void(int)> m_onSignal;
    int m_read = -1;
    int m_write = -1;
    std::unique_ptr<QSocketNotifier> m_notifier;
    struct sigaction m_previous[std::size(kSignals)] = {};
    bool m_installed[std::size(kSignals)] = {};
};
