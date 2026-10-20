#include "OpenCodeModelPicker.h"
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextDocument>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
// Bounds for data from another computer: hgs keeps at most 256 short-list
// entries, 10,000 catalog models and 64 favorites; the list renders 500 rows.
constexpr int MaxShortList = 256, MaxCatalog = 10000, MaxFavorites = 64, MaxRows = 500;
// Qt::UserRole holds the exact model value.
constexpr int LabelRole = Qt::UserRole + 1, UnavailableRole = Qt::UserRole + 2, FavoriteRole = Qt::UserRole + 3,
              FreeRole = Qt::UserRole + 4, DefaultRole = Qt::UserRole + 5;
// Reasons come from hgs or OpenCode: one bounded line of text.
QString noticeText(const QString &text)
{
    QString line;
    for (const QChar c : text.simplified()) {
        if (c.category() != QChar::Other_Control) line += c;
        if (line.size() > 400) return line.left(399) + QStringLiteral("…");
    }
    return line;
}
// Catalog text in a tooltip is shown as text, never read as markup.
QString plainTip(const QString &text) { return QStringLiteral("<p>") + text.toHtmlEscaped() + QStringLiteral("</p>"); }
// An owner's reason as a tooltip: bounded like a notice, and through plainTip
// whenever Qt would read it as markup (tooltips use Qt::mightBeRichText), so it
// always shows as literal text. Accessible text uses noticeText() directly.
QString reasonTip(const QString &reason)
{
    const auto text = noticeText(reason);
    return Qt::mightBeRichText(text) ? plainTip(text) : text;
}
// A value no list describes: its provider is the part before the first '/'.
OpenCodeModels::Model unlisted(const QString &value)
{
    OpenCodeModels::Model model; model.value = model.label = value;
    model.provider = model.providerLabel = value.left(value.indexOf(QLatin1Char('/')));
    return model;
}
}

OpenCodeModelPicker::OpenCodeModelPicker(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(6);
    m_search = new QLineEdit; m_search->setObjectName("openCodeModelSearch"); m_search->setPlaceholderText(tr("Search models"));
    m_search->setAccessibleName(tr("Search models")); m_search->setClearButtonEnabled(true); m_search->setMaxLength(200);
    layout->addWidget(m_search); setFocusProxy(m_search);
    m_list = new QTreeWidget; m_list->setObjectName("openCodeModelList"); m_list->setAccessibleName(tr("Models"));
    m_list->setColumnCount(2); m_list->setHeaderHidden(true); m_list->setRootIsDecorated(false);
    m_list->setItemsExpandable(false); m_list->setExpandsOnDoubleClick(false); m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection); m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->header()->setStretchLastSection(false); m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Fixed); m_list->setColumnWidth(1, 32);
    m_list->installEventFilter(this); layout->addWidget(m_list, 1);
    auto *footer = new QHBoxLayout; footer->setContentsMargins(0, 0, 0, 0); footer->setSpacing(8);
    m_all = new QPushButton(tr("All models")); m_all->setObjectName("openCodeAllModels"); m_all->setCheckable(true);
    m_all->setAutoDefault(false); m_all->setToolTip(tr("Search every model OpenCode offers"));
    m_notice = new QLabel; m_notice->setObjectName("openCodeModelNotice"); m_notice->setWordWrap(true); m_notice->setTextFormat(Qt::PlainText);
    footer->addWidget(m_all, 0, Qt::AlignTop); footer->addWidget(m_notice, 1); layout->addLayout(footer);
    // Search waits for a pause in typing: a catalog can hold 10,000 models.
    m_searchTimer = new QTimer(this); m_searchTimer->setSingleShot(true); m_searchTimer->setInterval(150);
    connect(m_searchTimer, &QTimer::timeout, this, &OpenCodeModelPicker::render);
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_searchTimer->start(); });
    connect(m_list, &QTreeWidget::itemClicked, this, &OpenCodeModelPicker::click);
    connect(m_all, &QPushButton::clicked, this, [this](bool checked) {
        m_failure.clear();
        // The first All models asks for the catalog; later clicks reuse a ready one.
        if (checked && !catalogReady() && !m_loading) { m_loading = true; render(); emit catalogRequested(); return; }
        render();
    });
    render();
}

void OpenCodeModelPicker::setShortList(const QJsonArray &options, bool truncated)
{
    // Owners pass the list on every refresh: an unchanged one keeps the scroll position.
    if (m_shortSource && *m_shortSource == options && m_shortTruncated == truncated) return;
    m_shortSource = options; m_shortTruncated = truncated;
    m_short = OpenCodeModels::parse(options, MaxShortList); render();
}

