#include "GraphPage.h"
#include "SessionCardDelegate.h"
#include "WorkspacePageHeader.h"
#include "WorkspaceStyle.h"
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {
QLabel *plain(const QString &text = {}) {
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    return label;
}
} // namespace

class GraphSummary : public QWidget {
  public:
    explicit GraphSummary(QWidget *parent = nullptr)
        : QWidget(parent), model(this), delegate(this) {
        model.appendRow(new QStandardItem);
        setFixedHeight(104);
        setProperty("expansion", 1.0);
        setProperty("selectionIsInspection", true);
    }
    void setCard(const GraphCanvas::Node &card) {
        auto roles = card.roles;
        roles[SessionRoles::WorkingSince] = 0.;
        if (model.itemData(model.index(0, 0)) != roles) {
            model.clearItemData(model.index(0, 0));
            model.setItemData(model.index(0, 0), roles);
            update();
        }
        setFixedHeight(card.roles.value(SessionRoles::ChildId).toString().isEmpty() ? 104 : 64);
        setAccessibleName(card.roles.value(SessionRoles::Title).toString());
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QStyleOptionViewItem option;
        option.initFrom(this);
        option.widget = this;
        option.rect = rect();
        option.state.setFlag(QStyle::State_Selected, false);
        delegate.paintStandalone(&p, option, model.index(0, 0));
    }

  private:
    QStandardItemModel model;
    SessionDelegate delegate;
};

