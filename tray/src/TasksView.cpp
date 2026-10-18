#include "TasksView.h"
#include "HgsClient.h"
#include "TaskDialogs.h"
#include "MessageComposer.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include <QAbstractButton>
#include <QDateTime>
#include <QDrag>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QColor mix(const QColor &color, double share, const QColor &base) {
    return QColor::fromRgbF(base.redF() + (color.redF() - base.redF()) * share, base.greenF() + (color.greenF() - base.greenF()) * share,
                            base.blueF() + (color.blueF() - base.blueF()) * share);
}
QColor alpha(QColor color, double a) { color.setAlphaF(a); return color; }
QString sessionLabel(const QString &host, const QString &name) { return host.isEmpty() ? name : name + QStringLiteral(" @") + host; }
}

// A stage tile: its icon on a tinted circle, its name and its count.
class TaskTile : public QAbstractButton {
public:
    TaskTile(const QString &stage, QWidget *parent) : QAbstractButton(parent), m_stage(stage), m_colors(TaskColors::of(true)) {
        setObjectName(QStringLiteral("taskTile:") + stage); setCheckable(true); setFixedHeight(34); setMinimumWidth(72);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); setCursor(Qt::PointingHandCursor); setAttribute(Qt::WA_Hover);
        setToolTip(TaskBoard::stageLabel(stage)); setCount(0);
    }
    void setCount(int count) {
        setProperty("count", count); setAccessibleName(QStringLiteral("%1, %2").arg(TaskBoard::stageLabel(m_stage)).arg(count)); update();
    }
    void setColors(const TaskColors &colors) { m_colors = colors; update(); }
    // A dashed outline while a dragged task would move here.
    void setDropTarget(bool on) { if (property("dropTarget").toBool() != on) { setProperty("dropTarget", on); update(); } }
    QSize sizeHint() const override { return {120, 34}; }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QColor color = m_colors.stage(m_stage);
        const QColor fill = isChecked() ? mix(color, 0.17, m_colors.surface) : m_colors.surface;
        const QColor edge = isChecked() ? alpha(color, 0.45) : underMouse() ? m_colors.border : Qt::transparent;
        p.setPen(edge.alpha() ? QPen(edge, 1) : Qt::NoPen); p.setBrush(fill);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);
        if (property("dropTarget").toBool()) {
            p.setPen(QPen(color, 2, Qt::DashLine)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(rect()).adjusted(3, 3, -3, -3), 7, 7);
        }
        p.setPen(Qt::NoPen); p.setBrush(alpha(color, 0.18)); p.drawEllipse(QRectF(10, 6, 22, 22));
        workspaceIcon(TaskBoard::stageIcon(m_stage), color).paint(&p, QRect(14, 10, 14, 14));
        QFont count = font(); count.setPixelSize(15); count.setBold(true); p.setFont(count); p.setPen(m_colors.fg);
        const QString number = QString::number(property("count").toInt());
        const int numberWidth = QFontMetrics(count).horizontalAdvance(number);
        p.drawText(QRect(width() - 10 - numberWidth, 0, numberWidth, height()), Qt::AlignVCenter, number);
        QFont label = font(); label.setPixelSize(12); label.setWeight(QFont::DemiBold); p.setFont(label);
        p.setPen(isChecked() ? m_colors.fg : m_colors.muted);
        const QRect text(38, 0, qMax(0, width() - 38 - numberWidth - 16), height());
        p.drawText(text, Qt::AlignVCenter, QFontMetrics(label).elidedText(TaskBoard::stageLabel(m_stage), Qt::ElideRight, text.width()));
    }
private:
    QString m_stage;
    TaskColors m_colors;
};

TaskTabBadge::TaskTabBadge(QWidget *parent) : QWidget(parent) { setObjectName("taskTabBadge"); setCounts(0, 0); }

