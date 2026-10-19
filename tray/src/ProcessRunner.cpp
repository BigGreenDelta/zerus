#include "ProcessRunner.h"
#include "ChildProcess.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <utility>

#ifdef Q_OS_MACOS
#include <QDeadlineTimer>
#include <QFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <cerrno>
#include <crt_externs.h>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#endif

namespace ProcessRunner {
namespace {

#ifdef Q_OS_MACOS
// QProcess starts children with fork() on macOS (forkfd has no vfork there),
// copying the map of the whole GUI process: ~4 ms of CPU per launch in the
// live app against ~0.3 ms for posix_spawn. This is the QProcess subset the
// runner uses, on the worker thread: non-blocking pipes, exit through kqueue
// EVFILT_PROC and waitpid on its own pid only, so Qt's forkfd keeps its children.
// Like QProcess, the child gets SIGPIPE at its default action and inherits
// other ignored signals; its signal mask starts empty, as the worker's is.
class SpawnedProcess final : public QObject {
public:
    explicit SpawnedProcess(QObject *parent) : QObject(parent) {}
    ~SpawnedProcess() override
    {
        clearCallbacks();
        if (m_pid > 0) { kill(); waitForFinished(1000); }
        closeInput(); closeRead(m_out); closeRead(m_err); stopWatching();
    }
    std::function<void()> started;
    std::function<void()> outputLimitReached;
    std::function<void(QProcess::ProcessError)> errorOccurred;
    std::function<void(int, QProcess::ExitStatus)> finished;
    void clearCallbacks() { started = {}; outputLimitReached = {}; errorOccurred = {}; finished = {}; }
    void setProgram(const QString &program) { m_program = program; }
    void setArguments(const QStringList &arguments) { m_arguments = arguments; }
    void setProcessEnvironment(const QProcessEnvironment &environment) { m_environment = environment; }
    void setMaximumOutputBytes(qsizetype limit) { m_outputLimit = limit; }
    bool outputLimitExceeded() const { return m_outputLimitExceeded; }
    QString errorString() const { return m_errorString; }
    QProcess::ProcessState state() const { return m_pid > 0 ? QProcess::Running : QProcess::NotRunning; }
    QByteArray readAllStandardOutput() { return std::exchange(m_out.data, {}); }
    QByteArray readAllStandardError() { return std::exchange(m_err.data, {}); }
    void write(const QByteArray &data) { m_input += data; flushInput(); }
    // Like QProcess: stdin closes once everything written so far has gone out.
    void closeWriteChannel() { m_closeInput = true; flushInput(); }
    void kill() { if (m_pid > 0) ::kill(m_pid, SIGKILL); }
    void start()
    {
        int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
        const auto failed = [&](const char *call, int error) {
            for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) if (fd >= 0) ::close(fd);
            fail(QProcess::FailedToStart, QStringLiteral("%1: %2").arg(QLatin1String(call), QString::fromLocal8Bit(strerror(error))));
        };
        if (!pipeAbove2(in) || !pipeAbove2(out) || !pipeAbove2(err)) return failed("pipe", errno);
        for (int fd : {in[1], out[0], err[0]}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        // A child that exits without reading must not kill the GUI with SIGPIPE.
        ::fcntl(in[1], F_SETNOSIGPIPE, 1);
        // Like QProcess, a bare name is looked up in PATH and a path runs as is.
        QString program = m_program;
        if (!program.contains(QLatin1Char('/'))) if (const auto found = QStandardPaths::findExecutable(program); !found.isEmpty()) program = found;
        QList<QByteArray> encoded{QFile::encodeName(program)};
        for (const auto &argument : std::as_const(m_arguments)) encoded << QFile::encodeName(argument);
        std::vector<char *> argv; for (auto &value : encoded) argv.push_back(value.data()); argv.push_back(nullptr);
        QList<QByteArray> variables; std::vector<char *> envp; char **environment = *_NSGetEnviron();
        if (m_environment) {
            for (const auto &entry : m_environment->toStringList()) variables << entry.toLocal8Bit();
            for (auto &value : variables) envp.push_back(value.data());
            envp.push_back(nullptr); environment = envp.data();
        }
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, in[0], 0); posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_adddup2(&actions, err[1], 2);
        posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
        sigset_t none, defaults; sigemptyset(&none); sigemptyset(&defaults); sigaddset(&defaults, SIGPIPE);
        posix_spawnattr_setsigmask(&attributes, &none); posix_spawnattr_setsigdefault(&attributes, &defaults);
        // Only stdin, stdout and stderr reach the child, whatever else the GUI has open.
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        pid_t pid = -1;
        const int error = posix_spawn(&pid, argv.front(), &actions, &attributes, argv.data(), environment);
        posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
        if (error) return failed("posix_spawn", error);
        ::close(in[0]); ::close(out[1]); ::close(err[1]);
        m_pid = pid; m_in = in[1]; watchRead(m_out, out[0]); watchRead(m_err, err[0]);
        watchExit();
        QMetaObject::invokeMethod(this, [this] { if (started) started(); }, Qt::QueuedConnection);
    }
    // Blocking reap for cancellation and shutdown, after kill().
    bool waitForFinished(int msecs)
    {
        const QDeadlineTimer deadline(msecs);
        while (m_pid > 0) {
            int status = 0; const pid_t reapedPid = ::waitpid(m_pid, &status, WNOHANG);
            if (reapedPid == m_pid || (reapedPid < 0 && errno != EINTR)) { exited(reapedPid == m_pid, status); break; }
            if (deadline.hasExpired()) return false;
            ::usleep(1000);
        }
        return true;
    }

private:
    struct Channel { int fd = -1; QSocketNotifier *notifier = nullptr; QByteArray data; };
    static bool pipeAbove2(int ends[2])
    {
        if (::pipe(ends)) return false;
        // Keep the parent's stdio slots free so the child's dup2 targets never collide.
        for (int i = 0; i < 2; ++i) {
            if (ends[i] > 2) { ::fcntl(ends[i], F_SETFD, FD_CLOEXEC); continue; }
            const int moved = ::fcntl(ends[i], F_DUPFD_CLOEXEC, 3); ::close(ends[i]); ends[i] = moved;
            if (moved < 0) { if (ends[1 - i] >= 0) ::close(ends[1 - i]); ends[0] = ends[1] = -1; return false; }
        }
        return true;
    }
    void fail(QProcess::ProcessError error, const QString &detail)
    {
        m_errorString = error == QProcess::FailedToStart
            ? QCoreApplication::translate("QProcess", "Child process set up failed: %1").arg(detail) : detail;
        // QProcess reports a failed start after start() returns.
        const auto notify = [this, error] { if (errorOccurred) errorOccurred(error); };
        if (error == QProcess::FailedToStart) QMetaObject::invokeMethod(this, notify, Qt::QueuedConnection); else notify();
    }
    void watchRead(Channel &channel, int fd)
    {
        channel.fd = fd; channel.notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        QObject::connect(channel.notifier, &QSocketNotifier::activated, this, [this, &channel] { drain(channel); });
    }
    void drain(Channel &channel)
    {
        char chunk[65536];
        int chunks = 0;
        while (channel.fd >= 0) {
            const auto count = ::read(channel.fd, chunk, sizeof chunk);
            if (count > 0) {
                const auto keep = m_outputLimit > 0 ? qMin<qsizetype>(count, qMax<qsizetype>(0,m_outputLimit - m_retained)) : count;
                channel.data.append(chunk, keep); m_retained += keep;
                if (keep < count && !m_outputLimitExceeded) {
                    m_outputLimitExceeded = true;
                    if (outputLimitReached) outputLimitReached();
                    return;
                }
                // A producer that never pauses must still allow the deadline
                // and unrelated helper children to run on this worker.
                if (m_outputLimit > 0 && ++chunks >= 16) return;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno == EAGAIN) return;
            closeRead(channel);   // end of stream: the child closed it, possibly still running
        }
    }
    void closeRead(Channel &channel)
    {
        if (channel.notifier) { channel.notifier->setEnabled(false); channel.notifier->deleteLater(); channel.notifier = nullptr; }
        if (channel.fd >= 0) { ::close(channel.fd); channel.fd = -1; }
    }
    void flushInput()
    {
        if (m_in < 0) return;   // buffered until the child starts
        while (m_written < m_input.size()) {
            const auto count = ::write(m_in, m_input.constData() + m_written, size_t(m_input.size() - m_written));
            if (count > 0) { m_written += count; continue; }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno == EAGAIN) {
                if (!m_inNotifier) {
                    m_inNotifier = new QSocketNotifier(m_in, QSocketNotifier::Write, this);
                    QObject::connect(m_inNotifier, &QSocketNotifier::activated, this, [this] { flushInput(); });
                }
                m_inNotifier->setEnabled(true); return;
            }
            // The child closed stdin. QProcess reports a write error and stops writing.
            closeInput(); fail(QProcess::WriteError, QCoreApplication::translate("QProcess", "Error writing to process"));
            return;
        }
        // Payloads can carry secrets: do not keep a copy once written.
        m_input.fill('\0'); m_input.clear(); m_written = 0;
        if (m_inNotifier) m_inNotifier->setEnabled(false);
        if (m_closeInput) closeInput();
    }
    void closeInput()
    {
        if (m_inNotifier) { m_inNotifier->setEnabled(false); m_inNotifier->deleteLater(); m_inNotifier = nullptr; }
        if (m_in >= 0) { ::close(m_in); m_in = -1; }
        m_input.fill('\0'); m_input.clear(); m_written = 0;
    }
    void watchExit()
    {
        m_kqueue = ::kqueue();
        struct kevent change; EV_SET(&change, m_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        if (m_kqueue >= 0 && ::kevent(m_kqueue, &change, 1, nullptr, 0, nullptr) == 0) {
            m_exitNotifier = new QSocketNotifier(m_kqueue, QSocketNotifier::Read, this);
            QObject::connect(m_exitNotifier, &QSocketNotifier::activated, this, [this] { reap(); });
            return;
        }
        // ESRCH: the child already exited and waits as a zombie. Without kqueue,
        // reap() polls; either way only this pid is ever waited for.
        stopWatching();
        QMetaObject::invokeMethod(this, [this] { reap(); }, Qt::QueuedConnection);
    }
    void stopWatching()
    {
        if (m_exitNotifier) { m_exitNotifier->setEnabled(false); m_exitNotifier->deleteLater(); m_exitNotifier = nullptr; }
        if (m_kqueue >= 0) { ::close(m_kqueue); m_kqueue = -1; }
    }
    void reap()
    {
        if (m_pid <= 0) return;
        int status = 0; pid_t reapedPid;
        do reapedPid = ::waitpid(m_pid, &status, WNOHANG); while (reapedPid < 0 && errno == EINTR);
        // NOTE_EXIT can precede the zombie becoming waitable; look again shortly.
        if (reapedPid == 0) {
            if (m_exitNotifier) m_exitNotifier->setEnabled(false);
            QTimer::singleShot(m_kqueue >= 0 ? 1 : 20, this, [this] { reap(); }); return;
        }
        exited(reapedPid == m_pid, status);
    }
    void exited(bool known, int status)
    {
        m_pid = -1; stopWatching();
        // Output written before the exit is already in the pipes. Like QProcess,
        // do not wait for end of stream that a surviving grandchild may hold open.
        drain(m_out); drain(m_err); closeRead(m_out); closeRead(m_err); closeInput();
        const bool crashed = !known || WIFSIGNALED(status);
        const int code = !known ? -1 : crashed ? WTERMSIG(status) : WEXITSTATUS(status);
        if (crashed) fail(QProcess::Crashed, QCoreApplication::translate("QProcess", "Process crashed"));
        if (finished) finished(code, crashed ? QProcess::CrashExit : QProcess::NormalExit);
    }

    QString m_program, m_errorString;
    QStringList m_arguments;
    std::optional<QProcessEnvironment> m_environment;
    pid_t m_pid = -1;
    int m_in = -1, m_kqueue = -1;
    QSocketNotifier *m_inNotifier = nullptr, *m_exitNotifier = nullptr;
    Channel m_out, m_err;
    QByteArray m_input;
    qsizetype m_written = 0;
    qsizetype m_outputLimit = 0, m_retained = 0;
    bool m_outputLimitExceeded = false;
    bool m_closeInput = false;
};
using Child = SpawnedProcess;
template <typename F> void onStarted(Child *child, F f) { child->started = std::move(f); }
template <typename F> void onError(Child *child, F f) { child->errorOccurred = std::move(f); }
template <typename F> void onFinished(Child *child, F f) { child->finished = std::move(f); }
void detach(Child *child) { child->clearCallbacks(); child->disconnect(); }
#else
using Child = QProcess;
template <typename F> void onStarted(Child *child, F f) { QObject::connect(child, &QProcess::started, child, std::move(f)); }
template <typename F> void onError(Child *child, F f) { QObject::connect(child, &QProcess::errorOccurred, child, std::move(f)); }
template <typename F> void onFinished(Child *child, F f) { QObject::connect(child, &QProcess::finished, child, std::move(f)); }
void detach(Child *child) { child->disconnect(); }
#endif

// Everything below the worker object is touched only on the worker thread;
// pending results only on the application thread. The two sides talk through
// queued calls, so neither needs locks beyond the test observer.
struct Job {
    Child *process = nullptr;
    QTimer *timer = nullptr;
    bool reported = false;
    Result result;
};

struct Pending {
    QPointer<QObject> owner;
    std::function<void(const Result &)> done;
    QMetaObject::Connection destroyed;
};

class Runner {
public:
    Runner()
    {
        m_thread.setObjectName(QStringLiteral("hgs-process-runner"));
        m_worker = new QObject; m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }
    void run(Request request, QObject *owner, std::function<void(const Result &)> done)
    {
        const quint64 id = ++m_next;
        Pending pending{owner, std::move(done), {}};
        // Owner destruction kills the child synchronously, as the old per-object
        // QProcess children did, so fixtures and sockets are not used afterwards.
        pending.destroyed = QObject::connect(owner, &QObject::destroyed, &m_front, [this, id] { cancel(id); },
                                             Qt::DirectConnection);
        m_pending.insert(id, std::move(pending));
        QMetaObject::invokeMethod(m_worker, [this, id, request = std::move(request)]() mutable { start(id, std::move(request)); },
                                  Qt::QueuedConnection);
    }
    void stop()
    {
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) QObject::disconnect(it->destroyed);
        m_pending.clear();
        // Quit must not leave children running or unreaped: kill and reap them on
        // the worker, then stop its loop. SIGKILL makes each wait short.
        QMetaObject::invokeMethod(m_worker, [this] {
            for (auto *job : std::as_const(m_jobs)) discard(job);
            qDeleteAll(m_jobs); m_jobs.clear();
        }, Qt::BlockingQueuedConnection);
        m_thread.quit(); m_thread.wait();
    }
    QThread *thread() { return &m_thread; }
    void setObserver(std::function<void()> observer) { QMutexLocker lock(&m_observerMutex); m_observer = std::move(observer); }

private:
    void start(quint64 id, Request request)   // worker thread
    {
        auto *job = new Job; m_jobs.insert(id, job);
        auto *process = job->process = new Child(m_worker);
        process->setProgram(request.program); process->setArguments(request.arguments);
        if (request.environment) process->setProcessEnvironment(*request.environment);
#ifdef Q_OS_MACOS
        process->setMaximumOutputBytes(request.maximumOutputBytes);
        process->outputLimitReached = [this,id,job,process] {
            job->result.standardOutput=process->readAllStandardOutput();job->result.standardError=process->readAllStandardError();
            job->result.outputLimitExceeded=true;
            report(id,job,Result::OutputLimitExceeded);process->kill();
        };
#else
        if (request.maximumOutputBytes > 0) {
            const auto drain = [this, id, job, process, limit=request.maximumOutputBytes] {
                const auto append = [job,limit](QByteArray &target,const QByteArray &bytes) {
                    const auto remaining = qMax<qsizetype>(0,limit - job->result.standardOutput.size() - job->result.standardError.size());
                    target.append(bytes.constData(),qMin(remaining,bytes.size()));
                    if (bytes.size() > remaining) job->result.outputLimitExceeded = true;
                };
                append(job->result.standardOutput,process->readAllStandardOutput());
                append(job->result.standardError,process->readAllStandardError());
                if(job->result.outputLimitExceeded&&!job->reported){report(id,job,Result::OutputLimitExceeded);process->kill();}
            };
            QObject::connect(process,&QProcess::readyReadStandardOutput,process,drain);
            QObject::connect(process,&QProcess::readyReadStandardError,process,drain);
        }
#endif
#if defined(Q_OS_LINUX) && QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        // No child modifier runs here, so Linux may use vfork and skip copying
        // the page tables of the large GUI process.
        process->setUnixProcessParameters(QProcess::UnixProcessFlag::UseVFork);
#endif
#ifndef Q_OS_MACOS
        // As with posix_spawn on macOS, whatever else the GUI has open stays
        // out, such as a PTY forkpty() has just made on the GUI thread.
        ChildProcess::inheritStdioOnly(*process);
#endif
        if (request.input) onStarted(process, [process, input = std::move(*request.input)]() mutable {
            if (!input.isEmpty()) process->write(input);
            input.fill('\0'); input.clear(); process->closeWriteChannel();
        });
        onError(process, [this, id, job, process](QProcess::ProcessError error) {
            if (job->result.error == QProcess::UnknownError) { job->result.error = error; job->result.errorString = process->errorString(); }
            // FailedToStart is the only error not followed by finished().
            if (error == QProcess::FailedToStart) { report(id, job, Result::FailedToStart); finish(id, job); }
        });
        onFinished(process, [this, id, job, process, limit=request.maximumOutputBytes](int code, QProcess::ExitStatus status) {
            job->result.exitCode = code; job->result.exitStatus = status;
#ifdef Q_OS_MACOS
            job->result.standardOutput = process->readAllStandardOutput(); job->result.standardError = process->readAllStandardError();
            job->result.outputLimitExceeded = process->outputLimitExceeded();
            Q_UNUSED(limit);
#else
            const auto append = [job,limit](QByteArray &target,const QByteArray &bytes) {
                const auto remaining = limit > 0 ? qMax<qsizetype>(0,limit - job->result.standardOutput.size() - job->result.standardError.size()) : bytes.size();
                target.append(bytes.constData(),qMin(remaining,bytes.size()));
                if (bytes.size() > remaining) job->result.outputLimitExceeded = true;
            };
            append(job->result.standardOutput,process->readAllStandardOutput());
            append(job->result.standardError,process->readAllStandardError());
#endif
            report(id, job, job->result.outputLimitExceeded ? Result::OutputLimitExceeded : Result::Finished); finish(id, job);
        });
        if (request.timeoutMs > 0) {
            auto *timer = job->timer = new QTimer(process); timer->setSingleShot(true);
            // Report at the deadline; kill() still delivers finished() later,
            // which only reaps the child.
            QObject::connect(timer, &QTimer::timeout, process, [this, id, job, process] {
                report(id, job, Result::TimedOut); process->kill();
            });
            timer->start(request.timeoutMs);
        }
        {
            QMutexLocker lock(&m_observerMutex);
            if (m_observer) m_observer();
        }
        process->start();
    }
    void report(quint64 id, Job *job, Result::Outcome outcome)   // worker thread
    {
        if (job->reported) return;
        job->reported = true;
        Result result = outcome == Result::TimedOut ? Result{} : job->result; result.outcome = outcome;
        QMetaObject::invokeMethod(&m_front, [this, id, result = std::move(result)] { deliver(id, result); }, Qt::QueuedConnection);
    }
    void finish(quint64 id, Job *job)   // worker thread
    {
        if (m_jobs.take(id) != job) return;
        // The process is deleted later; its handlers and deadline must not see
        // the freed job meanwhile.
        if (job->timer) { job->timer->stop(); job->timer->disconnect(); }
        detach(job->process); job->process->deleteLater(); delete job;
    }
    void discard(Job *job)   // worker thread
    {
        auto *process = job->process; detach(process);
        if (job->timer) { job->timer->stop(); job->timer->disconnect(); }
        if (process->state() != QProcess::NotRunning) { process->kill(); process->waitForFinished(1000); }
        delete process;
    }
    void deliver(quint64 id, const Result &result)   // application thread
    {
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        Pending pending = std::move(*it); m_pending.erase(it);
        QObject::disconnect(pending.destroyed);
        if (pending.owner) pending.done(result);
    }
    void cancel(quint64 id)   // application thread, while the owner is destroyed
    {
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;
        m_pending.erase(it);
        QMetaObject::invokeMethod(m_worker, [this, id] {
            if (auto *job = m_jobs.take(id)) { discard(job); delete job; }
        }, Qt::BlockingQueuedConnection);
    }