GraphPage::GraphPage(QWidget *parent) : QWidget(parent) {
    setObjectName("graphPage");
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 12);
    root->setSpacing(12);
    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *heading = plain(tr("Graph"));
    heading->setStyleSheet("font-size:16px;font-weight:600");
    header->addWidget(heading);
    m_search = new QLineEdit;
    m_search->setObjectName("graphSearch");
    m_search->setPlaceholderText(tr("Search sessions, projects, computers…"));
    m_search->setClearButtonEnabled(true);
    header->addWidget(m_search, 1);
    m_filters = new QPushButton(tr("Filters"));
    m_filters->setObjectName("graphFilters");
    m_filterMenu = new QMenu(m_filters);
    m_filters->setMenu(m_filterMenu);
    header->addWidget(m_filters);
    m_listToggle = new QPushButton(tr("List"));
    m_listToggle->setCheckable(true);
    m_listToggle->setObjectName("graphListToggle");
    header->addWidget(m_listToggle);
    m_refresh = new QPushButton;
    header->addWidget(m_refresh);
    root->addLayout(header);
    m_count = plain();
    m_count->setObjectName("graphScope");
    m_count->setMinimumHeight(20);
    root->addWidget(m_count);
    m_splitter = new QSplitter;
    m_views = new QStackedWidget;
    m_canvas = new GraphCanvas;
    m_list = new QListView;
    m_list->setObjectName("graphAccessibleList");
    m_list->setAccessibleName(tr("Graph sessions"));
    m_list->setModel(m_canvas->nodeModel());
    m_list->setItemDelegate(new SessionDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setProperty("expansion", 1.0);
    m_list->setProperty("selectionIsInspection", true);
    m_views->addWidget(m_canvas);
    m_views->addWidget(m_list);
    m_splitter->addWidget(m_views);
    m_inspector = new QFrame;
    m_inspector->setObjectName("graphInspector");
    m_inspector->setMinimumWidth(280);
    m_inspector->setMaximumWidth(440);
    auto *panel = new QVBoxLayout(m_inspector);
    panel->setContentsMargins(18, 16, 18, 16);
    panel->setSpacing(12);
    auto *titleRow = new QHBoxLayout;
    m_title = plain(tr("Select a session or connection"));
    m_title->setWordWrap(true);
    m_title->setObjectName("graphInspectorTitle");
    titleRow->addWidget(m_title, 1);
    auto *close = new QPushButton;
    close->setObjectName("graphInspectorClose");
    close->setAccessibleName(tr("Close inspector"));
    close->setToolTip(close->accessibleName());
    close->setProperty("glyph", "close");
    close->setFixedSize(32, 32);
    titleRow->addWidget(close);
    panel->addLayout(titleRow);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    content->setObjectName("graphInspectorContent");
    auto *body = new QVBoxLayout(content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);
    m_summary = new GraphSummary;
    body->addWidget(m_summary);
    m_details = plain();
    m_details->setObjectName("graphInspectorDetails");
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(m_details);
    m_targets = new QListWidget;
    m_targets->setObjectName("graphBindings");
    m_targets->setAccessibleName(tr("Exact session bindings"));
    m_targets->setMaximumHeight(130);
    body->addWidget(m_targets);
    m_open = new QPushButton(tr("Open Activity"));
    m_open->setObjectName("graphOpenActivity");
    body->addWidget(m_open);
    m_focus = new QPushButton(tr("Focus connections"));
    m_focus->setObjectName("graphFocusConnections");
    body->addWidget(m_focus);
    body->addWidget(plain(tr("Connections")));
    m_connections = new QListWidget;
    m_connections->setObjectName("graphConnections");
    m_connections->setAccessibleName(tr("Recorded connections"));
    m_connections->setMinimumHeight(130);
    body->addWidget(m_connections, 1);
    body->addStretch();
    scroll->setWidget(content);
    panel->addWidget(scroll, 1);
    m_splitter->addWidget(m_inspector);
    m_splitter->setStretchFactor(0, 1);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setSizes({900, 360});
    m_inspector->hide();
    root->addWidget(m_splitter, 1);
    auto *footer = new QHBoxLayout;
    m_freshness = plain(tr("Waiting for graph data"));
    m_freshness->setObjectName("graphFreshness");
    m_freshness->setMinimumHeight(24);
    m_freshness->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    footer->addWidget(m_freshness, 1);
    for (const auto &action : QList<QPair<QString, std::function<void()>>>{
             {tr("−"), [this] { m_canvas->zoomBy(1 / 1.2); }},
             {tr("+"), [this] { m_canvas->zoomBy(1.2); }},
             {tr("Fit"), [this] { m_canvas->fitGraph(); }},
             {tr("Reset layout"), [this] {
                  m_canvas->resetLayout();
                  m_canvas->fitGraph();
              }}}) {
        auto *button = new QPushButton(action.first);
        button->setAccessibleName(action.first == "−"   ? tr("Zoom out")
                                  : action.first == "+" ? tr("Zoom in")
                                                        : action.first);
        footer->addWidget(button);
        connect(button, &QPushButton::clicked, this, action.second);
    }
    root->addLayout(footer);
    auto *searchDelay = new QTimer(this);
    searchDelay->setSingleShot(true);
    searchDelay->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, searchDelay, qOverload<>(&QTimer::start));
    connect(searchDelay, &QTimer::timeout, this, &GraphPage::render);
    connect(m_refresh, &QPushButton::clicked, this, &GraphPage::refreshRequested);
    connect(m_listToggle, &QPushButton::toggled, this,
            [this](bool list) { m_views->setCurrentIndex(list ? 1 : 0); });
    connect(m_canvas, &GraphCanvas::nodeSelected, this, &GraphPage::inspectNode);
    connect(m_canvas, &GraphCanvas::relationSelected, this, &GraphPage::inspectRelation);
    connect(m_list, &QListView::activated, this, [this](const QModelIndex &index) {
        inspectNode(index.data(SessionRoles::Key).toString());
    });
    connect(m_canvas, &GraphCanvas::selectionCleared, this, [this] {
        if (!m_rendering) {
            m_selectedNode.clear();
            m_selectedRelation.clear();
            m_inspector->hide();
        }
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (!m_rendering && current.isValid())
                    inspectNode(current.data(SessionRoles::Key).toString());
            });
    connect(close, &QPushButton::clicked, m_inspector, &QWidget::hide);
    connect(m_focus, &QPushButton::clicked, this, [this] {
        m_neighborhood = m_selectedNode;
        render();
    });
    connect(m_connections, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!item->data(Qt::UserRole).toString().isEmpty())
            inspectRelation(item->data(Qt::UserRole).toString());
        else if (!item->data(Qt::UserRole + 1).toString().isEmpty())
            focusNode(item->data(Qt::UserRole + 1).toString());
    });
    connect(m_targets, &QListWidget::currentRowChanged, this, [this](int row) {
        const auto node = std::find_if(m_nodes.cbegin(), m_nodes.cend(), [this](const Node &node) {
            return node.card.id == m_selectedNode;
        });
        m_open->setEnabled(node != m_nodes.cend() && row >= 0 && row < node->targets.size() &&
                           node->targets[row].available);
    });
    connect(m_open, &QPushButton::clicked, this, [this] {
        const auto node = std::find_if(m_nodes.cbegin(), m_nodes.cend(), [this](const Node &node) {
            return node.card.id == m_selectedNode;
        });
        const int row = m_targets->currentRow();
        if (node == m_nodes.cend() || row < 0 || row >= node->targets.size() ||
            !node->targets[row].available)
            return;
        const auto target = node->targets[row];
        emit openRequested(target.host, target.name, target.run, target.conversation,
                           target.archive, target.agent);
    });
    connect(m_filterMenu, &QMenu::aboutToShow, this, &GraphPage::rebuildFilters);
    setTheme(true);
    render();
}

