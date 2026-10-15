#pragma once
#include "GraphCanvas.h"
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QListView;
class QListWidget;
class QSplitter;
class QStackedWidget;
class QMenu;
class GraphSummary;

// Inspection only. The owner must revalidate an exact target before navigating;
// this page never reads/answers a conversation or starts a native operation.
class GraphPage : public QWidget {
    Q_OBJECT
  public:
    struct Target {
        QString host, name, run, conversation, archive, agent;
        bool available = false;
    };
    struct Node {
        GraphCanvas::Node card;
        QString project, identity, detail;
        QList<Target> targets;
    };
    struct Relation {
        GraphCanvas::Relation line;
        QString evidence;
    };
    explicit GraphPage(QWidget *parent = nullptr);
    bool setProjection(const QList<Node> &nodes, const QList<Relation> &relations);
    void setTheme(bool dark);
    void setFreshness(const QString &summary, const QString &details = {});
    void focusNode(const QString &id);
    void focusProject(const QString &id);
    GraphCanvas *canvas() const { return m_canvas; }
    QJsonObject viewState() const;
    bool restoreViewState(const QJsonObject &state);
  signals:
    void refreshRequested();
    void openRequested(const QString &host, const QString &name, const QString &run,
                       const QString &conversation, const QString &archive, const QString &agent);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    void render();
    void inspectNode(const QString &id);
    void inspectRelation(const QString &id);
    void inspectSelection();
    void arrangeInspector();
    void rebuildFilters();
    QList<Node> m_nodes;
    QList<Relation> m_relations;
    GraphCanvas *m_canvas;
    QLineEdit *m_search;
    QLabel *m_count, *m_freshness, *m_title, *m_details;
    QPushButton *m_refresh, *m_filters, *m_listToggle, *m_open, *m_focus;
    QWidget *m_inspector;
    GraphSummary *m_summary;
    QListView *m_list;
    QListWidget *m_connections, *m_targets;
    QSplitter *m_splitter;
    QStackedWidget *m_views;
    QMenu *m_filterMenu;
    QByteArray m_inspectionSignature;
    QString m_selectedNode, m_selectedRelation, m_project, m_neighborhood;
    bool m_attentionOnly = false, m_dark = true, m_overlay = false, m_rendering = false;
};
