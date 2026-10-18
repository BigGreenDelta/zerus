#include "TaskDialogs.h"
#include "ScopeFilterWidgets.h"
#include "TasksView.h"
#include "WorkspaceIcons.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
QString sessionLabel(const QString &host, const QString &name) { return host.isEmpty() ? name : name + QStringLiteral(" @") + host; }
QLabel *section(const QString &text) {
    auto *label = new QLabel(text); label->setProperty("taskSection", true);
    QFont font = label->font(); font.setPixelSize(11); font.setWeight(QFont::DemiBold); label->setFont(font);
    return label;
}
QLabel *errorLabel(const QString &name) {
    auto *label = new QLabel; label->setObjectName(name); label->setWordWrap(true); label->setTextFormat(Qt::PlainText);
    label->setStyleSheet(QStringLiteral("color:#e06c75;")); label->hide(); return label;
}
QString time(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("d MMM HH:mm")); }
QString actor(const QJsonObject &actor) {
    return actor.value("kind").toString() == QLatin1String("agent") ? actor.value("session").toString() : QObject::tr("You");
}
// One history line: who, when and what happened.
QString historyLine(const QJsonObject &entry) {
    const QString kind = entry.value("kind").toString(), text = entry.value("text").toString();
    QString what;
    if (kind == QLatin1String("created")) what = QObject::tr("created the task");
    else if (kind == QLatin1String("edited")) what = QObject::tr("edited the prompt");
    else if (kind == QLatin1String("stage")) what = QObject::tr("moved it to %1").arg(TaskBoard::stageLabel(text));
    else if (kind == QLatin1String("target")) what = QObject::tr("changed the session: %1").arg(text);
    else if (kind == QLatin1String("sent")) {
        const auto session = entry.value("session").toObject();
        what = QObject::tr("sent it to %1").arg(sessionLabel(session.value("host").toString(), session.value("session").toString()));
    } else if (kind == QLatin1String("report")) what = QObject::tr("reported: %1").arg(text);
    else if (kind == QLatin1String("cancelled")) what = QObject::tr("cancelled it");
    else if (kind == QLatin1String("comment")) what = QObject::tr("commented: %1").arg(text);
    else what = text.isEmpty() ? kind : text;
    return QStringLiteral("<p style='margin:0 0 6px 0'><b>%1</b> <span style='opacity:.7'>%2</span> · %3</p>")
        .arg(actor(entry.value("actor").toObject()).toHtmlEscaped(), time(qint64(entry.value("at").toDouble())), what.toHtmlEscaped());
}
}

