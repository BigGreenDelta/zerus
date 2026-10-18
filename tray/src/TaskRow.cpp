#include "TaskRow.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

TaskColors TaskColors::of(bool dark) {
    if (dark) return {QColor("#e8edf4"), QColor("#9eabba"), QColor("#242d36"), QColor("#34404a"), QColor("#8bdfc0"),
                      QColor("#2b3540"), QColor("#edbd77"), QColor("#98bdeb"), QColor("#c5a8f5")};
    return {QColor("#1a2733"), QColor("#657487"), QColor("#f3f6f8"), QColor("#dbe3e9"), QColor("#167357"),
            QColor("#edf2f5"), QColor("#9b6216"), QColor("#366ba9"), QColor("#6c43b8")};
}

QColor TaskColors::stage(const QString &stage) const {
    if (stage == QLatin1String("review")) return warn;
    if (stage == QLatin1String("active")) return accent;
    if (stage == QLatin1String("todo")) return blue;
    if (stage == QLatin1String("done")) return violet;
    return muted;
}

namespace {
QString sessionLabel(const QString &host, const QString &name) {
    return host.isEmpty() ? name : name + QStringLiteral(" @") + host;
}
QString age(qint64 ms) {
    const qint64 minutes = qMax<qint64>(0, ms / 60000);
    if (minutes < 60) return QStringLiteral("%1 min").arg(minutes);
    if (minutes < 24 * 60) return QStringLiteral("%1 h").arg(minutes / 60);
    return QStringLiteral("%1 d").arg(minutes / (24 * 60));
}
// The text as at most `lines` lines of `width`, the last one elided.
QString clamp(const QString &text, const QFont &font, int width, int lines) {
    if (width <= 0) return text;
    QTextLayout layout(text, font); layout.beginLayout();
    QStringList out;
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        if (int(out.size()) == lines - 1) {
            const QString rest = text.mid(line.textStart()).simplified();
            out << QFontMetrics(font).elidedText(rest, Qt::ElideRight, width); break;
        }
        out << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return out.join(QLatin1Char('\n'));
}
}

QString TaskRow::actionKey(TaskAction action) {
    switch (action) {
    case TaskAction::Edit: return QStringLiteral("edit");
    case TaskAction::ToBacklog: return QStringLiteral("toBacklog");
    case TaskAction::Top: return QStringLiteral("top");
    case TaskAction::SendNow: return QStringLiteral("sendNow");
    case TaskAction::ToTodo: return QStringLiteral("toTodo");
    case TaskAction::Cancel: return QStringLiteral("cancel");
    case TaskAction::BackToTodo: return QStringLiteral("backToTodo");
    case TaskAction::Accept: return QStringLiteral("accept");
    }
    return {};
}

TaskRow::TaskRow(QWidget *parent) : QWidget(parent), m_colors(TaskColors::of(true)) {
    setAttribute(Qt::WA_Hover); setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);   // rows never take the list's spare height
    auto *layout = new QHBoxLayout(this); layout->setContentsMargins(12, 6, 12, 10); layout->setSpacing(11);
    m_glyph = new QLabel; m_glyph->setObjectName("taskGlyph"); m_glyph->setFixedSize(18, 18); m_glyph->setAlignment(Qt::AlignCenter);
    auto *glyphColumn = new QVBoxLayout; glyphColumn->setContentsMargins(0, 4, 0, 0); glyphColumn->addWidget(m_glyph); glyphColumn->addStretch();
    layout->addLayout(glyphColumn);
    auto *main = new QVBoxLayout; main->setContentsMargins(0, 0, 0, 0); main->setSpacing(0); layout->addLayout(main, 1);
    // The header's height is fixed, so showing the icons never moves anything.
    auto *header = new QWidget; header->setFixedHeight(26);
    auto *headerLayout = new QHBoxLayout(header); headerLayout->setContentsMargins(0, 0, 0, 0); headerLayout->setSpacing(8);
    m_meta = new QLabel; m_meta->setObjectName("taskMeta"); m_meta->setTextFormat(Qt::RichText);
    m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_actions = new QWidget; m_actions->setObjectName("taskActions");
    auto *actionsLayout = new QHBoxLayout(m_actions); actionsLayout->setContentsMargins(0, 0, 0, 0); actionsLayout->setSpacing(2);
    headerLayout->addWidget(m_meta, 1); headerLayout->addWidget(m_actions);
    main->addWidget(header);
    m_text = new QLabel; m_text->setObjectName("taskText"); m_text->setTextFormat(Qt::PlainText);
    m_text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    main->addWidget(m_text);
    m_summary = new QLabel; m_summary->setObjectName("taskSummary"); m_summary->setTextFormat(Qt::RichText); m_summary->setWordWrap(true);
    main->addSpacing(2); main->addWidget(m_summary); m_summary->hide();
}

