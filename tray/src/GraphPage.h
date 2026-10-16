#pragma once
#include "GraphCanvas.h"
#include <QJsonArray>
#include <QWidget>
#include <optional>

class QLabel;
class QLineEdit;
class QPushButton;
class QListView;
class QListWidget;
class QSplitter;
class QStackedWidget;
class QMenu;
class GraphSummary;
class GraphViewStateStore;

// Inspection only. The owner must revalidate an exact target before navigating;
// this page never reads/answers a conversation or starts a native operation.
class GraphPage : public QWidget {
    Q_OBJECT
  public:
    struct Target {
        QString host, name, run, conversation, archive, agent;
        bool available = false;
        QJsonObject metadata;
        QString machine;
    };
    struct Node {
        GraphCanvas::Node card;
        QString project, identity, detail;
        QList<Target> targets;
        bool archived = false, completedChild = false;
    };
    struct Relation {
        GraphCanvas::Relation line;
        QString evidence;
        QJsonArray events;
    };
    // An empty key disables local persistence for isolated preview/fixtures.
    explicit GraphPage(QWidget *parent = nullptr,
                       const QString &viewStateKey = QStringLiteral("workspace/graphView"));
    ~GraphPage() override;
    bool setProjection(const QList<Node> &nodes, const QList<Relation> &relations,
                       const QHash<QString, QString> &aliases = {});
    void setTheme(bool dark);
    void setFreshness(const QString &summary, const QString &details = {});
    void focusNode(const QString &id);
    bool focusTarget(const QString &host, const QString &name, const QString &run,
                     const QString &conversation, const QString &archive = {});
    void focusProject(const QString &id);
    GraphCanvas *canvas() const { return m_canvas; }
    QJsonObject viewState() const;
    bool restoreViewState(const QJsonObject &state);
    std::optional<Target> target(const QString &host, const QString &name, const QString &run,
                                 const QString &conversation, const QString &archive,
                                 const QString &agent) const;
  signals:
    void refreshRequested();
    void openRequested(const QString &host, const QString &name, const QString &run,
                       const QString &conversation, const QString &archive, const QString &agent);
    void evidenceRequested(const QString &host, const QString &name, const QString &run,
                           const QString &conversation, const QString &archive,
                           const QString &request);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void render();
    void inspectNode(const QString &id);
    void inspectRelation(const QString &id);
    void inspectSelection();
    void arrangeInspector();
    void revealInspectedNode();
    void rebuildFilters();
    void saveView();
    void arrangeVisible();
    void undoArrange();
    void updateFreshness();
    void renderEvidence();
    void updateEvidenceAction();
    QJsonObject applyAliases(QJsonObject state) const;
    QHash<QString, QString> m_aliases;
    QList<Node> m_nodes;
    QList<Relation> m_relations;
    GraphCanvas *m_canvas;
    QLineEdit *m_search;
    QLabel *m_count, *m_freshness, *m_title, *m_details;
    QPushButton *m_refresh, *m_filters, *m_listToggle, *m_open, *m_focus;
    QPushButton *m_arrange, *m_undo;
    GraphViewStateStore *m_viewStore = nullptr;
    QJsonObject m_beforeArrange;
    QString m_freshnessSummary, m_freshnessDetails, m_viewError;
    bool m_restoring = false, m_arranging = false, m_viewTouched = false;
    bool m_inspectorWanted = false;
    bool m_deferredInspector = false;
    bool m_viewLoaded = false, m_haveProjection = false;
    QWidget *m_inspector;
    GraphSummary *m_summary;
    QListView *m_list;
    QListWidget *m_connections, *m_targets;
    QListWidget *m_evidence;
    QLabel *m_evidenceCount;
    QPushButton *m_evidenceNewer, *m_evidenceOlder, *m_evidenceOpen;
    QWidget *m_evidencePanel;
    QString m_evidenceRelation;
    int m_evidenceOffset = 0;
    QSplitter *m_splitter;
    QStackedWidget *m_views;
    QMenu *m_filterMenu;
    QByteArray m_inspectionSignature;
    QString m_selectedNode, m_selectedRelation, m_project, m_neighborhood;
    QString m_pendingReveal;
    bool m_attentionOnly = false, m_showArchives = false, m_showCompleted = false;
    bool m_dark = true, m_overlay = false, m_rendering = false;
};
