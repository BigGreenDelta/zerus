#include "ProjectFilter.h"
#include "ScopeFilterWidgets.h"
#include "MachineAppearance.h"
#include "WorkspaceIcons.h"
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidgetAction>

ProjectFilter::ProjectFilter(QWidget *parent) : QWidget(parent)
{
    setObjectName("projectFilterChips"); setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    m_flow = new ScopeFilterWidgets::ChipFlow(this);
    m_button = new QPushButton; m_button->setObjectName("projectFilter");
    m_button->setFixedHeight(30); m_button->setMinimumWidth(28);
    m_button->setAttribute(Qt::WA_LayoutUsesWidgetRect); m_button->setIconSize(QSize(16,16));
    m_button->setAccessibleName(tr("Filter sessions by projects"));
    m_menu = new QMenu(m_button); m_menu->setObjectName("projectFilterMenu"); m_button->setMenu(m_menu);
    connect(m_menu, &QMenu::aboutToShow, this, &ProjectFilter::rebuildMenu);
    rebuildChips();
}
QString ProjectFilter::name(const QString &id) const
{
    for (const auto &project : m_projects) if (project.id == id) return project.name;
    return tr("Unavailable project");
}
QColor ProjectFilter::color(const QString &id) const
{
    for (const auto &project : m_projects) if (project.id == id && QColor(project.color).isValid()) return QColor(project.color);
    return QColor(m_dark ? "#8bdfc0" : "#167357");
}
bool ProjectFilter::vivid(const QString &id) const
{
    for (const auto &project : m_projects) if (project.id == id) return project.vivid;
    return false;
}
void ProjectFilter::setSelection(const QSet<QString> &projects)
{
    if (m_selection == projects) return;
    m_selection = projects; rebuildChips(); updateMenu(); emit selectionChanged(projects);
}
void ProjectFilter::toggle(const QString &id, bool selected)
{
    auto next = m_selection; if (selected) next.insert(id); else next.remove(id); setSelection(next);
}
void ProjectFilter::setProjects(const QList<SessionOrganization::Group> &projects) { m_projects = projects; rebuildChips(); updateMenu(); }
void ProjectFilter::setSessionCounts(const QMap<QString, int> &counts, const QString &caption) { m_counts = counts; m_caption = caption; updateMenu(); }
void ProjectFilter::setTheme(bool dark) { m_dark = dark; m_signature.clear(); rebuildChips(); }
void ProjectFilter::setConcealed(bool concealed) { m_concealed = concealed; setVisible(!concealed && !m_selection.isEmpty()); }
void ProjectFilter::rebuildChips()
{
    QStringList ids(m_selection.cbegin(), m_selection.cend()); ids.sort();
    QString signature = QString::number(m_dark);
    for (const auto &id : ids) signature += '\n' + id + name(id) + color(id).name() + QString::number(vivid(id));
    m_button->setIcon(workspaceIcon("projects", QColor(m_selection.isEmpty() ? (m_dark ? "#a1adbb" : "#647386") : (m_dark ? "#8bdfc0" : "#167357"))));
    m_button->setText(ids.isEmpty() ? QString() : QString::number(ids.size()));
    m_button->setToolTip(ids.isEmpty() ? tr("All projects: choose one or more") : tr("%1 projects selected; click to change").arg(ids.size()));
    setVisible(!m_concealed && !ids.isEmpty());
    if (signature == m_signature) return;
    m_signature = signature;
    while (auto *item = m_flow->takeAt(0)) { item->widget()->hide(); item->widget()->deleteLater(); delete item; }
    for (const auto &id : ids) {
        auto *chip = new QPushButton; chip->setObjectName("projectFilterChip"); chip->setProperty("project", id);
        chip->setAttribute(Qt::WA_LayoutUsesWidgetRect); chip->setAutoDefault(false);
        chip->setText(chip->fontMetrics().elidedText(name(id), Qt::ElideMiddle, 160).replace('&', "&&") + "  ×"); chip->setFixedHeight(27);
        chip->setIcon(workspaceIcon("projects", color(id))); chip->setIconSize(QSize(14,14));
        chip->setToolTip(tr("Remove %1 from project filter").arg(name(id))); chip->setAccessibleName(chip->toolTip());
        chip->setStyleSheet(QString("QPushButton { background:%1; color:%2; border:1px solid %3; border-radius:7px; padding:0 7px; min-height:25px; font-size:11px; } QPushButton:hover, QPushButton:focus[keyboardFocus=\"true\"] { border-color:%2; }")
            .arg(MachineAppearance::backgroundColor(color(id), m_dark, vivid(id)).name(), MachineAppearance::textColor(color(id), m_dark, vivid(id)).name(), MachineAppearance::blend(color(id), m_dark ? QColor("#171e25") : QColor("#fff"), .55).name()));
        connect(chip, &QPushButton::clicked, this, [this,id] { toggle(id, false); }); m_flow->addWidget(chip);
    }
    m_flow->invalidate(); updateGeometry();
}
void ProjectFilter::rebuildMenu()
{
    m_menu->clear(); auto *panel = new QWidget; auto *layout = new QVBoxLayout(panel); layout->setContentsMargins(10,8,10,8); layout->setSpacing(4);
    auto *title = new QLabel(tr("Choose projects")); title->setStyleSheet("font-size:11px; padding:2px 6px;"); layout->addWidget(title);
    auto *scope = new QLabel; scope->setObjectName("projectFilterCountScope"); scope->setStyleSheet("font-size:10px; padding:2px 6px;"); layout->addWidget(scope);
    auto *all = new QPushButton; all->setObjectName("allProjectFilters"); all->setCheckable(true); all->setAttribute(Qt::WA_LayoutUsesWidgetRect); layout->addWidget(all);
    connect(all, &QPushButton::clicked, this, [this] { setSelection({}); updateMenu(); });
    QStringList ids;
    for (const auto &project : m_projects) if (project.accessible || m_counts.value(project.id) || m_selection.contains(project.id)) ids << project.id;
    QStringList missing(m_selection.cbegin(), m_selection.cend()); missing.sort();
    for (const auto &id : missing) if (!ids.contains(id)) ids << id;
    auto *choices = new QWidget; auto *rows = new QVBoxLayout(choices); rows->setContentsMargins(0,0,0,0); rows->setSpacing(4);
    for (const auto &id : ids) {
        auto *check = new ScopeFilterWidgets::Choice(name(id), m_dark); check->setObjectName("projectFilterChoice"); check->setProperty("project", id);
        check->setIcon(workspaceIcon("projects", color(id))); check->setChecked(m_selection.contains(id)); rows->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [this,id](bool selected) { toggle(id, selected); });
    }
    auto *scroll = new QScrollArea; scroll->setObjectName("projectFilterChoices"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); scroll->setWidget(choices);
    scroll->setStyleSheet("QScrollArea#projectFilterChoices { background:transparent; border:0; }");
    scroll->viewport()->setAutoFillBackground(false); choices->setAutoFillBackground(false);
    scroll->setMinimumWidth(qMin(520, choices->sizeHint().width() + 24)); scroll->setFixedHeight(qMin(324, choices->sizeHint().height() + 4)); layout->addWidget(scroll);
    auto *hint = new QLabel(tr("Changes apply immediately")); hint->setStyleSheet("font-size:10px; padding:4px 6px;"); layout->addWidget(hint);
    auto *action = new QWidgetAction(m_menu); action->setDefaultWidget(panel); m_menu->addAction(action); updateMenu();
}
void ProjectFilter::updateMenu()
{
    int total = 0; for (auto count : m_counts) total += count;
    if (auto *scope = m_menu->findChild<QLabel *>("projectFilterCountScope")) scope->setText(tr("%1 before project selection and search").arg(m_caption));
    if (auto *all = m_menu->findChild<QPushButton *>("allProjectFilters")) {
        all->setText(tr("All projects (%1)").arg(total)); all->setChecked(m_selection.isEmpty());
        all->setAccessibleName(tr("All projects, %1 sessions").arg(total)); all->setProperty("sessionCount", total);
    }
    for (auto *check : m_menu->findChildren<QCheckBox *>("projectFilterChoice")) {
        const auto id = check->property("project").toString(); const int count = m_counts.value(id);
        const QSignalBlocker blocker(check); check->setChecked(m_selection.contains(id));
        check->setText(tr("%1 (%2)").arg(check->fontMetrics().elidedText(name(id), Qt::ElideMiddle, 360).replace('&', "&&")).arg(count)); check->setToolTip(name(id)); check->setIcon(workspaceIcon("projects", color(id)));
        check->setAccessibleName(tr("%1, %2 sessions").arg(name(id)).arg(count)); check->setProperty("sessionCount", count);
    }
}
