#pragma once

#include "GraphPage.h"
#include "SessionGraphStore.h"
#include <QSet>

// Join host-owned graph facts to current workspace presentation. A mutable name
// is never enough to attach a live card or authorize navigation.
namespace GraphProjection {
struct Binding {
    QString route, name, run, conversation, provider, archive, agent;
    QString project, projectLabel;
    QMap<int, QVariant> roles;
    bool available = false;
    QJsonValue generation;
    bool completed = false;
};
struct Result {
    QList<GraphPage::Node> nodes;
    QList<GraphPage::Relation> relations;
    QHash<QString, QString> aliases;
    int unresolved = 0, omitted = 0;
};
Result compose(const SessionGraphStore &store, const QList<Binding> &bindings,
               const QSet<QString> &confirmedRoutes, int limit = 2000,
               const QJsonObject &context = {});
} // namespace GraphProjection
