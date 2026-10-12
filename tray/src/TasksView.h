#pragma once

#include "OvermindModel.h"
#include "SessionOrganization.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QToolButton;
class TaskCardDelegate;
class TaskColumn;
class TaskPanel;

// Item data of a card in a column's list.
namespace TaskRoles {
enum {
    Uuid = Qt::UserRole + 1,
    Ref,
    Title,
    Labels,       // shown labels: the verdict labels are markers instead
    Markers,      // working, reviewed, needs_work, conflict, uncertain, orphaned
    Session,      // the session tag: assignee, else the queue's or pin's session, else the reviewer
    Highlighted,  // a task of the selected session
    Priority,
    Stage,
    Owners,       // "machine\nname" of the assignee, queue, pin and reviewer sessions
};
}

// The Tasks tab: the Overmind board of one logical project. The tab belongs to
// the project, not to the selected session: switching sessions only changes
// which cards are highlighted, never the project, the scroll positions, the
// filter or the open card. The project is chosen in the tab and kept in
// workspace/tasksProject; on first use it is the selected session's project.
//
// Columns follow the project workflow. The needs_human stages are not columns
// but the Your Call strip above the board. A card can be dragged only to
// columns the workflow's human move rules allow from its stage; there it runs
// `move`. The CLI stays the authority: its refusal is shown inline while the
// card stays where it was. Dragging inside a column runs `reorder`. The card
// menu (context menu key, Shift+F10 or a right click) moves it by keyboard.
//
// An open card shows its panel on the right of the board (TaskPanel). Writes
// from the board and the panel report in one message bar: a confirmable
// refusal offers to repeat the request with the same request_id and its code
// confirmed; a delivery that may have arrived is never repeated, it offers
// Resolve delivery instead. Moves that take a task from its session or reopen
// it, and other destructive actions, ask first.
class TasksView : public QWidget {
    Q_OBJECT
public:
    static constexpr const char *MimeType = "application/x-zerus-overmind-task";
    // The stage a dragged card comes from, next to MimeType.
    static constexpr const char *StageMimeType = "application/x-zerus-overmind-stage";
    // The session selected in the window.
    struct Selection {
        QString key;        // the window's session key; empty when none is selected
        QString project;    // its logical project (SessionOrganization group ID)
        QString host;       // its host as the window names it; empty for this computer
        // The machine is the Overmind node ID, empty while unknown. Names then
        // match on their own (highlights only): nothing is sent to it.
        OvermindModel::Session session;
    };
    // Colors of SessionsWindow::applyTheme, plus the board's own.
    struct Palette {
        QColor fg, muted, surface, border, accent, hover, selected, column, chip, warning, warningFill, warningBorder, danger, working;
        static Palette of(bool dark);
    };
    // A session a card can be sent to, pinned to or reviewed by. The window
    // passes all its sessions; a card offers those of its project.
    struct Recipient {
        QString key, project, host;   // as in Selection
        OvermindModel::Session session;   // machine "" = unknown: offered, but disabled
        QString status;               // the session's state as the window shows it ("Working", "Ready", ...)
    };
    // A write, and what the message bar says about it.
    struct Write {
        QString command;
        QJsonObject request;
        QString task, ref;   // the task's UUID and ref
        QString session;     // the receiving session's name, for the delivering commands
        QString done;        // shown on success; empty: nothing
    };

    explicit TasksView(OvermindModel *model, QWidget *parent = nullptr);

