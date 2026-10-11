#include "MachineFilter.h"
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

namespace {
class MachineChoice final : public QCheckBox {
public:
    MachineChoice(const QString &text, bool dark) : QCheckBox(text), m_ink(dark ? "#10231b" : "#ffffff") {}
protected:
    void paintEvent(QPaintEvent *event) override {
        QCheckBox::paintEvent(event);
        if (!isChecked()) return;
        QStyleOptionButton option; initStyleOption(&option);
        const QRectF indicator = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(m_ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath tick; tick.moveTo(indicator.left() + indicator.width()*.25, indicator.top() + indicator.height()*.52);
        tick.lineTo(indicator.left() + indicator.width()*.44, indicator.top() + indicator.height()*.70);
        tick.lineTo(indicator.left() + indicator.width()*.77, indicator.top() + indicator.height()*.31);
        painter.drawPath(tick);
    }
private:
    QColor m_ink;
};
class ChipFlow final : public QLayout {
public:
    explicit ChipFlow(QWidget *parent) : QLayout(parent) { setContentsMargins(0,0,0,0); setSpacing(5); }
    ~ChipFlow() override { while (auto *item = takeAt(0)) delete item; }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0,0,width,0), false); }
    QSize sizeHint() const override { return {200, heightForWidth(parentWidget()->width())}; }
    QSize minimumSize() const override { return {0, m_items.isEmpty() ? 0 : 27}; }
    void setGeometry(const QRect &r) override { QLayout::setGeometry(r); arrange(r,true); }
private:
    int arrange(const QRect &r, bool apply) const {
        int x = r.x(), y = r.y(), height = 0;
        for (auto *item : m_items) {
            const QSize s(qMin(r.width(),item->sizeHint().width()),item->sizeHint().height());
            if (x > r.x() && x + s.width() > r.right()+1) { x = r.x(); y += height + spacing(); height=0; }
            if (apply) item->setGeometry(QRect(QPoint(x,y),s));
            x += s.width()+spacing(); height=qMax(height,s.height());
        }
        return y-r.y()+height;
    }
    QList<QLayoutItem *> m_items;
};
}
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
void MachineFilter::setTheme(bool dark) { m_dark=dark; refreshColors(); }
void MachineFilter::refreshColors() { m_signature.clear(); rebuildChips(); }
void MachineFilter::rebuildChips() {
    QStringList ids(m_selection.begin(),m_selection.end()); ids.sort();
    QString signature=QString::number(m_dark);
    for (const auto &id:ids) signature += '\n'+id+name(id)+MachineAppearance::color(name(id)).name();
    m_button->setIcon(workspaceIcon("machines", QColor(m_selection.isEmpty() ? (m_dark ? "#a1adbb" : "#647386") : (m_dark ? "#8bdfc0" : "#167357"))));
    m_button->setText(m_selection.isEmpty() ? QString() : QString::number(m_selection.size()));
    m_button->setToolTip(m_selection.isEmpty() ? tr("All machines: choose one or more") : tr("%1 machines selected; click to change").arg(m_selection.size()));
    setVisible(!ids.isEmpty());
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
    auto *all=new QPushButton(tr("All machines")); all->setObjectName("allMachineFilters"); all->setCheckable(true); all->setChecked(m_selection.isEmpty()); layout->addWidget(all);
    all->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    connect(all,&QPushButton::clicked,this,[this,all] {
        setSelection({}); all->setChecked(true);
        for (auto *check:m_menu->findChildren<QCheckBox *>()) { const QSignalBlocker blocker(check); check->setChecked(false); }
    });
    QStringList ids{"@local"}; ids.append(m_fleet.peerNames());
    for (const auto &id:ids) {
        auto *check=new MachineChoice(name(id)+(id=="@local" ? tr(" (this machine)") : QString()),m_dark); check->setObjectName("machineFilterChoice");
        check->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        check->setProperty("host",id); check->setIcon(MachineAppearance::icon(name(id))); check->setChecked(m_selection.contains(id)); check->setMinimumHeight(32);
        // Paint the indicator through the stylesheet: QMacStyle's native
        // checkbox on Liquid Glass draws above the supplied indicator rectangle.
        check->setStyleSheet(QString("QCheckBox { padding:2px 6px; spacing:8px; } QCheckBox::indicator { width:14px; height:14px; border:1px solid %1; border-radius:4px; background:%2; } QCheckBox::indicator:checked { background:%3; border-color:%3; } QCheckBox::indicator:hover, QCheckBox::indicator:focus { border-color:%3; }")
            .arg(m_dark ? "#a2adbc" : "#627082",m_dark ? "#1c2229" : "#ffffff",m_dark ? "#8bdfc0" : "#167357")); layout->addWidget(check);
        connect(check,&QCheckBox::toggled,this,[this,id](bool selected){toggle(id,selected);});
    }
    auto *hint=new QLabel(tr("Changes apply immediately")); hint->setStyleSheet("font-size:10px; padding:4px 6px;"); layout->addWidget(hint);
    auto *action=new QWidgetAction(m_menu); action->setDefaultWidget(panel); m_menu->addAction(action);
}