bool GraphPage::setProjection(const QList<Node> &nodes, const QList<Relation> &relations) {
    if (nodes.size() > 2000 || relations.size() > 10000)
        return false;
    QSet<QString> ids, edges;
    for (const auto &node : nodes) {
        if (node.card.id.isEmpty() || ids.contains(node.card.id))
            return false;
        ids.insert(node.card.id);
    }
    for (const auto &edge : relations) {
        if (edge.line.id.isEmpty() || edges.contains(edge.line.id) ||
            !ids.contains(edge.line.from) || !ids.contains(edge.line.to))
            return false;
        edges.insert(edge.line.id);
    }
    m_nodes = nodes;
    m_relations = relations;
    render();
    return true;
}
void GraphPage::render() {
    const auto query = m_search->text().trimmed();
    QSet<QString> visible, primary;
    for (const auto &node : m_nodes) {
        const auto &roles = node.card.roles;
        const QString text = QStringList{roles.value(SessionRoles::Title).toString(),
                                         roles.value(SessionRoles::Host).toString(),
                                         roles.value(SessionRoles::Agent).toString(),
                                         roles.value(SessionRoles::Meta).toString(),
                                         node.project,
                                         node.identity,
                                         node.card.id}
                                 .join(' ');
        if ((m_project.isEmpty() || (node.card.group.isEmpty() ? QStringLiteral("@unassigned")
                                                               : node.card.group) == m_project) &&
            (!m_attentionOnly || roles.value(SessionRoles::Attention).toBool() ||
             roles.value(SessionRoles::Unread).toBool()) &&
            (query.isEmpty() || text.contains(query, Qt::CaseInsensitive)))
            primary.insert(node.card.id);
    }
    visible = primary;
    if (!m_project.isEmpty())
        for (const auto &edge : m_relations)
            if (primary.contains(edge.line.from) || primary.contains(edge.line.to)) {
                visible.insert(edge.line.from);
                visible.insert(edge.line.to);
            }
    if (!m_neighborhood.isEmpty()) {
        QSet<QString> neighborhood{m_neighborhood};
        for (const auto &edge : m_relations) {
            if (edge.line.from == m_neighborhood)
                neighborhood.insert(edge.line.to);
            if (edge.line.to == m_neighborhood)
                neighborhood.insert(edge.line.from);
        }
        visible.intersect(neighborhood);
    }
    QList<GraphCanvas::Node> nodes;
    QList<GraphCanvas::Relation> edges;
    for (const auto &node : m_nodes)
        if (visible.contains(node.card.id))
            nodes.append(node.card);
    for (const auto &edge : m_relations)
        if (visible.contains(edge.line.from) && visible.contains(edge.line.to))
            edges.append(edge.line);
    QMap<QString, QString> groups;
    for (const auto &node : m_nodes)
        groups[node.card.group] = node.project;
    m_rendering = true;
    m_canvas->setGroupLabels(groups);
    m_canvas->setGraph(nodes, edges);
    m_rendering = false;
    m_count->setText(
        tr("%1 of %2 sessions and agents · %3 connections%4")
            .arg(nodes.size())
            .arg(m_nodes.size())
            .arg(edges.size())
            .arg(m_neighborhood.isEmpty() ? QString() : tr(" · Focused neighborhood")));
    m_filters->setText(m_project.isEmpty() && !m_attentionOnly && m_neighborhood.isEmpty()
                           ? tr("Filters")
                           : tr("Filters (%1)")
                                 .arg(int(!m_project.isEmpty()) + int(m_attentionOnly) +
                                      int(!m_neighborhood.isEmpty())));
    if (!m_inspector->isHidden())
        inspectSelection();
}
void GraphPage::inspectNode(const QString &id) {
    if (m_rendering)
        return;
    m_selectedNode = id;
    m_selectedRelation.clear();
    {
        const QSignalBlocker block(m_canvas);
        if (m_canvas->selectedNode() != id)
            m_canvas->selectNode(id);
    }
    for (int row = 0; row < m_list->model()->rowCount(); ++row) {
        const auto index = m_list->model()->index(row, 0);
        if (index.data(SessionRoles::Key) == id) {
            const QSignalBlocker block(m_list->selectionModel());
            m_list->setCurrentIndex(index);
            break;
        }
    }
    inspectSelection();
}
void GraphPage::inspectRelation(const QString &id) {
    if (m_rendering)
        return;
    m_selectedRelation = id;
    m_selectedNode.clear();
    inspectSelection();
}
void GraphPage::inspectSelection() {
    const auto selectedNode =
        std::find_if(m_nodes.cbegin(), m_nodes.cend(),
                     [this](const Node &node) { return node.card.id == m_selectedNode; });
    QJsonArray signature{m_selectedNode, m_selectedRelation};
    if (selectedNode != m_nodes.cend()) {
        m_summary->setCard(selectedNode->card);
        signature.append(selectedNode->card.roles.value(SessionRoles::Title).toString());
        signature.append(selectedNode->project);
        signature.append(selectedNode->identity);
        signature.append(selectedNode->detail);
        for (const auto &target : selectedNode->targets)
            signature.append(QJsonArray{target.host, target.name, target.run, target.conversation,
                                        target.archive, target.agent, target.available});
    }
    for (const auto &edge : m_relations)
        if ((!m_selectedNode.isEmpty() &&
             (edge.line.from == m_selectedNode || edge.line.to == m_selectedNode)) ||
            edge.line.id == m_selectedRelation)
            signature.append(QJsonArray{edge.line.id, edge.line.from, edge.line.to, edge.line.kind,
                                        edge.line.description, edge.evidence});
    const auto currentSignature = QJsonDocument(signature).toJson(QJsonDocument::Compact);
    if (!m_inspector->isHidden() && currentSignature == m_inspectionSignature)
        return;
    m_inspectionSignature = currentSignature;
    const QString previousTarget =
        m_targets->currentItem() ? m_targets->currentItem()->toolTip() : QString();
    const int oldScroll = m_connections->verticalScrollBar()->value();
    m_targets->clear();
    m_connections->clear();
    m_open->setEnabled(false);
    const auto node = std::find_if(m_nodes.cbegin(), m_nodes.cend(),
                                   [this](const Node &n) { return n.card.id == m_selectedNode; });
    m_targets->setVisible(node != m_nodes.cend());
    m_open->setVisible(node != m_nodes.cend());
    m_focus->setVisible(node != m_nodes.cend());
    m_summary->setVisible(node != m_nodes.cend());
    if (node != m_nodes.cend()) {
        m_summary->setCard(node->card);
        m_title->setText(node->card.roles.value(SessionRoles::Title).toString());
        m_details->setText(QStringList{node->project, node->detail, node->identity}.join("\n\n"));
        for (const auto &target : node->targets) {
            auto *item = new QListWidgetItem(
                target.name + (target.available ? QString() : tr(" — unavailable")), m_targets);
            item->setToolTip(QStringList{target.host, target.run, target.conversation,
                                         target.archive, target.agent}
                                 .join('\n'));
        }
        if (node->targets.size() == 1)
            m_targets->setCurrentRow(0);
        else
            for (int row = 0; row < m_targets->count(); ++row)
                if (m_targets->item(row)->toolTip() == previousTarget)
                    m_targets->setCurrentRow(row);
        m_targets->setFixedHeight(qBound(
            38, int(node->targets.size()) * qMax(24, m_targets->sizeHintForRow(0)) + 16, 130));
        for (const auto &edge : m_relations)
            if (edge.line.from == m_selectedNode || edge.line.to == m_selectedNode) {
                auto *item = new QListWidgetItem(edge.line.description, m_connections);
                item->setData(Qt::UserRole, edge.line.id);
                item->setToolTip(edge.evidence);
            }
        if (!m_connections->count()) {
            auto *empty =
                new QListWidgetItem(tr("No recorded connections in loaded data"), m_connections);
            empty->setFlags(Qt::NoItemFlags);
        }
    } else {
        const auto edge =
            std::find_if(m_relations.cbegin(), m_relations.cend(),
                         [this](const Relation &r) { return r.line.id == m_selectedRelation; });
        if (edge == m_relations.cend()) {
            m_inspector->hide();
            return;
        }
        m_title->setText(edge->line.description);
        m_details->setText(edge->evidence);
        for (const auto &id : {edge->line.from, edge->line.to}) {
            const auto end = std::find_if(m_nodes.cbegin(), m_nodes.cend(),
                                          [&id](const Node &n) { return n.card.id == id; });
            if (end != m_nodes.cend()) {
                auto *item = new QListWidgetItem(
                    end->card.roles.value(SessionRoles::Title).toString(), m_connections);
                item->setData(Qt::UserRole + 1, id);
            }
        }
    }
    // Connections select edges; endpoint rows select their exact node.
    m_connections->verticalScrollBar()->setValue(oldScroll);
    m_inspector->show();
    arrangeInspector();
}
void GraphPage::focusNode(const QString &id) {
    m_project.clear();
    m_attentionOnly = false;
    m_neighborhood.clear();
    m_search->clear();
    render();
    m_canvas->selectNode(id);
    inspectNode(id);
}
void GraphPage::focusProject(const QString &id) {
    m_project = id;
    m_neighborhood.clear();
    render();
}
void GraphPage::setFreshness(const QString &summary, const QString &details) {
    m_freshness->setText(summary);
    m_freshness->setToolTip(details);
    m_freshness->setAccessibleName(summary);
}
void GraphPage::rebuildFilters() {
    m_filterMenu->clear();
    auto *all = m_filterMenu->addAction(tr("All current sessions"));
    connect(all, &QAction::triggered, this, [this] {
        m_project.clear();
        m_neighborhood.clear();
        m_attentionOnly = false;
        m_search->clear();
        render();
    });
    auto *attention = m_filterMenu->addAction(tr("Needs attention"));
    attention->setCheckable(true);
    attention->setChecked(m_attentionOnly);
    connect(attention, &QAction::toggled, this, [this](bool enabled) {
        m_attentionOnly = enabled;
        render();
    });
    auto *projects = m_filterMenu->addMenu(tr("Project"));
    QMap<QString, QString> groups;
    for (const auto &node : m_nodes)
        groups[node.card.group] = node.project;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        auto *action = projects->addAction(it.value().isEmpty() ? tr("Unassigned") : it.value());
        action->setCheckable(true);
        action->setChecked(m_project == it.key());
        connect(action, &QAction::triggered, this, [this, id = it.key()] {
            focusProject(id.isEmpty() ? QStringLiteral("@unassigned") : id);
        });
    }
    if (!m_neighborhood.isEmpty()) {
        auto *exit = m_filterMenu->addAction(tr("Leave focused neighborhood"));
        connect(exit, &QAction::triggered, this, [this] {
            m_neighborhood.clear();
            render();
        });
    }
}
void GraphPage::arrangeInspector() {
    const bool overlay = width() < 1000;
    if (overlay != m_overlay) {
        const bool visible = !m_inspector->isHidden();
        m_overlay = overlay;
        if (overlay)
            m_inspector->setParent(this);
        else {
            m_splitter->addWidget(m_inspector);
            m_splitter->setSizes({qMax(400, width() - 400), 360});
        }
        m_inspector->setVisible(visible);
    }
    if (m_overlay) {
        const auto area = m_splitter->geometry();
        const int w = qMin(360, area.width());
        m_inspector->setGeometry(area.right() - w + 1, area.y(), w, area.height());
        m_inspector->raise();
    }
}
void GraphPage::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    arrangeInspector();
}
void GraphPage::setTheme(bool dark) {
    m_dark = dark;
    m_summary->setProperty("hgsDark", dark);
    m_summary->update();
    m_canvas->setTheme(dark);
    m_list->setProperty("hgsDark", dark);
    m_list->viewport()->update();
    WorkspacePageHeader::theme(m_refresh, nullptr, tr("Refresh graph"), dark);
    setStyleSheet(
        QString("#graphPage {background:%4;color:%5;} #graphPage QLabel {color:%5;} #graphPage "
                "QLineEdit,#graphPage QListView {background:%1;color:%5;border:1px solid "
                "%2;border-radius:5px;padding:5px;} #graphPage QListView::item:selected "
                "{background:%6;color:%5;} #graphPage QPushButton "
                "{background:%1;color:%5;border:1px solid %2;border-radius:6px;padding:6px "
                "10px;min-height:20px;} #graphPage QPushButton:disabled {color:%3;} #graphPage "
                "QPushButton:hover {border-color:%3;} #graphInspectorContent {background:%1;} "
                "#graphPage QScrollArea {background:transparent;border:0;} #graphInspector "
                "{background:%1;border:1px solid %2;border-radius:8px;} #graphInspectorTitle "
                "{font-size:15px;font-weight:600;} #graphScope,#graphFreshness {color:%3;}")
            .arg(dark ? "#1c2229" : "#ffffff", dark ? "#333c47" : "#dce2e8",
                 dark ? "#a2adbc" : "#627082", dark ? "#171d24" : "#f2f5f6",
                 dark ? "#e8edf4" : "#1a2733", dark ? "#303b47" : "#dde7ef") +
        workspaceScrollbars(dark));
    for (auto *button : findChildren<QPushButton *>())
        if (button->property("glyph").isValid())
            button->setIcon(workspaceIcon(button->property("glyph").toString(),
                                          QColor(dark ? "#a2adbc" : "#627082")));
}
QJsonObject GraphPage::viewState() const {
    return {{"schema", 1},
            {"canvas", m_canvas->viewState()},
            {"project", m_project},
            {"query", m_search->text()},
            {"attention", m_attentionOnly},
            {"neighborhood", m_neighborhood},
            {"list", m_listToggle->isChecked()}};
}
bool GraphPage::restoreViewState(const QJsonObject &state) {
    if (state["schema"] != 1 || !state["canvas"].isObject() ||
        !m_canvas->restoreViewState(state["canvas"].toObject()))
        return false;
    m_project = state["project"].toString().left(160);
    m_neighborhood = state["neighborhood"].toString().left(512);
    m_search->setText(state["query"].toString().left(512));
    m_attentionOnly = state["attention"].toBool();
    m_listToggle->setChecked(state["list"].toBool());
    render();
    return true;
}
