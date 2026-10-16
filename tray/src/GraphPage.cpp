#include "GraphPage.h"
#include "GraphViewStateStore.h"
#include "SessionCardDelegate.h"
#include "WorkspacePageHeader.h"
#include "WorkspaceStyle.h"
#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
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
#include <utility>

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

// QListView's native Return activation differs between macOS and Unix styles.
// Keep the graph inspector's documented Enter action consistent on both.
class GraphConnections final : public QListWidget {
  protected:
    void keyPressEvent(QKeyEvent *event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentItem() &&
            (currentItem()->flags() & Qt::ItemIsEnabled)) {
            emit itemActivated(currentItem());
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
};

GraphPage::GraphPage(QWidget *parent, const QString &viewStateKey) : QWidget(parent) {
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
    m_connections = new GraphConnections;
    m_connections->setObjectName("graphConnections");
    m_connections->setAccessibleName(tr("Recorded connections"));
    m_connections->setMinimumHeight(130);
    body->addWidget(m_connections, 1);
    m_evidencePanel = new QWidget;
    auto *evidenceLayout = new QVBoxLayout(m_evidencePanel);
    evidenceLayout->setContentsMargins(0, 0, 0, 0);
    evidenceLayout->addWidget(plain(tr("Delivery evidence")));
    m_evidenceCount = plain();
    m_evidenceCount->setWordWrap(true);
    m_evidenceCount->setObjectName("graphEvidenceCount");
    evidenceLayout->addWidget(m_evidenceCount);
    m_evidence = new QListWidget;
    m_evidence->setObjectName("graphEvidence");
    m_evidence->setAccessibleName(tr("Recorded delivery receipts"));
    m_evidence->setMinimumHeight(100);
    m_evidence->setMaximumHeight(200);
    evidenceLayout->addWidget(m_evidence);
    auto *evidencePages = new QHBoxLayout;
    m_evidenceNewer = new QPushButton(tr("Newer"));
    m_evidenceNewer->setObjectName("graphEvidenceNewer");
    m_evidenceOlder = new QPushButton(tr("Older"));
    m_evidenceOlder->setObjectName("graphEvidenceOlder");
    evidencePages->addWidget(m_evidenceNewer);
    evidencePages->addWidget(m_evidenceOlder);
    evidenceLayout->addLayout(evidencePages);
    m_evidenceOpen = new QPushButton(tr("Open delivered message"));
    m_evidenceOpen->setObjectName("graphEvidenceOpen");
    evidenceLayout->addWidget(m_evidenceOpen);
    body->addWidget(m_evidencePanel);
    m_evidencePanel->hide();
    connect(m_evidenceNewer, &QPushButton::clicked, this, [this] {
        m_evidenceOffset = qMax(0, m_evidenceOffset - 50);
        renderEvidence();
    });
    connect(m_evidenceOlder, &QPushButton::clicked, this, [this] {
        m_evidenceOffset += 50;
        renderEvidence();
    });
    connect(m_evidence, &QListWidget::currentRowChanged, this, [this] { updateEvidenceAction(); });
    connect(m_evidenceOpen, &QPushButton::clicked, this, [this] {
        const auto edge =
            std::find_if(m_relations.cbegin(), m_relations.cend(),
                         [this](const Relation &r) { return r.line.id == m_selectedRelation; });
        if (edge == m_relations.cend() || !m_evidence->currentItem())
            return;
        const auto node = std::find_if(m_nodes.cbegin(), m_nodes.cend(),
                                       [&](const Node &n) { return n.card.id == edge->line.to; });
        if (node == m_nodes.cend())
            return;
        QList<Target> candidates;
        for (const auto &t : node->targets)
            if (t.available && t.agent.isEmpty() && t.archive.isEmpty())
                candidates.append(t);
        if (candidates.isEmpty())
            for (const auto &t : node->targets)
                if (t.available && t.agent.isEmpty())
                    candidates.append(t);
        if (candidates.size() != 1)
            return;
        const auto &t = candidates.first();
        emit evidenceRequested(t.host, t.name, t.run, t.conversation, t.archive,
                               m_evidence->currentItem()->data(Qt::UserRole).toString());
    });
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
    m_freshnessSummary = m_freshness->text();
    m_freshness->setObjectName("graphFreshness");
    m_freshness->setMinimumHeight(24);
    m_freshness->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    footer->addWidget(m_freshness, 1);
    for (const auto &action : QList<QPair<QString, std::function<void()>>>{
             {tr("−"), [this] { m_canvas->zoomBy(1 / 1.2); }},
             {tr("+"), [this] { m_canvas->zoomBy(1.2); }},
             {tr("Fit"), [this] { m_canvas->fitGraph(); }}}) {
        auto *button = new QPushButton(action.first);
        button->setAccessibleName(action.first == "−"   ? tr("Zoom out")
                                  : action.first == "+" ? tr("Zoom in")
                                                        : action.first);
        footer->addWidget(button);
        connect(button, &QPushButton::clicked, this, action.second);
    }
    m_arrange = new QPushButton(tr("Arrange visible"));
    m_arrange->setObjectName("graphArrange");
    m_arrange->setToolTip(tr("Rearrange only the visible nodes. Your layout is saved on this "
                             "device; refresh never rearranges it. You can undo this action."));
    footer->addWidget(m_arrange);
    m_undo = new QPushButton(tr("Undo arrange"));
    m_undo->setObjectName("graphUndoArrange");
    m_undo->setToolTip(tr("Restore positions and camera from before the last arrangement"));
    m_undo->hide();
    footer->addWidget(m_undo);
    connect(m_arrange, &QPushButton::clicked, this, &GraphPage::arrangeVisible);
    connect(m_undo, &QPushButton::clicked, this, &GraphPage::undoArrange);
    connect(m_canvas, &GraphCanvas::positionsChanged, this, [this] {
        if (!m_arranging && !m_restoring) {
            m_beforeArrange = {};
            m_undo->hide();
        }
    });
    connect(m_canvas, &GraphCanvas::viewStateChanged, this, &GraphPage::saveView);
    root->addLayout(footer);
    auto *searchDelay = new QTimer(this);
    searchDelay->setSingleShot(true);
    searchDelay->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, searchDelay, qOverload<>(&QTimer::start));
    connect(m_search, &QLineEdit::textChanged, this, &GraphPage::saveView);
    connect(searchDelay, &QTimer::timeout, this, &GraphPage::render);
    connect(m_refresh, &QPushButton::clicked, this, &GraphPage::refreshRequested);
    connect(m_listToggle, &QPushButton::toggled, this, [this](bool list) {
        m_views->setCurrentIndex(list ? 1 : 0);
        saveView();
    });
    connect(m_canvas, &GraphCanvas::nodeSelected, this, &GraphPage::inspectNode);
    connect(m_canvas, &GraphCanvas::pointerInteractionFinished, this,
            &GraphPage::revealInspectedNode);
    connect(m_canvas, &GraphCanvas::relationSelected, this, &GraphPage::inspectRelation);
    connect(m_list, &QListView::activated, this, [this](const QModelIndex &index) {
        inspectNode(index.data(SessionRoles::Key).toString());
    });
    connect(m_canvas, &GraphCanvas::selectionCleared, this, [this] {
        if (!m_rendering) {
            m_selectedNode.clear();
            m_selectedRelation.clear();
            m_inspectorWanted = false;
            m_inspector->hide();
        }
    });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
                if (!m_rendering && current.isValid())
                    inspectNode(current.data(SessionRoles::Key).toString());
            });
    connect(close, &QPushButton::clicked, this, [this] {
        m_inspectorWanted = false;
        m_inspector->hide();
        saveView();
    });
    connect(m_focus, &QPushButton::clicked, this, [this] {
        m_neighborhood = m_selectedNode;
        render();
        saveView();
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
    if (!viewStateKey.isEmpty()) {
        m_viewStore = new GraphViewStateStore(viewStateKey, this);
        connect(m_viewStore, &GraphViewStateStore::loaded, this,
                [this](const QJsonObject &state, const QString &error) {
                    m_viewError = error;
                    // A late settings read cannot override an interaction that
                    // already happened in this window.
                    if (!m_viewTouched && !state.isEmpty() && !restoreViewState(state))
                        m_viewError = tr("Saved layout could not be loaded");
                    m_viewLoaded = true;
                    if (m_haveProjection && m_viewError.isEmpty())
                        m_viewStore->save(viewState());
                    updateFreshness();
                });
        connect(m_viewStore, &GraphViewStateStore::saved, this, [this](const QString &error) {
            m_viewError = error;
            updateFreshness();
        });
    }
}
GraphPage::~GraphPage() {
    if (m_viewStore) {
        if (m_viewTouched)
            m_viewStore->save(viewState());
        m_viewStore->flush();
    }
}
void GraphPage::saveView() {
    if (m_restoring || m_rendering || m_arranging)
        return;
    m_viewTouched = true;
    if (m_viewStore)
        m_viewStore->save(viewState());
}
void GraphPage::arrangeVisible() {
    if (!m_canvas->nodeCount())
        return;
    m_beforeArrange = m_canvas->viewState();
    m_arranging = true;
    m_canvas->resetLayout();
    m_canvas->fitGraph();
    m_arranging = false;
    m_undo->show();
    saveView();
}
void GraphPage::undoArrange() {
    if (m_beforeArrange.isEmpty())
        return;
    auto previous = m_beforeArrange;
    previous["selected"] = m_canvas->viewState()["selected"];
    m_canvas->restoreViewState(previous);
    m_beforeArrange = {};
    m_undo->hide();
    saveView();
}
void GraphPage::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    if (m_viewStore && m_viewTouched) {
        m_viewStore->save(viewState());
        m_viewStore->flush();
    }
}

