#include "SwarmWorkerSupervisor.h"
#include "ChildProcess.h"

#include <QDebug>

#include <algorithm>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
constexpr int MaximumDelayMs = 5 * 60 * 1000;
constexpr qint64 HealthyRunMs = 60 * 1000;
constexpr qsizetype ErrorTail = 4096;
} // namespace

SwarmWorkerSupervisor::SwarmWorkerSupervisor(QString hgsPath, int firstRestartMs, QObject *parent)
    : QObject(parent), m_hgs(hgsPath.isEmpty() ? QStringLiteral("hgs") : std::move(hgsPath)),
      m_firstRestartMs(std::max(1, firstRestartMs))
{
    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, &SwarmWorkerSupervisor::launch);
}

SwarmWorkerSupervisor::~SwarmWorkerSupervisor()
{
    stop();
}

void SwarmWorkerSupervisor::closeJob()
{
#ifdef Q_OS_WIN
    // Ends whatever the finished worker left running in it.
    if (m_job) CloseHandle(static_cast<HANDLE>(m_job));
#endif
    m_job = nullptr;
}

bool SwarmWorkerSupervisor::workerContained() const
{
#ifdef Q_OS_WIN
    if (!m_job || !isRunning()) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(static_cast<HANDLE>(m_job), JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits), nullptr)
        || !(limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE))
        return false;
    BOOL contained = FALSE;
    if (HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(m_process->processId()))) {
        if (!IsProcessInJob(handle, static_cast<HANDLE>(m_job), &contained)) contained = FALSE;
        CloseHandle(handle);
    }
    return contained;
#else
    return false;
#endif
}

int SwarmWorkerSupervisor::nextDelay(int firstMs, int previousMs, qint64 runMs)
{
    if (previousMs <= 0 || runMs >= HealthyRunMs) return firstMs;
    return std::min(MaximumDelayMs, previousMs * 2);
}

void SwarmWorkerSupervisor::start()
{
    if (m_active) return;
    m_active = true;
    m_delayMs = 0;
    launch();
}

void SwarmWorkerSupervisor::stop()
{
    m_active = false;
    m_restart.stop();
    if (!m_process) return;
    QProcess *process = std::exchange(m_process, nullptr);
    process->disconnect(this);
    if (process->state() != QProcess::NotRunning) {
        // No console and no signals: the worker keeps its state in atomic files.
        process->kill();
        process->waitForFinished(2000);
    }
    delete process;
    closeJob();
}

void SwarmWorkerSupervisor::launch()
{
    if (!m_active || isRunning()) return;
    delete std::exchange(m_process, nullptr);
    closeJob();
    m_errors.clear();
#ifdef Q_OS_WIN
    // A new job for every worker: hgs.exe joins a kill-on-close job of its own at
    // startup, and when it does so before this assignment, the job assigned here
    // becomes part of that worker's job tree for good. A reused job then refuses
    // every later worker (ERROR_ACCESS_DENIED), and those would outlive the tray.
    // It forbids breakaway, so the worker must never create a session: a host it
    // started would stop in this job and be refused (D4). It only syncs catalogs.
    if (HANDLE job = CreateJobObjectW(nullptr, nullptr)) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) m_job = job;
        else CloseHandle(job);
    }
#endif
    auto *process = m_process = new QProcess(this);
    process->setProgram(m_hgs);
    process->setArguments({QStringLiteral("swarm"), QStringLiteral("worker")});
    process->setStandardInputFile(QProcess::nullDevice());
    process->setStandardOutputFile(QProcess::nullDevice());
    ChildProcess::inheritStdioOnly(*process);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        m_errors += process->readAllStandardError();
        if (m_errors.size() > ErrorTail) m_errors = m_errors.right(ErrorTail);
    });
    connect(process, &QProcess::started, this, [this, process] {
        m_uptime.start();
#ifdef Q_OS_WIN
        // Assigned once running: hgs.exe starts ssh.exe only later, inside its own
        // job, which is then nested in this one (or this one in it).
        DWORD error = ERROR_INVALID_HANDLE;
        if (m_job) {
            if (HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process->processId()))) {
                error = AssignProcessToJobObject(static_cast<HANDLE>(m_job), handle) ? ERROR_SUCCESS : GetLastError();
                CloseHandle(handle);
            } else {
                error = GetLastError();
            }
        }
        if (error != ERROR_SUCCESS)
            qWarning().noquote() << "hgs zerus: swarm worker" << process->processId()
                                 << "may outlive the tray: no kill-on-close job, error" << error;
#endif
        emit workerStarted(process->processId());
    });
    connect(process, &QProcess::finished, this, &SwarmWorkerSupervisor::exited);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // FailedToStart is the only error without a finished() afterwards.
        if (error == QProcess::FailedToStart) exited(-1, QProcess::CrashExit);
    });
    process->start();
}

void SwarmWorkerSupervisor::exited(int exitCode, QProcess::ExitStatus status)
{
    closeJob();
    const qint64 ran = m_uptime.isValid() ? m_uptime.elapsed() : 0;
    m_uptime.invalidate();
    QString detail = QString::fromUtf8(m_errors).trimmed();
    if (m_process && m_process->error() == QProcess::FailedToStart) detail = m_process->errorString();
    if (status != QProcess::NormalExit || exitCode != 0)
        qWarning().noquote() << "hgs zerus: swarm worker exited with code" << exitCode << detail;
    emit workerExited(exitCode, detail);
    if (!m_active) return;
    m_delayMs = nextDelay(m_firstRestartMs, m_delayMs, ran);
    m_restart.start(m_delayMs);
}
