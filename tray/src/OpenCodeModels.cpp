#include "OpenCodeModels.h"
#include "OpenCodeEffort.h"
#include <QCoreApplication>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
bool controlText(const QString &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.category() == QChar::Other_Control; });
}
// Catalog text is shown only when it is a short line: anything longer, with a
// control character or of another JSON type reads as empty.
QString shortText(const QJsonValue &value, int limit)
{
    const auto text = value.toString();
    return text.size() > limit || controlText(text) ? QString() : text;
}
// Provider IDs as hgs accepts them: at most 200 UTF-8 bytes, no '/', whitespace or control character.
bool validProvider(const QString &id)
{
    return !id.isEmpty() && id.toUtf8().size() <= 200 && !id.contains(QLatin1Char('/'))
        && std::none_of(id.cbegin(), id.cend(), [](QChar c) { return c.isSpace() || c.category() == QChar::Other_Control; });
}
}

namespace OpenCodeModels {
bool validValue(const QString &value)
{
    return !value.isEmpty() && value.toUtf8().size() <= 256 && value.contains(QLatin1Char('/')) && !controlText(value);
}

QList<Model> parse(const QJsonArray &entries, int limit)
{
    QList<Model> models; QSet<QString> seen;
    for (const auto &entry : entries) {
        if (models.size() >= limit) break;
        const auto option = entry.toObject();
        Model model; model.value = option["value"].toString(option["id"].toString());
        if (!validValue(model.value) || seen.contains(model.value)) continue;
        seen.insert(model.value);
        model.label = shortText(option["label"], 160); if (model.label.isEmpty()) model.label = model.value;
        // The provider is the value's prefix unless the entry names a valid one.
        model.provider = option["provider"].toString();
        if (!validProvider(model.provider)) model.provider = model.value.left(model.value.indexOf(QLatin1Char('/')));
        model.providerLabel = shortText(option["provider_label"], 160); if (model.providerLabel.isEmpty()) model.providerLabel = model.provider;
        model.free = option["free"].toBool(); model.favorite = option["favorite"].toBool(); model.isDefault = option["default"].toBool();
        // Effort options are this model's variants, as New session reads them:
        // at most 64 read and 16 kept, each valid and distinct.
        const auto variants = option["effort_options"].toArray(), variantLabels = option["effort_labels"].toArray();
        for (qsizetype index = 0; index < qMin<qsizetype>(64, variants.size()) && model.efforts.size() < 16; ++index) {
            const auto effort = variants.at(index).toString();
            if (!OpenCodeEffort::valid(effort) || model.efforts.contains(effort)) continue;
            auto effortLabel = shortText(variantLabels.at(index), 80); if (effortLabel.isEmpty()) effortLabel = effort;
            model.efforts << effort; model.effortLabels << effortLabel;
        }
        models << model;
    }
    return models;
}

QList<Provider> providers(const QJsonArray &entries, int limit)
{
    QList<Provider> result; QSet<QString> seen;
    for (const auto &entry : entries) {
        if (result.size() >= limit) break;
        const auto row = entry.toObject();
        Provider provider; provider.id = row["id"].toString();
        if (!validProvider(provider.id) || seen.contains(provider.id)) continue;
        seen.insert(provider.id);
        provider.label = shortText(row["label"], 160); if (provider.label.isEmpty()) provider.label = provider.id;
        provider.kind = shortText(row["kind"], 32); provider.activeLabel = shortText(row["active_label"], 160);
        provider.credentials = qMax(-1, row["credential_count"].toInt(-1)); provider.models = qMax(-1, row["model_count"].toInt(-1));
        provider.folder = row["folder"].toBool();
        result << provider;
    }
    return result;
}

QList<Group> group(const QList<Model> &models, const QList<Provider> &providers)
{
    QList<Group> groups; QHash<QString, qsizetype> positions;
    for (const auto &model : models) {
        auto position = positions.value(model.provider, -1);
        if (position < 0) {
            position = groups.size(); positions.insert(model.provider, position);
            // Named by its first model, else by the provider row, else by the provider ID.
            Group added; added.provider = model.provider; added.label = model.providerLabel;
            const auto known = std::find_if(providers.cbegin(), providers.cend(), [&](const Provider &p) { return p.id == model.provider; });
            if (known != providers.cend()) { if (added.label.isEmpty()) added.label = known->label; added.folder = known->folder; }
            groups << added;
        }
        groups[position].models << model;
    }
    for (auto &target : groups) if (target.label.isEmpty()) target.label = target.provider;
    return groups;
}

QList<Model> filter(const QList<Model> &models, const QString &query)
{
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    const auto words = query.toCaseFolded().split(spaces, Qt::SkipEmptyParts);
    if (words.isEmpty()) return models;
    QList<Model> matches;
    for (const auto &model : models) {
        const auto label = model.label.toCaseFolded(), value = model.value.toCaseFolded(), provider = model.providerLabel.toCaseFolded();
        if (std::all_of(words.cbegin(), words.cend(), [&](const QString &word) { return label.contains(word) || value.contains(word) || provider.contains(word); }))
            matches << model;
    }
    return matches;
}

QString kindText(const QString &kind)
{
    static const struct { const char *kind, *text; } kinds[] = {
        {"free", QT_TRANSLATE_NOOP("OpenCodeModels", "Free, no key")},
        {"key", QT_TRANSLATE_NOOP("OpenCodeModels", "API key")},
        {"oauth", QT_TRANSLATE_NOOP("OpenCodeModels", "Signed in")},
        {"env", QT_TRANSLATE_NOOP("OpenCodeModels", "Environment variable")},
        {"config", QT_TRANSLATE_NOOP("OpenCodeModels", "Configuration")},
        {"command", QT_TRANSLATE_NOOP("OpenCodeModels", "Command")},
        {"external", QT_TRANSLATE_NOOP("OpenCodeModels", "External credentials")},
        {"local", QT_TRANSLATE_NOOP("OpenCodeModels", "Detected locally")},
    };
    for (const auto &entry : kinds) if (kind == QLatin1String(entry.kind)) return QCoreApplication::translate("OpenCodeModels", entry.text);
    // "other" and kinds a newer hgs may report.
    return QCoreApplication::translate("OpenCodeModels", "Connected");
}

QString marks(const Model &model)
{
    QStringList parts;
    if (model.isDefault) parts << QCoreApplication::translate("OpenCodeModels", "Default");
    if (model.favorite) parts << QCoreApplication::translate("OpenCodeModels", "Favorite");
    if (model.free) parts << QCoreApplication::translate("OpenCodeModels", "Free");
    return parts.join(QStringLiteral(" · "));
}

bool catalogSupported(const QJsonObject &rowOrDetails)
{
    return rowOrDetails.contains(QStringLiteral("model_options_truncated"));
}

QString updateText(const QString &computer)
{
    return QCoreApplication::translate("OpenCodeModels", "Update Zerus on %1 to search all models and star favorites.").arg(computer);
}

QString errorText(const QString &error, const QString &computer)
{
    // hgs prefixes its errors with "hgs: ", and a remote answer can follow ssh
    // warnings, so the usage line is looked for in the first lines.
    const auto lines = error.left(4000).split(QLatin1Char('\n'));
    for (qsizetype index = 0; index < qMin<qsizetype>(16, lines.size()); ++index) {
        auto line = lines.at(index).trimmed();
        while (line.startsWith(QLatin1String("hgs: "))) line = line.mid(5).trimmed();
        if (line.startsWith(QLatin1String("usage: hgs account"))) return updateText(computer);
    }
    return error;
}
}
