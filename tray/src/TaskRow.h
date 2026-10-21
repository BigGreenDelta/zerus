#pragma once
#include "TaskBoard.h"
#include <QColor>
#include <QHash>
#include <QWidget>

class QLabel;
class QToolButton;

// The Tasks tab's colours, from design/task-tracker/index.html.
struct TaskColors {
    QColor fg, muted, surface, border, accent, hover, warn, blue, violet;
    static TaskColors of(bool dark);
    QColor stage(const QString &stage) const;
};

// What a row shows beyond the task itself.
struct TaskRowState {
    bool mine = false;               // the open session's task: accent tint and bar
    QString sessionStatus;           // In progress: the session's status label
    QHash<int, QString> blocked;     // TaskAction → why it is unavailable now
    QString anyNow;                  // To do and Backlog: the session an Any task goes to now
};

// One task in the list: a fixed-height header with the meta on the left and the
// stage's action icons on the right (shown on hover), then the text clamped to
// two lines, and for In review the report or the cancellation.
class TaskRow : public QWidget {
    Q_OBJECT
public:
    explicit TaskRow(QWidget *parent = nullptr);
    void setTask(const TaskItem &task, const TaskRowState &state, const TaskColors &colors, qint64 now);
    const TaskItem &task() const { return m_task; }
    static QString actionKey(TaskAction action);
    // While its drag runs the row stays in its place, faded.
    void setDragging(bool dragging);
signals:
    void actionRequested(TaskAction action);
    // A blocked action was clicked: it does nothing, and the owner says why.
    void actionBlocked(const QString &reason);
    void opened();
    // A To do or Backlog row was pressed and moved: the owner drags it from hotSpot.
    void dragStarted(const QPoint &hotSpot);
protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void showActions(bool shown);
    // The actions show while the pointer is over the row or the focus is in it.
    void updateActions();
    void layoutText();
    TaskItem m_task;
    TaskRowState m_state;
    TaskColors m_colors;
    QLabel *m_glyph, *m_meta, *m_text, *m_summary;
    QWidget *m_actions;
    QHash<int, QToolButton *> m_buttons;
    QPoint m_pressAt;
    bool m_hovered = false, m_pressed = false;
};