void OpenCodeModelPicker::setCatalog(const QJsonObject &catalog)
{
    m_loading = false; m_failure.clear(); m_hasCatalog = !catalog.isEmpty();
    // An empty object is the owner's context reset: it forgets the catalog (the
    // next All models asks again) and this widget's pending stars, whose values
    // may recur in the next context. Owners keep their requests and ignore late
    // answers; an ordinary refresh, a non-empty catalog, keeps pending stars.
    if (!m_hasCatalog) { m_all->setChecked(false); m_pending.clear(); }
    m_models = OpenCodeModels::parse(catalog["models"].toArray(), MaxCatalog);
    m_providers = OpenCodeModels::providers(catalog["providers"].toArray());
    m_modelsTruncated = catalog["models_truncated"].toBool();
    m_catalogState = catalog["catalog_state"].toString().left(32);
    m_catalogReason = noticeText(catalog["catalog_reason"].toString());
    // `available` is authoritative only in a ready catalog; an entry without it is not unavailable.
    m_unavailable.clear();
    if (catalogReady()) {
        const auto favorites = catalog["favorites"].toArray();
        for (qsizetype index = 0; index < qMin<qsizetype>(MaxFavorites, favorites.size()); ++index) {
            const auto favorite = favorites.at(index).toObject(); const auto value = favorite["value"].toString();
            const auto available = favorite["available"];
            if (!available.isBool() || available.toBool() || !OpenCodeModels::validValue(value) || m_unavailable.contains(value)) continue;
            if (std::none_of(m_models.cbegin(), m_models.cend(), [&](const OpenCodeModels::Model &m) { return m.value == value; })) m_unavailable << value;
        }
    }
    render();
}

void OpenCodeModelPicker::setCatalogFailed(const QString &reason)
{
    m_loading = false; m_failure = noticeText(reason); m_all->setChecked(false); render();
}

void OpenCodeModelPicker::setCurrent(const QString &value)
{
    if (value == m_current) return;
    m_current = value;
    if (m_offList || !row(value)) render(); else selectCurrent();
}

QString OpenCodeModelPicker::current() const { return m_current; }

QStringList OpenCodeModelPicker::values() const
{
    QStringList values;
    for (int group = 0; group < m_list->topLevelItemCount(); ++group) {
        const auto *header = m_list->topLevelItem(group);
        for (int index = 0; index < header->childCount(); ++index) values << header->child(index)->data(0, Qt::UserRole).toString();
    }
    return values;
}

std::optional<OpenCodeModels::Model> OpenCodeModelPicker::model(const QString &value) const
{
    // The list shown now answers first; the off-list current model is described by its value.
    const auto &first = showingAll() ? m_models : m_short, &second = showingAll() ? m_short : m_models;
    for (const auto *models : {&first, &second})
        for (const auto &model : *models)
            if (model.value == value) {
                // A ready catalog's unavailable favorite stays a favorite, whatever a stale short list says.
                auto found = model; found.favorite = found.favorite || m_unavailable.contains(value); return found;
            }
    if (value == m_current && OpenCodeModels::validValue(value)) {
        // An off-list current model can be a favorite the ready catalog no longer offers.
        auto unknown = unlisted(value); unknown.favorite = m_unavailable.contains(value); return unknown;
    }
    return std::nullopt;
}

void OpenCodeModelPicker::select(const QString &value)
{
    // As a click on that row: an unavailable favorite, the current one included, is never chosen.
    const auto *item = row(value);
    if (item ? !item->data(0, UnavailableRole).toBool() : model(value) && !m_unavailable.contains(value)) choose(value);
}

void OpenCodeModelPicker::setFavoritesAvailable(bool available, const QString &reason)
{
    m_favoritesAvailable = available; m_favoritesReason = noticeText(reason); m_favoritesTip = reasonTip(reason);
    for (int group = 0; group < m_list->topLevelItemCount(); ++group) {
        auto *header = m_list->topLevelItem(group);
        for (int index = 0; index < header->childCount(); ++index) decorate(header->child(index));
    }
}

void OpenCodeModelPicker::setAllModelsAvailable(bool available, const QString &reason)
{
    m_all->setEnabled(available); m_all->setToolTip(available ? tr("Search every model OpenCode offers") : reasonTip(reason));
    m_all->setAccessibleDescription(available ? QString() : noticeText(reason));
    if (!available && m_all->isChecked()) { m_all->setChecked(false); render(); }
}

void OpenCodeModelPicker::setFavoritePending(const QString &value, bool pending)
{
    if (pending) m_pending.insert(value); else m_pending.remove(value);
    if (auto *item = row(value)) decorate(item);
}