    QThread m_thread;
    QObject *m_worker = nullptr;
    QObject m_front;   // receives results on the application thread
    QHash<quint64, Pending> m_pending;
    QHash<quint64, Job *> m_jobs;
    quint64 m_next = 0;
    QMutex m_observerMutex;
    std::function<void()> m_observer;
};

Runner *s_runner = nullptr;
bool s_stopped = false;

void shutdown()
{
    s_stopped = true;
    auto *runner = std::exchange(s_runner, nullptr);
    if (!runner) return;
    runner->stop(); delete runner;
}

Runner *runner()
{
    if (!s_runner && !s_stopped) {
        Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
        s_runner = new Runner;
        qAddPostRoutine(shutdown);
    }
    return s_runner;
}

}

void run(Request request, QObject *owner, std::function<void(const Result &)> done)
{
    Q_ASSERT(owner && owner->thread() == QThread::currentThread());
    // After the application's post routines nothing can receive a result.
    if (auto *instance = runner()) instance->run(std::move(request), owner, std::move(done));
}

#ifdef Q_OS_MACOS
const char *launchBackend() { return "posix_spawn"; }
#else
const char *launchBackend() { return "QProcess"; }
#endif

QThread *workerThread() { auto *instance = runner(); return instance ? instance->thread() : nullptr; }

void setLaunchObserver(std::function<void()> observer) { if (auto *instance = runner()) instance->setObserver(std::move(observer)); }

}