void TaskTabBadge::setCounts(int review, int todo) {
    m_review = review; m_todo = todo; setProperty("review", review); setProperty("todo", todo);
    setAccessibleName(tr("%1 to review, %2 to do").arg(review).arg(todo));
    // A tab button keeps the size it has; the owner relayouts the tab for it.
    resize(sizeHint()); setVisible(review > 0 || todo > 0); update();
}

void TaskTabBadge::setTheme(bool dark) { m_dark = dark; update(); }

QSize TaskTabBadge::sizeHint() const {
    QFont small = font(); small.setPixelSize(10); small.setBold(true); const QFontMetrics metrics(small);
    int width = 0;
    for (const int count : {m_review, m_todo}) if (count) width += qMax(16, metrics.horizontalAdvance(QString::number(count)) + 10) + 4;
    return {qMax(0, width), 16};
}

void TaskTabBadge::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    QFont small = font(); small.setPixelSize(10); small.setBold(true); p.setFont(small); const QFontMetrics metrics(small);
    int x = 0;
    const auto pill = [&](int count, const QColor &fill, const QColor &text) {
        if (!count) return;
        const QString number = QString::number(count); const int width = qMax(16, metrics.horizontalAdvance(number) + 10);
        p.setPen(Qt::NoPen); p.setBrush(fill); p.drawRoundedRect(QRectF(x, (height() - 16) / 2.0, width, 16), 8, 8);
        p.setPen(text); p.drawText(QRect(x, (height() - 16) / 2, width, 16), Qt::AlignCenter, number); x += width + 4;
    };
    pill(m_review, QColor(m_dark ? "#ffda76" : "#f4ce65"), QColor("#392900"));
    pill(m_todo, QColor(m_dark ? "#303942" : "#e5ebef"), QColor(m_dark ? "#a2adbc" : "#627082"));
}