void OpenCodeModelPicker::setFavorite(const QString &value, bool favorite)
{
    m_pending.remove(value); m_shortSource.reset();
    for (auto *models : {&m_short, &m_models})
        for (auto &model : *models) if (model.value == value) model.favorite = favorite;
    if (!favorite) m_unavailable.removeAll(value);
    auto *item = row(value); if (!item) return;
    // Stars change in place, so the list keeps its scroll position.
    if (!favorite && item->data(0, UnavailableRole).toBool()) {
        // A row that stays listed (the current model, a stale short-list entry) is drawn again without the mark.
        if (model(value)) { render(); return; }
        auto *header = item->parent(); delete item;
        if (header->childCount() == 0) delete header;
        return;
    }
    item->setData(0, FavoriteRole, favorite); decorate(item);
}

bool OpenCodeModelPicker::showingAll() const { return m_all->isChecked(); }

bool OpenCodeModelPicker::catalogReady() const { return m_hasCatalog && m_catalogState == QLatin1String("ready"); }

void OpenCodeModelPicker::render()
{
    using OpenCodeModels::Model;
    m_searchTimer->stop();
    const auto query = m_search->text();
    // All models lists the catalog once it has one to show; until then the short list stays.
    const bool all = showingAll() && m_hasCatalog && (catalogReady() || !m_models.isEmpty());
    const auto &listed = all ? m_models : m_short;
    m_offList = OpenCodeModels::validValue(m_current)
        && std::none_of(listed.cbegin(), listed.cend(), [this](const Model &model) { return model.value == m_current; });
    const auto current = m_offList ? OpenCodeModels::filter({*model(m_current)}, query) : QList<Model>();
    auto matches = OpenCodeModels::filter(listed, query);
    QList<Model> gone;
    if (all && catalogReady())
        for (const auto &value : std::as_const(m_unavailable)) if (!m_offList || value != m_current) gone << unlisted(value);
    gone = OpenCodeModels::filter(gone, query);
    // One budget of MaxRows model rows in display order: the off-list current
    // model, the list, then unavailable favorites. A listed current model past
    // the cut replaces the last kept row, so it stays visible in its group.
    const auto total = current.size() + matches.size() + gone.size();
    auto budget = MaxRows - current.size();
    if (matches.size() > budget) {
        const auto found = std::find_if(matches.cbegin(), matches.cend(), [this](const Model &model) { return model.value == m_current; });
        const auto at = found - matches.cbegin();
        auto kept = matches.mid(0, budget);
        if (at >= budget && at < matches.size()) { kept.removeLast(); kept << matches.at(at); }
        matches = kept;
    }
    budget -= matches.size();
    if (gone.size() > budget) gone = gone.mid(0, budget);
    const auto hidden = total - current.size() - matches.size() - gone.size();
    m_list->clear();
    if (!current.isEmpty()) {
        // The current model is listed first, in its own group, even when this list does not offer it.
        OpenCodeModels::Group group; group.provider = current.first().provider; group.label = current.first().providerLabel;
        group.models = current; addGroup(group, false);
    }
    for (const auto &group : OpenCodeModels::group(matches, m_providers)) addGroup(group, false);
    if (!gone.isEmpty()) {
        OpenCodeModels::Group group; group.label = tr("Unavailable favorites"); group.models = gone; addGroup(group, true);
    }
    m_list->expandAll(); selectCurrent();
    QString notice;
    const bool reason = m_hasCatalog && !catalogReady() && !m_catalogReason.isEmpty();
    if (showingAll() && m_loading) notice = tr("Loading all models…");
    else if (!showingAll() && !m_failure.isEmpty()) notice = m_failure;
    else if (reason) notice = m_catalogReason;
    else if (hidden) notice = tr("Showing the first %1 matches. Search to narrow the list.").arg(MaxRows);
    else if (all && m_modelsTruncated) notice = tr("Showing the first %1 models. Search to narrow the list.").arg(m_models.size());
    else if (!all && m_shortTruncated) notice = tr("Some models are not in this short list. Use All models to search every model.");
    m_notice->setText(notice); m_notice->setVisible(!notice.isEmpty());
}

