#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <limits>

class HgsClient;

// Overmind boards for the desktop, read with `hgs [@host] overmind ... --json`.
// Every machine holds a full replica, so the host is empty (local) except on a
// computer without Overmind (a Windows console), which asks the machine of the
// selected session. There the model starts disabled until a host is set.
//
// Changes are found by polling `rev`, which opens the store without the write
// lock: every ActiveIntervalMs while the Tasks tab is visible (setActive), every
// IdleIntervalMs otherwise. A watched board, the decision list and this
// machine's ID are read again only when the cursor differs from the one they
// were read at, so a board of hundreds of tasks costs one read per change, never
// one per card or per poll. A read that failed waits for the next change.
class OvermindModel : public QObject {
    Q_OBJECT
public:
    static constexpr int ActiveIntervalMs = 2000, IdleIntervalMs = 10000;
    // Deadlines of one CLI call. Delivering commands may type into a session or
    // reach its host over SSH (like `hgs send`), so they get longer ones.
    struct Timeouts {
        int local = 10000, remote = 30000, deliveryLocal = 45000, deliveryRemote = 60000;
    };
    // send, review-send, rework-send, answer, resolve-delivery.
    static bool delivers(const QString &command);

    // A session as Overmind names it: the machine's swarm node ID and the name.
    struct Session {
        QString machine, name;
        bool isEmpty() const { return name.isEmpty(); }
    };
    struct Stage {
        QString name;
        QString role;   // backlog | ready | active | review | needs_human | done | parked
    };
    struct Task {
        QString uuid, ref, project, title, stage, rank;
        int priority = 2;
        QStringList labels, flags;
        // working, reviewed, needs_work, conflict, uncertain, orphaned (in this order).
        QStringList markers;
        Session pinned, assignee, reviewer;
        // The session whose queue holds the task: set only in a ready stage.
        Session queueOf;
        // The pin's current name when its session was renamed on this machine.
        QString pinnedCurrent;
        // The whole task view as the CLI sent it, for the fields above and the rest.
        QJsonObject json;
    };
    struct Board {
        QString project;
        bool loaded = false;    // a board reply arrived since watching (or the host changed)
        qint64 cursor = -1;     // the store cursor this board was read at
        QJsonObject workflow;   // {version, stages, rules, evidence, session_wip}
        QList<Stage> stages;
        QList<Task> tasks;      // board order: by stage, then by rank
        QString error;          // the last read failed; the tasks are those of the read before
        QJsonObject refusal;    // its parsed `error` object, when the CLI refused
        QString role(const QString &stage) const;   // empty for a stage the workflow lacks
    };
    struct Decision {
        QString task, ref, project, title, stage, question;
        // Changes whenever the task is parked again or moved: notify once per value.
        QString signature;
        Session assignee;
    };
    struct Result {
        bool ok = false;
        QJsonObject reply;      // the CLI's answer when ok
        // Otherwise the parsed `error` ({code, message, field?, state?}); without
        // one only `message`. A deadline: {code: "timed_out", message, uncertain: true}.
        // A disabled model: {code: "unavailable", message}.
        QJsonObject refusal;
        // The request_id a write sent: retrying with it replays instead of
        // writing twice. Empty for a read.
        QString requestId;
        bool uncertain() const { return refusal.value(QStringLiteral("uncertain")).toBool(); }
    };
    using Callback = std::function<void(const Result &result)>;

    explicit OvermindModel(HgsClient *client, QObject *parent = nullptr);