TaskDialog::TaskDialog(const QJsonObject &task, const QList<TaskSession> &sessions, const QString &sendBlocker, bool dark, QWidget *parent)
    : QDialog(parent), m_task(TaskBoard::item(task)) {
    setObjectName("taskDialog"); setWindowTitle(tr("Task %1").arg(m_task.id)); resize(640, 640);
    m_editable = m_task.writable && (m_task.stage == QLatin1String("todo") || m_task.stage == QLatin1String("backlog"));
    m_target = {m_task.targetSession.isEmpty(), m_task.targetHost, m_task.targetSession};
    const auto colors = TaskColors::of(dark);
    auto *layout = new QVBoxLayout(this); layout->setSpacing(8);
    auto *header = new QHBoxLayout; header->setSpacing(10);
    auto *stage = new QLabel(TaskBoard::stageLabel(m_task.stage)); stage->setObjectName("taskDialogStage");
    stage->setStyleSheet(QStringLiteral("color:%1;border:1px solid %1;border-radius:9px;padding:1px 8px;font-size:11px;font-weight:600;")
        .arg(colors.stage(m_task.stage).name()));
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(m_task.id.toHtmlEscaped()));
    auto *meta = new QLabel((m_task.author.isEmpty() ? tr("You") : m_task.author) + QStringLiteral(" · ") + time(m_task.createdAt));
    meta->setStyleSheet(QStringLiteral("color:%1;font-size:11px;").arg(colors.muted.name()));
    header->addWidget(stage); header->addWidget(title); header->addWidget(meta); header->addStretch(); layout->addLayout(header);
    layout->addWidget(section(tr("Prompt")));
    m_prompt = new QPlainTextEdit(m_task.text); m_prompt->setObjectName("taskPrompt"); m_prompt->setReadOnly(!m_editable);
    m_prompt->setAccessibleName(tr("Task prompt")); layout->addWidget(m_prompt, 2);
    if (m_task.stage == QLatin1String("review") && !m_task.summary.isEmpty()) {
        auto *summary = new QLabel(QStringLiteral("<b>%1 reported:</b> %2").arg(sessionLabel(m_task.sessionHost, m_task.sessionName).toHtmlEscaped(),
            m_task.summary.toHtmlEscaped()));
        summary->setObjectName("taskDialogSummary"); summary->setWordWrap(true);
        summary->setStyleSheet(QStringLiteral("border-left:2px solid %1;padding:1px 0 1px 10px;").arg(colors.warn.name()));
        layout->addWidget(summary);
    }
    layout->addWidget(section(tr("Session")));
    if (m_editable) {
        // The sessions wrap onto rows and scroll past three, however many the project has.
        auto *area = new QScrollArea; area->setObjectName("taskSessionChoices"); area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame); area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); area->setMaximumHeight(104);
        auto *choices = new QWidget; auto *chips = new ScopeFilterWidgets::ChipFlow(choices); m_sessions = new QButtonGroup(this);
        m_choices << TaskTarget{true, {}, {}};
        for (const auto &session : sessions) m_choices << TaskTarget{false, session.host, session.name};
        // A pinned session that no longer exists stays a choice until it is changed.
        if (!m_target.any && !m_choices.contains(m_target)) m_choices << m_target;
        for (int i = 0; i < m_choices.size(); ++i) {
            const auto &choice = m_choices.at(i);
            auto *chip = new QPushButton(choice.any ? tr("Any session") : sessionLabel(choice.host, choice.session));
            chip->setObjectName(QStringLiteral("taskSession:") + (choice.any ? QStringLiteral("any") : choice.host + QLatin1Char('/') + choice.session));
            chip->setCheckable(true); chip->setChecked(choice == m_target); m_sessions->addButton(chip, i); chips->addWidget(chip);
            for (const auto &session : sessions)
                if (!choice.any && session.host == choice.host && session.name == choice.session) chip->setToolTip(session.status);
        }
        area->setWidget(choices); layout->addWidget(area);
    } else {
        auto *fixed = new QLabel(m_task.sessionName.isEmpty() ? (m_task.targetSession.isEmpty() ? tr("Any session")
            : sessionLabel(m_task.targetHost, m_task.targetSession)) : sessionLabel(m_task.sessionHost, m_task.sessionName));
        fixed->setObjectName("taskDialogSession"); layout->addWidget(fixed);
    }
    layout->addWidget(section(tr("History")));
    m_history = new QTextBrowser; m_history->setObjectName("taskHistory"); m_history->setOpenLinks(false); layout->addWidget(m_history, 1);
    setHistory(task.value("log").toArray());
    auto *comment = new QHBoxLayout;
    m_comment = new QLineEdit; m_comment->setObjectName("taskComment");
    m_comment->setPlaceholderText(tr("Comment — people and agents see it; it is not sent to the session"));
    auto *add = new QPushButton(tr("Add")); add->setObjectName("addTaskComment"); add->setAutoDefault(false);
    const auto commentNow = [this] { if (!m_comment->text().trimmed().isEmpty()) emit commentRequested(m_comment->text().trimmed()); };
    connect(add, &QPushButton::clicked, this, commentNow); connect(m_comment, &QLineEdit::returnPressed, this, commentNow);
    comment->addWidget(m_comment, 1); comment->addWidget(add); layout->addLayout(comment);
    m_error = errorLabel("taskDialogError"); layout->addWidget(m_error);
    auto *actions = new QHBoxLayout; actions->setSpacing(8);
    const auto stageActions = TaskBoard::actions(m_task.stage);
    for (const auto action : stageActions) {
        if (action == TaskAction::Edit) continue;
        auto *button = new QPushButton(workspaceIcon(TaskBoard::actionIcon(action), colors.muted), TaskBoard::actionLabel(action).remove(QChar(0x2026)));
        button->setObjectName(QStringLiteral("taskDialogAction:") + TaskRow::actionKey(action)); button->setAutoDefault(false);
        const QString why = !m_task.writable ? tr("Update Zerus to edit this task") : action == TaskAction::SendNow ? sendBlocker : QString();
        button->setEnabled(why.isEmpty()); button->setToolTip(why.isEmpty() ? TaskBoard::actionLabel(action) : why);
        if (action == stageActions.last()) button->setStyleSheet(QStringLiteral("color:%1;").arg(colors.accent.name()));
        button->setProperty("ready", why.isEmpty());
        connect(button, &QPushButton::clicked, this, [this, action] { setBusy(true); emit actionRequested(edits(), action); });
        actions->addWidget(button);
    }
    actions->addStretch();
    if (m_editable) {
        auto *cancel = new QPushButton(tr("Cancel")); cancel->setObjectName("discardTaskEdits"); cancel->setAutoDefault(false);
        auto *save = new QPushButton(tr("Save")); save->setObjectName("saveTask"); save->setDefault(true);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        save->setProperty("ready", true);
        connect(save, &QPushButton::clicked, this, [this] { setBusy(true); emit saveRequested(edits()); });
        actions->addWidget(cancel); actions->addWidget(save);
    } else {
        auto *close = new QPushButton(tr("Close")); close->setObjectName("closeTask"); close->setDefault(true);
        connect(close, &QPushButton::clicked, this, &QDialog::reject); actions->addWidget(close);
    }
    layout->addLayout(actions);
}

