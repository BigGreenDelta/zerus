#include "SwarmConflictReview.h"
#include <algorithm>
#include "FleetState.h"
#include "IdentityBadge.h"
#include "ProjectAppearance.h"
#include "SessionPresentation.h"
#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QLabel *paragraph(const QString &name, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
    return label;
}
QString setting(const QJsonObject &field)
{
    const auto kind = field.value("kind").toString();
    const auto name = field.value("field").toString();
    if (kind == "session") return QObject::tr("Session project");
    if (kind == "folder") return QObject::tr("Project folder");
    if (kind == "project") {
        if (name == "name") return QObject::tr("Project name");
        if (name == "color") return QObject::tr("Project color");
        if (name == "alive") return QObject::tr("Keep or remove project");
    }
    if (kind == "machine") return name == "name" ? QObject::tr("Computer name") : QObject::tr("Computer identity");
    return QObject::tr("Shared setting");
}
bool darkPalette(const QPalette &palette) { return palette.color(QPalette::Window).lightness() < 128; }

// Stacks a caption, a project label, text and machine badges, as the
// presentation lists them. Paints when given a painter; returns the height.
int layoutPresentation(const QVariantMap &view, const QStyleOptionViewItem &option, QPainter *p)
{
    constexpr int gap = 6, padding = 6;
    // A list asks for a size before it has laid the row out: wrap to its viewport.
    int available = option.rect.width();
    if (const auto *view = qobject_cast<const QAbstractItemView *>(option.widget); available <= 0 && view) available = view->viewport()->width();
    const int width = available > 0 ? available - 2 * padding - 4 : 360;
    const int left = option.rect.left() + padding + 2;
    const bool dark = darkPalette(option.palette), selected = option.state & QStyle::State_Selected;
    const QColor text = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
    auto secondary = text; secondary.setAlphaF(.68);
    int y = option.rect.top() + padding;
    const auto paragraph = [&](const QString &value, const QFont &font, const QColor &color) {
        if (value.isEmpty()) return;
        const QFontMetrics metrics(font);
        const auto bounds = metrics.boundingRect(QRect(0, 0, qMax(1, width), 100000), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, value);
        if (p) { p->setFont(font); p->setPen(color); p->drawText(QRect(left, y, width, bounds.height()), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, value); }
        y += bounds.height() + gap;
    };
    if (p) { p->save(); p->setRenderHint(QPainter::Antialiasing); if (!(option.state & QStyle::State_Enabled)) p->setOpacity(.55); }
    auto caption = option.font; caption.setPixelSize(11);
    paragraph(view.value("caption").toString(), caption, secondary);
    if (view.contains("project")) {
        const auto project = view.value("project").toMap(); const auto name = project.value("name").toString();
        auto font = option.font; font.setPixelSize(12); font.setWeight(QFont::DemiBold);
        const int chip = qMin(width, qCeil(QFontMetricsF(font).horizontalAdvance(name)) + 46);
        if (p) ProjectAppearance::paint(p, QRect(left, y, chip, 28), name, QColor(project.value("color").toString()), dark, project.value("vivid").toBool());
        y += 28 + gap;
    }
    paragraph(view.value("text").toString(), option.font, text);
    for (const auto &entry : view.value("machines").toList()) {
        const auto machine = entry.toMap(); const auto alias = machine.value("alias").toString();
        const int badge = IdentityBadges::width(IdentityBadges::Machine, alias, option.font, width);
        if (p) {
            IdentityBadges::paint(p, QRect(left, y, badge, 18), IdentityBadges::Machine, alias, dark);
            if (machine.value("local").toBool()) {
                p->setFont(caption); p->setPen(secondary);
                p->drawText(QRect(left + badge + 6, y, qMax(0, width - badge - 6), 18), Qt::AlignVCenter | Qt::AlignLeft, QObject::tr("this computer"));
            }
        }
        y += 18 + gap;
    }
    if (p) p->restore();
    return y - gap + padding - option.rect.top();
}