    // Disabled: no polls or reads, cached data cleared, writes and reads fail
    // with code "unavailable". Enabled by default, except on Windows until a host
    // is set; setHost with a host enables the model.
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }
    // Empty: the local replica (on Windows: disabled). A change drops every
    // cached board, the decisions and machine().
    void setHost(const QString &host);
    QString host() const { return m_host; }
    // True while a board is visible: polls every ActiveIntervalMs, and at once.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // Tests: shorter intervals and deadlines.
    void setIntervals(int activeMs, int idleMs);
    void setTimeouts(const Timeouts &timeouts) { m_timeouts = timeouts; }
    Timeouts timeouts() const { return m_timeouts; }

    // Starts reading the project's board now and on every change. Watching a
    // board whose last read failed reads it again now.
    void watchProject(const QString &project);
    void unwatchProject(const QString &project);
    QStringList watchedProjects() const { return m_watched; }
    // An empty, not loaded board for a project that is not watched. The
    // reference is valid only until the next call that watches, unwatches or
    // reloads boards (or returns to the event loop): copy what must be kept.
    const Board &board(const QString &project) const;
    // Your Call tasks across projects, oldest first (at most 50).
    const QList<Decision> &decisions() const { return m_decisions; }
    bool decisionsLoaded() const { return m_decisionsLoaded; }
    QString decisionsError() const { return m_decisionsError; }   // the last read failed
    // The node ID of the machine whose replica is read; empty until known.
    QString machine() const { return m_machine; }
    // Tasks queued for the session in the watched boards. An empty machine
    // means machine(); 0 while it is unknown.
    int queueCount(const QString &machine, const QString &session) const;
    qint64 cursor() const { return m_cursor; }   // -1 before the first answer
    QString error() const { return m_error; }    // the last poll failed

    // A write ("move", "workflow set", ...). Adds a fresh request_id unless the
    // request has one (a retry). On success it checks for changes; on failure it
    // emits writeFailed. The callback runs last and is skipped once a given
    // context is destroyed. Returns the client request, 0 when disabled.
    quint64 write(const QString &command, QJsonObject request, Callback callback = {}, QObject *context = nullptr);
    // A read that is not cached ("show", "workflow get", ...): only the callback.
    quint64 read(const QString &command, const QJsonObject &request, Callback callback, QObject *context = nullptr);

public slots:
    // Polls rev now (one poll at a time; a request during one runs after it).
    void refresh();

signals:
    // Only when what a view shows changed (tasks, workflow, error, cleared),
    // not when a read merely confirms a newer cursor.
    void boardChanged(const QString &project);
    // The list or decisionsError() changed, or it was cleared.
    void decisionsChanged();
    void machineChanged();
    void writeFailed(const QString &command, const QJsonObject &refusal, const QString &requestId);
    void errorChanged();

private:
    enum class Kind { Rev, Board, Decisions, Status, Write, Read };
    struct Pending {
        Kind kind = Kind::Rev;
        QString name;   // the project of a board, the command of a write or read
        Callback callback;
        QPointer<QObject> context;
        bool guarded = false;   // a context was given: skip the callback once it is gone
        quint64 generation = 0;
        QString requestId;
    };
    static constexpr qint64 Never = std::numeric_limits<qint64>::min();
    static void finish(const Pending &pending, const Result &result);
    void replied(quint64 request, const QString &host, const QJsonObject &reply);
    void failed(quint64 request, const QString &host, const QJsonObject &refusal, const QString &detail);
    void reset();
    void reloadStale();
    void loadBoard(const QString &project);
    void loadDecisions();
    void loadStatus();
    void setError(const QString &error);
    quint64 send(const QString &command, const QJsonObject &request, int timeoutMs = 0);
    quint64 track(Pending pending, const QString &command, const QJsonObject &request, int timeoutMs = 0);
    void failLater(Pending pending);

    HgsClient *m_client;
    QString m_host;
    bool m_enabled = true;
    bool m_active = false;
    int m_activeMs = ActiveIntervalMs, m_idleMs = IdleIntervalMs;
    Timeouts m_timeouts;
    QTimer m_timer;
    // Bumped by setHost and setEnabled: answers to earlier reads are dropped.
    quint64 m_generation = 0;
    QHash<quint64, Pending> m_pending;
    bool m_revInFlight = false, m_revAgain = false;
    bool m_decisionsInFlight = false, m_decisionsLoaded = false, m_statusInFlight = false;
    QStringList m_boardsInFlight;
    // The cursor at which a read failed: it is retried once the cursor differs.
    QHash<QString, qint64> m_boardsFailedAt;
    qint64 m_decisionsFailedAt = Never, m_statusFailedAt = Never;
    qint64 m_cursor = -1, m_decisionsCursor = -1;
    QString m_error, m_decisionsError, m_machine;
    QStringList m_watched;
    QHash<QString, Board> m_boards;
    QList<Decision> m_decisions;
};
