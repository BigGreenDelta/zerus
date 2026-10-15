#include "GraphCanvas.h"
#include "SessionCardDelegate.h"

#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QSignalBlocker>
#include <QStyleOptionGraphicsItem>
#include <QWheelEvent>
#include <cmath>
#include <functional>

namespace {
constexpr qreal CardWidth = 340;
bool finitePoint(QPointF p) {
    return std::isfinite(p.x()) && std::isfinite(p.y()) && qAbs(p.x()) <= 100000 &&
           qAbs(p.y()) <= 100000;
}
} // namespace

class GraphNodeItem : public QGraphicsItem {
  public:
    GraphNodeItem(GraphCanvas *view, QString key, QPersistentModelIndex row)
        : owner(view), id(std::move(key)), index(std::move(row)) {
        setFlags(ItemIsSelectable | ItemIsMovable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        setZValue(1);
    }
    QRectF boundingRect() const override { return {0, 0, CardWidth, height}; }
    void setHeight(qreal value) {
        if (height != value) {
            prepareGeometryChange();
            height = value;
        }
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *style, QWidget *) override {
        QStyleOptionViewItem option;
        option.initFrom(owner->viewport());
        option.widget = owner->viewport();
        option.rect = boundingRect().toRect();
        option.state.setFlag(QStyle::State_Selected, isSelected());
        option.state.setFlag(QStyle::State_MouseOver,
                             style->state.testFlag(QStyle::State_MouseOver));
        // The exact Sessions painter supplies status/provider/machine badges,
        // configured colors, branch icon, font metrics and selection surface.
        owner->m_delegate->paintStandalone(p, option, index);
    }
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override {
        if (change == ItemPositionChange && !finitePoint(value.toPointF()))
            return pos();
        if (change == ItemPositionHasChanged)
            owner->moved(id);
        return QGraphicsItem::itemChange(change, value);
    }
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        const bool alreadySelected = isSelected();
        QGraphicsItem::mousePressEvent(event);
        if (alreadySelected && event->button() == Qt::LeftButton)
            emit owner->nodeSelected(id);
    }
    GraphCanvas *owner;
    QString id, group;
    QPersistentModelIndex index;
    qreal height = 104;
};

class GraphRelationItem : public QGraphicsPathItem {
  public:
    GraphRelationItem(GraphCanvas *view, GraphCanvas::Relation value, GraphNodeItem *a,
                      GraphNodeItem *b)
        : relation(std::move(value)), from(a), to(b), owner(view) {
        setFlag(ItemIsSelectable);
        setZValue(-1);
        setToolTip(relation.description);
        updatePath();
    }
    void updatePath() {
        const QRectF a = from->sceneBoundingRect(), b = to->sceneBoundingRect();
        const auto delta = b.center() - a.center();
        const bool horizontal = qAbs(delta.x()) >= qAbs(delta.y());
        const bool forward = horizontal ? delta.x() >= 0 : delta.y() >= 0;
        const QPointF start = horizontal ? QPointF(forward ? a.right() : a.left(), a.center().y())
                                         : QPointF(a.center().x(), forward ? a.bottom() : a.top());
        const QPointF end = horizontal ? QPointF(forward ? b.left() : b.right(), b.center().y())
                                       : QPointF(b.center().x(), forward ? b.top() : b.bottom());
        const qreal reach =
            qMax<qreal>(50, (horizontal ? qAbs(end.x() - start.x()) : qAbs(end.y() - start.y())) *
                                .45) *
            (forward ? 1 : -1);
        const QPointF control = horizontal ? QPointF(reach, 0) : QPointF(0, reach);
        QPainterPath p(start);
        if (from == to)
            p.cubicTo(start + QPointF(85, -100), start + QPointF(85, 100), start + QPointF(0, 20));
        else
            p.cubicTo(start + control, end - control, end);
        setPath(p);
    }
    QPainterPath shape() const override {
        QPainterPathStroker s;
        s.setWidth(12);
        return s.createStroke(path());
    }
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override {
        const bool alreadySelected = isSelected();
        QGraphicsPathItem::mousePressEvent(event);
        if (alreadySelected && event->button() == Qt::LeftButton)
            emit owner->relationSelected(relation.id);
    }
    QRectF boundingRect() const override {
        return QGraphicsPathItem::boundingRect().adjusted(-10, -10, 10, 10);
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        const QColor color = isSelected() ? (dark ? QColor("#c7eee0") : QColor("#14664c"))
                             : relation.kind == "message"
                                 ? (dark ? QColor("#75cbb1") : QColor("#227e61"))
                                 : (dark ? QColor("#71808e") : QColor("#7b8790"));
        p->setPen(QPen(color, isSelected() ? 2.5 : 1.5,
                       relation.kind == "reference" ? Qt::DotLine : Qt::SolidLine));
        p->setBrush(Qt::NoBrush);
        p->drawPath(path());
        const auto end = path().pointAtPercent(1), before = path().pointAtPercent(.97);
        const auto angle = std::atan2(end.y() - before.y(), end.x() - before.x());
        QPolygonF arrow;
        arrow << end << end - QPointF(std::cos(angle - .5) * 9, std::sin(angle - .5) * 9)
              << end - QPointF(std::cos(angle + .5) * 9, std::sin(angle + .5) * 9);
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawPolygon(arrow);
    }
    GraphCanvas::Relation relation;
    GraphNodeItem *from, *to;
    GraphCanvas *owner;
    bool dark = true;
};

