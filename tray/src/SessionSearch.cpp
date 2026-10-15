#include "SessionSearch.h"
#include "SessionPresentation.h"
#include "IdentityBadge.h"
#include "WorkspaceList.h"
#include "WorkspaceIcons.h"
#include "MachinePlatform.h"
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
enum { Title = Qt::UserRole + 60, Meta, Snippet, Query, Hit, Host, Name, Archive, Run, Provider, Machine };
class SearchDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &i) const override {
        return {250, i.data(Snippet).toString().isEmpty() ? 54 : 96};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        p->save(); p->setClipRect(o.rect); p->setRenderHint(QPainter::Antialiasing);
        const bool dark = o.widget->property("hgsDark").toBool();
        const QRect r = o.rect.adjusted(2, 2, -2, -2);
        IdentityBadges::paintRow(p, o, dark);
        const auto line = [&](const QString &text, int y, int size, bool bold, const QColor &color, bool highlight, int available = -1) {
            QFont font = o.font; font.setPixelSize(size); font.setBold(bold); p->setFont(font);
            const QFontMetrics fm(font); const QString visible = fm.elidedText(text, Qt::ElideRight, available < 0 ? r.width() - 20 : available);
            int x = r.x() + 10; const QString query = i.data(Query).toString(); int start = 0;
            while (start < visible.size()) {
                int at = highlight && !query.isEmpty() ? visible.indexOf(query, start, Qt::CaseInsensitive) : -1;
                if (at < 0) at = visible.size();
                const QString before = visible.mid(start, at - start); p->setPen(color); p->drawText(x, y, before); x += fm.horizontalAdvance(before);
                if (at == visible.size()) break;
                const QString match = visible.mid(at, query.size()); const int w = fm.horizontalAdvance(match);
                p->fillRect(QRect(x, y - fm.ascent(), w, fm.height()), QColor(dark ? "#605032" : "#ffebb4"));
                p->setPen(QColor(dark ? "#ffdda0" : "#6e4b00")); p->drawText(x, y, match); x += w; start = at + query.size();
            }
        };
        const QColor fg(dark ? "#e8edf4" : "#1a2733"), muted(dark ? "#a1adbb" : "#647386");
        const auto provider = i.data(Provider).toString(), machine = i.data(Machine).toString();
        const int pw = IdentityBadges::width(IdentityBadges::Provider, provider, o.font, 95);
        const int mw = IdentityBadges::width(IdentityBadges::Machine, machine, o.font, r.width() / 3 + 10);
        IdentityBadges::paint(p, QRect(r.right() - pw - 10, r.y() + 5, pw, 18), IdentityBadges::Provider, provider, dark);
        IdentityBadges::paint(p, QRect(r.right() - mw - 10, r.y() + 27, mw, 18), IdentityBadges::Machine, machine, dark);
        line(i.data(Title).toString(), r.y() + 18, 13, true, fg, true, qMax(0, r.width() - pw - 28));
        line(i.data(Meta).toString(), r.y() + 39, 10, false, muted, false, qMax(0, r.width() - mw - 28));
        const QString snippet = i.data(Snippet).toString().simplified();
        if (!snippet.isEmpty()) {
            QFont font = o.font; font.setPixelSize(12); const QFontMetrics fm(font);
            int split = 0;
            while (split < snippet.size() && fm.horizontalAdvance(snippet.left(split + 1)) <= r.width() - 20) ++split;
            if (split < snippet.size()) { const int space = snippet.lastIndexOf(' ', split); if (space > split / 2) split = space; }
            line(snippet.left(split), r.y() + 58, 12, false, fg, true);
            line(snippet.mid(split).trimmed(), r.y() + 76, 12, false, fg, true);
        }
        p->restore();
    }
};
}