TasksView::TasksView(HgsClient *client, QWidget *parent) : QWidget(parent), m_client(client) {
    setObjectName("tasksView");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(16, 12, 16, 0); layout->setSpacing(0);
    auto *tiles = new QHBoxLayout; tiles->setSpacing(8);
    for (const auto &stage : TaskBoard::stages()) {
        auto *tile = new TaskTile(stage, this); m_tiles.insert(stage, tile); tiles->addWidget(tile, 1);
        tile->setAcceptDrops(true); tile->installEventFilter(this);
        connect(tile, &QAbstractButton::clicked, this, [this, stage] { selectStage(stage); });
    }
    const auto square = [this](const QString &name) {
        auto *button = new QToolButton(this); button->setObjectName(name); button->setFixedSize(34, 34); button->setIconSize(QSize(17, 17));
        button->setCursor(Qt::PointingHandCursor); return button;
    };
    m_only = square("taskOnlySession"); m_only->setCheckable(true);
    m_only->setChecked(QSettings().value("tasks/onlySession").toBool());
    connect(m_only, &QToolButton::toggled, this, [this](bool only) {
        QSettings().setValue("tasks/onlySession", only);
        const auto colors = TaskColors::of(m_dark); m_only->setIcon(workspaceIcon(QStringLiteral("task-funnel"), only ? colors.accent : colors.muted));
        render();
    });
    m_settings = square("taskSettings");
    connect(m_settings, &QToolButton::clicked, this, &TasksView::openSettings);
    tiles->addWidget(m_only); tiles->addWidget(m_settings);
    layout->addLayout(tiles); layout->addSpacing(14);
    m_errorLabel = new QLabel; m_errorLabel->setObjectName("tasksError"); m_errorLabel->setWordWrap(true); m_errorLabel->setTextFormat(Qt::PlainText);
    m_errorLabel->hide(); layout->addWidget(m_errorLabel);
    m_damaged = new QLabel; m_damaged->setObjectName("tasksDamaged"); m_damaged->setWordWrap(true); m_damaged->setTextFormat(Qt::PlainText);
    m_damaged->hide(); layout->addWidget(m_damaged);
    m_scroll = new QScrollArea; m_scroll->setObjectName("taskList"); m_scroll->setWidgetResizable(true); m_scroll->setFrameShape(QFrame::NoFrame);
    // The list's surface holds only its rows; the spare height below stays the page's.
    auto *page = new QWidget; page->setObjectName("taskListPage");
    auto *pageLayout = new QVBoxLayout(page); pageLayout->setContentsMargins(0, 0, 0, 0); pageLayout->setSpacing(0);
    m_listHost = new QWidget; m_listHost->setObjectName("taskListHost"); pageLayout->addWidget(m_listHost); pageLayout->addStretch();
    m_list = new QVBoxLayout(m_listHost); m_list->setContentsMargins(0, 0, 0, 0); m_list->setSpacing(0);
    m_empty = new QLabel; m_empty->setObjectName("taskEmpty"); m_empty->setAlignment(Qt::AlignCenter); m_empty->setMargin(18);
    m_list->addWidget(m_empty);
    m_scroll->setWidget(page); layout->addWidget(m_scroll, 1);
    m_older = new QPushButton; m_older->setObjectName("showOlderTasks"); m_older->setFlat(true); m_older->setCursor(Qt::PointingHandCursor); m_older->hide();
    connect(m_older, &QPushButton::clicked, this, [this] { m_showOlder = !m_showOlder; render(); });
    layout->addWidget(m_older, 0, Qt::AlignLeft);
    m_composer = new MessageComposer(this); m_composer->setObjectName("taskComposer"); m_composer->setTaskMode(true);
    connect(m_composer, &MessageComposer::sendRequested, this, [this](const QString &key, const QString &text) { addTask(key, text); });
    layout->addWidget(m_composer);
    connect(m_client, &HgsClient::taskReady, this, [this](quint64 id, const QJsonObject &result) {
        const auto pending = m_pending.take(id); if (pending.first) pending.first(result);
    });
    connect(m_client, &HgsClient::taskFailed, this, [this](quint64 id, const QString &error) {
        if (!m_pending.contains(id)) return;
        const auto pending = m_pending.take(id); if (pending.second) pending.second(error);
    });
    m_timer.setSingleShot(true); connect(&m_timer, &QTimer::timeout, this, &TasksView::refresh);
    setTheme(true);
}

void TasksView::setContext(const QString &projectId, const QString &projectName, const QString &host, const QString &session,
                           const QString &sessionKey, const QList<TaskSession> &sessions) {
    const bool project = projectId != m_projectId;
    // The window passes the context on every update; only a change is drawn again.
    if (!project && projectName == m_projectName && host == m_host && session == m_session && sessionKey == m_sessionKey && sessions == m_sessions) return;
    m_projectName = projectName; m_host = host; m_session = session; m_sessions = sessions;
    if (project) {
        m_projectId = projectId; ++m_generation; m_board = {}; m_loaded = false; m_failing = false; m_error.clear(); m_showOlder = false;
        m_stage = QStringLiteral("todo");
        for (auto *row : std::as_const(m_rows)) { row->hide(); row->setObjectName({}); row->deleteLater(); }
        m_rows.clear();
        emit countsChanged(0, 0);   // until the new project's listing arrives
    }
    m_sessionKey = sessionKey; applyComposerKey();
    m_composer->setAvailability(!m_projectId.isEmpty() && !m_session.isEmpty(), m_session.isEmpty() ? tr("Open a session to add tasks for it.")
        : tr("This session is in no project; tasks belong to a project."));
    render();
    if (project) refresh();
}

void TasksView::setPolling(bool polling) {
    m_polling = polling;
    if (!polling) m_timer.stop(); else schedule();
}

void TasksView::setRefreshIntervals(int visibleMs, int hiddenMs) { m_visibleMs = visibleMs; m_hiddenMs = hiddenMs; schedule(); }

void TasksView::showEvent(QShowEvent *event) { QWidget::showEvent(event); applyComposerKey(); if (m_loaded) refresh(); }

