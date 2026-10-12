#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

// Windows has no user service manager in Phase 1; systemd and launchd run `hgs
// swarm worker` on Linux and macOS. There the tray keeps one hidden worker: it is
// restarted with a growing delay after it exits and ends with the tray, also when
// the tray crashes (a kill-on-close Job Object of its own for every worker).
// TrayAgent creates it on Windows only; the class itself is portable so the
// tests run everywhere.
class SwarmWorkerSupervisor : public QObject {
    Q_OBJECT
public:
    explicit SwarmWorkerSupervisor(QString hgsPath, int firstRestartMs = 1000, QObject *parent = nullptr);
    ~SwarmWorkerSupervisor() override;
    void start();
    void stop();
    bool isRunning() const { return m_process && m_process->state() != QProcess::NotRunning; }
    qint64 processId() const { return isRunning() ? m_process->processId() : 0; }
    // Windows: the running worker is in its kill-on-close job, so it cannot
    // outlive the tray. For tests and diagnostics; false elsewhere.
    bool workerContained() const;
    // The delay before the next start: doubled after a short run, back to the first
    // delay after a run of a minute or more, at most five minutes.
    static int nextDelay(int firstMs, int previousMs, qint64 runMs);

signals:
    void workerStarted(qint64 pid);
    void workerExited(int exitCode, const QString &detail);

private:
    void launch();
    void exited(int exitCode, QProcess::ExitStatus status);
    void closeJob();
    QString m_hgs;
    int m_firstRestartMs;
    int m_delayMs = 0;
    bool m_active = false;
    QProcess *m_process = nullptr;
    QTimer m_restart;
    QElapsedTimer m_uptime;
    QByteArray m_errors;
    void *m_job = nullptr;
};
