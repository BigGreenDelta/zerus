#pragma once

#include <QGraphicsView>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QStandardItemModel>

class SessionDelegate;
class GraphNodeItem;
class GraphRelationItem;
class GraphGroupItem;

// Pure presentation: callers supply already scoped, verified identities. This
// widget neither polls a host nor opens/reads/answers a conversation.
class GraphCanvas : public QGraphicsView {
    Q_OBJECT
  public:
    struct Node {
        QString id, group;
        QMap<int, QVariant> roles;
    };
    struct Relation {
        QString id, from, to, kind, description;
    };
    explicit GraphCanvas(QWidget *parent = nullptr);
    ~GraphCanvas() override;
    bool setGraph(const QList<Node> &nodes, const QList<Relation> &relations);
    void setTheme(bool dark);
    void selectNode(const QString &id);
    void fitGraph();
    void zoomBy(qreal factor);
    void resetLayout();
    void setGroupLabels(const QMap<QString, QString> &labels);
    QJsonObject viewState() const;
    bool restoreViewState(const QJsonObject &state);
    QStandardItemModel *nodeModel() { return &m_model; }
    QPointF nodePosition(const QString &id) const;
    QRectF nodeRect(const QString &id) const;
    QRectF groupRect(const QString &id) const;
    QString selectedNode() const;
    int nodeCount() const { return m_nodes.size(); }
    int relationCount() const { return m_relations.size(); }
  signals:
    void nodeSelected(const QString &id);
    void relationSelected(const QString &id);
    void selectionCleared();
    void positionsChanged();
    void viewStateChanged();
    void pointerInteractionFinished();

  protected:
    bool event(QEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    bool nativeGesture(QEvent *event);
    void zoomAt(qreal factor, const QPointF &anchor);
    friend class GraphNodeItem;
    void moved(const QString &id);
    void updateBounds();
    QPointF freePosition(const QString &group) const;
    void rememberCamera();
    void trimPositions();
    QStandardItemModel m_model;
    SessionDelegate *m_delegate;
    QHash<QString, GraphNodeItem *> m_nodes;
    QHash<QString, GraphRelationItem *> m_relations;
    QHash<QString, QList<GraphRelationItem *>> m_incident;
    QHash<QString, QPointF> m_positions;
    QHash<QString, QString> m_positionGroups;
    QMap<QString, QString> m_groupLabels;
    QHash<QString, GraphGroupItem *> m_groups;
    QStringList m_order;
    QPointF m_center;
    QString m_selection;
    bool m_haveCamera = false;
    bool m_dark = true, m_applying = false;
};