void TasksView::hideEvent(QHideEvent *event) { QWidget::hideEvent(event); applyComposerKey(); schedule(); }

// The input holds the open session's task draft only while the tab shows: a hidden
// tab keeps no enabled field in the window.
void TasksView::applyComposerKey() {
    const QString key = isVisible() && !m_sessionKey.isEmpty() ? QStringLiteral("task-draft\n") + m_sessionKey : QString();
    if (key != m_composer->sessionKey()) m_composer->setSessionKey(key);
}

// A project hgs cannot list is retried at the slow pace until a listing succeeds.
void TasksView::schedule() {
    if (m_polling && !m_projectId.isEmpty() && !m_listing) m_timer.start(isVisible() && !m_failing ? m_visibleMs : m_hiddenMs);
}

QStringList TasksView::withProject(QStringList arguments) const { return arguments << QStringLiteral("--project") << m_projectId; }

void TasksView::request(const QStringList &arguments, const QString &input, std::function<void(const QJsonObject &)> done,
                        std::function<void(const QString &)> failed) {
    if (!failed) failed = [this](const QString &error) { emit notice(error, true); refresh(); };
    m_pending.insert(m_client->requestTask(arguments, input), {std::move(done), std::move(failed)});
}

void TasksView::refresh() {
    m_timer.stop();
    if (m_projectId.isEmpty() || m_listing) return;
    const auto generation = m_generation; m_listing = generation + 1;
    request({QStringLiteral("list"), QStringLiteral("--project"), m_projectId}, {},
        [this, generation](const QJsonObject &listing) { m_listing = 0; received(listing, generation); },
        [this, generation](const QString &error) {
            m_listing = 0;
            if (generation != m_generation) return refresh();
            // What is not readable now is not shown as current: no rows, no counts.
            m_error = error; m_failing = true; m_board = {}; render(); emit countsChanged(0, 0); schedule();
        });
}

void TasksView::received(const QJsonObject &listing, quint64 generation) {
    if (generation != m_generation) return refresh();   // the project changed meanwhile
    m_error.clear(); m_failing = false; m_board = TaskBoard::fromListing(listing);
    if (!m_loaded) { m_loaded = true; m_stage = m_board.count("review", view()) ? QStringLiteral("review") : QStringLiteral("todo"); }
    render();
    int review = 0, todo = 0;
    for (const auto &task : std::as_const(m_board.tasks)) { review += task.stage == QLatin1String("review"); todo += task.stage == QLatin1String("todo"); }
    emit countsChanged(review, todo);
    schedule();
}

TaskBoard::View TasksView::view() const { return {m_host, m_session, m_only->isChecked(), QDateTime::currentMSecsSinceEpoch()}; }

const TaskSession *TasksView::session(const QString &host, const QString &name) const {
    for (const auto &session : m_sessions) if (session.host == host && session.name == name) return &session;
    return nullptr;
}

// Why Send now cannot paste into the session the task would go to, as hgs would say it.
QString TasksView::sendBlocker(const TaskItem &task) const {
    const bool any = task.targetSession.isEmpty();
    const QString host = any ? m_host : task.targetHost, name = any ? m_session : task.targetSession;
    if (name.isEmpty()) return tr("Open the session to send this task to");
    if (!any && !task.targetKnown) return tr("%1 is on a computer this one has not verified").arg(name);
    const auto *found = session(host, name);
    if (!found) return tr("%1 no longer exists — choose another session").arg(name);
    if (!host.isEmpty()) return tr("%1 runs on another computer; sending there comes with replication").arg(name);
    return found->blocker;
}

void TasksView::selectStage(const QString &stage) {
    if (stage != m_stage) m_showOlder = false;
    m_stage = stage; render();
}