void TaskRow::setTask(const TaskItem &task, const TaskRowState &state, const TaskColors &colors, qint64 now) {
    const bool rebuild = task.stage != m_task.stage || m_buttons.isEmpty();
    m_task = task; m_state = state; m_colors = colors;
    setProperty("mine", state.mine);
    const QColor stage = colors.stage(task.stage);
    const QString glyph = task.stage == QLatin1String("todo") ? QString::number(task.queue)
        : task.stage == QLatin1String("review") || task.stage == QLatin1String("done") ? QStringLiteral("✓")
        : task.stage == QLatin1String("active") ? QStringLiteral("•") : QString();
    m_glyph->setText(glyph);
    setWorkspaceStyle(m_glyph, QStringLiteral("border:2px solid %1;border-radius:9px;color:%1;font-size:9.5px;font-weight:700;").arg(stage.name()));
    // Meta: ID, the session (or the target), the session's status for In
    // progress, the time, and who filed it.
    const auto who = [&](const QString &text) {
        return state.mine ? QStringLiteral("<span style='color:%1;font-weight:600'>%2</span>").arg(colors.accent.name(), text.toHtmlEscaped())
                          : text.toHtmlEscaped();
    };
    QStringList meta{QStringLiteral("<span style='font-family:monospace;font-size:10.5px'>%1</span>").arg(task.id.toHtmlEscaped())};
    if (task.stage == QLatin1String("todo") || task.stage == QLatin1String("backlog")) {
        meta << (task.targetSession.isEmpty()
            ? QStringLiteral("→ any session") + (state.anyNow.isEmpty() ? QString() : QStringLiteral(" · now ") + state.anyNow.toHtmlEscaped())
            : who(QStringLiteral("→ ") + sessionLabel(task.targetHost, task.targetSession)));
    } else if (!task.sessionName.isEmpty()) {
        meta << who(sessionLabel(task.sessionHost, task.sessionName));
    }
    if (task.stage == QLatin1String("active")) {
        if (!state.sessionStatus.isEmpty()) meta << state.sessionStatus.toHtmlEscaped();
        meta << age(now - task.stageAt);
    }
    if (task.stage == QLatin1String("done") && now - task.stageAt > TaskBoard::RecentDoneMs)
        meta << QStringLiteral("accepted ") + QDateTime::fromMSecsSinceEpoch(task.stageAt).toString(QStringLiteral("d MMM"));
    if (!task.author.isEmpty())
        meta << QStringLiteral("<span style='color:%1'>filed by %2</span>").arg(colors.violet.name(), task.author.toHtmlEscaped());
    m_meta->setText(meta.join(QStringLiteral("&nbsp;&nbsp;")));
    setWorkspaceStyle(m_meta, QStringLiteral("color:%1;font-size:11px;").arg(colors.muted.name()));
    const bool quiet = task.stage == QLatin1String("backlog") || task.stage == QLatin1String("done");
    setWorkspaceStyle(m_text, QStringLiteral("color:%1;").arg((quiet ? colors.muted : colors.fg).name()));
    // In review: the agent's latest report, or the cancellation.
    QString summary;
    if (task.stage == QLatin1String("review")) {
        if (task.cancelledAt > 0)
            summary = QStringLiteral("<b>Cancelled at %1%2</b> %3").arg(QDateTime::fromMSecsSinceEpoch(task.cancelledAt).toString(QStringLiteral("HH:mm")),
                task.cancelReason.isEmpty() ? QString() : QStringLiteral(":"), task.cancelReason.toHtmlEscaped());
        else if (!task.summary.isEmpty())
            summary = QStringLiteral("<b>%1 reported:</b> %2").arg(sessionLabel(task.sessionHost, task.sessionName).toHtmlEscaped(), task.summary.toHtmlEscaped());
    }
    m_summary->setText(summary); m_summary->setVisible(!summary.isEmpty());
    setWorkspaceStyle(m_summary, QStringLiteral("color:%1;border-left:2px solid %2;padding:1px 0 1px 10px;").arg(colors.muted.name(), stage.name()));
    if (rebuild) {
        qDeleteAll(m_buttons); m_buttons.clear();
        const auto actions = TaskBoard::actions(task.stage);
        for (const auto action : actions) {
            auto *button = new QToolButton(m_actions); button->setObjectName(QStringLiteral("taskAction:") + actionKey(action));
            button->setFixedSize(26, 26); button->setIconSize(QSize(15, 15)); button->setAutoRaise(true); button->setCursor(Qt::ArrowCursor);
            connect(button, &QToolButton::clicked, this, [this, action] { emit actionRequested(action); });
            m_actions->layout()->addWidget(button); m_buttons.insert(int(action), button);
        }
        m_actions->setFixedWidth(int(actions.size()) * 26 + qMax(0, int(actions.size()) - 1) * 2);
    }
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) {
        const auto action = TaskAction(it.key()); const QString why = state.blocked.value(it.key());
        it.value()->setIcon(workspaceIcon(TaskBoard::actionIcon(action), colors.muted));
        it.value()->setEnabled(why.isEmpty()); it.value()->setToolTip(why.isEmpty() ? TaskBoard::actionLabel(action) : why);
        it.value()->setAccessibleName(TaskBoard::actionLabel(action));
        setWorkspaceStyle(it.value(), QStringLiteral("QToolButton{border:0;border-radius:6px;background:transparent;}QToolButton:hover{background:%1;}").arg(colors.hover.name()));
    }
    showActions(m_hovered);
    layoutText(); update();
}