TaskEdits TaskDialog::edits() const {
    TaskEdits edits;
    if (!m_editable) return edits;
    if (const QString text = m_prompt->toPlainText(); text != m_task.text && !text.trimmed().isEmpty()) edits.text = text;
    if (m_sessions && m_sessions->checkedId() >= 0)
        if (const auto chosen = m_choices.value(m_sessions->checkedId()); !(chosen == m_target)) edits.target = chosen;
    return edits;
}

void TaskDialog::setHistory(const QJsonArray &log) {
    QString html;
    for (const auto &entry : log) html += historyLine(entry.toObject());
    m_history->setHtml(html);
}

void TaskDialog::clearComment() { m_comment->clear(); }

// While hgs works the buttons wait; a refusal shows here and gives them back.
void TaskDialog::setBusy(bool busy) {
    for (auto *button : findChildren<QPushButton *>())
        if (button->property("ready").isValid()) button->setEnabled(!busy && button->property("ready").toBool());
    if (busy) m_error->hide();
}

void TaskDialog::setError(const QString &error) { m_error->setText(error); m_error->show(); setBusy(false); }

TaskCancelDialog::TaskCancelDialog(const TaskItem &task, const QString &sessionStatus, bool running, QWidget *parent) : QDialog(parent) {
    setObjectName("taskCancelDialog"); setWindowTitle(tr("Cancel %1?").arg(task.id));
    auto *layout = new QVBoxLayout(this); layout->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Cancel %1?").arg(task.id).toHtmlEscaped())); layout->addWidget(title);
    auto *text = new QLabel(task.text); text->setObjectName("taskCancelText"); text->setWordWrap(true); text->setTextFormat(Qt::PlainText);
    text->setMaximumHeight(text->fontMetrics().lineSpacing() * 4 + 4); layout->addWidget(text);
    const QString session = sessionLabel(task.sessionHost, task.sessionName);
    QString what = tr("%1 moves to In review, and %2 gets the cancel message.").arg(task.id, session);
    if (!running) what += QLatin1Char(' ') + tr("The agent is not running, so hgs will refuse the message; the task is cancelled all the same.");
    else if (sessionStatus == QLatin1String("Working")) what += QLatin1Char(' ') + tr("%1 is working; it reads the message when its turn ends.").arg(session);
    auto *warning = new QLabel(what); warning->setObjectName("taskCancelWarning"); warning->setWordWrap(true); layout->addWidget(warning);
    layout->addWidget(section(tr("Reason (optional) — saved as a comment on the task, not sent to the agent")));
    m_reason = new QLineEdit; m_reason->setObjectName("taskCancelReason"); m_reason->setPlaceholderText(tr("Why it is cancelled"));
    layout->addWidget(m_reason);
    auto *buttons = new QHBoxLayout;
    auto *keep = new QPushButton(tr("Keep running")); keep->setObjectName("keepTaskRunning"); keep->setDefault(true);
    auto *cancel = new QPushButton(tr("Cancel task")); cancel->setObjectName("confirmTaskCancel"); cancel->setAutoDefault(false);
    cancel->setStyleSheet(QStringLiteral("color:#e06c75;"));
    connect(keep, &QPushButton::clicked, this, &QDialog::reject); connect(cancel, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(keep); buttons->addStretch(); buttons->addWidget(cancel); layout->addLayout(buttons);
}

QString TaskCancelDialog::reason() const { return m_reason->text().trimmed(); }

TaskSettingsDialog::TaskSettingsDialog(const QString &projectName, const QJsonObject &settings, QWidget *parent)
    : QDialog(parent), m_settings(settings) {
    setObjectName("taskSettingsDialog"); setWindowTitle(tr("Project tasks · %1").arg(projectName)); resize(620, 560);
    auto *layout = new QVBoxLayout(this); layout->setSpacing(8);
    layout->addWidget(section(tr("Task prompt (Send now)")));
    m_task = new QPlainTextEdit(settings.value("task_template").toString()); m_task->setObjectName("taskTemplate"); layout->addWidget(m_task, 1);
    layout->addWidget(section(tr("Cancel message")));
    m_cancel = new QPlainTextEdit(settings.value("cancel_template").toString()); m_cancel->setObjectName("cancelTemplate"); layout->addWidget(m_cancel, 1);
    auto *hint = new QLabel(tr("Placeholders: {ref}, {author}, {prompt}, {tools}, {history}")); hint->setObjectName("taskTemplateHint");
    layout->addWidget(hint);
    layout->addWidget(section(tr("Agents")));
    m_agentsOther = new QCheckBox(tr("Agents may start and cancel tasks in other sessions")); m_agentsOther->setObjectName("agentsOther");
    m_agentsOther->setChecked(settings.value("agents_other").toBool(true)); layout->addWidget(m_agentsOther);
    auto *limit = new QHBoxLayout;
    m_limit = new QSpinBox; m_limit->setObjectName("agentLimit"); m_limit->setRange(0, 1000); m_limit->setSpecialValueText(tr("no limit"));
    m_limit->setValue(settings.value("agent_limit").toInt());
    limit->addWidget(new QLabel(tr("Limit tasks started by agents"))); limit->addWidget(m_limit); limit->addWidget(new QLabel(tr("per hour")));
    limit->addStretch(); layout->addLayout(limit);
    m_error = errorLabel("taskSettingsError"); layout->addWidget(m_error);
    auto *buttons = new QHBoxLayout; buttons->addStretch();
    auto *cancel = new QPushButton(tr("Cancel")); cancel->setObjectName("cancelTaskSettings"); cancel->setAutoDefault(false);
    m_save = new QPushButton(tr("Save")); m_save->setObjectName("saveTaskSettings"); m_save->setDefault(true);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_save, &QPushButton::clicked, this, [this] { m_save->setEnabled(false); m_error->hide(); emit saveRequested(changes()); });
    buttons->addWidget(cancel); buttons->addWidget(m_save); layout->addLayout(buttons);
}

void TaskSettingsDialog::setError(const QString &error) { m_error->setText(error); m_error->show(); m_save->setEnabled(true); }

QJsonObject TaskSettingsDialog::changes() const {
    QJsonObject changes;
    const auto changed = [&](const char *key, const QJsonValue &value) { if (m_settings.value(key) != value) changes.insert(key, value); };
    changed("task_template", m_task->toPlainText()); changed("cancel_template", m_cancel->toPlainText());
    changed("agents_other", m_agentsOther->isChecked()); changed("agent_limit", m_limit->value());
    return changes;
}