    // The projects to choose from: the accessible groups are offered.
    void setProjects(const QList<SessionOrganization::Group> &groups);
    void setSession(const Selection &selection);
    // Opens the board of `project` and asks for the tab (showRequested). The
    // selected session's tasks are highlighted when it belongs to the project;
    // when sessionKey is that session, the board scrolls to its first task.
    void showProject(const QString &project, const QString &sessionKey = {});
    QString project() const { return m_project; }
    // The card that is open (selected), empty when none.
    QString openedTask() const { return m_openTask; }
    // Selects the card, scrolls to it and emits taskOpened.
    void openTask(const QString &uuid);
    // Closes the open card and its panel.
    void closeTask();
    void setTheme(bool dark);
    bool isDark() const { return m_dark; }
    const Palette &palette() const { return m_palette; }
    const Selection &selection() const { return m_selection; }
    void setRecipients(const QList<Recipient> &recipients);
    // The sessions of the shown project, the selected session first.
    QList<Recipient> recipients() const;
    // A card dropped on `stage`, before the card `before` (empty: at the end).
    // Another column moves it (moveTask), the same column runs `reorder`; a
    // drop where the card already is writes nothing.
    void dropTask(const QString &uuid, const QString &stage, const QString &before);
    // The stages a person may move the task to: the workflow's human move rules.
    QList<OvermindModel::Stage> moveTargets(const QString &uuid) const;
    // Runs `move`. Taking a task from its session (an active or Your Call
    // stage) or out of a done stage asks first.
    void moveTask(const QString &uuid, const QString &stage);
    // Runs a write and reports it in the message bar; `after` runs last.
    void perform(const Write &write, OvermindModel::Callback after = {});
    // A delivering write for the task is running.
    bool isDelivering(const QString &uuid) const { return m_delivering.value(uuid) > 0; }
    // Asks before a destructive action: Cancel is the default.
    static bool confirm(QWidget *parent, const QString &title, const QString &text, const QString &accept);
    // A message about the open card in the message bar (an error by default).
    void notify(const QString &text, bool error = true);

signals:
    // A card was opened: clicked on the board or in the Your Call strip.
    void taskOpened(const QString &uuid);
    // showProject asks the window to show this tab.
    void showRequested();
    // The project shown changed (its board is watched; the previous one is
    // left to the window, which knows whether other views still need it).
    void projectChanged(const QString &project);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setProject(const QString &project, bool persist);
    void ensureProject();
    void fillProjects();
    void render();
    void rebuildColumns();
    void fillColumns(const OvermindModel::Board &board);
    void renderDecisions(const OvermindModel::Board &board);
    void renderSession(const OvermindModel::Board &board);
    void renderStatus(const OvermindModel::Board &board);
    void applyFilter();
    void applyHighlights();
    void applyStyle();
    bool belongs(const QString &machine, const QString &name) const;
    bool owned(const QStringList &owners) const;
    bool collapsed(const QString &stage, const QString &role) const;
    void toggleColumn(const QString &stage);
    // Whether a person may drag the card to `stage`: its own column, or a
    // column the workflow's human move rules allow from the card's stage.
    bool canDrop(const QString &uuid, const QString &stage) const;
    QString nextTask(const OvermindModel::Board &board) const;
    void sendNext(const QJsonObject &retry = {});
    // A write; sessionKey: it went to the selected session (Send next), whose
    // retry is offered only while that session stays selected.
    void run(const Write &write, OvermindModel::Callback after, const QString &sessionKey);
    void report(const Write &write, const OvermindModel::Result &result, const OvermindModel::Callback &after, const QString &sessionKey);
    // The question to ask before moving the task, empty when none is needed.
    QString moveQuestion(const QString &uuid, const QString &stage, QString *accept) const;
    void showCardMenu(const QString &uuid, const QPoint &global);
    // Shows the panel of the open card, if any, at its remembered width.
    void showPanel(const QString &uuid);
    // Shares the width between the board and the panel: the board keeps at
    // least a column; too narrow for both, the panel takes the board's place.
    void layoutPanel(bool opening);
    // Marks the open card's Your Call button.
    void markDecisions();
    enum class Tone { Info, Warning, Error };
    // Send anyway repeats `write`, labelled `label`; `sessionKey` as in run().
    struct Retry {
        Write write;
        OvermindModel::Callback after;
        QString label, sessionKey;
        bool isEmpty() const { return write.command.isEmpty(); }
    };
    // With a retry, Send anyway repeats it; with a task, Resolve delivery opens it.
    void showMessage(const QString &text, Tone tone, const Retry &retry = {}, const QString &resolveTask = {});
    void clearMessage();
    // Opens a card; revealing scrolls it into view (a click does not).
    void open(const QString &uuid, bool reveal);
    void selectCard(const QString &uuid, bool reveal);

    QPointer<OvermindModel> m_model;
    QList<SessionOrganization::Group> m_groups;   // accessible ones
    Selection m_selection;
    QString m_project, m_openTask;
    QHash<QString, QString> m_openTasks;          // the open card of each project
    QHash<QString, bool> m_collapsed;             // project + '\n' + stage, as chosen by a click
    QList<OvermindModel::Stage> m_stages;         // the columns shown (needs_human excluded)
    QList<TaskColumn *> m_columns;
    bool m_dark = false, m_opened = false, m_sending = false;
    // The project shown stands in for a stored one that is not offered; the
    // stored one returns once it is offered again.
    bool m_fallback = false;
    Palette m_palette;
    Retry m_retry;                                // what Send anyway repeats
    QString m_resolveTask;                        // the task Resolve delivery opens
    QList<Recipient> m_recipients;
    QHash<QString, int> m_delivering;             // delivering writes running, per task

    QComboBox *m_projects;
    QLineEdit *m_filter;
    QLabel *m_status;
    QFrame *m_messageBar;
    QLabel *m_message;
    QPushButton *m_sendAnyway, *m_resolve, *m_dismiss;
    QFrame *m_yourCall;
    QLabel *m_yourCallIcon, *m_yourCallTitle;
    QHBoxLayout *m_yourCallItems;
    QToolButton *m_yourCallMore;
    QFrame *m_sessionStrip;
    QLabel *m_sessionQueue;
    QPushButton *m_sendNext;
    QSplitter *m_split;
    QScrollArea *m_board;
    QHBoxLayout *m_boardLayout;
    TaskPanel *m_panel;
    TaskCardDelegate *m_delegate;
};
