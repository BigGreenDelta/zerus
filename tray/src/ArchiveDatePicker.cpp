#include "ArchiveDatePicker.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include <QBoxLayout>
#include <QCalendarWidget>
#include <QDateEdit>
#include <QDateTime>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QScreen>
#include <QStyleOptionButton>
#include <QTextCharFormat>
#include <QToolButton>

ArchiveDatePicker::ArchiveDatePicker(QWidget *parent) : QPushButton(parent)
{
    setObjectName("archivePeriod");
    setCheckable(true); setAutoDefault(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_panel = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    m_panel->setObjectName("archiveDatePanel");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->installEventFilter(this);
    auto *layout = new QVBoxLayout(m_panel);
    layout->setContentsMargins(16, 16, 16, 16); layout->setSpacing(14);
    auto *title = new QLabel(tr("Archived date")); title->setObjectName("archiveRangeTitle");
    layout->addWidget(title);
    auto *body = new QHBoxLayout; body->setSpacing(20); layout->addLayout(body);
    auto *custom = new QWidget; auto *dates = new QVBoxLayout(custom);
    dates->setContentsMargins(0, 0, 0, 0); dates->setSpacing(8);
    m_from = new QDateEdit; m_until = new QDateEdit;
    m_from->setObjectName("archiveFrom"); m_until->setObjectName("archiveUntil");
    for (auto *edit : {m_from, m_until}) {
        edit->setMinimumDate(QDate(1970, 1, 2)); edit->setMaximumDate(QDate(9999, 12, 30));
        edit->setCalendarPopup(true); edit->setDisplayFormat("dd MMM yyyy");
        edit->setAccessibleName(edit == m_from ? tr("Archived from, inclusive") : tr("Archived through, inclusive"));
        auto *label = new QLabel(edit == m_from ? tr("From") : tr("Through"));
        label->setBuddy(edit); dates->addWidget(label); dates->addWidget(edit);
        connect(edit, &QDateEdit::dateChanged, this, &ArchiveDatePicker::updateDraft);
    }
    m_hint = new QLabel; m_hint->setObjectName("archiveRangeHint"); m_hint->setWordWrap(true);
    m_hint->setMinimumHeight(fontMetrics().height() * 2);
    dates->addWidget(m_hint);
    auto *applyRow = new QHBoxLayout; applyRow->setSpacing(8);
    auto *cancel = new QPushButton(tr("Cancel")); cancel->setObjectName("archiveRangeCancel");
    m_apply = new QPushButton(tr("Apply range")); m_apply->setObjectName("archiveRangeApply");
    applyRow->addWidget(cancel); applyRow->addWidget(m_apply, 1); dates->addLayout(applyRow);
    connect(cancel, &QPushButton::clicked, m_panel, &QWidget::hide);
    connect(m_apply, &QPushButton::clicked, this, [this] { apply(4); });
    body->addWidget(custom, 3);
    auto *quick = new QWidget; auto *presets = new QVBoxLayout(quick);
    presets->setContentsMargins(0, 0, 0, 0); presets->setSpacing(4);
    auto *quickTitle = new QLabel(tr("Quick ranges")); quickTitle->setObjectName("archiveQuickTitle");
    presets->addWidget(quickTitle);
    const QStringList captions{tr("All dates"), tr("Today"), tr("Last 7 days"), tr("Last 30 days")};
    for (int i = 0; i < captions.size(); ++i) {
        auto *button = new QPushButton(captions[i]);
        button->setObjectName("archivePreset" + QString::number(i));
        button->setProperty("archivePreset", true); button->setCheckable(true);
        presets->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, i] { apply(i); });
    }
    presets->addStretch(); body->addWidget(quick, 2);
    connect(this, &QPushButton::clicked, this, [this] { if (m_panel->isVisible()) m_panel->hide(); else openPanel(); });
    setTheme(true); updateCaption();
}