SessionSearch::SessionSearch(const QString &hgs, QWidget *parent) : QWidget(parent), m_client(hgs, this)
{
    setObjectName("sessionSearch"); auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0); layout->setSpacing(5);
    auto *row = new QHBoxLayout; m_status = new QLabel; m_status->setObjectName("searchStatus"); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText);
    m_refresh = new QPushButton; m_refresh->setFixedSize(28,28); m_refresh->setIconSize(QSize(16,16)); m_refresh->setToolTip(tr("Search again")); m_refresh->setAccessibleName(tr("Search again"));
    row->addWidget(m_status, 1); row->addWidget(m_refresh); layout->addLayout(row);
    m_list = new WorkspaceList; m_list->setObjectName("searchResults"); m_list->setFrameShape(QFrame::NoFrame); m_list->setMouseTracking(true);
    m_list->setItemDelegate(new SearchDelegate(m_list)); m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setAccessibleName(tr("Matching sessions and messages")); layout->addWidget(m_list, 1);
    m_list->installEventFilter(this); m_list->viewport()->installEventFilter(this);
    m_debounce.setSingleShot(true); m_debounce.setInterval(350);
    connect(&m_debounce, &QTimer::timeout, this, &SessionSearch::request);
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_replies.clear(); m_errors.clear(); request(); });
    connect(&m_client, &HgsClient::searchReady, this, [this](quint64 id, const QString &host, const QJsonObject &data) {
        if (m_pending.value(host).first != id) return;
        const auto query = m_pending.take(host).second;
        if (query == m_query) { m_replies[host] = data; m_errors.remove(host); rebuild(); }
        else request();
    });
    connect(&m_client, &HgsClient::searchFailed, this, [this](quint64 id, const QString &host, const QString &error) {
        if (m_pending.value(host).first != id) return;
        const auto query = m_pending.take(host).second;
        if (query == m_query) { m_errors[host] = error; rebuild(); } else request();
    });
    connect(m_list, &QListWidget::currentItemChanged, this, &SessionSearch::activateResult);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const bool repeat = item && !m_pressedCurrentKey.isEmpty() && item->data(Qt::UserRole).toString() == m_pressedCurrentKey;
        m_pressedCurrentKey.clear();
        if (repeat) activateResult(item);
    });
    setTheme(false);
}

QString SessionSearch::sessionKey(const QListWidgetItem *item)
{
    if (!item) return {};
    const QString archive = item->data(Archive).toString();
    return item->data(Host).toString() + '\n' + item->data(Name).toString() + (archive.isEmpty() ? QString() : '\n' + archive);
}