void TasksView::render() {
    const auto v = view(); const auto colors = TaskColors::of(m_dark);
    for (const auto &stage : TaskBoard::stages()) {
        auto *tile = m_tiles.value(stage); tile->setCount(m_board.count(stage, v)); tile->setChecked(stage == m_stage);
    }
    m_errorLabel->setText(tr("Tasks could not be read: %1").arg(m_error)); m_errorLabel->setVisible(!m_error.isEmpty());
    m_damaged->setText(m_board.damaged == 1 ? tr("1 task file of %1 could not be read.").arg(m_projectName)
                                            : tr("%1 task files of %2 could not be read.").arg(m_board.damaged).arg(m_projectName));
    m_damaged->setVisible(m_board.damaged > 0 && m_error.isEmpty());
    const bool done = m_stage == QLatin1String("done");
    QList<TaskItem> rows = m_error.isEmpty() ? m_board.rows(m_stage, v) : QList<TaskItem>();
    const auto older = done && m_error.isEmpty() ? m_board.rows(m_stage, v, true) : QList<TaskItem>();
    if (m_showOlder) rows += older;
    QSet<QString> shown; int index = 0;
    for (const auto &task : std::as_const(rows)) {
        auto *row = m_rows.value(task.uuid);
        if (!row) {
            row = new TaskRow(m_listHost); m_rows.insert(task.uuid, row);
            connect(row, &TaskRow::actionRequested, this, [this, row](TaskAction action) { act(row->task(), action); });
            connect(row, &TaskRow::opened, this, [this, row] { openTask(row->task()); });
            connect(row, &TaskRow::dragStarted, this, [this, row](const QPoint &hotSpot) { startDrag(row, hotSpot); });
        }
        row->setObjectName(QStringLiteral("taskRow:") + task.id);
        TaskRowState state; state.mine = TaskBoard::belongsTo(task, m_host, m_session); state.anyNow = sessionLabel(m_host, m_session);
        if (const auto *ran = session(task.sessionHost, task.sessionName); ran && task.stage == QLatin1String("active")) state.sessionStatus = ran->status;
        if (TaskBoard::actions(task.stage).contains(TaskAction::SendNow))
            if (const auto why = sendBlocker(task); !why.isEmpty()) state.blocked.insert(int(TaskAction::SendNow), why);
        if (!task.writable)
            for (const auto action : TaskBoard::actions(task.stage)) if (action != TaskAction::Edit) state.blocked.insert(int(action), tr("Update Zerus to edit this task"));
        row->setProperty("separated", index > 0);
        row->setTask(task, state, colors, v.now);
        m_list->removeWidget(row); m_list->insertWidget(index++, row); row->show(); shown.insert(task.uuid);
    }
    for (auto it = m_rows.begin(); it != m_rows.end();) {
        if (shown.contains(it.key())) { ++it; continue; }
        it.value()->hide(); it.value()->setObjectName({}); it.value()->deleteLater(); it = m_rows.erase(it);
    }
    const int hidden = m_board.rows(m_stage, {m_host, m_session, false, v.now}).size() - m_board.rows(m_stage, v).size();
    const QString empty = v.only && hidden > 0 ? tr("No tasks of %1 here").arg(sessionLabel(m_host, m_session))
        : m_stage == QLatin1String("review") ? tr("Nothing to review") : m_stage == QLatin1String("active") ? tr("No session is running a task")
        : m_stage == QLatin1String("todo") ? tr("Empty — write a prompt in the input below") : m_stage == QLatin1String("backlog") ? tr("Empty")
        : tr("Nothing yet");
    m_empty->setText(m_projectId.isEmpty() && !m_session.isEmpty() ? tr("Tasks belong to a project. Move this session into one to keep tasks for it.") : empty);
    m_empty->setVisible(rows.isEmpty() && m_error.isEmpty() && (!m_projectId.isEmpty() || !m_session.isEmpty()));
    m_older->setText(m_showOlder ? tr("Hide older") : tr("Show older (%1)").arg(older.size())); m_older->setVisible(!older.isEmpty());
    m_only->setToolTip(m_only->isChecked() ? tr("Show the tasks of every session of %1").arg(m_projectName)
                                           : tr("Show only the tasks of %1").arg(sessionLabel(m_host, m_session)));
    m_only->setAccessibleName(m_only->toolTip());
    m_settings->setToolTip(tr("Task settings for %1").arg(m_projectName)); m_settings->setAccessibleName(m_settings->toolTip());
    updateComposer();
}