class GraphGroupItem : public QGraphicsRectItem {
  public:
    GraphGroupItem() { setZValue(-10); }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        p->setPen(QPen(QColor(dark ? "#2b3540" : "#d9e1e5"), 1));
        p->setBrush(QColor(dark ? "#1a222a" : "#eaf0f2"));
        p->drawRoundedRect(rect(), 12, 12);
        auto font = p->font();
        font.setPixelSize(13);
        font.setWeight(QFont::Medium);
        p->setFont(font);
        p->setPen(QColor(dark ? "#a2adbc" : "#627082"));
        p->drawText(rect().adjusted(18, 10, -18, 0), Qt::AlignTop | Qt::AlignLeft, label);
    }
    QString label;
    bool dark = true;
};

GraphCanvas::GraphCanvas(QWidget *parent)
    : QGraphicsView(parent), m_model(this), m_delegate(new SessionDelegate(this)) {
    setObjectName("sessionGraphCanvas");
    setAccessibleName(tr("Session graph"));
    setAccessibleDescription(tr(
        "Select a session to inspect it. Use the list for keyboard and screen-reader navigation."));
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::Antialiasing);
    setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setFrameShape(QFrame::NoFrame);
    viewport()->setProperty("expansion", 1.0);
    viewport()->setProperty("compact", false);
    viewport()->setProperty("selectionIsInspection", true);
    connect(scene(), &QGraphicsScene::selectionChanged, this, [this] {
        if (m_applying)
            return;
        for (auto *item : scene()->selectedItems()) {
            if (auto *node = dynamic_cast<GraphNodeItem *>(item)) {
                emit nodeSelected(node->id);
                return;
            }
            if (auto *edge = dynamic_cast<GraphRelationItem *>(item)) {
                emit relationSelected(edge->relation.id);
                return;
            }
        }
        emit selectionCleared();
    });
    setTheme(true);
}
GraphCanvas::~GraphCanvas() {
    m_applying = true;
    scene()->clear();
    m_nodes.clear();
    m_relations.clear();
}