void TaskRow::showActions(bool shown) {
    // The container keeps its width, so the meta never moves.
    for (auto *button : std::as_const(m_buttons)) button->setVisible(shown);
}

void TaskRow::layoutText() {
    m_text->setText(clamp(m_task.text, m_text->font(), m_text->width() > 0 ? m_text->width() : width() - 60, 2));
}

void TaskRow::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); layoutText(); }

void TaskRow::enterEvent(QEnterEvent *event) { QWidget::enterEvent(event); m_hovered = true; showActions(true); update(); }

void TaskRow::leaveEvent(QEvent *event) { QWidget::leaveEvent(event); m_hovered = false; showActions(false); update(); }

void TaskRow::mouseReleaseEvent(QMouseEvent *event) {
    QWidget::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit opened();
}

void TaskRow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QColor tint = m_state.mine ? m_colors.accent : m_colors.fg;
    tint.setAlphaF(m_state.mine ? (m_hovered ? 0.11 : 0.07) : (m_hovered ? 0.04 : 0));
    p.fillRect(rect(), tint);
    if (m_state.mine) { p.setPen(Qt::NoPen); p.setBrush(m_colors.accent); p.drawRoundedRect(QRectF(0, 6, 3, height() - 12), 1.5, 1.5); }
    if (property("separated").toBool()) { p.setPen(QPen(m_colors.border, 1)); p.drawLine(41, 0, width(), 0); }
}