void TasksView::updateComposer() {
    const QString target = m_stage == QLatin1String("backlog") ? tr("Backlog") : tr("To do");
    const QString session = sessionLabel(m_host, m_session);
    m_composer->setSendLabel(target);
    m_composer->setPlaceholder(m_session.isEmpty() ? tr("Task prompt…") : tr("Task prompt for %1 / %2…").arg(m_projectName, session));
    m_composer->setSessionStatus(m_session.isEmpty() || m_projectId.isEmpty() ? QString()
        : tr("Task for %1 · Enter adds to %2 · Shift+Enter for a new line").arg(session, target));
}

QList<TasksView::Step> TasksView::steps(const TaskItem &task, TaskAction action) const {
    switch (action) {
    case TaskAction::ToBacklog: return {{withProject({"stage", task.uuid, "backlog"}), {}}};
    case TaskAction::Top: return {{withProject({"move", task.uuid, "--top"}), {}}};
    case TaskAction::ToTodo: case TaskAction::BackToTodo: return {{withProject({"stage", task.uuid, "todo"}), {}}};
    case TaskAction::Accept: return {{withProject({"stage", task.uuid, "done"}), {}}};
    case TaskAction::SendNow: {
        // An Any session task goes to the open session.
        QStringList arguments{"send", task.uuid};
        if (task.targetSession.isEmpty()) arguments << "--session" << m_session;
        return {{withProject(arguments), {}}};
    }
    case TaskAction::Edit: case TaskAction::Cancel: break;
    }
    return {};
}

QList<TasksView::Step> TasksView::editSteps(const TaskItem &task, const TaskEdits &edits) const {
    QList<Step> out;
    if (edits.text) out << Step{withProject({"edit", task.uuid}), *edits.text};
    if (edits.target) {
        QStringList arguments{"target", task.uuid, edits.target->any ? QStringLiteral("any") : edits.target->session};
        if (!edits.target->any && !edits.target->host.isEmpty()) arguments << "--host" << edits.target->host;
        out << Step{withProject(arguments), {}};
    }
    return out;
}

void TasksView::run(QList<Step> steps, std::function<void()> finished, std::function<void(const QString &)> failed) {
    if (steps.isEmpty()) { refresh(); if (finished) finished(); return; }
    auto step = steps.takeFirst();
    request(step.arguments, step.input, [this, steps, then = step.then, finished, failed](const QJsonObject &reply) {
        if (then) then(reply);
        run(steps, finished, failed);
    }, failed ? std::function<void(const QString &)>([this, failed](const QString &error) { failed(error); refresh(); })
              : std::function<void(const QString &)>());
}

void TasksView::act(const TaskItem &task, TaskAction action) {
    if (action == TaskAction::Edit) return openTask(task);
    if (action == TaskAction::Cancel) return confirmCancel(task);
    if (action == TaskAction::SendNow && !sendBlocker(task).isEmpty()) return;
    run(steps(task, action));
}

void TasksView::addTask(const QString &key, const QString &text) {
    if (key != m_composer->sessionKey() || m_projectId.isEmpty() || m_session.isEmpty()) return;
    const QString stage = m_stage == QLatin1String("backlog") ? QStringLiteral("backlog") : QStringLiteral("todo");
    QStringList arguments{"add", "--project", m_projectId, "--target", m_session};
    if (!m_host.isEmpty()) arguments << "--host" << m_host;
    if (stage == QLatin1String("backlog")) arguments << "--backlog";
    m_composer->prepareSendingAsync(key, false, [this, key, arguments, text, stage](bool ready) {
        if (!ready) return;
        request(arguments, text,
            [this, key, stage](const QJsonObject &) { m_composer->deliveryFinished(key, true); selectStage(stage); refresh(); },
            [this, key](const QString &error) { m_composer->deliveryFinished(key, false, error); emit notice(error, true); });
    });
}