void ArchiveDatePicker::openPanel()
{
    m_panel->setFont(font());
    m_hint->setMinimumHeight(fontMetrics().height() * 2);
    for (auto *edit : {m_from, m_until}) edit->calendarWidget()->setFont(font());
    const auto today = QDate::currentDate();
    m_from->setDate(m_period > 0 && m_period < 4 ? today.addDays(m_period == 1 ? 0 : m_period == 2 ? -6 : -29) : m_first);
    m_until->setDate(m_period > 0 && m_period < 4 ? today : m_last);
    updateDraft();
    for (int i = 0; i < 4; ++i) m_panel->findChild<QPushButton *>("archivePreset" + QString::number(i))->setChecked(m_period == i);
    const auto available = screen()->availableGeometry().adjusted(8, 8, -8, -8);
    auto *body = qobject_cast<QBoxLayout *>(m_panel->layout()->itemAt(1)->layout());
    const int width = qMin(qMax(480, fontMetrics().horizontalAdvance(QString(52, 'x'))), available.width());
    body->setDirection(width < fontMetrics().horizontalAdvance(QString(45, 'x')) ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_panel->setFixedWidth(width); m_panel->adjustSize();
    const auto below = mapToGlobal(QPoint(0, height() + 6));
    const int y = below.y() + m_panel->height() <= available.bottom() ? below.y() : mapToGlobal(QPoint()).y() - m_panel->height() - 6;
    m_panel->move(qBound(available.left(), below.x(), qMax(available.left(), available.right() - m_panel->width() + 1)),
                  qBound(available.top(), y, qMax(available.top(), available.bottom() - m_panel->height() + 1)));
    m_panel->show(); setChecked(true);
    m_from->setFocus(Qt::PopupFocusReason);
}

void ArchiveDatePicker::apply(int period)
{
    if (period == 4 && m_from->date() > m_until->date()) return;
    const bool changed = m_period != period || (period == 4 && (m_first != m_from->date() || m_last != m_until->date()));
    m_period = period;
    if (period == 4) { m_first = m_from->date(); m_last = m_until->date(); }
    updateCaption(); m_panel->hide();
    if (changed) emit rangeChanged();
}

void ArchiveDatePicker::updateDraft()
{
    const bool valid = m_from->date() <= m_until->date(); m_apply->setEnabled(valid);
    m_hint->setText(valid ? tr("Both dates included (local time)") : tr("End date must be on or after the start date."));
    m_hint->setProperty("invalid", !valid);
    m_hint->style()->unpolish(m_hint); m_hint->style()->polish(m_hint);
}

QPair<qint64, qint64> ArchiveDatePicker::range() const
{
    if (!m_period) return {0, 253402300800LL};
    const auto today = QDate::currentDate();
    const auto first = m_period == 4 ? m_first : today.addDays(m_period == 1 ? 0 : m_period == 2 ? -6 : -29);
    const auto last = m_period == 4 ? m_last : today;
    return {first.startOfDay().toSecsSinceEpoch(), last.addDays(1).startOfDay().toSecsSinceEpoch()};
}

void ArchiveDatePicker::updateCaption()
{
    const QStringList captions{tr("All dates"), tr("Today"), tr("Last 7 days"), tr("Last 30 days")};
    const auto caption = m_period == 4 ? (m_first == m_last ? locale().toString(m_first, "d MMM yyyy") :
        tr("%1 – %2").arg(locale().toString(m_first, m_first.year() == m_last.year() ? "d MMM" : "d MMM yyyy"), locale().toString(m_last, "d MMM yyyy"))) : captions[m_period];
    setText(caption); setAccessibleName(tr("Archived date: %1. Choose period").arg(caption)); setToolTip(accessibleName());
    updateGeometry(); update();
}

QSize ArchiveDatePicker::sizeHint() const { return {fontMetrics().horizontalAdvance(text()) + 76, qMax(36, fontMetrics().height() + 16)}; }
QSize ArchiveDatePicker::minimumSizeHint() const { return {120, sizeHint().height()}; }
void ArchiveDatePicker::paintEvent(QPaintEvent *)
{
    QStyleOptionButton option; initStyleOption(&option); option.text.clear(); option.icon = {};
    QPainter painter(this); style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
    const QColor ink(m_dark ? "#e8edf4" : "#1a2733");
    workspaceIcon("calendar", ink).paint(&painter, QRect(12, (height() - 18) / 2, 18, 18));
    workspaceIcon("chevron-down", ink).paint(&painter, QRect(width() - 28, (height() - 16) / 2, 16, 16));
    painter.setPen(ink); painter.drawText(QRect(38, 0, qMax(0, width() - 72), height()), Qt::AlignVCenter | Qt::AlignLeft,
        fontMetrics().elidedText(text(), Qt::ElideMiddle, qMax(0, width() - 72)));
}
void ArchiveDatePicker::hideEvent(QHideEvent *event) { m_panel->hide(); QPushButton::hideEvent(event); }
bool ArchiveDatePicker::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_panel && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) { m_panel->hide(); return true; }
    if (object == m_panel && event->type() == QEvent::Hide) { setChecked(false); if (isVisible()) setFocus(Qt::PopupFocusReason); }
    return QPushButton::eventFilter(object, event);
}

