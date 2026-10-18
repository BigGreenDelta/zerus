#pragma once
#include "TaskBoard.h"
#include "TaskRow.h"
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include <optional>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTextBrowser;
struct TaskSession;

// The session a task is pinned to; any is Any session.
struct TaskTarget {
    bool any = true;
    QString host, session;
    bool operator==(const TaskTarget &other) const { return any == other.any && host == other.host && session == other.session; }
};

// Unsaved changes in the task settings: only what the person changed.
struct TaskEdits {
    std::optional<QString> text;
    std::optional<TaskTarget> target;
};

// A task's settings: its prompt and session (editable in To do and Backlog), its
// history, a comment field and the stage's actions. An action first applies the
// unsaved prompt and session; Cancel or Escape discards them. Save and the actions
// wait for their owner: it closes the dialog once hgs took them, or calls setError
// and the person's edits stay.
class TaskDialog : public QDialog {
    Q_OBJECT
public:
    // task: the `hgs task get` view, history included. sendBlocker: why Send now
    // cannot paste into a session the task would go to (empty when it can).
    TaskDialog(const QJsonObject &task, const QList<TaskSession> &sessions, std::function<QString(const TaskTarget &)> sendBlocker,
               bool dark, QWidget *parent = nullptr);
    TaskEdits edits() const;
    void setHistory(const QJsonArray &log);
    void clearComment();
    void setError(const QString &error);
signals:
    void saveRequested(const TaskEdits &edits);
    void actionRequested(const TaskEdits &edits, TaskAction action);
    void commentRequested(const QString &text);
private:
    void setBusy(bool busy);
    // Each button's state: busy, an empty prompt, a newer task, and Send now's
    // reason for the session chosen now.
    void refreshButtons();
    TaskTarget chosenTarget() const;
    TaskItem m_task;
    TaskTarget m_target;
    std::function<QString(const TaskTarget &)> m_sendBlocker;
    QLabel *m_error;
    bool m_busy = false;
    QPlainTextEdit *m_prompt;
    QButtonGroup *m_sessions = nullptr;
    QList<TaskTarget> m_choices;
    QTextBrowser *m_history;
    QLineEdit *m_comment;
    bool m_editable;
};

// Confirms a Cancel: the task moves to In review and its session gets the cancel
// message; an optional reason becomes a comment and is never sent.
class TaskCancelDialog : public QDialog {
    Q_OBJECT
public:
    // running: the session's agent runs; working: it is in a turn now.
    TaskCancelDialog(const TaskItem &task, bool running, bool working, QWidget *parent = nullptr);
    QString reason() const;
private:
    QLineEdit *m_reason;
};

// The project's task settings: the prompt templates, the switch for agents acting
// on other sessions and the limit on tasks agents start per hour. Save waits for
// its owner like the task settings.
class TaskSettingsDialog : public QDialog {
    Q_OBJECT
public:
    TaskSettingsDialog(const QString &projectName, const QJsonObject &settings, QWidget *parent = nullptr);
    // The fields the person changed, for `hgs task settings --set`.
    QJsonObject changes() const;
    void setError(const QString &error);
signals:
    void saveRequested(const QJsonObject &changes);
private:
    QJsonObject m_settings;
    QLabel *m_error;
    QPushButton *m_save;
    QPlainTextEdit *m_task, *m_cancel;
    QCheckBox *m_agentsOther;
    QSpinBox *m_limit;
};