// The desktop's notation for catalog values: the item text stays the
// accessible and copied form, PresentationRole is what the row shows.
class PresentationDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto view = index.data(SwarmConflictReview::PresentationRole).toMap();
        if (view.isEmpty()) { QStyledItemDelegate::paint(p, option, index); return; }
        QStyleOptionViewItem base(option); initStyleOption(&base, index); base.text.clear();
        const auto *style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &base, p, option.widget);
        layoutPresentation(view, option, p);
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto size = QStyledItemDelegate::sizeHint(option, index);
        const auto view = index.data(SwarmConflictReview::PresentationRole).toMap();
        return view.isEmpty() ? size : QSize(size.width(), layoutPresentation(view, option, nullptr));
    }
};
}

SwarmConflictReview::SwarmConflictReview(const FleetState &fleet, QWidget *parent)
    : QWidget(parent), m_fleet(fleet)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 0, 0, 0); outer->setSpacing(12);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("swarmConflictScroll");
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    content->setObjectName("swarmConflictContent");
    scroll->setWidget(content); outer->addWidget(scroll, 1);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_title = paragraph("swarmConflictTitle", this);
    auto font = m_title->font(); font.setBold(true); m_title->setFont(font);
    m_subject = paragraph("swarmConflictSubject", this);
    m_subjectMachine = new IdentityBadge(IdentityBadges::Machine, this);
    m_subjectMachine->setObjectName("swarmConflictMachine"); m_subjectMachine->hide();
    auto *subjectRow = new QHBoxLayout; subjectRow->setSpacing(8);
    subjectRow->addWidget(m_subject, 1); subjectRow->addWidget(m_subjectMachine, 0, Qt::AlignTop);
    m_explanation = paragraph("swarmConflictExplanation", this);
    layout->addWidget(m_title); layout->addLayout(subjectRow); layout->addWidget(m_explanation);
    m_choices = new QTableWidget(this);
    m_choices->setObjectName("swarmConflictChoices");
    m_choices->setItemDelegate(createDelegate(m_choices));
    m_choices->setColumnCount(3);
    m_choices->setHorizontalHeaderLabels({tr("Value to keep"), tr("Changed on"), tr("Display")});
    m_choices->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_choices->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_choices->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_choices->setColumnWidth(1, 190); // A badge and its “this computer” note.
    m_choices->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_choices->verticalHeader()->hide();
    m_choices->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_choices->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_choices->setSelectionMode(QAbstractItemView::SingleSelection);
    m_choices->setShowGrid(false);
    m_choices->setWordWrap(true);
    m_choices->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Wrapping depends on column widths, including a dragged splitter. Coalesce
    // header changes after layout and keep the user's selected value intact.
    auto *rowLayout = new QTimer(m_choices); rowLayout->setSingleShot(true);
    connect(m_choices->horizontalHeader(), &QHeaderView::sectionResized, rowLayout, [rowLayout] { rowLayout->start(); });
    connect(rowLayout, &QTimer::timeout, m_choices, &QTableWidget::resizeRowsToContents);
    m_choices->setMinimumHeight(130);
    m_choices->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    layout->addWidget(m_choices, 1);
    m_outcome = paragraph("swarmConflictOutcome", this);
    layout->addWidget(m_outcome);
    m_confirm = new QPushButton(tr("Choose a value above"), this);
    m_confirm->setObjectName("swarmResolve");
    m_confirm->setEnabled(false);
    m_confirm->setAutoDefault(false);
    outer->addWidget(m_confirm, 0, Qt::AlignRight);
    connect(m_choices, &QTableWidget::itemSelectionChanged, this, &SwarmConflictReview::updateChoice);
    connect(m_confirm, &QPushButton::clicked, this, [this] {
        if (m_busy || m_choices->currentRow() < 0 || m_choices->selectedItems().isEmpty()) return;
        const auto reviewed = m_conflict;
        const auto value = m_choices->item(m_choices->currentRow(), 0)->data(Qt::UserRole).value<QJsonValue>();
        const auto field = m_conflict.value("field").toObject();
        const auto kind = field.value("kind").toString();
        const bool removal = (kind == "project" && field.value("field") == "alive" && value == false)
            || (kind == "folder" && value.isNull()) || (kind == "machine" && field.value("field") == "redirect");
        if (removal && QMessageBox::question(this, tr("Confirm catalog change"),
                subject(field, m_snapshot, reviewed) + "\n\n" + consequence(value),
                QMessageBox::Apply | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Apply) return;
        // A modal confirmation runs an event loop: a snapshot can change while
        // it is open. Never combine its old value with newly observed versions.
        if (m_conflict != reviewed || m_busy) return;
        QJsonArray versions;
        for (const auto &op : reviewed.value("variants").toArray()) versions.append(op.toObject().value("id"));
        emit resolutionRequested({{"key", reviewed.value("key")}, {"versions", versions}, {"value", value}});
    });
}

