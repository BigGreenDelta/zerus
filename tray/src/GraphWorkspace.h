#pragma once
#include "GraphProjection.h"
#include <QWidget>

class HgsClient;
class GraphController;

// The workspace owns read-only graph transport, projection and presentation.
// Activity navigation remains the owning SessionsWindow's exact-identity check.
class GraphWorkspace : public QWidget {
    Q_OBJECT
  public:
    explicit GraphWorkspace(const QString &hgs, const QString &cachePath,
                            QWidget *parent = nullptr);
    static constexpr int MaximumBindings = 2000;
    void setRoutes(const QSet<QString> &routes);
    void setContext(const QSet<QString> &routes, const QList<GraphProjection::Binding> &bindings,
                    int omitted = 0, const QJsonObject &context = {});
    void setTheme(bool dark);
    void focusTarget(const QString &host, const QString &name, const QString &run,
                     const QString &conversation, const QString &archive);
    GraphPage *page() const { return m_page; }
  signals:
    void openRequested(const QString &host, const QString &name, const QString &run,
                       const QString &conversation, const QString &archive, const QString &agent);

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void project();
    void status();
    HgsClient *m_client;
    GraphController *m_controller;
    GraphPage *m_page;
    QList<GraphProjection::Binding> m_bindings;
    QJsonObject m_context, m_focusTarget;
    QSet<QString> m_routes;
    bool m_configured = false;
    quint64 m_transport = 0, m_request = 0, m_generation = 0;
    QString m_route;
    bool m_dirty = true, m_projecting = false;
    int m_omitted = 0, m_unresolved = 0, m_bindingOmitted = 0;
};