void OpenCodeModelPicker::addGroup(const OpenCodeModels::Group &group, bool unavailable)
{
    auto *header = new QTreeWidgetItem(m_list);
    header->setText(0, group.folder ? tr("%1 · This folder").arg(group.label) : group.label);
    header->setFlags(Qt::ItemIsEnabled); header->setFirstColumnSpanned(true);
    auto font = header->font(0); font.setBold(true); header->setFont(0, font);
    for (const auto &model : group.models) {
        // A ready catalog's unavailable favorite is marked in every group, a stale
        // short-list row and the current model included: starred, never chosen.
        const bool gone = unavailable || m_unavailable.contains(model.value);
        auto *item = new QTreeWidgetItem(header);
        item->setData(0, Qt::UserRole, model.value); item->setData(0, LabelRole, model.label);
        item->setData(0, UnavailableRole, gone); item->setData(0, FavoriteRole, gone || model.favorite);
        item->setData(0, FreeRole, model.free); item->setData(0, DefaultRole, model.isDefault);
        Qt::ItemFlags flags = Qt::ItemIsEnabled; if (!gone) flags |= Qt::ItemIsSelectable;
        item->setFlags(flags); item->setTextAlignment(1, Qt::Alignment(Qt::AlignCenter));
        if (gone) {
            const auto why = tr("OpenCode does not offer %1 here. Its star removes the favorite.").arg(model.value);
            item->setToolTip(0, plainTip(why)); item->setData(0, Qt::AccessibleDescriptionRole, why);
            item->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
        } else item->setToolTip(0, plainTip(model.value));
        decorate(item);
    }
}

void OpenCodeModelPicker::decorate(QTreeWidgetItem *item) const
{
    const auto value = item->data(0, Qt::UserRole).toString();
    const bool favorite = item->data(0, FavoriteRole).toBool();
    OpenCodeModels::Model marked; marked.favorite = favorite;
    marked.free = item->data(0, FreeRole).toBool(); marked.isDefault = item->data(0, DefaultRole).toBool();
    // An unavailable favorite says so in plain text wherever it is listed; other marks no longer apply.
    const auto marks = item->data(0, UnavailableRole).toBool() ? tr("Unavailable") : OpenCodeModels::marks(marked);
    const auto label = item->data(0, LabelRole).toString();
    item->setText(0, marks.isEmpty() ? label : label + QStringLiteral(" · ") + marks);
    item->setText(1, m_pending.contains(value) ? QStringLiteral("…") : favorite ? QStringLiteral("★") : QStringLiteral("☆"));
    // The reason's tooltip may be escaped markup; accessible text is always the literal reason.
    const auto action = favorite ? tr("Remove from favorites") : tr("Add to favorites");
    item->setToolTip(1, m_favoritesAvailable ? action : m_favoritesTip);
    item->setData(1, Qt::AccessibleTextRole, m_favoritesAvailable ? action : m_favoritesReason);
    item->setForeground(1, m_favoritesAvailable ? QBrush() : palette().brush(QPalette::Disabled, QPalette::Text));
}

void OpenCodeModelPicker::click(QTreeWidgetItem *item, int column)
{
    if (item && item->parent()) {
        const auto value = item->data(0, Qt::UserRole).toString();
        if (column == 1) {
            // The owner changes the favorite; a pending change ignores further clicks.
            if (m_favoritesAvailable && !m_pending.contains(value)) emit favoriteRequested(value, !item->data(0, FavoriteRole).toBool());
        } else if (!item->data(0, UnavailableRole).toBool()) {
            choose(value); return;
        }
    }
    // A star or a group header never moves the selection off the current model.
    selectCurrent();
}

void OpenCodeModelPicker::choose(const QString &value)
{
    // Every route (click, Enter, select) ends here: a ready catalog's unavailable favorite is never chosen.
    if (m_unavailable.contains(value)) { selectCurrent(); return; }
    const bool rebuild = m_offList || !row(value);
    m_current = value;
    if (rebuild) render(); else selectCurrent();
    emit chosen(value);
}

void OpenCodeModelPicker::selectCurrent()
{
    auto *item = row(m_current);
    if (item && (item->flags() & Qt::ItemIsSelectable)) m_list->setCurrentItem(item);
    else m_list->clearSelection();
}

QTreeWidgetItem *OpenCodeModelPicker::row(const QString &value) const
{
    if (value.isEmpty()) return nullptr;
    for (int group = 0; group < m_list->topLevelItemCount(); ++group) {
        auto *header = m_list->topLevelItem(group);
        for (int index = 0; index < header->childCount(); ++index)
            if (header->child(index)->data(0, Qt::UserRole).toString() == value) return header->child(index);
    }
    return nullptr;
}

bool OpenCodeModelPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            // Enter chooses the focused model row, as a click would.
            auto *item = m_list->currentItem();
            if (item && item->parent() && !item->data(0, UnavailableRole).toBool()) choose(item->data(0, Qt::UserRole).toString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
