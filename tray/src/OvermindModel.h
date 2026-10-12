#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

class HgsClient;

// Overmind boards for the desktop, read with `hgs [@host] overmind ... --json`.
// Every machine holds a full replica, so the host is empty (local) except on a
// computer without Overmind, which asks the machine of the selected session.
//
// Changes are found by polling `rev`, which opens the store without the write
// lock: every ActiveIntervalMs while the Tasks tab is visible (setActive), every
// IdleIntervalMs otherwise. A watched board and the decision list are read again
// only when the cursor differs from the one they were read at, so a board of
// hundreds of tasks costs one read per change, never one per card or per poll.
// A read that failed is retried on the next poll.
class OvermindModel : public QObject {
    Q_OBJECT
public:
    static constexpr int ActiveIntervalMs = 2000, IdleIntervalMs = 10000;

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
    // ok: reply is the CLI's answer. Otherwise refusal is the parsed `error`
    // ({code, message, field?, state?}); without one it has only `message`.
    using Callback = std::function<void(bool ok, const QJsonObject &reply, const QJsonObject &refusal)>;

    explicit OvermindModel(HgsClient *client, QObject *parent = nullptr);

    // Empty: the local replica. A change drops every cached board and the decisions.
    void setHost(const QString &host);
    QString host() const { return m_host; }
    // True while a board is visible: polls every ActiveIntervalMs, and at once.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    // Tests: shorter intervals.
    void setIntervals(int activeMs, int idleMs);

    // Starts reading the project's board now and on every change.
    void watchProject(const QString &project);
    void unwatchProject(const QString &project);
    QStringList watchedProjects() const { return m_watched; }
    // An empty, not loaded board for a project that is not watched.
    const Board &board(const QString &project) const;
    // Your Call tasks across projects, oldest first (at most 50).
    const QList<Decision> &decisions() const { return m_decisions; }
    bool decisionsLoaded() const { return m_decisionsLoaded; }
    // Tasks queued for the session in the watched boards; an empty machine matches any.
    int queueCount(const QString &machine, const QString &session) const;
    qint64 cursor() const { return m_cursor; }   // -1 before the first answer
    QString error() const { return m_error; }    // the last poll failed

    // A write ("move", "workflow set", ...). Adds a fresh request_id unless the
    // request has one (a retry), checks for changes on success and always emits
    // writeFailed on failure; callback runs first. Returns the client request.
    quint64 write(const QString &command, QJsonObject request, Callback callback = {});
    // A read that is not cached ("show", "workflow get", ...): only the callback.
    quint64 read(const QString &command, const QJsonObject &request, Callback callback);

public slots:
    // Polls rev now (one poll at a time; a request during one runs after it).
    void refresh();

signals:
    // Only when what a view shows changed (tasks, workflow, error, cleared by
    // setHost), not when a read merely confirms a newer cursor.
    void boardChanged(const QString &project);
    void decisionsChanged();
    void writeFailed(const QString &command, const QJsonObject &refusal);
    void errorChanged();

private:
    enum class Kind { Rev, Board, Decisions, Write, Read };
    struct Pending {
        Kind kind;
        QString name;   // the project of a board, the command of a write or read
        Callback callback;
        quint64 generation = 0;
    };
    void replied(quint64 request, const QString &host, const QJsonObject &reply);
    void failed(quint64 request, const QString &host, const QJsonObject &refusal, const QString &detail);
    void reloadStale();
    void loadBoard(const QString &project);
    void loadDecisions();
    void setError(const QString &error);
    quint64 send(const QString &command, const QJsonObject &request);

    HgsClient *m_client;
    QString m_host;
    bool m_active = false;
    int m_activeMs = ActiveIntervalMs, m_idleMs = IdleIntervalMs;
    QTimer m_timer;
    // Bumped by setHost: answers from the previous host are dropped.
    quint64 m_generation = 0;
    QHash<quint64, Pending> m_pending;
    bool m_revInFlight = false, m_revAgain = false;
    bool m_decisionsInFlight = false, m_decisionsLoaded = false;
    QStringList m_boardsInFlight;
    qint64 m_cursor = -1, m_decisionsCursor = -1;
    QString m_error;
    QStringList m_watched;
    QHash<QString, Board> m_boards;
    QList<Decision> m_decisions;
};