bool GraphCanvas::setGraph(const QList<Node> &nodes, const QList<Relation> &relations) {
    if (nodes.size() > 2000 || relations.size() > 10000)
        return false;
    QSet<QString> ids, edgeIds;
    for (const auto &node : nodes) {
        if (node.id.isEmpty() || ids.contains(node.id))
            return false;
        ids.insert(node.id);
    }
    for (const auto &edge : relations) {
        if (edge.id.isEmpty() || edgeIds.contains(edge.id) || !ids.contains(edge.from) ||
            !ids.contains(edge.to))
            return false;
        edgeIds.insert(edge.id);
    }
    const bool hadNodes = !m_nodes.isEmpty(), initial = !hadNodes && m_positions.isEmpty();
    const auto center = mapToScene(viewport()->rect().center());
    m_applying = true;
    const QSignalBlocker block(scene());
    // Remove relations before endpoint items, including replacements whose
    // endpoints or type changed. Ordinary refresh preserves item identities.
    QHash<QString, Relation> nextEdges;
    for (const auto &edge : relations)
        nextEdges.insert(edge.id, edge);
    for (const auto &key : m_relations.keys()) {
        auto *edge = m_relations.value(key);
        const auto next = nextEdges.value(key);
        if (!edgeIds.contains(key) || next.from != edge->relation.from ||
            next.to != edge->relation.to || next.kind != edge->relation.kind)
            delete m_relations.take(key);
    }
    m_incident.clear();
    for (const auto &key : m_nodes.keys())
        if (!ids.contains(key)) {
            auto *node = m_nodes.take(key);
            m_positions[key] = node->pos();
            m_model.removeRow(node->index.row());
            delete node;
            m_order.removeAll(key);
        }
    for (const auto &node : nodes) {
        auto *item = m_nodes.value(node.id);
        if (!item) {
            auto *row = new QStandardItem;
            m_model.appendRow(row);
            item = new GraphNodeItem(this, node.id, row->index());
            m_nodes.insert(node.id, item);
            scene()->addItem(item);
            const int slot = m_order.size();
            m_order.append(node.id);
            item->setPos(m_positions.value(
                node.id, QPointF((slot % 4) * (CardWidth + 64), (slot / 4) * 152)));
        }
        item->group = node.group;
        item->setHeight(node.roles.value(SessionRoles::ChildId).toString().isEmpty() ? 104 : 64);
        auto roles = node.roles;
        roles.insert(SessionRoles::Key, node.id);
        roles.insert(SessionRoles::WorkingSince, 0.0);
        roles.insert(Qt::DisplayRole, node.roles.value(SessionRoles::Title));
        // Replace obsolete roles as well as changed values, without resetting
        // the shared model or disturbing list/canvas selection.
        if (m_model.itemData(item->index) != roles) {
            m_model.clearItemData(item->index);
            m_model.setItemData(item->index, roles);
            item->update();
        }
        item->setToolTip(
            node.roles.value(Qt::ToolTipRole, node.roles.value(SessionRoles::Title)).toString());
    }
    for (const auto &edge : relations) {
        auto *item = m_relations.value(edge.id);
        if (!item) {
            item =
                new GraphRelationItem(this, edge, m_nodes.value(edge.from), m_nodes.value(edge.to));
            m_relations.insert(edge.id, item);
            scene()->addItem(item);
        }
        item->dark = m_dark;
        item->relation.description = edge.description;
        item->setToolTip(edge.description);
        item->updatePath();
        m_incident[edge.from].append(item);
        if (edge.to != edge.from)
            m_incident[edge.to].append(item);
    }
    if (m_positions.size() > 2000)
        for (const auto &key : m_positions.keys())
            if (!m_nodes.contains(key) && m_positions.size() > 2000)
                m_positions.remove(key);
    if (initial)
        resetLayout();
    updateBounds();
    if (hadNodes)
        centerOn(center);
    m_applying = false;
    return true;
}