void SessionSearch::activateResult(QListWidgetItem *item)
{
    if (!item) return;
    m_selectedHit = item->data(Qt::UserRole).toString();
    emit resultActivated(item->data(Host).toString(), item->data(Name).toString(), item->data(Archive).toString(),
                         item->data(Run).toString(), item->data(Hit).toJsonObject(), m_query);
}
bool SessionSearch::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list->viewport() && event->type() == QEvent::MouseButtonPress) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const auto *item = m_list->itemAt(mouse->position().toPoint());
        m_pressedCurrentKey = mouse->button() == Qt::LeftButton && item && item == m_list->currentItem()
            ? item->data(Qt::UserRole).toString() : QString();
    } else if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && m_list->currentItem()) {
            activateResult(m_list->currentItem()); return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SessionSearch::setQuery(const QString &query)
{
    if (query.trimmed() == m_query) return;
    m_query = query.trimmed(); m_selectedHit.clear(); m_replies.clear(); m_errors.clear(); m_debounce.stop();
    rebuild(); if (m_query.size() >= 2) m_debounce.start();
}
void SessionSearch::setScope(const FleetState &fleet, const QString &filter, const QString &host, const QSet<QString> &drafts) {
    setHostsScope(fleet,filter,host.isEmpty()?QSet<QString>():QSet<QString>{host},drafts);
}
void SessionSearch::setHostsScope(const FleetState &fleet, const QString &filter, const QSet<QString> &selectedHosts, const QSet<QString> &drafts)
{
    bool changed = m_filter != filter || m_hosts != selectedHosts;
    QStringList hosts{QString()}; hosts.append(fleet.peerNames());
    for (const auto &alias : hosts) {
        const auto *before = alias.isEmpty() ? &m_fleet.local() : m_fleet.peer(alias);
        const auto *after = alias.isEmpty() ? &fleet.local() : fleet.peer(alias);
        if (after && after->ok && (!before || !before->ok)) {
            m_errors.remove(alias); m_replies.remove(alias); changed = true;
        }
    }
    m_fleet = fleet; m_filter = filter; m_hosts = selectedHosts; m_drafts = drafts;
    if (active()) { rebuild(); if (changed) m_debounce.start(); }
}
void SessionSearch::setFolderScope(const QString &host,const QString &path,bool checkout)
{
    if(m_folderHost==host&&m_folderPath==path&&m_folderCheckout==checkout)return;
    m_folderHost=host;m_folderPath=path;m_folderCheckout=checkout;if(active())rebuild();
}
void SessionSearch::setTheme(bool dark)
{
    m_dark = dark; m_list->setProperty("hgsDark", dark); m_list->viewport()->update();
    m_refresh->setIcon(workspaceIcon("refresh", QColor(dark ? "#a1adbb" : "#647386")));
    setStyleSheet(QString("QListWidget#searchResults { background:transparent; border:0; outline:0; } QLabel#searchStatus { color:%1; font-size:11px; padding:2px 4px; } QPushButton { padding:0; border:0; }").arg(dark ? "#a1adbb" : "#647386"));
}
bool SessionSearch::accepts(const QString &host, const SessionInfo &s, bool online) const
{
    if (!m_hosts.isEmpty() && !m_hosts.contains(host.isEmpty()?QStringLiteral("@local"):host)) return false;
    if(!m_folderPath.isEmpty()&&(host!=m_folderHost||!MachinePlatform::same(m_folderCheckout?s.gitRoot:s.canonicalCwd,m_folderPath,MachinePlatform::of(m_fleet.local().metrics,host.isEmpty()))))return false;
    if (m_filter == "archived") return s.state == "archived";
    // Global text search includes saved and archived conversations. Other state
    // filters still narrow it explicitly, with archive labels on every hit.
    if (m_filter == "attention") return SessionPresentation::needsAttention(s, online);
    if (m_filter == "active") return SessionPresentation::active(s, online, m_drafts.contains(host + '\n' + s.name));
    if (m_filter == "paused") return s.state == "paused" || s.state == "stopped";
    return true;
}
void SessionSearch::request()
{
    if (m_query.size() < 2) return;
    // A control-only computer (hgs.exe on Windows) has no conversations to search.
    QStringList hosts; if (m_fleet.local().localSessions) hosts << QString(); hosts.append(m_fleet.peerNames());
    for (const auto &host : hosts) {
        if (!m_hosts.isEmpty() && !m_hosts.contains(host.isEmpty()?QStringLiteral("@local"):host)) continue;
        if (m_pending.contains(host) || m_replies.contains(host) || m_errors.contains(host)) continue;
        const auto *box = host.isEmpty() ? &m_fleet.local() : m_fleet.peer(host);
        if (!box || !box->ok) { m_errors[host] = tr("Machine offline; contents unavailable"); continue; }
        m_pending[host] = {m_client.requestSearch(host, m_query), m_query};
    }
    rebuild();
}
void SessionSearch::rebuild()
{
    const QSignalBlocker block(m_list);
    const int scroll = m_list->verticalScrollBar()->value(); m_list->clear();
    int sessions = 0, messages = 0; bool truncated = false; int unavailable = 0;
    QStringList visibleHosts;
    const auto append = [&](const QString &host, const BoxState &box) {
        if (!m_hosts.isEmpty() && !m_hosts.contains(host.isEmpty()?QStringLiteral("@local"):host)) return;
        visibleHosts << host;
        const auto reply = m_replies.value(host); truncated |= reply.value("truncated").toBool(); unavailable += reply.value("unavailable_sessions").toInt();
        for (const auto &s : box.sessions) {
            const bool reachable = box.ok && (host.isEmpty() || QDateTime::currentMSecsSinceEpoch() - m_fleet.peerPolledAt(host) < FleetState::kPeerStaleMs);
            if (!accepts(host, s, reachable) || !active()) continue;
            const QString machine = host.isEmpty() ? box.host : host;
            QString matchedIdentity;
            const auto identity = [&](const QString &id, const QString &label) {
                if (!id.isEmpty() && id.contains(m_query, Qt::CaseInsensitive) && matchedIdentity.isEmpty())
                    matchedIdentity = label + ": " + id;
            };
            identity(s.conversationId, tr("Session ID")); identity(s.runId, tr("Run ID")); identity(s.archiveId, tr("Archive ID"));
            const auto childIdentity = [&](const QString &id, const QJsonObject &child) {
                identity(id, tr("Agent ID")); identity(child.value("conversation_id").toString(), tr("Child session ID"));
            };
            if (s.subagentSource != "hook_profiles") {
                for (auto it = s.subagents.begin(); it != s.subagents.end(); ++it) childIdentity(it.key(), it.value().toObject());
                for (const auto &value : s.subagentPreviews) {
                    const auto child = value.toObject(); childIdentity(child.value("id").toString(child.value("agent_id").toString()), child);
                }
            }
            const auto add = [&](const QJsonObject &hit) {
                const bool content = !hit.isEmpty(); auto *item = new QListWidgetItem(m_list);
                const QString key = host + '\n' + s.name + '\n' + s.archiveId + '\n' + hit.value("message_id").toString(); item->setData(Qt::UserRole, key);
                const QString title = s.tag.isEmpty() ? s.project.isEmpty() ? s.name : s.project : s.project + " / " + s.tag;
                QString meta = s.state == "archived" ? tr("Archive") : QString();
                if (content) meta += (meta.isEmpty() ? QString() : QStringLiteral(" / ")) + (hit.value("role") == "user" ? tr("You") : hit.value("role") == "assistant" ? tr("Agent") : tr("Tool")) + " / " + QDateTime::fromSecsSinceEpoch(qint64(hit.value("at").toDouble())).toLocalTime().toString("d MMM HH:mm");
                else {
                    const auto description = matchedIdentity.isEmpty() ? s.model : matchedIdentity;
                    if (!description.isEmpty()) meta += (meta.isEmpty() ? QString() : QStringLiteral(" / ")) + description;
                }
                item->setData(Provider, s.cmd); item->setData(Machine, machine);
                item->setData(Title, title); item->setData(Meta, meta); item->setData(Query, m_query); item->setData(Snippet, hit.value("snippet").toString());
                item->setData(Host, host); item->setData(Name, s.name); item->setData(Archive, s.archiveId); item->setData(Run, s.runId);
                auto event = hit.value("event").toObject();
                if (hit.value("content_truncated").toBool()) event["content_truncated"] = true;
                item->setData(Hit, event); item->setData(Qt::AccessibleTextRole, title + '\n' + IdentityBadges::providerName(s.cmd) + " / " + machine + '\n' + meta + '\n' + hit.value("snippet").toString());
                QStringList tooltip{title, machine};
                if (!s.conversationId.isEmpty()) tooltip << tr("Session ID: %1").arg(s.conversationId);
                if (!s.runId.isEmpty()) tooltip << tr("Run ID: %1").arg(s.runId);
                if (!s.archiveId.isEmpty()) tooltip << tr("Archive ID: %1").arg(s.archiveId);
                if (!matchedIdentity.isEmpty() && !tooltip.contains(matchedIdentity)) tooltip << matchedIdentity;
                item->setToolTip("<qt>" + tooltip.join('\n').toHtmlEscaped().replace('\n', "<br>") + "</qt>");
                if (key == m_selectedHit) m_list->setCurrentItem(item);
            };
            const QString metadata = s.name + ' ' + machine + ' ' + s.model + ' ' + s.effort + ' ' + s.cwd + ' ' + s.gitBranch + ' ' + s.gitWorktreeName;
            bool shown = false;
            if (metadata.contains(m_query, Qt::CaseInsensitive) || !matchedIdentity.isEmpty()) { add({}); shown = true; }
            for (const auto &value : reply.value("results").toArray()) {
                const auto hit = value.toObject();
                if (hit.value("name").toString() != s.name || hit.value("archive_id").toString() != s.archiveId) continue;
                if (!hit.value("run_id").toString().isEmpty() && !s.runId.isEmpty() && hit.value("run_id").toString() != s.runId) continue;
                add(hit); shown = true; ++messages;
            }
            if (shown) ++sessions;
        }
    };
    if (m_fleet.local().localSessions) append({}, m_fleet.local());
    for (const auto &host : m_fleet.peerNames()) if (const auto *box = m_fleet.peer(host)) append(host, *box);
    m_list->verticalScrollBar()->setValue(scroll);
    int pending = 0; QStringList errors;
    for (const auto &host : visibleHosts) {
        if (m_pending.contains(host)) ++pending;
        if (m_errors.contains(host)) errors << (host.isEmpty() ? m_fleet.local().host : host) + ": " + m_errors.value(host).left(200);
    }
    QString status = tr("%1 messages, %2 sessions").arg(messages).arg(sessions);
    if (m_query.size() < 2) status = tr("Type 2+ characters to search message contents");
    else if (pending || m_debounce.isActive()) status += tr(" — Searching…");
    if (truncated) status += tr(" — Limited history searched");
    if (!errors.isEmpty() || unavailable) status += tr(" — Some history unavailable");
    m_status->setText(status);
    m_status->setToolTip(errors.join('\n')); m_refresh->setEnabled(!pending && m_query.size() >= 2);
}
