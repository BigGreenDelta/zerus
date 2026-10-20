#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// OpenCode models and providers as hgs reports them (account row
// `model_options`, `hgs account models`, session details), pinned by
// tests/fixtures/opencode-provider-catalog.json. Everything here is display
// data from another computer: entries are validated and bounded, never trusted.
namespace OpenCodeModels {
struct Model {
    QString value, label, provider, providerLabel;
    bool free = false, favorite = false, isDefault = false;
    QStringList efforts, effortLabels;
};
struct Provider { QString id, label, kind, activeLabel; int credentials = -1, models = -1; bool folder = false; };
struct Group { QString provider, label; bool folder = false; QList<Model> models; };
// Non-empty, at most 256 UTF-8 bytes, contains '/', no control characters.
bool validValue(const QString &value);
// Valid, distinct entries (value, else id), at most `limit`; efforts as OpenCodeEffort::valid, at most 16.
QList<Model> parse(const QJsonArray &entries, int limit);
// Valid, distinct provider rows, at most `limit`. Only `id` and `label` are
// certain; a missing kind is empty and missing counts are -1.
QList<Provider> providers(const QJsonArray &entries, int limit = 64);
// Provider groups in order of first appearance; models keep list order.
QList<Group> group(const QList<Model> &models, const QList<Provider> &providers = {});
// Every whitespace-separated word must match label, value or provider label (case-insensitive).
QList<Model> filter(const QList<Model> &models, const QString &query);
QString kindText(const QString &kind);
// "Default", "Favorite", "Free" in that order, joined by " · ".
QString marks(const Model &model);
// An OpenCode row or session details from an hgs that has `account models` and
// `account favorite`; every such hgs reports `model_options_truncated`.
bool catalogSupported(const QJsonObject &rowOrDetails);
// "Update Zerus on %1 to search all models and star favorites."
QString updateText(const QString &computer);
// A failure of those commands for display: an older hgs answers with its usage
// text (`usage: hgs account …`), shown as updateText(computer).
QString errorText(const QString &error, const QString &computer);
}