void TasksView::startDrag(TaskRow *row, const QPoint &hotSpot) {
    if (m_projectId.isEmpty()) return;
    // A listing may replace the row while the drag runs; the drag carries the task.
    QPointer<TaskRow> source(row);
    auto *drag = new QDrag(this);
    drag->setMimeData(TaskDrag::of(m_projectId, row->task()).mimeData());
    drag->setPixmap(row->grab()); drag->setHotSpot(hotSpot);
    row->setDragging(true);
    drag->exec(Qt::MoveAction);
    if (source) source->setDragging(false);
}

bool TasksView::takesOnTile(const TaskDrag &drag, const QString &stage) const {
    const auto list = [](const QString &s) { return s == QLatin1String("todo") || s == QLatin1String("backlog"); };
    return drag.projectId == m_projectId && list(stage) && list(drag.stage) && drag.stage != stage;
}

bool TasksView::eventFilter(QObject *watched, QEvent *event) {
    const auto type = event->type();
    if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::DragLeave && type != QEvent::Drop)
        return QWidget::eventFilter(watched, event);
    for (auto it = m_tiles.cbegin(); it != m_tiles.cend(); ++it) {
        if (it.value() != watched) continue;
        auto *tile = it.value();
        if (type == QEvent::DragLeave) { tile->setDropTarget(false); return true; }
        auto *drop = static_cast<QDropEvent *>(event);
        const auto drag = TaskDrag::from(drop->mimeData());
        const bool takes = drag && takesOnTile(*drag, it.key());
        tile->setDropTarget(takes && type != QEvent::Drop);
        if (!takes) { drop->ignore(); return true; }
        drop->setDropAction(Qt::MoveAction); drop->accept();
        if (type == QEvent::Drop) run({{withProject({"stage", drag->uuid, it.key()}), {}}});
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void TasksView::openTask(const TaskItem &task) {
    request(withProject({"get", task.uuid}), {}, [this, task](const QJsonObject &reply) {
        for (auto *open : findChildren<TaskDialog *>()) open->reject();
        // Send now follows the session chosen in the dialog.
        const auto blocker = [this, task](const TaskTarget &target) {
            if (target == TaskTarget{task.targetSession.isEmpty(), task.targetHost, task.targetSession}) return sendBlocker(task);
            auto chosen = task; chosen.targetSession = target.any ? QString() : target.session; chosen.targetHost = target.host; chosen.targetKnown = true;
            return sendBlocker(chosen);
        };
        auto *dialog = new TaskDialog(reply.value("task").toObject(), m_sessions, blocker, m_dark, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        // The dialog closes once hgs took everything; a refusal keeps it, and the edits, open.
        QPointer<TaskDialog> guard(dialog);
        const auto close = [guard] { if (guard) guard->accept(); };
        const auto refused = [this, guard](const QString &error) { if (guard) guard->setError(error); else emit notice(error, true); };
        connect(dialog, &TaskDialog::saveRequested, this, [this, task, close, refused](const TaskEdits &edits) { run(editSteps(task, edits), close, refused); });
        connect(dialog, &TaskDialog::actionRequested, this, [this, task, close, refused](const TaskEdits &edits, TaskAction action) {
            // The action meets the task as the edits leave it.
            auto edited = task;
            if (edits.target) { edited.targetSession = edits.target->any ? QString() : edits.target->session; edited.targetHost = edits.target->host; }
            if (action == TaskAction::Cancel) return run(editSteps(task, edits), [this, close, edited] { close(); confirmCancel(edited); }, refused);
            run(editSteps(task, edits) + steps(edited, action), close, refused);
        });
        connect(dialog, &TaskDialog::commentRequested, this, [this, guard, uuid = task.uuid](const QString &text) {
            request(withProject({"comment", uuid}), text, [guard](const QJsonObject &reply) {
                if (!guard) return;
                guard->clearComment(); guard->setHistory(reply.value("task").toObject().value("log").toArray());
            });
        });
        dialog->open();
    });
}

void TasksView::confirmCancel(const TaskItem &task) {
    const auto *ran = session(task.sessionHost, task.sessionName);
    auto *dialog = new TaskCancelDialog(task, ran && ran->running, ran && ran->working, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::accepted, this, [this, dialog, task] {
        QStringList arguments{"cancel", task.uuid};
        if (!dialog->reason().isEmpty()) arguments << "--reason" << dialog->reason();
        run({{withProject(arguments), {}, [this, task](const QJsonObject &reply) {
            // The task is cancelled even when hgs refused the cancel message.
            const auto message = reply.value("cancel_message").toObject();
            if (!message.value("sent").toBool(true))
                emit notice(tr("%1 is cancelled; its session did not get the cancel message: %2").arg(task.id, message.value("error").toString()), true);
        }}});
    });
    dialog->open();
}

void TasksView::openSettings() {
    if (m_projectId.isEmpty()) return;
    request({"settings", "--project", m_projectId}, {}, [this](const QJsonObject &reply) {
        auto *dialog = new TaskSettingsDialog(m_projectName, reply.value("settings").toObject(), this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<TaskSettingsDialog> guard(dialog);
        connect(dialog, &TaskSettingsDialog::saveRequested, this, [this, guard](const QJsonObject &changes) {
            if (changes.isEmpty()) { if (guard) guard->accept(); return; }
            request({"settings", "--project", m_projectId, "--set"}, QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact)),
                [guard](const QJsonObject &) { if (guard) guard->accept(); },
                [this, guard](const QString &error) { if (guard) guard->setError(error); else emit notice(error, true); });
        });
        dialog->open();
    });
}

void TasksView::setTheme(bool dark) {
    m_dark = dark; const auto colors = TaskColors::of(dark);
    for (auto *tile : std::as_const(m_tiles)) tile->setColors(colors);
    for (auto *button : {m_only, m_settings}) {
        const bool on = button->isChecked();
        button->setIcon(workspaceIcon(button == m_only ? QStringLiteral("task-funnel") : QStringLiteral("settings"), on ? colors.accent : colors.muted));
        setWorkspaceStyle(button, QStringLiteral("QToolButton{border:1px solid %1;border-radius:9px;background:transparent;}"
            "QToolButton:hover{border-color:%2;}QToolButton:checked{border-color:%3;background:%4;}")
            .arg(colors.border.name(), colors.muted.name(), alpha(colors.accent, 0.5).name(QColor::HexArgb), alpha(colors.accent, 0.12).name(QColor::HexArgb)));
    }
    setWorkspaceStyle(m_scroll, QStringLiteral("QScrollArea,#taskListPage{background:transparent;}") + workspaceScrollbars(dark));
    setWorkspaceStyle(m_listHost, QStringLiteral("#taskListHost{background:%1;border-radius:10px;}").arg(colors.surface.name()));
    setWorkspaceStyle(m_empty, QStringLiteral("color:%1;").arg(colors.muted.name()));
    setWorkspaceStyle(m_errorLabel, QStringLiteral("color:%1;padding:0 0 10px 0;").arg(dark ? "#f4ab9b" : "#a13224"));
    setWorkspaceStyle(m_damaged, QStringLiteral("color:%1;padding:0 0 10px 0;").arg(colors.warn.name()));
    setWorkspaceStyle(m_older, QStringLiteral("QPushButton{border:0;color:%1;padding:6px 2px;font-size:12px;}").arg(colors.accent.name()));
    m_composer->setTheme(dark);
    render();
}
