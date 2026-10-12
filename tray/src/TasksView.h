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
class QToolButton;
class TaskCardDelegate;
class TaskColumn;

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
// card stays where it was. Dragging inside a column runs `reorder`.
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
    void setTheme(bool dark);
    // A card dropped on `stage`, before the card `before` (empty: at the end).
    // Another column runs `move`, the same column `reorder`; a drop where the
    // card already is writes nothing.
    void dropTask(const QString &uuid, const QString &stage, const QString &before);

signals:
    // A card was opened: clicked on the board or in the Your Call strip.
    void taskOpened(const QString &uuid);
    // showProject asks the window to show this tab.
    void showRequested();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

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
    enum class Tone { Info, Warning, Error };
    // With a retry, Send anyway (labelled retryLabel) repeats that send.
    void showMessage(const QString &text, Tone tone, const QJsonObject &retry = {}, const QString &retryLabel = {});
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
    QJsonObject m_retry;                          // the send that Send anyway repeats
    QString m_retryKey;                           // the session key it was refused for

    QComboBox *m_projects;
    QLineEdit *m_filter;
    QLabel *m_status;
    QFrame *m_messageBar;
    QLabel *m_message;
    QPushButton *m_sendAnyway, *m_dismiss;
    QFrame *m_yourCall;
    QLabel *m_yourCallIcon, *m_yourCallTitle;
    QHBoxLayout *m_yourCallItems;
    QToolButton *m_yourCallMore;
    QFrame *m_sessionStrip;
    QLabel *m_sessionQueue;
    QPushButton *m_sendNext;
    QScrollArea *m_board;
    QHBoxLayout *m_boardLayout;
    TaskCardDelegate *m_delegate;
};