QString SwarmConflictReview::projectName(const QString &id, const QJsonObject &snapshot) const
{
    const auto projects = snapshot.value("organization").toObject().value("projects").toArray();
    for (const auto &entry : projects) {
        const auto project = entry.toObject();
        if (project.value("id").toString() != id) continue;
        const auto name = project.value("name").toString();
        int matches = 0;
        for (const auto &other : projects) if (other.toObject().value("name").toString() == name) ++matches;
        if (!name.isEmpty()) return matches > 1 ? tr("%1 (%2)").arg(name, id.left(8)) : name;
    }
    if (id == "ungrouped") return tr("General");
    return tr("Unavailable project (%1)").arg(id.left(8));
}

QString SwarmConflictReview::machineName(const QString &id, const QJsonObject &snapshot, bool origin) const
{
    for (const auto &entry : snapshot.value("machines").toArray()) {
        const auto machine = entry.toObject();
        if (machine.value("id").toString() != id) continue;
        auto name = machine.value("name").toString();
        if (name.isEmpty()) name = machine.value("connection").toString();
        if (name.isEmpty()) break;
        return origin && machine.value("local").toBool() ? tr("%1 (this computer)").arg(name) : name;
    }
    if (id == snapshot.value("node_id").toString())
        return m_fleet.local().host.isEmpty() ? tr("This computer") : tr("%1 (this computer)").arg(m_fleet.local().host);
    return tr("Unknown computer (%1)").arg(id.left(8));
}

QVariantMap SwarmConflictReview::projectMark(const QString &id, const QJsonObject &snapshot, const QString &name, const QString &color) const
{
    for (const auto &entry : snapshot.value("organization").toObject().value("projects").toArray()) {
        const auto project = entry.toObject();
        if (project.value("id").toString() != id) continue;
        // The color the Sessions list gives a project without a valid one.
        const QColor shown(color.isEmpty() ? project.value("color").toString() : color);
        return {{"name", name.isEmpty() ? projectName(id, snapshot) : name},
            {"color", shown.isValid() ? shown.name() : QStringLiteral("#64b5f6")}, {"vivid", project.value("vivid").toBool()}};
    }
    return {};
}

// The badge other views show for a computer: its connection alias here, the
// local host for this computer, or the catalog name of an unconfigured one.
QVariantMap SwarmConflictReview::machineMark(const QString &id, const QJsonObject &snapshot, bool origin) const
{
    QString alias; bool local = id == snapshot.value("node_id").toString();
    for (const auto &entry : snapshot.value("machines").toArray()) {
        const auto machine = entry.toObject();
        if (machine.value("id").toString() != id) continue;
        local = machine.value("local").toBool();
        alias = local ? m_fleet.local().host : machine.value("connection").toString();
        if (alias.isEmpty()) alias = machine.value("name").toString();
    }
    if (alias.isEmpty() && local) alias = m_fleet.local().host;
    if (alias.isEmpty()) return {};
    return {{"alias", alias}, {"local", origin && local}};
}

QString SwarmConflictReview::subject(const QJsonObject &field, const QJsonObject &snapshot, const QJsonObject &conflict) const
{
    const auto title = subjectTitle(field, snapshot, conflict);
    if (field.value("kind").toString() != "session") return title;
    return tr("%1 on %2").arg(title, machineName(field.value("machine").toString(), snapshot));
}

