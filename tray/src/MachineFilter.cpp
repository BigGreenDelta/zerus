#include "MachineFilter.h"
#include "ScopeFilterWidgets.h"
#include "MachineAppearance.h"
#include "WorkspaceIcons.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyleOptionButton>
#include <QVBoxLayout>
#include <QWidgetAction>

using ScopeFilterWidgets::ChipFlow;
using ScopeFilterWidgets::Choice;

MachineFilter::MachineFilter(QWidget *parent) : QWidget(parent)
{
    setObjectName("machineFilterChips"); setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Minimum);
    m_flow = new ChipFlow(this);
    m_button = new QPushButton; m_button->setObjectName("machineFilter"); m_button->setFixedHeight(30); m_button->setMinimumWidth(28);
    m_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    m_button->setIconSize(QSize(16,16)); m_button->setAccessibleName(tr("Filter sessions by machines"));
    m_menu = new QMenu(m_button); m_menu->setObjectName("machineFilterMenu"); m_button->setMenu(m_menu);
    connect(m_menu,&QMenu::aboutToShow,this,&MachineFilter::rebuildMenu);
    setTheme(false); hide();
}
QString MachineFilter::name(const QString &host) const { return host == "@local" ? m_fleet.local().host : host; }
void MachineFilter::setSelection(const QSet<QString> &hosts) {
    if (m_selection == hosts) return;
    m_selection = hosts; rebuildChips(); emit selectionChanged(hosts);
}
void MachineFilter::toggle(const QString &host, bool selected) {
    auto next=m_selection; if (selected) next.insert(host); else next.remove(host); setSelection(next);
    if (auto *all = m_menu->findChild<QPushButton *>("allMachineFilters")) all->setChecked(next.isEmpty());
}
void MachineFilter::setFleet(const FleetState &fleet) { m_fleet=fleet; rebuildChips(); }
void MachineFilter::setSessionCounts(const QMap<QString, int> &counts, const QString &caption) {
    if (m_sessionCounts == counts && m_countCaption == caption) return;
    m_sessionCounts = counts; m_countCaption = caption; updateMenuCounts();
}
void MachineFilter::setTheme(bool dark) { m_dark=dark; refreshColors(); }
void MachineFilter::refreshColors() { m_signature.clear(); rebuildChips(); }
void MachineFilter::setConcealed(bool concealed) { m_concealed = concealed; setVisible(!m_concealed && !m_selection.isEmpty()); }
void MachineFilter::rebuildChips() {
    QStringList ids(m_selection.begin(),m_selection.end()); ids.sort();
    QString signature=QString::number(m_dark);
    for (const auto &id:ids) signature += '\n'+id+name(id)+MachineAppearance::color(name(id)).name();
    m_button->setIcon(workspaceIcon("machines", QColor(m_selection.isEmpty() ? (m_dark ? "#a1adbb" : "#647386") : (m_dark ? "#8bdfc0" : "#167357"))));
    m_button->setText(m_selection.isEmpty() ? QString() : QString::number(m_selection.size()));
    m_button->setToolTip(m_selection.isEmpty() ? tr("All machines: choose one or more") : tr("%1 machines selected; click to change").arg(m_selection.size()));
    setVisible(!m_concealed && !ids.isEmpty());
    if (signature==m_signature) return;
    m_signature=signature;
    while (auto *item=m_flow->takeAt(0)) { item->widget()->deleteLater(); item->widget()->hide(); delete item; }
    for (const auto &id:ids) {
        auto *chip=new QPushButton; chip->setObjectName("machineFilterChip"); chip->setProperty("host",id);
        chip->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        const QString label=name(id); chip->setText(chip->fontMetrics().elidedText(label,Qt::ElideMiddle,160)+"  ×"); chip->setFixedHeight(27);
        chip->setIcon(MachineAppearance::icon(label)); chip->setIconSize(QSize(14,14));
        chip->setToolTip(tr("Remove %1 from machine filter").arg(label)); chip->setAccessibleName(chip->toolTip());
        const auto color=MachineAppearance::color(label);
        chip->setStyleSheet(QString("QPushButton { background:%1; color:%2; border:1px solid %3; border-radius:7px; padding:0 7px; min-height:25px; font-size:11px; } QPushButton:hover, QPushButton:focus[keyboardFocus=\"true\"] { border-color:%2; }")
            .arg(MachineAppearance::backgroundColor(color,m_dark,MachineAppearance::vivid(label)).name(),MachineAppearance::textColor(color,m_dark,MachineAppearance::vivid(label)).name(),MachineAppearance::blend(color,m_dark?QColor("#171e25"):QColor("#fff"),.55).name()));
        connect(chip,&QPushButton::clicked,this,[this,id]{toggle(id,false);}); m_flow->addWidget(chip);
    }
    m_flow->invalidate(); updateGeometry();
}
void MachineFilter::rebuildMenu() {
    m_menu->clear(); auto *panel=new QWidget; auto *layout=new QVBoxLayout(panel); layout->setContentsMargins(10,8,10,8); layout->setSpacing(4);
    auto *caption=new QLabel(tr("Choose machines")); caption->setStyleSheet("font-size:11px; padding:2px 6px;"); layout->addWidget(caption);
    auto *scope=new QLabel; scope->setObjectName("machineFilterCountScope"); scope->setStyleSheet("font-size:10px; padding:2px 6px;"); layout->addWidget(scope);
    auto *all=new QPushButton(tr("All machines")); all->setObjectName("allMachineFilters"); all->setCheckable(true); all->setChecked(m_selection.isEmpty()); layout->addWidget(all);
    all->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    connect(all,&QPushButton::clicked,this,[this,all] {
        setSelection({}); all->setChecked(true);
        for (auto *check:m_menu->findChildren<QCheckBox *>()) { const QSignalBlocker blocker(check); check->setChecked(false); }
    });
    QStringList ids; if (m_fleet.local().localSessions) ids << "@local"; ids.append(m_fleet.peerNames());
    for (const auto &id:ids) {
        auto *check=new Choice(name(id)+(id=="@local" ? tr(" (this machine)") : QString()),m_dark); check->setObjectName("machineFilterChoice");
        check->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        check->setProperty("machineLabel", check->text());
        check->setProperty("host",id); check->setIcon(MachineAppearance::icon(name(id))); check->setChecked(m_selection.contains(id)); check->setMinimumHeight(32);
        layout->addWidget(check);
        connect(check,&QCheckBox::toggled,this,[this,id](bool selected){toggle(id,selected);});
    }
    auto *hint=new QLabel(tr("Changes apply immediately")); hint->setStyleSheet("font-size:10px; padding:4px 6px;"); layout->addWidget(hint);
    auto *action=new QWidgetAction(m_menu); action->setDefaultWidget(panel); m_menu->addAction(action);
    updateMenuCounts();
}
void MachineFilter::updateMenuCounts() {
    int total = 0; for (auto count : m_sessionCounts) total += count;
    if (auto *scope = m_menu->findChild<QLabel *>("machineFilterCountScope"))
        scope->setText(tr("%1 before search").arg(m_countCaption.isEmpty() ? tr("All sessions") : m_countCaption));
    if (auto *all = m_menu->findChild<QPushButton *>("allMachineFilters")) {
        all->setText(tr("All machines (%1)").arg(total));
        all->setAccessibleName(tr("All machines, %1 sessions").arg(total)); all->setProperty("sessionCount", total);
    }
    for (auto *check : m_menu->findChildren<QCheckBox *>("machineFilterChoice")) {
        const auto label = check->property("machineLabel").toString();
        const int count = m_sessionCounts.value(check->property("host").toString());
        check->setText(tr("%1 (%2)").arg(label).arg(count));
        check->setAccessibleName(tr("%1, %2 sessions").arg(label).arg(count)); check->setProperty("sessionCount", count);
    }
}