void ArchiveDatePicker::setTheme(bool dark)
{
    m_dark = dark;
    const QString bg = dark ? "#1c2229" : "#ffffff", fg = dark ? "#e8edf4" : "#1a2733", border = dark ? "#3d4855" : "#dce2e8";
    const QString hover = dark ? "#2b3540" : "#edf2f5", accent = dark ? "#8bdfc0" : "#167357", selected = dark ? "#233d35" : "#e0f0e9", muted = dark ? "#a2adbc" : "#627082";
    setWorkspaceStyle(this, QString("QPushButton#archivePeriod{background:%1;color:%2;border:1px solid %3;border-radius:8px;padding:0;} QPushButton#archivePeriod:hover{background:%4;} QPushButton#archivePeriod:checked,QPushButton#archivePeriod:focus{border-color:%5;}").arg(bg, fg, border, hover, accent));
    setWorkspaceStyle(m_panel, QString(R"(
        QFrame#archiveDatePanel{background:%1;color:%2;border:1px solid %3;border-radius:12px;}
        QLabel{color:%2;background:transparent;border:0;}
        QLabel#archiveRangeTitle{font-weight:600;}
        QLabel#archiveRangeHint,QLabel#archiveQuickTitle{color:%7;}
        QLabel#archiveRangeHint[invalid="true"]{color:%8;}
        QPushButton{background:transparent;color:%2;border:1px solid %3;border-radius:7px;padding:7px 10px;}
        QPushButton:hover{background:%4;} QPushButton:focus{border-color:%5;}
        QPushButton[archivePreset="true"]{text-align:left;border-color:transparent;}
        QPushButton[archivePreset="true"]:checked,QPushButton#archiveRangeApply{background:%6;color:%5;border-color:%5;}
        QPushButton:disabled{color:%7;border-color:%3;background:transparent;}
        QDateEdit{background:%1;color:%2;border:1px solid %3;border-radius:7px;padding:8px 34px 8px 10px;selection-background-color:%6;selection-color:%2;}
        QDateEdit:focus{border-color:%5;}
        QDateEdit::drop-down{width:28px;border:0;border-left:1px solid %3;}
        QDateEdit::down-arrow{image:url(:/hgs/chevron-down.svg);width:12px;height:12px;}
        QCalendarWidget QWidget{background:%1;color:%2;}
        QCalendarWidget QToolButton{color:%2;background:transparent;border:0;border-radius:4px;padding:5px;}
        QCalendarWidget QToolButton:hover{background:%4;}
        QCalendarWidget QAbstractItemView{background:%1;color:%2;selection-background-color:%6;selection-color:%5;border:0;outline:0;}
        QCalendarWidget QSpinBox{background:%1;color:%2;selection-background-color:%6;}
    )").arg(bg, fg, border, hover, accent, selected, muted, dark ? "#ff9ca8" : "#b52d48"));
    for (auto *edit : {m_from, m_until}) {
        auto *calendar = edit->calendarWidget();
        auto palette = calendar->palette(); palette.setColor(QPalette::Base, QColor(bg)); palette.setColor(QPalette::Window, QColor(bg));
        palette.setColor(QPalette::Text, QColor(fg)); palette.setColor(QPalette::WindowText, QColor(fg));
        palette.setColor(QPalette::Highlight, QColor(selected)); palette.setColor(QPalette::HighlightedText, QColor(accent));
        palette.setColor(QPalette::Button, QColor(bg)); palette.setColor(QPalette::ButtonText, QColor(fg)); palette.setColor(QPalette::AlternateBase, QColor(bg));
        calendar->setPalette(palette);
        QTextCharFormat header; header.setBackground(QColor(bg)); header.setForeground(QColor(muted)); calendar->setHeaderTextFormat(header);
        QTextCharFormat weekend; weekend.setForeground(QColor(fg));
        calendar->setWeekdayTextFormat(Qt::Saturday, weekend); calendar->setWeekdayTextFormat(Qt::Sunday, weekend);
        for (const auto &entry : {qMakePair("qt_calendar_prevmonth", "chevron-left"), qMakePair("qt_calendar_nextmonth", "chevron-right")})
            if (auto *button = calendar->findChild<QToolButton *>(entry.first)) button->setIcon(workspaceIcon(entry.second, QColor(fg)));
    }
    update();
}