QString SwarmConflictReview::subjectTitle(const QJsonObject &field, const QJsonObject &snapshot, const QJsonObject &conflict) const
{
    const auto kind = field.value("kind").toString();
    if (kind == "project") return projectName(field.value("id").toString(), snapshot);
    if (kind == "machine") return machineName(field.value("id").toString(), snapshot);
    if (kind == "folder") {
        for (const auto &project : snapshot.value("organization").toObject().value("projects").toArray())
            for (const auto &entry : project.toObject().value("folders").toArray())
                if (entry.toObject().value("id") == field.value("id"))
                    return entry.toObject().value("path").toString();
        for (const auto &entry : conflict.value("variants").toArray()) {
            const auto path = entry.toObject().value("value").toObject().value("path").toString();
            if (!path.isEmpty()) return path;
        }
        return tr("Folder link (%1)").arg(field.value("id").toString().left(8));
    }
    const auto session = field.value("session").toString();
    const auto machine = field.value("machine").toString();
    QString alias;
    for (const auto &entry : snapshot.value("machines").toArray())
        if (entry.toObject().value("id").toString() == machine)
            alias = entry.toObject().value("local").toBool() ? m_fleet.local().host : entry.toObject().value("connection").toString();
    const auto *box = alias == m_fleet.local().host && !alias.isEmpty() ? &m_fleet.local() : m_fleet.peer(alias);
    const bool archived = session.startsWith("archive\n");
    if (box) for (const auto &info : box->sessions) {
        if (archived ? info.archiveId != session.mid(8) : info.state == "archived" || info.name != session) continue;
        const auto title = SessionPresentation::displayTitle(info);
        return archived ? tr("Archived session: %1").arg(title) : title;
    }
    return archived ? tr("Archived session (%1)").arg(session.mid(8).left(8)) : session;
}

QString SwarmConflictReview::conflictLabel(const QJsonObject &conflict, const QJsonObject &snapshot) const
{
    const auto field = conflict.value("field").toObject();
    return setting(field) + "\n" + subject(field, snapshot, conflict);
}

QVariantMap SwarmConflictReview::conflictPresentation(const QJsonObject &conflict, const QJsonObject &snapshot) const
{
    const auto field = conflict.value("field").toObject();
    const auto kind = field.value("kind").toString();
    QVariantMap view{{"caption", setting(field)}};
    const auto machine = kind == "session" || kind == "machine" ? machineMark(field.value(kind == "session" ? "machine" : "id").toString(), snapshot) : QVariantMap();
    // A computer subject is its badge; a project subject is its label.
    const auto project = kind == "project" ? projectMark(field.value("id").toString(), snapshot) : QVariantMap();
    if (!project.isEmpty()) view.insert("project", project);
    if ((kind != "machine" || machine.isEmpty()) && project.isEmpty()) view.insert("text", subjectTitle(field, snapshot, conflict));
    if (!machine.isEmpty()) view.insert("machines", QVariantList{machine});
    return view;
}

QAbstractItemDelegate *SwarmConflictReview::createDelegate(QObject *parent)
{
    return new PresentationDelegate(parent);
}

void SwarmConflictReview::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (m_subjectMachine && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)) m_subjectMachine->setTheme(darkPalette(palette()));
}