void GraphCanvas::setTheme(bool dark) {
    m_dark = dark;
    viewport()->setProperty("hgsDark", dark);
    setBackgroundBrush(dark ? QColor("#171d24") : QColor("#f2f5f6"));
    for (auto *edge : m_relations) {
        edge->dark = dark;
        edge->update();
    }
    scene()->update();
    for (auto *group : m_groups) {
        group->dark = dark;
        group->update();
    }
}
void GraphCanvas::moved(const QString &id) {
    auto *node = m_nodes.value(id);
    if (!node)
        return;
    m_positions[id] = node->pos();
    for (auto *edge : m_incident.value(id))
        edge->updatePath();
    if (!m_applying) {
        updateBounds();
        emit positionsChanged();
    }
}
void GraphCanvas::setGroupLabels(const QMap<QString, QString> &labels) {
    m_groupLabels = labels;
    updateBounds();
}
void GraphCanvas::updateBounds() {
    QMap<QString, QRectF> regions;
    for (auto *node : m_nodes)
        regions[node->group] = regions.contains(node->group)
                                   ? regions[node->group].united(node->sceneBoundingRect())
                                   : node->sceneBoundingRect();
    for (const auto &key : m_groups.keys())
        if (!regions.contains(key))
            delete m_groups.take(key);
    for (auto it = regions.cbegin(); it != regions.cend(); ++it) {
        auto *group = m_groups.value(it.key());
        if (!group) {
            group = new GraphGroupItem;
            m_groups[it.key()] = group;
            scene()->addItem(group);
        }
        group->setRect(it.value().adjusted(-24, -46, 24, 24));
        group->label =
            m_groupLabels.value(it.key(), it.key().isEmpty() ? tr("Unassigned") : it.key());
        group->dark = m_dark;
        group->update();
    }
    scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-64, -64, 64, 64));
}
void GraphCanvas::selectNode(const QString &id) {
    if (auto *node = m_nodes.value(id)) {
        scene()->clearSelection();
        node->setSelected(true);
        ensureVisible(node, 40, 40);
    }
}
void GraphCanvas::fitGraph() {
    if (!m_nodes.isEmpty()) {
        fitInView(scene()->itemsBoundingRect().adjusted(-24, -24, 24, 24), Qt::KeepAspectRatio);
        if (transform().m11() > 1.25)
            zoomBy(1.25 / transform().m11());
    }
}
void GraphCanvas::zoomBy(qreal factor) {
    if (!std::isfinite(factor) || factor <= 0)
        return;
    const auto target = qBound(.15, transform().m11() * factor, 2.5);
    scale(target / transform().m11(), target / transform().m11());
}
void GraphCanvas::resetLayout() {
    const bool wasApplying = m_applying;
    m_applying = true;
    QMap<QString, QStringList> groups;
    for (const auto &id : m_order)
        groups[m_nodes.value(id)->group].append(id);
    qreal x = 0, y = 0, rowHeight = 0;
    for (const auto &ids : groups) {
        const int columns = qMin(2, ids.size());
        const qreal width = columns * (CardWidth + 64),
                    height = ((ids.size() + columns - 1) / columns) * 152 + 64;
        if (x > 0 && x + width > 1300) {
            x = 0;
            y += rowHeight;
            rowHeight = 0;
        }
        for (int i = 0; i < ids.size(); ++i)
            m_nodes.value(ids[i])->setPos(x + (i % columns) * (CardWidth + 64),
                                          y + (i / columns) * 152);
        x += width + 48;
        rowHeight = qMax(rowHeight, height);
    }
    m_applying = wasApplying;
    updateBounds();
    if (!wasApplying)
        emit positionsChanged();
}
QPointF GraphCanvas::nodePosition(const QString &id) const {
    const auto *node = m_nodes.value(id);
    return node ? node->pos() : QPointF();
}
QRectF GraphCanvas::nodeRect(const QString &id) const {
    const auto *node = m_nodes.value(id);
    return node ? node->sceneBoundingRect() : QRectF();
}
QString GraphCanvas::selectedNode() const {
    for (auto *item : scene()->selectedItems())
        if (auto *node = dynamic_cast<GraphNodeItem *>(item))
            return node->id;
    return {};
}
QJsonObject GraphCanvas::viewState() const {
    QJsonObject positions;
    for (auto it = m_positions.cbegin(); it != m_positions.cend(); ++it)
        positions[it.key()] = QJsonArray{it.value().x(), it.value().y()};
    for (auto it = m_nodes.cbegin(); it != m_nodes.cend(); ++it)
        positions[it.key()] = QJsonArray{it.value()->pos().x(), it.value()->pos().y()};
    const auto center = mapToScene(viewport()->rect().center());
    return {{"schema", 1},
            {"positions", positions},
            {"scale", transform().m11()},
            {"center", QJsonArray{center.x(), center.y()}},
            {"selected", selectedNode()}};
}
bool GraphCanvas::restoreViewState(const QJsonObject &state) {
    const auto positions = state["positions"].toObject();
    const qreal zoom = state["scale"].toDouble(-1);
    const auto center = state["center"].toArray();
    if (state["schema"] != 1 || positions.size() > 2000 || !std::isfinite(zoom) || zoom < .15 ||
        zoom > 2.5 || center.size() != 2 || !center[0].isDouble() || !center[1].isDouble() ||
        !finitePoint({center[0].toDouble(), center[1].toDouble()}))
        return false;
    QHash<QString, QPointF> parsed;
    for (auto it = positions.begin(); it != positions.end(); ++it) {
        const auto a = it.value().toArray();
        if (it.key().isEmpty() || it.key().size() > 512 || a.size() != 2 || !a[0].isDouble() ||
            !a[1].isDouble() || !finitePoint({a[0].toDouble(), a[1].toDouble()}))
            return false;
        parsed[it.key()] = {a[0].toDouble(), a[1].toDouble()};
    }
    m_applying = true;
    m_positions = parsed;
    for (auto it = m_nodes.begin(); it != m_nodes.end(); ++it)
        if (parsed.contains(it.key()))
            it.value()->setPos(parsed.value(it.key()));
    updateBounds();
    setTransform(QTransform::fromScale(zoom, zoom));
    centerOn(center[0].toDouble(), center[1].toDouble());
    m_applying = false;
    const auto selected = state["selected"].toString();
    if (m_nodes.contains(selected)) {
        scene()->clearSelection();
        m_nodes.value(selected)->setSelected(true);
    }
    return true;
}
void GraphCanvas::wheelEvent(QWheelEvent *event) {
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        zoomBy(std::pow(1.0015, event->angleDelta().y()));
        event->accept();
    } else
        QGraphicsView::wheelEvent(event);
}
void GraphCanvas::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        scene()->clearSelection();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) {
        zoomBy(1.2);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Minus) {
        zoomBy(1 / 1.2);
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}