bool GraphPage::setProjection(const QList<Node> &nodes, const QList<Relation> &relations,
                              const QHash<QString, QString> &aliases) {
    if (nodes.size() > 2000 || relations.size() > 10000)
        return false;
    QSet<QString> ids, edges;
    for (const auto &node : nodes) {
        if (node.card.id.isEmpty() || node.card.id.size() > 512 || ids.contains(node.card.id))
            return false;
        ids.insert(node.card.id);
    }
    for (const auto &edge : relations) {
        if (edge.line.id.isEmpty() || edges.contains(edge.line.id) ||
            !ids.contains(edge.line.from) || !ids.contains(edge.line.to))
            return false;
        edges.insert(edge.line.id);
    }
    m_aliases = aliases;
    const auto previous = viewState();
    const auto upgraded = applyAliases(previous);
    if (!m_beforeArrange.isEmpty())
        m_beforeArrange =
            applyAliases(QJsonObject{{"canvas", m_beforeArrange}})["canvas"].toObject();
    m_nodes = nodes;
    m_relations = relations;
    m_haveProjection = true;
    render();
    if (previous != upgraded)
        restoreViewState(upgraded);
    // Remember new automatically placed nodes too, after the old saved layout
    // has been applied. Ordinary identical refreshes are coalesced by the store.
    if (m_viewStore && m_viewLoaded && m_viewError.isEmpty())
        m_viewStore->save(viewState());
    return true;
}
void GraphPage::render() {
    const auto query = m_search->text().trimmed();
    QSet<QString> visible, primary, archives;
    for (const auto &node : m_nodes) {
        if (node.archived)
            archives.insert(node.card.id);
        const auto &roles = node.card.roles;
        const QString text = QStringList{roles.value(SessionRoles::Title).toString(),
                                         roles.value(SessionRoles::Host).toString(),
                                         roles.value(SessionRoles::Agent).toString(),
                                         roles.value(SessionRoles::Meta).toString(),
                                         node.project,
                                         node.identity,
                                         node.card.id}
                                 .join(' ');
        if ((m_showArchives || !node.archived) &&
            (m_showCompleted || !node.completedChild ||
             roles.value(SessionRoles::Attention).toBool() ||
             roles.value(SessionRoles::Draft).toBool()) &&
            (m_project.isEmpty() || (node.card.group.isEmpty() ? QStringLiteral("@unassigned")
                                                               : node.card.group) == m_project) &&
            (!m_attentionOnly || roles.value(SessionRoles::Attention).toBool() ||
             roles.value(SessionRoles::Unread).toBool()) &&
            (query.isEmpty() || text.contains(query, Qt::CaseInsensitive)))
            primary.insert(node.card.id);
    }
    visible = primary;
    // An archived endpoint remains useful context for a visible current
    // conversation; unrelated archives remain opt-in.
    for (const auto &edge : m_relations)
        if (primary.contains(edge.line.from) || primary.contains(edge.line.to)) {
            if (archives.contains(edge.line.from))
                visible.insert(edge.line.from);
            if (archives.contains(edge.line.to))
                visible.insert(edge.line.to);
        }
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
    m_arrange->setEnabled(!nodes.isEmpty());
    m_count->setText(
        tr("%1 of %2 sessions and agents · %3 connections%4")
            .arg(nodes.size())
            .arg(m_nodes.size())
            .arg(edges.size())
            .arg(m_neighborhood.isEmpty() ? QString() : tr(" · Focused neighborhood")));
    m_filters->setText(m_project.isEmpty() && !m_attentionOnly && m_neighborhood.isEmpty() &&
                               !m_showArchives && !m_showCompleted
                           ? tr("Filters")
                           : tr("Filters (%1)")
                                 .arg(int(!m_project.isEmpty()) + int(m_attentionOnly) +
                                      int(!m_neighborhood.isEmpty()) + int(m_showArchives) +
                                      int(m_showCompleted)));
    if (m_inspectorWanted)
        inspectSelection();
}
void GraphPage::inspectNode(const QString &id) {
    if (m_rendering)
        return;
    const bool opening = !m_inspectorWanted;
    m_selectedNode = id;
    m_selectedRelation.clear();
    m_inspectorWanted = true;
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
    if (QApplication::mouseButtons() != Qt::NoButton) {
        // Resizing the view during QGraphicsItem's press/drag changes the
        // scene coordinates beneath the pointer, moving a card on release.
        m_deferredInspector = true;
        m_pendingReveal = id;
    } else {
        inspectSelection();
        if (opening)
            m_pendingReveal = id;
        revealInspectedNode();
    }
    saveView();
}
void GraphPage::revealInspectedNode() {
    if (m_pendingReveal.isEmpty())
        return;
    // Let the splitter finish shrinking the canvas. Never move the camera
    // beneath a held pointer or recenter on an ordinary projection refresh.
    QTimer::singleShot(0, this, [this] {
        if (QApplication::mouseButtons() != Qt::NoButton)
            return;
        if (m_deferredInspector) {
            m_deferredInspector = false;
            if (m_inspectorWanted && m_pendingReveal == m_selectedNode)
                inspectSelection();
            revealInspectedNode();
            return;
        }
        const auto id = std::exchange(m_pendingReveal, QString());
        if (id.isEmpty() || id != m_selectedNode || !m_inspectorWanted || m_overlay ||
            !m_canvas->isVisible())
            return;
        const auto bounds = m_canvas->nodeRect(id);
        if (!bounds.isEmpty())
            m_canvas->ensureVisible(bounds, 24, 24);
    });
}
void GraphPage::inspectRelation(const QString &id) {
    if (m_rendering)
        return;
    m_selectedRelation = id;
    m_selectedNode.clear();
    m_inspectorWanted = true;
    inspectSelection();
    saveView();
}
void GraphPage::inspectSelection() {
    if (m_deferredInspector && QApplication::mouseButtons() != Qt::NoButton)
        return;
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
                                        edge.line.description, edge.evidence, edge.events.size(),
                                        edge.events.isEmpty() ? QJsonValue() : edge.events.first(),
                                        edge.events.isEmpty() ? QJsonValue() : edge.events.last()});
    const auto currentSignature = QJsonDocument(signature).toJson(QJsonDocument::Compact);
    if (!m_inspector->isHidden() && currentSignature == m_inspectionSignature) {
        updateEvidenceAction();
        return;
    }
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
    m_evidencePanel->hide();
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
    renderEvidence();
    // Connections select edges; endpoint rows select their exact node.
    m_connections->verticalScrollBar()->setValue(oldScroll);
    m_inspector->show();
    arrangeInspector();
}
void GraphPage::renderEvidence() {
    const auto edge =
        std::find_if(m_relations.cbegin(), m_relations.cend(),
                     [this](const Relation &r) { return r.line.id == m_selectedRelation; });
    if (edge == m_relations.cend() || edge->events.isEmpty()) {
        m_evidencePanel->hide();
        return;
    }
    const auto previous = m_evidence->currentItem()
                              ? m_evidence->currentItem()->data(Qt::UserRole).toString()
                              : QString();
    if (m_evidenceRelation != edge->line.id) {
        m_evidenceRelation = edge->line.id;
        m_evidenceOffset = 0;
    }
    m_evidenceOffset = qBound(0, m_evidenceOffset, int((edge->events.size() - 1) / 50) * 50);
    const int end = qMin(m_evidenceOffset + 50, int(edge->events.size()));
    {
        const QSignalBlocker block(m_evidence);
        m_evidence->clear();
        int selected = 0;
        for (int i = m_evidenceOffset; i < end; ++i) {
            const auto event = edge->events[i].toObject();
            const auto request = event["request_id"].toString();
            auto *item = new QListWidgetItem(
                tr("%1 · Submitted\nRequest %2")
                    .arg(QDateTime::fromSecsSinceEpoch(qint64(event["at"].toDouble()))
                             .toLocalTime()
                             .toString("d MMM HH:mm:ss"),
                         request.left(12)),
                m_evidence);
            item->setData(Qt::UserRole, request);
            item->setToolTip(tr("Request: %1\nEvidence: %2\nConfirmed terminal/native submission; "
                                "response and task completion are not implied.")
                                 .arg(request, event["id"].toString()));
            if (request == previous)
                selected = i - m_evidenceOffset;
        }
        m_evidence->setCurrentRow(selected);
    }
    m_evidenceCount->setText(tr("%1–%2 of %3 receipts in loaded history")
                                 .arg(m_evidenceOffset + 1)
                                 .arg(end)
                                 .arg(edge->events.size()));
    m_evidenceNewer->setEnabled(m_evidenceOffset > 0);
    m_evidenceOlder->setEnabled(end < edge->events.size());
    updateEvidenceAction();
    m_evidencePanel->show();
}
void GraphPage::updateEvidenceAction() {
    const auto edge =
        std::find_if(m_relations.cbegin(), m_relations.cend(),
                     [this](const Relation &r) { return r.line.id == m_selectedRelation; });
    if (edge == m_relations.cend()) {
        m_evidenceOpen->setEnabled(false);
        return;
    }
    const auto node = std::find_if(m_nodes.cbegin(), m_nodes.cend(),
                                   [&](const Node &n) { return n.card.id == edge->line.to; });
    int current = 0, available = 0;
    if (node != m_nodes.cend())
        for (const auto &t : node->targets)
            if (t.available && t.agent.isEmpty()) {
                ++available;
                if (t.archive.isEmpty())
                    ++current;
            }
    m_evidenceOpen->setEnabled(m_evidence->currentItem() &&
                               (current == 1 || (current == 0 && available == 1)));
    m_evidenceOpen->setToolTip(
        m_evidenceOpen->isEnabled()
            ? tr("Read this exact delivery receipt from the recipient computer "
                 "and open it in Activity.")
            : tr("The recipient needs one available exact binding. Open its node "
                 "to inspect the available bindings."));
}
std::optional<GraphPage::Target> GraphPage::target(const QString &host, const QString &name,
                                                   const QString &run, const QString &conversation,
                                                   const QString &archive,
                                                   const QString &agent) const {
    std::optional<Target> result;
    for (const auto &node : m_nodes)
        for (const auto &t : node.targets)
            if (t.host == host && t.name == name && t.run == run &&
                t.conversation == conversation && t.archive == archive && t.agent == agent &&
                t.available) {
                if (result)
                    return std::nullopt;
                result = t;
            }
    return result;
}
bool GraphPage::focusTarget(const QString &host, const QString &name, const QString &run,
                            const QString &conversation, const QString &archive) {
    QString found;
    for (const auto &node : m_nodes)
        for (const auto &t : node.targets)
            if (t.host == host && t.name == name && t.run == run &&
                t.conversation == conversation && t.archive == archive && t.agent.isEmpty()) {
                if (!found.isEmpty() && found != node.card.id)
                    return false;
                found = node.card.id;
            }
    if (found.isEmpty())
        return false;
    focusNode(found);
    return true;
}
void GraphPage::focusNode(const QString &id) {
    for (const auto &node : m_nodes)
        if (node.card.id == id) {
            m_showArchives |= node.archived;
            m_showCompleted |= node.completedChild;
            break;
        }
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
    saveView();
}
void GraphPage::setFreshness(const QString &summary, const QString &details) {
    m_freshnessSummary = summary;
    m_freshnessDetails = details;
    updateFreshness();
}
void GraphPage::updateFreshness() {
    const QString summary = m_viewError.isEmpty()
                                ? m_freshnessSummary
                                : m_viewError + tr(" · changes kept in this window");
    m_freshness->setText(summary);
    m_freshness->setToolTip(m_freshnessDetails + "\n" +
                            tr("Positions, zoom and camera are saved on this "
                               "device. Refresh keeps your layout."));
    m_freshness->setAccessibleName(summary);
}
void GraphPage::rebuildFilters() {
    m_filterMenu->clear();
    auto *all = m_filterMenu->addAction(tr("All current sessions"));
    connect(all, &QAction::triggered, this, [this] {
        m_project.clear();
        m_neighborhood.clear();
        m_attentionOnly = false;
        m_showArchives = false;
        m_showCompleted = false;
        m_search->clear();
        render();
        saveView();
    });
    auto *attention = m_filterMenu->addAction(tr("Needs attention"));
    attention->setCheckable(true);
    attention->setChecked(m_attentionOnly);
    connect(attention, &QAction::toggled, this, [this](bool enabled) {
        m_attentionOnly = enabled;
        render();
        saveView();
    });
    auto *archives = m_filterMenu->addAction(tr("Archived sessions"));
    archives->setObjectName("graphShowArchives");
    archives->setCheckable(true);
    archives->setChecked(m_showArchives);
    connect(archives, &QAction::toggled, this, [this](bool on) {
        m_showArchives = on;
        render();
        saveView();
    });
    auto *completed = m_filterMenu->addAction(tr("Completed agents"));
    completed->setObjectName("graphShowCompleted");
    completed->setCheckable(true);
    completed->setChecked(m_showCompleted);
    connect(completed, &QAction::toggled, this, [this](bool on) {
        m_showCompleted = on;
        render();
        saveView();
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
            saveView();
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
    setStyleSheet(QString("#graphPage {background:%4;color:%5;} #graphPage QLabel "
                          "{color:%5;} #graphPage "
                          "QLineEdit,#graphPage QListView "
                          "{background:%1;color:%5;border:1px solid "
                          "%2;border-radius:5px;padding:5px;} #graphPage "
                          "QListView::item:selected "
                          "{background:%6;color:%5;} #graphPage QPushButton "
                          "{background:%1;color:%5;border:1px solid "
                          "%2;border-radius:6px;padding:6px "
                          "10px;min-height:20px;} #graphPage QPushButton:disabled "
                          "{color:%3;} #graphPage "
                          "QPushButton:hover {border-color:%3;} #graphInspectorContent "
                          "{background:%1;} "
                          "#graphPage QScrollArea {background:transparent;border:0;} "
                          "#graphInspector "
                          "{background:%1;border:1px solid %2;border-radius:8px;} "
                          "#graphInspectorTitle "
                          "{font-size:15px;font-weight:600;} #graphScope,#graphFreshness "
                          "{color:%3;}")
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
            {"archives", m_showArchives},
            {"completed", m_showCompleted},
            {"neighborhood", m_neighborhood},
            {"selectedNode", m_selectedNode},
            {"selectedRelation", m_selectedRelation},
            {"inspector", m_inspectorWanted},
            {"list", m_listToggle->isChecked()}};
}
QJsonObject GraphPage::applyAliases(QJsonObject state) const {
    auto canvas = state["canvas"].toObject();
    auto positions = canvas["positions"].toObject();
    for (auto it = m_aliases.cbegin(); it != m_aliases.cend(); ++it) {
        if (it.key() == it.value() || it.key().isEmpty() || it.value().isEmpty())
            continue;
        if (positions.contains(it.key())) {
            if (!positions.contains(it.value()))
                positions[it.value()] = positions[it.key()];
            positions.remove(it.key());
        }
        for (const auto &field : {"selectedNode", "neighborhood"})
            if (state[field] == it.key())
                state[field] = it.value();
        if (canvas["selected"] == it.key())
            canvas["selected"] = it.value();
    }
    canvas["positions"] = positions;
    state["canvas"] = canvas;
    return state;
}
bool GraphPage::restoreViewState(const QJsonObject &rawState) {
    const auto state = applyAliases(rawState);
    m_restoring = true;
    if (state["schema"] != 1 || !state["canvas"].isObject() ||
        !m_canvas->restoreViewState(state["canvas"].toObject())) {
        m_restoring = false;
        return false;
    }
    m_project = state["project"].toString().left(160);
    m_neighborhood = state["neighborhood"].toString().left(512);
    m_search->setText(state["query"].toString().left(512));
    m_attentionOnly = state["attention"].toBool();
    m_showArchives = state["archives"].toBool();
    m_showCompleted = state["completed"].toBool();
    m_selectedNode = state["selectedNode"].toString().left(512);
    m_selectedRelation = state["selectedRelation"].toString().left(512);
    m_inspectorWanted = state["inspector"].toBool();
    m_listToggle->setChecked(state["list"].toBool());
    render();
    if (!m_inspectorWanted)
        m_inspector->hide();
    m_restoring = false;
    return true;
}