QString SwarmConflictReview::valueText(const QJsonObject &field, const QJsonValue &value) const
{
    const auto kind = field.value("kind").toString();
    if (kind == "session") return value.isNull() ? tr("Use the local default project") : projectName(value.toString(), m_snapshot);
    if (kind == "folder") {
        if (value.isNull()) return tr("Remove folder link from the catalog");
        const auto folder = value.toObject();
        QStringList lines{tr("Project: %1").arg(projectName(folder.value("project").toString(), m_snapshot)),
            tr("Computer: %1").arg(machineName(folder.value("machine").toString(), m_snapshot)), folder.value("path").toString()};
        if (!folder.value("name").toString().isEmpty()) lines.prepend(tr("Name: %1").arg(folder.value("name").toString()));
        return lines.join('\n');
    }
    if (kind == "project" && field.value("field") == "alive")
        return value.toBool() ? tr("Keep project in the catalog") : tr("Remove project from the catalog");
    if (kind == "machine" && field.value("field") == "redirect") return machineName(value.toString(), m_snapshot);
    if (value.isString()) return value.toString();
    if (value.isNull()) return tr("Not set");
    if (value.isBool()) return value.toBool() ? tr("Enabled") : tr("Disabled");
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

QVariantMap SwarmConflictReview::valuePresentation(const QJsonObject &field, const QJsonValue &value) const
{
    const auto kind = field.value("kind").toString(), name = field.value("field").toString();
    if (kind == "session" && value.isString()) {
        const auto project = projectMark(value.toString(), m_snapshot);
        return project.isEmpty() ? QVariantMap() : QVariantMap{{"project", project}};
    }
    if (kind == "folder" && value.isObject()) {
        const auto folder = value.toObject();
        QVariantMap view; QStringList lines;
        const auto project = projectMark(folder.value("project").toString(), m_snapshot);
        if (project.isEmpty()) lines << tr("Project: %1").arg(projectName(folder.value("project").toString(), m_snapshot));
        else view.insert("project", project);
        if (!folder.value("name").toString().isEmpty()) lines << tr("Name: %1").arg(folder.value("name").toString());
        lines << folder.value("path").toString();
        const auto machine = machineMark(folder.value("machine").toString(), m_snapshot);
        if (machine.isEmpty()) lines << tr("Computer: %1").arg(machineName(folder.value("machine").toString(), m_snapshot));
        else view.insert("machines", QVariantList{machine});
        view.insert("text", lines.join('\n'));
        return view;
    }
    // A project's name or color reads as the label it would give the project.
    if (kind == "project" && (name == "name" || name == "color") && value.isString()) {
        const auto id = field.value("id").toString();
        const auto project = name == "name" ? projectMark(id, m_snapshot, value.toString()) : projectMark(id, m_snapshot, {}, value.toString());
        if (project.isEmpty()) return {};
        QVariantMap view{{"project", project}};
        if (name == "color") view.insert("text", value.toString());
        return view;
    }
    if (kind == "machine" && name == "redirect" && value.isString()) {
        const auto machine = machineMark(value.toString(), m_snapshot);
        return machine.isEmpty() ? QVariantMap() : QVariantMap{{"machines", QVariantList{machine}}};
    }
    return {};
}

QString SwarmConflictReview::consequence(const QJsonValue &value) const
{
    const auto field = m_conflict.value("field").toObject();
    const auto kind = field.value("kind").toString();
    QString result;
    if (kind == "session") {
        result = value.isNull() ? tr("Remove the explicit project assignment. Each computer will use its local default project.")
            : tr("Assign this session to “%1”.").arg(projectName(value.toString(), m_snapshot));
        result += " " + tr("The session, its messages and running agent stay intact.");
        if (!value.isNull()) {
            bool available = false;
            for (const auto &project : m_snapshot.value("organization").toObject().value("projects").toArray())
                if (project.toObject().value("id") == value) available = true;
            if (!available) result += "\n" + tr("This project is not in the current catalog. Until it is restored, the session is displayed under each computer's local default project.");
        }
    } else if (kind == "folder") {
        result = value.isNull() ? tr("Remove this folder link from the shared catalog.") : tr("Keep this folder location:\n%1").arg(valueText(field, value));
        result += "\n" + tr("Files on disk stay intact.");
    } else if (kind == "project" && field.value("field") == "alive") {
        result = value.toBool() ? tr("Keep “%1” in the shared catalog.").arg(subject(field, m_snapshot))
            : tr("Remove “%1” from the shared catalog. Its folder links and project placement will no longer be displayed. Files, sessions and messages stay intact.").arg(subject(field, m_snapshot));
    } else result = tr("Set %1 to “%2”.").arg(setting(field).toLower(), valueText(field, value));
    return result + "\n" + tr("This choice will synchronize to connected computers. Offline computers receive it when they reconnect.");
}

void SwarmConflictReview::setConflict(const QJsonObject &conflict, const QJsonObject &snapshot)
{
    QJsonValue previous(QJsonValue::Undefined);
    const bool sameVersions = m_conflict.value("key") == conflict.value("key")
        && m_conflict.value("variants") == conflict.value("variants");
    if (sameVersions && m_choices->currentRow() >= 0 && !m_choices->selectedItems().isEmpty())
        previous = m_choices->item(m_choices->currentRow(), 0)->data(Qt::UserRole).value<QJsonValue>();
    const bool changed = !sameVersions && m_conflict.value("key") == conflict.value("key") && !m_conflict.isEmpty();
    m_conflict = conflict; m_snapshot = snapshot;
    const auto field = conflict.value("field").toObject();
    const auto kind = field.value("kind").toString();
    // The computer reads as the badge other views show; the label keeps its name.
    const auto machine = kind == "session" || kind == "machine" ? machineMark(field.value(kind == "session" ? "machine" : "id").toString(), snapshot) : QVariantMap();
    m_title->setText(setting(field)); m_subject->setText(machine.isEmpty() ? subject(field, snapshot, conflict) : subjectTitle(field, snapshot, conflict));
    m_subject->setAccessibleName(subject(field, snapshot, conflict));
    m_subject->setToolTip(QString::fromUtf8(QJsonDocument(field).toJson(QJsonDocument::Indented)));
    m_subjectMachine->setTheme(darkPalette(palette())); m_subjectMachine->setValue(machine.value("alias").toString());
    m_subject->setVisible(kind != "machine" || machine.isEmpty());
    QString explanation = kind == "session" ? tr("Connected computers have different project assignments for this session. Choose the project it should belong to.")
        : kind == "folder" ? tr("Different folder locations or a removal were saved before the changes were synchronized. Choose the folder link to keep.")
        : tr("Different changes to this setting were saved before they were synchronized. Choose the value to keep.");
    explanation += "\n" + tr("“Shown now” is a temporary display choice, not a resolved conflict.");
    if (changed) explanation.prepend(tr("This conflict changed. Review the updated choices again.\n"));
    m_explanation->setText(explanation);
    QSignalBlocker blocker(m_choices);
    m_choices->clearContents(); m_choices->setRowCount(0);
    QList<QJsonValue> values; QStringList origins; QList<bool> displayed; QList<QVariantList> marks;
    for (const auto &entry : conflict.value("variants").toArray()) {
        const auto op = entry.toObject(); const auto value = op.value("value");
        int row = values.indexOf(value);
        if (row < 0) { row = values.size(); values.append(value); origins.append(QString()); displayed.append(false); marks.append(QVariantList()); }
        const auto actor = op.value("actor").toString(); const auto origin = machineName(actor, snapshot, true);
        if (!origins[row].split('\n').contains(origin)) {
            origins[row] += (origins[row].isEmpty() ? QString() : "\n") + origin;
            const auto mark = machineMark(actor, snapshot, true);
            marks[row].append(mark.isEmpty() ? QVariantMap{{"text", origin}} : mark);
        }
        if (op.value("id") == conflict.value("selected")) displayed[row] = true;
    }
    m_choices->setRowCount(values.size()); int selected = -1;
    for (int row = 0; row < values.size(); ++row) {
        auto *item = new QTableWidgetItem(valueText(field, values[row])); item->setData(Qt::UserRole, QVariant::fromValue(values[row])); item->setToolTip(item->text());
        item->setData(PresentationRole, valuePresentation(field, values[row]));
        auto *origin = new QTableWidgetItem(origins[row]); origin->setToolTip(origins[row]);
        // An origin this computer cannot name as a badge stays text.
        if (std::none_of(marks[row].cbegin(), marks[row].cend(), [](const QVariant &mark) { return mark.toMap().contains("text"); }))
            origin->setData(PresentationRole, QVariantMap{{"machines", marks[row]}});
        m_choices->setItem(row, 0, item); m_choices->setItem(row, 1, origin);
        m_choices->setItem(row, 2, new QTableWidgetItem(displayed[row] ? tr("Shown now") : QString()));
        if (!previous.isUndefined() && values[row] == previous) selected = row;
    }
    m_choices->resizeRowsToContents(); m_choices->clearSelection(); m_choices->setCurrentCell(-1, -1);
    if (selected >= 0) m_choices->setCurrentCell(selected, 0);
    blocker.unblock(); updateChoice();
}

void SwarmConflictReview::updateChoice()
{
    const bool chosen = m_choices->currentRow() >= 0 && !m_choices->selectedItems().isEmpty();
    m_confirm->setEnabled(chosen && !m_busy && !m_conflict.isEmpty());
    m_outcome->setText(chosen ? consequence(m_choices->item(m_choices->currentRow(), 0)->data(Qt::UserRole).value<QJsonValue>())
        : tr("Select a row to see what will change. Closing this window leaves the conflict for later."));
    const auto field = m_conflict.value("field").toObject();
    const auto kind = field.value("kind").toString();
    QString action = tr("Keep this value");
    if (kind == "session") action = tr("Keep this project");
    if (kind == "folder") action = tr("Keep this folder link");
    if (kind == "project" && field.value("field") == "alive") action = tr("Apply project choice");
    m_confirm->setText(chosen ? action : tr("Choose a value above"));
}

void SwarmConflictReview::setBusy(bool busy)
{
    m_busy = busy; m_choices->setEnabled(!busy); updateChoice();
}
