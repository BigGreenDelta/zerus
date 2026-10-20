#include "AccountsPage.h"
#include "AccountUsage.h"
#include "AccountUsageStore.h"
#include "AccountCatalog.h"
#include "AccountChoice.h"
#include "IdentityBadge.h"
#include "LocalSessionHost.h"
#include "MachinePlatform.h"
#include "OpenCodeModels.h"
#include "WorkspaceIcons.h"
#include "WorkspacePageHeader.h"
#include "WorkspaceList.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include "BusyIndicator.h"
#include <QRegularExpression>
#include <QRadioButton>
#include <QShowEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStackedLayout>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
namespace {
QLabel *label(const QString &text = {}) { auto *w = new QLabel(text); w->setTextFormat(Qt::PlainText); w->setWordWrap(true); return w; }
// Add account's agents: every row stays visible, measured in the current style.
class AgentList final : public WorkspaceList {
public:
    using WorkspaceList::WorkspaceList;
    QSize sizeHint() const override { return {380, count() ? sizeHintForRow(0) * count() + 2 * frameWidth() : 0}; }
    QSize minimumSizeHint() const override { return sizeHint(); }
};
// A wrapped hint as tall as its longest alternative at its width. Its height is a
// minimum rather than height-for-width, which windows cap at part of the screen.
class ReservedLabel final : public QLabel {
public:
    ReservedLabel() { setTextFormat(Qt::PlainText); setWordWrap(true); setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); }
    void reserve(const QStringList &texts) { m_texts = texts; fit(); }
    void fit() {
        const auto margins = contentsMargins(); int height = 0;
        const QRect bounds(0, 0, qMax(1, width() - margins.left() - margins.right()), QWIDGETSIZE_MAX);
        for (const auto &text : m_texts + QStringList{text()})
            height = qMax(height, fontMetrics().boundingRect(bounds, Qt::TextWordWrap | int(alignment()), text).height());
        height += margins.top() + margins.bottom();
        if (height != m_height) { m_height = height; updateGeometry(); }
    }
    bool hasHeightForWidth() const override { return false; }
    QSize sizeHint() const override { return {0, m_height}; }
    QSize minimumSizeHint() const override { return {0, m_height}; }
protected:
    void resizeEvent(QResizeEvent *event) override { QLabel::resizeEvent(event); fit(); }
private:
    QStringList m_texts;
    int m_height = 0;
};
using IdentityBadges::providerName;
QString permissionMode(const QJsonObject &p) {
    return p["permission_mode"] == "bypass" ? "bypass" : p["effective_permission_mode"].toString("unknown");
}
QString permissionLabel(const QString &mode) {
    return mode == "bypass" ? QObject::tr("Bypass") : mode == "auto" ? QObject::tr("Auto")
        : mode == "plan" ? QObject::tr("Plan") : mode == "mixed" ? QObject::tr("Varies") : QObject::tr("Default");
}
QString accountPermission(const QJsonObject &account) {
    QString mode;
    for (const auto &value : account["members"].toArray()) {
        const auto next = permissionMode(value.toObject());
        if (!mode.isEmpty() && mode != next) return "mixed";
        mode = next;
    }
    return mode;
}
QString permissionHint(const QJsonObject &p) {
    const auto source = p["permission_mode"] == "bypass" ? QObject::tr("Set in Zerus") : QObject::tr("From provider settings");
    return source + "\n" + p["permission_detail"].toString(QObject::tr("Provider defaults have not been verified."))
        + "\n" + QObject::tr("Applies to new and resumed sessions; a running session may use a different mode.");
}
class AccountKeyDialog : public QDialog {
public:
    using QDialog::QDialog;
    bool saving = false;
    void reject() override { if (!saving) QDialog::reject(); }
};
class MachineTagsLayout final : public QLayout {
public:
    MachineTagsLayout() { setContentsMargins(0, 0, 0, 0); setSpacing(6); }
    ~MachineTagsLayout() override { while (auto *item = takeAt(0)) delete item; }
    void addItem(QLayoutItem *item) override { items.append(item); }
    int count() const override { return items.size(); }
    QLayoutItem *itemAt(int index) const override { return items.value(index); }
    QLayoutItem *takeAt(int index) override { return index >= 0 && index < items.size() ? items.takeAt(index) : nullptr; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override { QSize size; for (auto *item : items) size = size.expandedTo(item->minimumSize()); return size; }
    Qt::Orientations expandingDirections() const override { return {}; }
    void setGeometry(const QRect &rect) override { QLayout::setGeometry(rect); arrange(rect, true); }
private:
    int arrange(const QRect &rect, bool apply) const {
        int x = rect.x(), y = rect.y(), height = 0;
        for (auto *item : items) {
            const auto size = item->sizeHint();
            if (x > rect.x() && x + size.width() > rect.right() + 1) { x = rect.x(); y += height + spacing(); height = 0; }
            if (apply) item->setGeometry(QRect(QPoint(x, y), size));
            x += size.width() + spacing(); height = qMax(height, size.height());
        }
        return y + height - rect.y();
    }
    QList<QLayoutItem *> items;
};
class AccountDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {240, 64}; }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &index) const override {
        p->save(); p->setClipRect(o.rect); const bool dark = o.widget->property("hgsDark").toBool();
        IdentityBadges::paintRow(p, o, dark);
        const auto profile = index.data(Qt::UserRole).toJsonObject(); const auto provider = profile["provider"].toString();
        const QRect r = o.rect.adjusted(13, 10, -13, -10);
        const int pw = IdentityBadges::width(IdentityBadges::Provider, provider, o.font, 95);
        IdentityBadges::paint(p, QRect(r.right() - pw, r.y(), pw, 18), IdentityBadges::Provider, provider, dark);
        QFont font = o.font; font.setPixelSize(14); font.setWeight(QFont::DemiBold); p->setFont(font);
        p->setPen(QColor(dark ? "#e8edf4" : "#1a2733"));
        const QRect title(r.x(), r.y() - 1, qMax(0, r.width() - pw - 10), 20);
        p->drawText(title, Qt::AlignVCenter, QFontMetrics(font).elidedText(profile["label"].toString(), Qt::ElideRight, title.width()));
        font.setPixelSize(11); font.setWeight(QFont::Normal); p->setFont(font); p->setPen(QColor(dark ? "#a1adbb" : "#647386"));
        const auto mode = accountPermission(profile), caption = permissionLabel(mode);
        const int badgeWidth = QFontMetrics(font).horizontalAdvance(caption) + 14;
        const QRect badge(r.right() - badgeWidth, r.y() + 24, badgeWidth, 18);
        p->setPen(Qt::NoPen); p->setBrush(QColor(mode == "bypass" ? (dark ? "#572b35" : "#fbe3e7") : (dark ? "#2c3540" : "#e8edf2")));
        p->drawRoundedRect(badge, 4, 4);
        p->setPen(QColor(mode == "bypass" ? (dark ? "#ffa0aa" : "#a7243b") : (dark ? "#bec8d3" : "#536270")));
        p->drawText(badge, Qt::AlignCenter, caption);
        const auto machines = profile["machines"].toArray(); int x = r.x();
        const int tagsRight = badge.left() - 8;
        for (int i = 0; i < machines.size(); ++i) {
            const auto machine = machines[i].toString();
            const int width = IdentityBadges::width(IdentityBadges::Machine, machine, o.font, r.width());
            if (x + width > tagsRight - (i + 1 < machines.size() ? 28 : 0)) {
                p->drawText(QRect(x, r.y() + 24, qMax(0, tagsRight - x), 18), Qt::AlignVCenter, "+" + QString::number(machines.size() - i)); break;
            }
            IdentityBadges::paint(p, QRect(x, r.y() + 24, width, 18), IdentityBadges::Machine, machine, dark); x += width + 6;
        }
        p->restore();
    }
};
QString profileKey(const QString &host, const QString &id) { return host + QChar('\x1f') + id; }
// Machines that keep agent accounts. A control-only computer (hgs.exe on
// Windows) has none of its own, so only remote machines are listed there.
QStringList accountHosts(const FleetState &fleet, const QString &explicitHost = {}) {
    QStringList hosts; if (fleet.local().localSessions) hosts << QString();
    hosts.append(fleet.peerNames());
    if (!explicitHost.isEmpty() && !hosts.contains(explicitHost)) hosts << explicitHost;
    return hosts;
}
// Without remote machines, a control console sends you to Machines: to add one or,
// where Machines offers it (spec D16), to enable local sessions.
QString remoteOnlyHint(const FleetState &fleet) {
    return LocalSessionHost::canEnable(fleet.local())
        ? QObject::tr("To manage agent accounts, enable local sessions for this computer in Machines or add a remote machine.")
        : QObject::tr("Add a remote machine in Machines to manage agent accounts; this computer runs no agents.");
}
QString machineChoice(const FleetState &fleet, const QString &host) {
    return host.isEmpty() ? fleet.local().host + QObject::tr(" (this machine)") : host;
}
void fillMachines(QComboBox *combo, const FleetState &fleet, const QString &explicitHost = {}) {
    for (const auto &host : accountHosts(fleet, explicitHost)) combo->addItem(machineChoice(fleet, host), host);
}
// Whether the machine starts this provider's agent: peers are POSIX, and the
// local Windows computer lists, adds and takes no DeepSeek or OpenCode account
// (spec D16).
bool launchable(const FleetState &fleet, const QString &host, const QString &provider) {
    return AccountCatalog::launchable(provider, MachinePlatform::of(host.isEmpty() ? fleet.local().metrics : QJsonObject(), host.isEmpty()));
}
}
AccountsPage::AccountsPage(const QString &hgsPath, QWidget *parent, AccountUsageStore *usage) : QWidget(parent), m_client(hgsPath, this)
{
    m_usageStore=usage?usage:new AccountUsageStore(hgsPath,this);
    setObjectName("accountsPage");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(20, 18, 20, 18); layout->setSpacing(14);
    auto *title = label(tr("Agent accounts")); title->setObjectName("heading");
    m_refresh = new AccountUsage::RefreshButton; m_refresh->setObjectName("refreshAccounts");
    m_add = new QPushButton(tr("Add account")); m_add->setObjectName("addAccount");
    layout->addLayout(WorkspacePageHeader::row(title, m_refresh, m_add));
    auto *intro = label(tr("Your accounts across all machines. Choose an account to manage where it is available.")); intro->setObjectName("muted"); layout->addWidget(intro);
    auto *filters = new QHBoxLayout; m_machine = new QComboBox; m_machine->setObjectName("accountMachineFilter"); m_machine->addItem(tr("All machines"), "*"); filters->addWidget(m_machine);
    auto *summary = new QHBoxLayout; summary->setSpacing(6);
    m_summarySpinner = new BusyIndicator; m_summarySpinner->setObjectName("accountsUpdatingSpinner"); summary->addWidget(m_summarySpinner,0,Qt::AlignVCenter);
    m_summary = label(); m_summary->setObjectName("muted"); summary->addWidget(m_summary,1); filters->addLayout(summary,1); layout->addLayout(filters);
    m_bodyStack = new QStackedLayout; layout->addLayout(m_bodyStack, 1);
    auto *loading = new QWidget; loading->setObjectName("accountsLoading");
    auto *loadingLayout = new QVBoxLayout(loading); loadingLayout->setSpacing(14); loadingLayout->addStretch();
    auto *loadingRow = new QHBoxLayout; loadingRow->setSpacing(8); loadingRow->addStretch();
    m_loadingSpinner = new BusyIndicator; m_loadingSpinner->setObjectName("accountsLoadingSpinner");
    loadingRow->addWidget(m_loadingSpinner,0,Qt::AlignVCenter); loadingRow->addWidget(label(tr("Loading accounts…"))); loadingRow->addStretch();
    loadingLayout->addLayout(loadingRow);
    auto *loadingHint = label(tr("Checking accounts across your machines.")); loadingHint->setObjectName("muted"); loadingHint->setAlignment(Qt::AlignCenter);
    loadingLayout->addWidget(loadingHint); loadingLayout->addStretch(); m_bodyStack->addWidget(loading);
    auto *content = new QWidget; content->setObjectName("accountsContent"); m_bodyStack->addWidget(content);
    auto *body = new QHBoxLayout(content); body->setContentsMargins(0,0,0,0); body->setSpacing(20);
    m_list = new WorkspaceList; m_list->setObjectName("accountProfiles"); m_list->setMinimumWidth(240); m_list->setMaximumWidth(360); body->addWidget(m_list, 1);
    m_list->setItemDelegate(new AccountDelegate(m_list)); m_list->setMouseTracking(true); m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setStyleSheet("QListWidget#accountProfiles { background:transparent; border:0; outline:0; } QListWidget#accountProfiles::item { padding:0; border:0; }");
    auto *detailsWidget = new QWidget; detailsWidget->setObjectName("accountDetails"); detailsWidget->setMaximumWidth(720);
    auto *details = new QVBoxLayout(detailsWidget); details->setSpacing(18);details->setContentsMargins(0,0,8,0);
    auto *accountHeading=new QHBoxLayout;
    m_title = label(tr("Choose an account")); m_title->setObjectName("accountTitle"); m_title->setStyleSheet("font-size:18px;font-weight:600;"); accountHeading->addWidget(m_title,1);
    m_rename=new QPushButton(tr("Rename…"));m_rename->setObjectName("renameAccount");accountHeading->addWidget(m_rename);details->addLayout(accountHeading);
    auto *badges = new QHBoxLayout; badges->setSpacing(12);
    m_providerBadge = new IdentityBadge(IdentityBadges::Provider); m_providerBadge->setObjectName("accountProviderBadge");
    badges->addWidget(m_providerBadge,0,Qt::AlignTop);
    m_identity=label();m_identity->setObjectName("accountIdentity");m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);badges->addWidget(m_identity,1);details->addLayout(badges);
    m_usage=new AccountUsage::Panel; m_usage->setMaximumWidth(QWIDGETSIZE_MAX);
    m_machineCard=new QFrame; m_machineCard->setObjectName("accountMachineCard");
    auto *machineLayout=new QVBoxLayout(m_machineCard); machineLayout->setContentsMargins(16,14,16,14);machineLayout->setSpacing(14);
    auto *machineTitle=label(tr("Machines"));machineTitle->setStyleSheet("font-weight:600;");machineLayout->addWidget(machineTitle);
    m_machineTags = new MachineTagsLayout; machineLayout->addLayout(m_machineTags);
    m_profileChoice = new QComboBox; m_profileChoice->setObjectName("accountMachineProfile"); machineLayout->addWidget(m_profileChoice);
    connect(m_profileChoice, &QComboBox::currentIndexChanged, this, [this] {
        if (m_profileChoice->currentIndex() < 0) return;
        m_activeProfile = m_profileChoice->currentData().toString(); selectionChanged();
    });
    m_detail = label(); m_detail->setObjectName("accountMachineStatus"); m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *actions = new QHBoxLayout; actions->setSpacing(12);actions->addWidget(m_detail);
    m_login = new QPushButton(tr("Sign in…")); m_login->setObjectName("loginAccount"); actions->addWidget(m_login);
    m_install = new QPushButton(tr("Install agent…")); m_install->setObjectName("installAccountAgent"); actions->addWidget(m_install);
    // Explicit only: inspecting accounts never starts OpenCode's shared service.
    m_startOpenCode = new QPushButton; m_startOpenCode->setObjectName("startOpenCode"); actions->addWidget(m_startOpenCode);
    m_startOpenCode->setToolTip(tr("Start the shared OpenCode service on this machine. Zerus never stops it.")); m_startOpenCode->hide();
    m_copy = new QPushButton(tr("Add to machine…"), this); m_copy->setObjectName("copyAccount"); m_copy->hide();
    actions->addStretch();machineLayout->addLayout(actions);
    // OpenCode's connected providers: plain, read-only text from that machine's
    // catalog. Showing them never starts the service or changes a provider.
    m_openCodeProviders = new QTreeWidget; m_openCodeProviders->setObjectName("openCodeProviders"); m_openCodeProviders->setAccessibleName(tr("OpenCode providers"));
    m_openCodeProviders->setColumnCount(3); m_openCodeProviders->setHeaderHidden(true); m_openCodeProviders->setRootIsDecorated(false); m_openCodeProviders->setItemsExpandable(false);
    m_openCodeProviders->setEditTriggers(QAbstractItemView::NoEditTriggers); m_openCodeProviders->setSelectionMode(QAbstractItemView::NoSelection); m_openCodeProviders->setFocusPolicy(Qt::NoFocus);
    m_openCodeProviders->setUniformRowHeights(true); m_openCodeProviders->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents); m_openCodeProviders->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    m_openCodeProviders->header()->setStretchLastSection(false); m_openCodeProviders->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_openCodeProviders->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents); m_openCodeProviders->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_openCodeProviders->setStyleSheet("QTreeWidget#openCodeProviders { background:transparent; border:0; }"); m_openCodeProviders->hide(); machineLayout->addWidget(m_openCodeProviders);
    m_openCodeProvidersNote = label(); m_openCodeProvidersNote->setObjectName("openCodeProvidersNote"); m_openCodeProvidersNote->hide(); machineLayout->addWidget(m_openCodeProvidersNote);
    auto *permissionRow = new QHBoxLayout; permissionRow->setSpacing(12);
    m_permissionState = label(); m_permissionState->setObjectName("accountPermissionState");m_permissionState->setWordWrap(false);
    m_permissions = new QPushButton(tr("Change…")); m_permissions->setObjectName("accountPermissions");
    permissionRow->addWidget(m_permissionState);permissionRow->addWidget(m_permissions);permissionRow->addStretch();machineLayout->addLayout(permissionRow);
    connect(m_permissions, &QPushButton::clicked, this, &AccountsPage::permissions);
    m_default=new QPushButton; m_default->setObjectName("defaultAccount");machineLayout->addWidget(m_default,0,Qt::AlignLeft);
    connect(m_default,&QPushButton::clicked,this,[this]{
        const auto p=selected();if(p.isEmpty()||p["is_default"].toBool())return;
        const auto host=p["host"].toString();
        request(host,{"default",p["id"].toString(),"--revision",m_catalogs[host]["revision"].toString()});
    });
    m_remove = new QPushButton(tr("Remove…")); m_remove->setObjectName("removeAccount");
    m_remove->setToolTip(tr("Remove this profile from the selected machine's Zerus catalog. You will confirm the machine and consequences before anything changes."));
    machineLayout->addSpacing(4);machineLayout->addWidget(m_remove,0,Qt::AlignLeft);details->addWidget(m_machineCard);
    details->addWidget(m_usage); details->addStretch();
    auto *scroll=new QScrollArea;scroll->setObjectName("accountDetailsScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setAlignment(Qt::AlignLeft|Qt::AlignTop);scroll->setWidget(detailsWidget);
    scroll->setAutoFillBackground(false);scroll->viewport()->setAutoFillBackground(false);detailsWidget->setAutoFillBackground(false);body->addWidget(scroll,2);
    connect(m_refresh, &QPushButton::clicked, this, &AccountsPage::reload);
    connect(m_machine, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_explicitHost.isEmpty() && m_machine->currentData().toString() != m_explicitHost) {
            const QString old = m_explicitHost; m_explicitHost.clear();
            if (!m_fleet.peerNames().contains(old)) m_catalogs.remove(old);
            setFleet(m_fleet);
        }
        render();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, &AccountsPage::selectionChanged);
    connect(m_add, &QPushButton::clicked, this, &AccountsPage::add);
    connect(m_copy, &QPushButton::clicked, this, [this] { copy(); });
    connect(m_install, &QPushButton::clicked, this, [this] { const auto p = selected(); if (!p.isEmpty()) emit installRequested(p["host"].toString(), p["provider"].toString()); });
    connect(m_remove, &QPushButton::clicked, this, &AccountsPage::remove);
    connect(m_startOpenCode, &QPushButton::clicked, this, [this] {
        const auto p = selected();
        if (p["provider"] != "opencode" || p["catalog_state"] != "not_running" || !m_requests.isEmpty()) return;
        request(p["host"].toString(), {"start", "native-opencode", "--json"});
    });
    connect(m_rename, &QPushButton::clicked, this, &AccountsPage::rename);
    connect(m_login, &QPushButton::clicked, this, [this] {
        const auto p = selected(); if (p.isEmpty()) return;
        if (p["provider"] == "dsh") { setupDeepSeek(p["host"].toString(), selectedAccount()["label"].toString()); return; }
        emit loginRequested(p["host"].toString(), p["id"].toString());
        emit notice(tr("Complete %1 sign-in in the terminal on %2, this page updates automatically.").arg(providerName(p["provider"].toString()), p["machine"].toString()));
    });
    connect(&m_client, &HgsClient::accountsReady, this, [this](quint64 id, const QString &host, const QJsonObject &data) {
        if (!m_requests.contains(id)) return;
        const QString op = m_requests.take(id).second;
        if (!host.isEmpty() && !m_fleet.peerNames().contains(host) && host != m_explicitHost) { render(); return; }
        if (op == "start") {
            // The answer is OpenCode's catalog, not the account list: read the list again.
            emit notice(tr("OpenCode started on %1.").arg(host.isEmpty() ? m_fleet.local().host : host));
            request(host, {"ls"}); render(); return;
        }
        if (op == "ls" || op == "add" || op == "rename" || op == "rm" || op == "restore" || op == "set-key" || op == "permissions" || op == "default") m_catalogs[host] = data;
        if (op == "rename" && !m_renameQueue.isEmpty()) { renameNext(); render(); return; }
        if (op != "ls") {
            emit notice(op == "default" ? tr("Default account saved for new sessions on %1.").arg(host.isEmpty() ? m_fleet.local().host : host) : op == "permissions" ? tr("Permission mode saved for new and resumed sessions on %1.").arg(host.isEmpty() ? m_fleet.local().host : host) : op == "set-key" ? tr("DeepSeek API key saved on %1.").arg(host.isEmpty() ? m_fleet.local().host : host) : op == "restore" ? tr("Account added on %1 using its saved sign-in.").arg(host.isEmpty() ? m_fleet.local().host : host) : op == "rename" ? tr("Account name updated.") : op == "copy" ? tr("Account copied into a new profile. It is available in New session on that machine.") : op == "rm" ? tr("Account removed from %1. Add it again whenever you need it.").arg(host.isEmpty() ? m_fleet.local().host : host) : tr("Account profile created. Sign in to use it."));
            emit accountsChanged();
            if (op == "add" && !m_loginAfterAddId.isEmpty()) {
                bool installed = false;
                for (const auto &value : data["profiles"].toArray()) if (value.toObject()["id"] == m_loginAfterAddId) installed = value.toObject()["installed"].toBool();
                if (installed) emit loginRequested(m_loginAfterAddHost, m_loginAfterAddId);
                else emit notice(tr("Account profile added. Install the agent on that machine, then sign in."));
                m_loginAfterAddId.clear();
            }
            if (op == "copy") ensureCatalogs(true);
            if (op == "set-key") inspectProfiles(true);
        }
        render();emit catalogChanged(); if (isVisible()) inspectProfiles();
    });
    connect(&m_client, &HgsClient::accountsFailed, this, [this](quint64 id, const QString &host, const QString &error) {
        if (!m_requests.contains(id)) return;
        const auto op = m_requests.take(id).second;
        if (!host.isEmpty() && !m_fleet.peerNames().contains(host) && host != m_explicitHost) { render(); return; }
        m_loginAfterAddId.clear(); m_renameQueue.clear(); emit notice((host.isEmpty() ? tr("This machine") : host) + ": " + error,true); render();emit catalogChanged();
    });
    connect(m_usageStore,&AccountUsageStore::changed,this,[this](const QString &){
        if (m_renderQueued) return; m_renderQueued = true;
        QTimer::singleShot(0, this, [this] { m_renderQueued = false; render(); });
    });
    auto *timer=new QTimer(this);timer->setInterval(30000);connect(timer,&QTimer::timeout,this,[this]{if(isVisible()){ensureCatalogs();inspectProfiles();}});timer->start();
    setTheme(false); selectionChanged();
}
void AccountsPage::setTheme(bool dark) {
    m_dark = dark;
    m_list->setProperty("hgsDark", dark); m_providerBadge->setTheme(dark);
    m_usage->setTheme(dark);
    m_refresh->setTheme(dark);
    m_loadingSpinner->setTheme(dark); m_summarySpinner->setTheme(dark);
    WorkspacePageHeader::theme(m_refresh, m_add, tr("Refresh accounts"), dark);
    m_machineCard->setStyleSheet(QString("QFrame#accountMachineCard {background:%1;border:1px solid %2;border-radius:9px;} QFrame#accountMachineCard QLabel {background:transparent;}")
        .arg(dark ? "#1c242c" : "#ffffff",dark ? "#34414b" : "#ccd7df"));
    if (m_refresh->isRefreshing()) m_refresh->setIcon({});
    refreshAppearance(); selectionChanged();
}
void AccountsPage::refreshAppearance() { m_list->viewport()->update(); }
void AccountsPage::showEvent(QShowEvent *event) { QWidget::showEvent(event); ensureCatalogs();inspectProfiles();render(); }
void AccountsPage::setFleet(const FleetState &fleet)
{
    m_fleet = fleet;m_usageStore->setFleet(fleet); const QString selectedHost = m_machine->currentData().toString();
    const auto desired = accountHosts(fleet, m_explicitHost);
    bool same = m_machine->count() == desired.size() + 1;
    for (int i = 0; same && i < desired.size(); ++i)
        same = m_machine->itemData(i + 1).toString() == desired[i] && m_machine->itemText(i + 1) == machineChoice(fleet, desired[i]);
    if (same) { render(); return; }
    const QSignalBlocker blocker(m_machine); m_machine->clear(); m_machine->addItem(tr("All machines"), "*"); fillMachines(m_machine, fleet, m_explicitHost);
    m_machine->setCurrentIndex(qMax(0, m_machine->findData(selectedHost)));
    for (auto it = m_catalogs.begin(); it != m_catalogs.end();) { if (!it.key().isEmpty() && !fleet.peerNames().contains(it.key()) && it.key() != m_explicitHost) { m_catalogChecked.remove(it.key()); it = m_catalogs.erase(it); } else ++it; }
    render();emit catalogChanged();
}
void AccountsPage::showMachine(const QString &host)
{
    // Explicit navigation can target a configured machine excluded from background polling.
    // Retain only this one extra host; changing the filter releases it and rejects stale replies.
    {
        const QSignalBlocker blocker(m_machine);
        m_explicitHost = !host.isEmpty() && !m_fleet.peerNames().contains(host) ? host : QString();
        setFleet(m_fleet); m_machine->setCurrentIndex(qMax(0, m_machine->findData(host)));
    }
    render();
    if (m_requests.isEmpty()) { ensureCatalogs(); return; }
    for (const auto &pending : m_requests) if (pending.first == host && pending.second == "ls") return;
    request(host, {"ls"});
}
void AccountsPage::showAccount(const QString &host,const QString &id) {
    m_activeProfile=m_requestedProfile=profileKey(host,id); showMachine(host);
    { const QSignalBlocker blocker(m_machine); m_machine->setCurrentIndex(0); }
    render();
}
void AccountsPage::request(const QString &host, const QStringList &args) {
    if (args.value(0)=="ls") m_catalogChecked[host]=QDateTime::currentMSecsSinceEpoch();
    const auto id = m_client.requestAccounts(host, args); m_requests[id] = {host, args.value(0)}; selectionChanged();
}
void AccountsPage::reload()
{
    if (!m_requests.isEmpty() || m_refresh->isRefreshing()) return;
    inspectProfiles(true);ensureCatalogs(true);
}
int AccountsPage::pendingAccounts() const
{
    int pending = 0;
    const auto hosts = accountHosts(m_fleet, m_explicitHost);
    for (const auto &host : hosts) {
        if (!m_catalogChecked.contains(host)) { ++pending; continue; }
        for (const auto &request : m_requests) if (request.first == host && request.second == "ls") ++pending;
    }
    for (const auto &value : profiles()) {
        const auto p=value.toObject(), usage=p["usage"].toObject();
        if (p["installed"].toBool() && online(p["host"].toString())
            && (usage["refreshing"].toBool() || usage["status"] == "loading")) ++pending;
    }
    return pending;
}
void AccountsPage::updateRefreshState()
{
    const int pending = pendingAccounts();
    m_summarySpinner->setRunning(pending > 0);
    m_loadingSpinner->setRunning(pending > 0 && !m_hasSnapshot);
    m_refresh->setRefreshing(pending > 0);
    m_refresh->setEnabled(m_requests.isEmpty() && pending == 0);
    const auto caption = pending ? tr("Updating accounts… %1 remaining").arg(pending) : tr("Refresh accounts");
    m_refresh->setToolTip(caption); m_refresh->setAccessibleName(caption);
    QSet<QString> loadedHosts;for(const auto &value:m_displayProfiles)loadedHosts.insert(value.toObject()["host"].toString());
    if (accountHosts(m_fleet, m_explicitHost).isEmpty()) { m_summary->setText(remoteOnlyHint(m_fleet)); return; }
    const int accounts = m_list->count(), machines = loadedHosts.size();
    const auto loaded = tr("%1, %2").arg(accounts == 1 ? tr("1 account") : tr("%1 accounts").arg(accounts),
        machines == 1 ? tr("1 machine loaded") : tr("%1 machines loaded").arg(machines));
    m_summary->setText(!m_hasSnapshot ? tr("Loading accounts…") : loaded + (pending ? tr(" (Updating… %1 remaining)").arg(pending) : QString()));
}
void AccountsPage::ensureCatalogs(bool force)
{
    const auto hosts=accountHosts(m_fleet,m_explicitHost);
    const auto now=QDateTime::currentMSecsSinceEpoch();
    for(const auto &host:hosts) {
        bool pending=false;for(const auto &r:m_requests)if(r.first==host && r.second=="ls")pending=true;
        if(!pending && (force || !m_catalogChecked.contains(host) || now-m_catalogChecked[host]>=30000)) {
            m_catalogChecked[host]=now;request(host,{"ls"});
        }
    }
}
QJsonArray AccountsPage::profiles(bool includeRemoved) const
{
    QJsonArray result;auto hosts=m_catalogs.keys();hosts.sort();
    for(const auto &host:hosts)for(const auto *field:{"profiles","removed_profiles"})for(const auto &value:m_catalogs[host][field].toArray()) {
        const bool removed=QString::fromLatin1(field)=="removed_profiles";
        if(removed && !includeRemoved)continue;
        auto p=value.toObject();
        if(!launchable(m_fleet,host,p["provider"].toString()))continue;
        p["host"]=host;p["machine"]=host.isEmpty()?m_fleet.local().host:host;
        p["removed"]=removed;
        auto usage = m_usageStore->data(AccountUsageRef::profile(p));
        if (usage["status"] == "loading" || ((usage["status"] == "unavailable" || usage["status"] == "offline") && usage["identity"].toObject().isEmpty())) {
            const auto cached = p["account_status"].toObject();
            if (!cached.isEmpty()) { usage["identity"] = cached["identity"]; usage["checked_at"] = cached["checked_at"]; }
        }
        p["usage"]=usage; result.append(p);
    }
    return result;
}
QJsonArray AccountsPage::savedProfiles(const QString &host, const QString &provider) const
{
    QJsonArray saved;
    for (const auto &value : profiles(true)) {
        const auto p=value.toObject();
        if(p["removed"].toBool() && p["host"]==host && p["provider"]==provider)saved.append(p);
    }
    return saved;
}
QJsonArray AccountsPage::savedForAccount(const QString &host) const
{
    const auto key=AccountCatalog::profileKey(selected());
    for(const auto &value:AccountCatalog::group(profiles(true))) {
        const auto account=value.toObject();
        if(!AccountCatalog::contains(account,key))continue;
        QJsonArray saved;
        for(const auto &member:account["members"].toArray()) {
            const auto p=member.toObject();
            if(p["removed"].toBool() && p["host"]==host)saved.append(p);
        }
        return saved;
    }
    return {};
}
QJsonObject AccountsPage::selectedAccount() const { return m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toJsonObject() : QJsonObject(); }
QJsonObject AccountsPage::selected() const {
    const auto members = selectedAccount()["members"].toArray();
    for (const auto &member : members) if (AccountCatalog::profileKey(member.toObject()) == m_activeProfile) return member.toObject();
    for (const auto &member : members) if (member.toObject()["host"] == m_machine->currentData().toString()) return member.toObject();
    return members.isEmpty() ? QJsonObject() : members.first().toObject();
}
void AccountsPage::render()
{
    // Publish identities as they become known, without flashing unresolved
    // machine-local duplicates while another machine is still being inspected.
    QHash<QString,QJsonObject> previous;
    for(const auto &value:m_displayProfiles){const auto p=value.toObject();previous.insert(AccountCatalog::profileKey(p),p);}
    QJsonArray ready;
    for(const auto &value:profiles()) {
        const auto p=value.toObject(),usage=p["usage"].toObject(),identity=usage["identity"].toObject();
        const bool resolving=p["installed"].toBool() && online(p["host"].toString())
            && (usage["refreshing"].toBool() || usage["status"]=="loading");
        const bool identified=!identity["account_id"].toString().isEmpty() || !identity["email"].toString().trimmed().isEmpty();
        if(!resolving || identified)ready.append(p);
        else if(previous.contains(AccountCatalog::profileKey(p)))ready.append(previous.value(AccountCatalog::profileKey(p)));
    }
    m_displayProfiles=ready;
    if(!ready.isEmpty() || !pendingAccounts())m_hasSnapshot=true;
    m_bodyStack->setCurrentIndex(m_hasSnapshot?1:0);
    const auto old = selected(); const QString oldKey = AccountCatalog::profileKey(old);
    const auto desiredKey=m_requestedProfile.isEmpty()?oldKey:m_requestedProfile;
    const QSignalBlocker blocker(m_list);
    const int scroll = m_list->verticalScrollBar()->value();
    QListWidgetItem *selection = nullptr;
    int row = 0;
    QJsonArray displayed;
    for (const auto &value : m_displayProfiles) {
        const auto host=value.toObject()["host"].toString();
        if (host.isEmpty() || m_fleet.peerNames().contains(host) || host==m_explicitHost) displayed.append(value);
    }
    for (const auto &entry : AccountCatalog::group(displayed)) {
        const auto p = entry.toObject();
        bool visible = m_machine->currentData().toString() == "*";
        for (const auto &member : p["members"].toArray()) visible |= member.toObject()["host"] == m_machine->currentData().toString();
        if (!visible) continue;
        QStringList machines; for (const auto &machine : p["machines"].toArray()) machines << machine.toString();
        QListWidgetItem *item = nullptr;
        for (int i = row; i < m_list->count(); ++i) {
            const auto previous=m_list->item(i)->data(Qt::UserRole).toJsonObject();
            bool same=previous["account_key"]==p["account_key"];
            for(const auto &member:p["members"].toArray())same|=AccountCatalog::contains(previous,AccountCatalog::profileKey(member.toObject()));
            if(same){item=m_list->item(i);break;}
        }
        if (!item) { item = new QListWidgetItem; m_list->insertItem(row, item); }
        else if (m_list->row(item) != row) { m_list->takeItem(m_list->row(item)); m_list->insertItem(row, item); }
        QStringList permissionLines;
        for (const auto &member : p["members"].toArray()) {
            const auto m = member.toObject();
            permissionLines << m["machine"].toString() + ": " + permissionLabel(permissionMode(m)) + "\n" + permissionHint(m);
        }
        item->setToolTip(permissionLines.join("\n\n"));
        const auto text = p["label"].toString() + "\n" + providerName(p["provider"].toString()) + " / " + machines.join(", ")
            + "\nPermissions: " + permissionLabel(accountPermission(p));
        if (item->text() != text) { item->setText(text); item->setData(Qt::AccessibleTextRole, text); }
        if (item->data(Qt::UserRole).toJsonObject() != p) item->setData(Qt::UserRole, p);
        if (AccountCatalog::contains(p, desiredKey)) { selection = item; m_requestedProfile.clear(); }
        ++row;
    }
    while (m_list->count() > row) delete m_list->takeItem(row);
    if (selection) m_list->setCurrentItem(selection);
    if (!m_list->currentItem() && m_list->count()) m_list->setCurrentRow(0);
    m_list->verticalScrollBar()->setValue(scroll);
    selectionChanged();
}
void AccountsPage::selectionChanged()
{
    const auto p = selected(); const bool has = !p.isEmpty(); const bool idle = m_requests.isEmpty();
    m_machineCard->setVisible(has);m_providerBadge->setVisible(has);
    const bool nativeConfig=p["provider"]=="opencode";
    m_rename->setToolTip({});m_remove->setToolTip({});m_install->setToolTip({});
    m_rename->setEnabled(has && idle);
    m_permissions->setEnabled(has && idle && online(p["host"].toString()) && !nativeConfig);
    m_default->setVisible(has && p["provider"]!="dsh" && !nativeConfig);
    m_default->setEnabled(has && idle && online(p["host"].toString()) && !p["is_default"].toBool());
    m_default->setText((p["is_default"].toBool()?tr("Default on %1"):tr("Make default on %1")).arg(p["machine"].toString()));
    m_default->setToolTip(tr("Use this account for new %1 sessions on %2 when no account is specified. Existing sessions keep their account.").arg(providerName(p["provider"].toString()),p["machine"].toString()));
    const auto mode = permissionMode(p);
    m_permissionState->setVisible(has); m_permissions->setVisible(has);
    m_permissionState->setText(tr("Permissions on %1: %2").arg(p["machine"].toString(), permissionLabel(mode)));
    m_permissionState->setToolTip(permissionHint(p));
    m_permissionState->setStyleSheet(mode == "bypass" ? QString("color:%1;font-weight:600;").arg(m_dark ? "#ffa0aa" : "#a7243b") : QString());
    m_permissions->setToolTip(tr("Change permission defaults on %1").arg(p["machine"].toString()));
    m_login->setEnabled(has && idle && online(p["host"].toString()) && p["installed"].toBool() && !nativeConfig); m_remove->setEnabled(has && idle && online(p["host"].toString()));
    const bool machines = !accountHosts(m_fleet, m_explicitHost).isEmpty();
    m_add->setEnabled(idle && machines); m_add->setToolTip(machines ? QString() : remoteOnlyHint(m_fleet));
    updateRefreshState();
    m_install->setVisible(has && !p["installed"].toBool()); m_install->setEnabled(has && idle && online(p["host"].toString()) && !nativeConfig);
    m_startOpenCode->setVisible(false);
    if(!nativeConfig){m_openCodeProviders->hide();m_openCodeProvidersNote->hide();}
    m_remove->setText(has ? tr("Remove from %1…").arg(p["machine"].toString()) : tr("Remove…"));
    m_login->setText(tr("Sign in…")); m_login->setToolTip({});
    renderMachines();
    m_providerBadge->setValue(has ? p["provider"].toString() : QString());
    if (!has) { m_title->setText(tr("Choose an account")); m_detail->clear();m_identity->clear();m_usage->hide();return; }
    m_title->setText(selectedAccount()["label"].toString());
    const auto info=selectedAccount()["usage"].toObject();const auto identity=info.value("identity").toObject();
    QStringList identityLines;QStringList seen{m_title->text().trimmed().toCaseFolded()};
    for(const auto *field:{"name","email","organization"}) {
        const auto text=identity.value(field).toString().trimmed(), normalized=text.toCaseFolded();
        if(!text.isEmpty() && !seen.contains(normalized)){identityLines<<text;seen<<normalized;}
    }
    if(!identity.value("plan").toString().isEmpty())identityLines<<tr("Plan: %1").arg(identity.value("plan").toString());
    if(!identity.value("auth_method").toString().isEmpty() && identity["auth_method"]!="none")identityLines<<tr("Sign-in: %1").arg(identity.value("auth_method").toString());
    m_identity->setText(identityLines.isEmpty()?(identity.isEmpty()?(info["status"]=="loading"?tr("Reading account details…"):tr("Account identity is unavailable.")):QString()):identityLines.join('\n'));
    m_identity->setVisible(!m_identity->text().isEmpty());
    m_usage->show();m_usage->setData(info);
    const auto machine = p["machine"].toString(); const auto status = p["usage"].toObject()["status"].toString();
    const bool signedIn = status == "ok" || status == "expired" || p["usage"].toObject()["auth_status"] == "signed_in";
    m_login->setText(p["provider"] == "dsh" ? (status == "configured" ? tr("Change API key on %1…") : tr("Set API key on %1…")).arg(machine)
        : (signedIn ? tr("Sign in again on %1…") : tr("Sign in on %1…")).arg(machine));
    m_login->setToolTip(p["provider"] == "dsh" ? tr("Save a DeepSeek API key on %1.").arg(machine)
        : tr("Open %1 sign-in in a terminal on %2 for this profile. Completing sign-in updates the credentials used by this profile on %2.").arg(providerName(p["provider"].toString()), machine));
    m_detail->setText(!online(p["host"].toString()) ? tr("%1 is offline.").arg(machine)
        : !p["installed"].toBool() ? tr("%1 is not installed on %2.").arg(providerName(p["provider"].toString()), machine)
        : status == "configured" ? tr("API key configured on %1.").arg(machine)
        : status == "signed_out" && p["provider"] == "dsh" ? tr("Set a DeepSeek API key on %1 to use API models.").arg(machine)
        : status == "signed_out" ? tr("Sign in on %1 to use this account.").arg(machine)
        : status == "credentials_locked" ? tr("The login Keychain on %1 is locked. Unlock it in Keychain Access, then refresh.").arg(machine)
        : status == "desktop_session_unavailable" ? tr("Desktop sign-in on %1 is unavailable. Log in to macOS and check the HGS installation.").arg(machine)
        : status == "credentials_unavailable" ? tr("The saved sign-in on %1 could not be used. Check Keychain access or sign in again.").arg(machine)
        : status == "expired" ? tr("Sign-in expired on %1.").arg(machine)
        : status == "loading" ? tr("Checking sign-in on %1…").arg(machine)
        : signedIn ? tr("Ready on %1.").arg(machine) : tr("Account status is unavailable on %1.").arg(machine));
    if(nativeConfig) {
        const auto setup=tr("Configure providers, models and permissions with OpenCode on %1. Zerus uses that native configuration.").arg(machine);
        m_login->setToolTip(setup);m_permissions->setToolTip(setup);m_install->setToolTip(setup);
        m_copy->setEnabled(false);
        // hgs explains a missing or unsupported OpenCode, and a service it cannot use.
        const auto state=p["catalog_state"].toString();
        const auto reason=!p["installed"].toBool()?p["install_reason"].toString():!state.isEmpty()&&state!="ready"?p["catalog_reason"].toString():QString();
        m_detail->setText(!online(p["host"].toString())?tr("%1 is offline.").arg(machine):!reason.isEmpty()?reason
            :!p["installed"].toBool()?tr("%1 is not installed on %2.").arg(providerName("opencode"),machine):setup);
        const bool stopped=p["installed"].toBool()&&state=="not_running";
        m_startOpenCode->setText(tr("Start OpenCode on %1").arg(machine));
        m_startOpenCode->setVisible(stopped);m_startOpenCode->setEnabled(stopped&&idle&&online(p["host"].toString()));
        // Connected providers as plain text: label, connection and model count, never the home or a URL.
        QList<QStringList> rows;
        for(const auto &provider:OpenCodeModels::providers(p["providers"].toArray())) {
            auto connection=OpenCodeModels::kindText(provider.kind);
            if(!provider.activeLabel.isEmpty())connection+=QStringLiteral(" · ")+provider.activeLabel;
            else if(provider.credentials>1)connection+=QStringLiteral(" · ")+tr("%1 accounts").arg(provider.credentials);
            rows<<QStringList{provider.label,connection,provider.models<0?QString():provider.models==1?tr("1 model"):tr("%1 models").arg(provider.models)};
        }
        // Rebuilt only when a row changes, so polling keeps the list in place.
        bool same=m_openCodeProviders->topLevelItemCount()==rows.size();
        for(int i=0;same&&i<rows.size();++i)for(int c=0;same&&c<3;++c)same=m_openCodeProviders->topLevelItem(i)->text(c)==rows.at(i).at(c);
        if(!same){
            m_openCodeProviders->clear();
            for(const auto &row:rows)(new QTreeWidgetItem(m_openCodeProviders,row))->setFlags(Qt::ItemIsEnabled);
        }
        m_openCodeProviders->setVisible(!rows.isEmpty());
        QStringList notes;
        if(p["providers_truncated"].toBool())notes<<tr("Showing the first %1 providers.").arg(int(rows.size()));
        // hgs reports float seconds or null; anything else, or beyond year 9999, is no date.
        const auto at=p["catalog_at"].toDouble(-1);
        if(state!="ready"&&p["catalog_at"].isDouble()&&at>0&&at<253402300800.0)
            notes<<tr("Last known catalog from %1.").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(qint64(at)),QLocale::ShortFormat));
        m_openCodeProvidersNote->setText(notes.join('\n'));m_openCodeProvidersNote->setVisible(!notes.isEmpty());
    }
    m_detail->setToolTip(p["home"].toString());
    if(isVisible())inspectSelected();
}
void AccountsPage::inspectSelected(bool refresh)
{
    const auto p=selected();if(p.isEmpty() || !p["installed"].toBool())return;
    // The displayed snapshot can still precede a credential revision change.
    for (const auto &value : profiles()) if (AccountCatalog::profileKey(value.toObject())==AccountCatalog::profileKey(p)) {
        m_usageStore->ensure(AccountUsageRef::profile(value.toObject()),refresh); return;
    }
}
void AccountsPage::inspectProfiles(bool refresh)
{
    for (const auto &value : profiles()) {
        const auto p = value.toObject();
        if (p["installed"].toBool()) m_usageStore->ensure(AccountUsageRef::profile(p), refresh);
    }
}
bool AccountsPage::online(const QString &host) const
{
    // This computer: usable unless it is control-only (no agents of its own).
    if (host.isEmpty()) return m_fleet.local().localSessions;
    const auto *box = m_fleet.peer(host);
    return !box || m_fleet.peerPolledAt(host) <= 0 || box->ok;
}
void AccountsPage::renderMachines()
{
    const auto account = selectedAccount(); const auto chosen = selected();
    QStringList signature{account["account_key"].toString(), AccountCatalog::profileKey(chosen), QString::number(m_dark)};
    QStringList present;
    for (const auto &value : account["members"].toArray()) {
        const auto p = value.toObject(); present << p["host"].toString();
        signature << AccountCatalog::profileKey(p) << p["label"].toString() << QString::number(online(p["host"].toString()));
    }
    auto hosts = accountHosts(m_fleet, m_explicitHost);
    hosts.removeIf([&](const QString &host) { return !launchable(m_fleet, host, account["provider"].toString()); });
    for (const auto &host : hosts) signature << host << QString::number(online(host));
    int available = 0; for (const auto &host : hosts) if (!account.isEmpty() && !present.contains(host)) ++available;
    m_copy->setEnabled(!account.isEmpty() && m_requests.isEmpty() && available > 0);
    for (int i = 0; i < m_machineTags->count(); ++i) if (auto *tag = m_machineTags->itemAt(i)->widget(); tag && tag->objectName()=="accountDestinationTag")
        tag->setEnabled(m_requests.isEmpty() && online(tag->property("host").toString()));
    if (m_machineSignature == signature.join('\n')) return;
    m_machineSignature = signature.join('\n');
    while (auto *item = m_machineTags->takeAt(0)) { if (item->widget()) { item->widget()->hide(); item->widget()->deleteLater(); } delete item; }
    const bool idle = m_requests.isEmpty();
    const auto tagStyle=QString("QPushButton { padding:5px 10px; border:1px solid %1; border-radius:6px; } QPushButton:checked { background:%2; border-color:%3; }")
        .arg(m_dark ? "#34414b" : "#c7d3dc", m_dark ? "#24473e" : "#d8eee5", m_dark ? "#8bdfc0" : "#167357");
    QStringList addedHosts;
    for (const auto &value : account["members"].toArray()) {
        const auto p = value.toObject(); const auto key = AccountCatalog::profileKey(p);
        if (addedHosts.contains(p["host"].toString())) continue;
        addedHosts << p["host"].toString();
        auto *tag = new QPushButton(p["machine"].toString()); tag->setObjectName("accountMachineTag");
        tag->setProperty("host", p["host"].toString()); tag->setProperty("profile", p["id"].toString());
        tag->setCheckable(true); tag->setChecked(p["host"] == chosen["host"]);
        tag->setToolTip(p["label"].toString() + (online(p["host"].toString()) ? QString() : tr(" (offline)")));
        tag->setStyleSheet(tagStyle);
        connect(tag, &QPushButton::clicked, this, [this, key] { m_activeProfile = key; selectionChanged(); });
        m_machineTags->addWidget(tag);
    }
    {
        const QSignalBlocker blocker(m_profileChoice); m_profileChoice->clear();
        for (const auto &member : account["members"].toArray()) {
            const auto p = member.toObject(); if (p["host"] != chosen["host"]) continue;
            m_profileChoice->addItem(p["label"].toString() + " / " + p["id"].toString(), AccountCatalog::profileKey(p));
        }
        m_profileChoice->setCurrentIndex(qMax(0, m_profileChoice->findData(AccountCatalog::profileKey(chosen))));
        m_profileChoice->setVisible(m_profileChoice->count() > 1);
    }
    for (const auto &host : hosts) {
        if (account.isEmpty() || present.contains(host)) continue;
        auto *tag = new QPushButton(tr("+ Add to %1").arg(host.isEmpty() ? m_fleet.local().host : host)); tag->setObjectName("accountDestinationTag");
        tag->setStyleSheet(tagStyle);
        tag->setProperty("host", host); tag->setEnabled(idle && online(host));
        tag->setToolTip(online(host) ? tr("Add this account to %1").arg(host.isEmpty() ? m_fleet.local().host : host) : tr("Machine is offline"));
        connect(tag, &QPushButton::clicked, this, [this, host] { copy(host); }); m_machineTags->addWidget(tag);
    }
    m_copy->setEnabled(!account.isEmpty() && idle && available > 0);
}
void AccountsPage::rename()
{
    const auto p=selected();if(p.isEmpty())return;
    QDialog dialog(this);dialog.setWindowTitle(tr("Rename account"));dialog.setObjectName("renameAccountDialog");dialog.resize(420,160);
    auto *layout=new QVBoxLayout(&dialog);layout->addWidget(label(tr("Account name")));
    auto *name=new QLineEdit(p["label"].toString());name->setObjectName("accountName");name->setMaxLength(160);layout->addWidget(name);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addWidget(buttons);
    connect(name,&QLineEdit::textChanged,&dialog,[=]{buttons->button(QDialogButtonBox::Save)->setEnabled(!name->text().trimmed().isEmpty());});
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);name->selectAll();name->setFocus();
    if(dialog.exec()!=QDialog::Accepted || name->text().trimmed()==p["label"].toString())return;
    m_renameLabel = name->text().trimmed();
    for (const auto &member : selectedAccount()["members"].toArray()) m_renameQueue.append(member.toObject());
    renameNext();
}
void AccountsPage::renameNext()
{
    if (m_renameQueue.isEmpty()) return;
    const auto p = m_renameQueue.takeFirst(); const auto host = p["host"].toString();
    request(host, {"rename", p["id"].toString(), "--label", m_renameLabel, "--revision", m_catalogs[host]["revision"].toString()});
}
void AccountsPage::add()
{
    QDialog dialog(this); dialog.setWindowTitle(tr("Add agent account")); dialog.setObjectName("accountDialog");
    auto *layout = new QVBoxLayout(&dialog);layout->setContentsMargins(28,24,28,24);layout->setSpacing(18);layout->setSizeConstraint(QLayout::SetMinimumSize);
    auto *heading = label(tr("Add account"));heading->setObjectName("heading");layout->addWidget(heading);
    auto *form = new QFormLayout;form->setSpacing(12);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *machine = new QComboBox;machine->setObjectName("addAccountMachine"); fillMachines(machine, m_fleet, m_explicitHost); machine->setCurrentIndex(qMax(0, machine->findData(m_machine->currentData())));
    // Agents use New session's rows: a colored logo, the agent and what adding does.
    auto *provider = new AgentList;provider->setObjectName("addAccountProvider");provider->setAccessibleName(tr("Agent"));
    provider->setProperty("hgsDark",m_dark);provider->setItemDelegate(new AccountChoice::Delegate(provider));
    provider->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);provider->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    provider->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    const auto agent=[provider] {return provider->currentItem()?provider->currentItem()->data(AccountChoice::ProviderRole).toString():QString();};
    // Every field stays in place for every agent; ones that do not apply are disabled.
    auto *saved=new QComboBox;saved->setObjectName("savedAccount");
    saved->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);saved->setMinimumContentsLength(16);
    auto *name = new QLineEdit;name->setObjectName("addAccountName"); name->setPlaceholderText(tr("Work, Personal, Team…"));
    form->addRow(tr("Machine"), machine); form->addRow(tr("Agent"), provider);form->addRow(tr("Account"),saved);form->addRow(tr("Account name"), name); layout->addLayout(form);
    auto *hint = new ReservedLabel;hint->setObjectName("muted");layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto *create = buttons->addButton(tr("Create and sign in"), QDialogButtonBox::AcceptRole); create->setEnabled(false); layout->addWidget(buttons);
    const auto where=[=] {const auto host=machine->currentData().toString();return host.isEmpty()?m_fleet.local().host:host;};
    const auto update=[=] {
        const bool reuse=!saved->currentData().toJsonObject().isEmpty();const auto host=machine->currentData().toString();const auto chosen=agent();
        // No machine to choose (a control-only computer without remote machines).
        const bool none=machine->count()==0;
        if(name->isEnabled()==reuse) {
            // A saved account keeps its name; a typed new name returns with New account.
            const QSignalBlocker blocker(name);
            if(reuse){name->setProperty("typedName",name->text());name->clear();}else name->setText(name->property("typedName").toString());
            name->setEnabled(!reuse);
        }
        create->setEnabled(!none && online(host) && !chosen.isEmpty() && (reuse || (!name->text().trimmed().isEmpty() && name->text().size()<=160)));
        create->setText(reuse?tr("Add account"):chosen=="dsh"?tr("Enter API key…"):tr("Create and sign in"));
        const QString openCode=tr("Use OpenCode's native configuration on %1 for new sessions again.").arg(where()),
            restore=tr("Use the saved sign-in on %1. The account will be available for new sessions again.").arg(where()),
            key=tr("Configure a DeepSeek API key on the selected machine."),fresh=tr("Create a separate account profile on the selected machine, then sign in.");
        hint->reserve({remoteOnlyHint(m_fleet),openCode,restore,key,fresh});
        hint->setText(none?remoteOnlyHint(m_fleet):reuse&&chosen=="opencode"?openCode:reuse?restore:chosen=="dsh"?key:fresh);
    };
    const auto populate=[=] {
        const QSignalBlocker blocker(saved);saved->clear();const auto chosen=agent();
        const auto icon=workspaceIcon(chosen,IdentityBadges::providerColor(chosen,m_dark));
        for(const auto &value:savedProfiles(machine->currentData().toString(),chosen)) {
            const auto p=value.toObject();const auto email=p["usage"].toObject()["identity"].toObject()["email"].toString();
            saved->addItem(icon,email.isEmpty() || email==p["label"].toString()?p["label"].toString():p["label"].toString()+" ("+email+")",p);
        }
        // OpenCode has one native configuration per machine and no separate profiles.
        if(chosen!="opencode")saved->addItem(tr("New account"),QJsonObject());
        saved->setCurrentIndex(0);saved->setEnabled(saved->count()>1);update();
    };
    const auto providers=[=] {
        const auto host=machine->currentData().toString(),previous=agent();
        const auto listed=[&](const QString &id) {
            for(const auto &value:m_catalogs.value(host)["profiles"].toArray())if(value.toObject()["id"]==id)return true;
            return false;
        };
        const QSignalBlocker blocker(provider);provider->clear();int row=0;
        for (const auto *value : {"codex", "claude", "kimi", "dsh", "opencode"}) {
            const QString id=value;if(!launchable(m_fleet,host,id))continue;
            auto *item=new QListWidgetItem(workspaceIcon(id,IdentityBadges::providerColor(id,m_dark)),providerName(id),provider);
            item->setData(AccountChoice::ProviderRole,id);
            const bool reusable=!savedProfiles(host,id).isEmpty();
            QString detail=id=="dsh"?(reusable?tr("Saved API key or a new one"):tr("DeepSeek API key"))
                :reusable?tr("Saved account or a new profile"):tr("New profile with its own sign-in");
            if(id=="opencode"){
                detail=reusable?tr("Add the native configuration again"):listed("native-opencode")?tr("Native configuration, already added"):tr("Not installed on %1").arg(where());
                if(!reusable){
                    item->setFlags(item->flags()&~(Qt::ItemIsEnabled|Qt::ItemIsSelectable));
                    item->setToolTip(listed("native-opencode")?tr("Configure providers, models and permissions with OpenCode on %1. Zerus uses that native configuration.").arg(where())
                        :tr("Install OpenCode on %1, then refresh Accounts.").arg(where()));
                }
            }
            item->setData(AccountChoice::UsageTextRole,detail);item->setData(Qt::AccessibleTextRole,providerName(id)+'\n'+detail);
            // Keep the chosen agent where this machine offers it, else the first one.
            if(id==previous&&(item->flags()&Qt::ItemIsEnabled))row=provider->count()-1;
        }
        provider->setCurrentRow(row);provider->updateGeometry();
    };
    connect(name,&QLineEdit::textChanged,&dialog,update);connect(saved,&QComboBox::currentIndexChanged,&dialog,update);
    connect(provider,&QListWidget::currentItemChanged,&dialog,populate);
    connect(machine,&QComboBox::currentIndexChanged,&dialog,[=]{providers();populate();});providers();populate();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.ensurePolished();
    // The action button is as wide as its longest label, so Cancel never moves.
    int widest=0;
    for(const auto &text:{tr("Create and sign in"),tr("Add account"),tr("Enter API key…")}){create->setText(text);widest=qMax(widest,create->sizeHint().width());}
    create->setMinimumWidth(widest);update();
    // Wrap the hint at its final width, then size the dialog around it.
    dialog.adjustSize();hint->fit();dialog.adjustSize();
    if (dialog.exec() != QDialog::Accepted || machine->count() == 0 || !online(machine->currentData().toString()) || agent().isEmpty()) return;
    const auto existing=saved->currentData().toJsonObject();if(!existing.isEmpty()){useSavedProfile(existing);return;}
    if (agent() == "opencode") return;
    if (agent() == "dsh") { setupDeepSeek(machine->currentData().toString(), name->text().trimmed()); return; }
    const QString id = agent() + "-" + QUuid::createUuid().toString(QUuid::Id128).left(10); const QString host = machine->currentData().toString();
    m_loginAfterAddHost = host; m_loginAfterAddId = id;
    m_activeProfile = m_requestedProfile = profileKey(host, id);
    { const QSignalBlocker blocker(m_machine); m_machine->setCurrentIndex(0); }
    QStringList args{"add", id, "--provider", agent(), "--label", name->text().trimmed()};
    if (m_catalogs.contains(host)) args << "--revision" << m_catalogs[host]["revision"].toString();
    request(host, args);
}
void AccountsPage::copy(const QString &target)
{
    const auto account = selectedAccount(); const auto p = selected(); if (p.isEmpty()) return;
    QDialog dialog(this); dialog.setWindowTitle(tr("Add account to a machine")); dialog.setObjectName("accountDialog"); dialog.resize(520, 330);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label(tr("Use %1 on another machine.").arg(account["label"].toString())));
    auto *form = new QFormLayout; auto *machine = new QComboBox; machine->setObjectName("accountDestination");
    QStringList present; for (const auto &member : account["members"].toArray()) present << member.toObject()["host"].toString();
    for (const auto &host : accountHosts(m_fleet, m_explicitHost))
        if (!present.contains(host) && online(host) && launchable(m_fleet, host, p["provider"].toString())) machine->addItem(host.isEmpty() ? m_fleet.local().host : host, host);
    if (machine->count() == 0) return;
    if (target != "*") machine->setCurrentIndex(qMax(0, machine->findData(target)));
    if (p["provider"] == "dsh") {
        const auto saved=savedForAccount(machine->currentData().toString());
        if(saved.size()==1)useSavedProfile(saved.first().toObject());
        else setupDeepSeek(machine->currentData().toString(), account["label"].toString());
        delete machine; delete form;
        return;
    }
    auto *source = new QComboBox; source->setObjectName("accountCopySource");
    for (const auto &member : account["members"].toArray()) {
        const auto profile = member.toObject();
        if (profile["portable"].toBool() && profile["credential_file"].toBool() && online(profile["host"].toString()))
            source->addItem(profile["machine"].toString() + " / " + profile["label"].toString(), profile);
    }
    form->addRow(tr("Machine"), machine); form->addRow(tr("Copy from"), source); layout->addLayout(form);
    auto *reuse=new QRadioButton(tr("Use saved sign-in on this machine"));reuse->setObjectName("reuseSavedSignIn");layout->addWidget(reuse);
    auto *saved=new QComboBox;saved->setObjectName("savedAccount");layout->addWidget(saved);
    auto *transfer = new QRadioButton(tr("Copy existing sign-in over SSH")); transfer->setObjectName("copyExistingSignIn");
    auto *signIn = new QRadioButton(tr("Sign in on this machine")); signIn->setObjectName("signInOnDestination");
    layout->addWidget(transfer); layout->addWidget(signIn);
    transfer->setEnabled(source->count() > 0); transfer->setChecked(source->count() > 0); signIn->setChecked(source->count() == 0);
    source->setEnabled(transfer->isChecked()); connect(transfer, &QRadioButton::toggled, source, &QWidget::setEnabled);
    connect(reuse,&QRadioButton::toggled,saved,&QWidget::setEnabled);
    const auto updateSaved=[=] {
        saved->clear();for(const auto &value:savedForAccount(machine->currentData().toString())) {
            const auto profile=value.toObject();saved->addItem(profile["label"].toString(),profile);
        }
        const bool any=saved->count()>0;reuse->setVisible(any);saved->setVisible(any);
        if(any)reuse->setChecked(true);else if(source->count()>0)transfer->setChecked(true);else signIn->setChecked(true);
        saved->setEnabled(any && reuse->isChecked());
    };
    connect(machine,&QComboBox::currentIndexChanged,&dialog,updateSaved);updateSaved();
    layout->addWidget(label(tr("A separate profile keeps other accounts and sessions on that machine intact. After sign-in, the provider identity determines which account it belongs to.")));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel); auto *apply = buttons->addButton(tr("Add account"), QDialogButtonBox::AcceptRole); apply->setObjectName("confirmAddToMachine"); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    if(reuse->isChecked()){useSavedProfile(saved->currentData().toJsonObject());return;}
    const QString id = p["provider"].toString() + "-" + QUuid::createUuid().toString(QUuid::Id128).left(10);
    const auto host = machine->currentData().toString(); m_requestedProfile = profileKey(host, id);
    m_activeProfile = m_requestedProfile;
    { const QSignalBlocker blocker(m_machine); m_machine->setCurrentIndex(0); }
    if (transfer->isChecked()) {
        const auto from = source->currentData().toJsonObject();
        request({}, {"copy", from["id"].toString(), "--from", from["host"].toString().isEmpty() ? "@local" : from["host"].toString(), "--to", host.isEmpty() ? "@local" : host, "--as", id, "--label", account["label"].toString()});
    } else {
        m_loginAfterAddHost = host; m_loginAfterAddId = id;
        QStringList args{"add", id, "--provider", p["provider"].toString(), "--label", account["label"].toString()};
        if (m_catalogs.contains(host)) args << "--revision" << m_catalogs[host]["revision"].toString();
        request(host, args);
    }
}
void AccountsPage::setupDeepSeek(const QString &host, const QString &accountLabel)
{
    const auto machine = host.isEmpty() ? m_fleet.local().host : host;
    if (!online(host)) { emit notice(tr("%1 is offline.").arg(machine), true); return; }
    bool installed = false;
    const auto catalog = m_catalogs.value(host);
    for (const auto *field : {"profiles", "removed_profiles"})
        for (const auto &value : catalog[field].toArray())
            if (value.toObject()["id"] == "native-dsh") installed = value.toObject()["installed"].toBool();
    if (!installed) {
        emit installRequested(host, "dsh");
        emit notice(tr("Finish installing DeepSeek on %1, then refresh Accounts and set its API key.").arg(machine));
        return;
    }
    AccountKeyDialog dialog(this); dialog.setObjectName("deepSeekKeyDialog");
    dialog.setWindowTitle(tr("DeepSeek API key on %1").arg(machine)); dialog.resize(460, 220);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(label(tr("Save an API key on %1. DeepSeek on that machine will use this key.").arg(machine)));
    auto *key = new QLineEdit; key->setObjectName("deepSeekApiKey"); key->setEchoMode(QLineEdit::Password);
    key->setMaxLength(4096); key->setPlaceholderText(tr("API key")); layout->addWidget(key);
    auto *status = label(); status->setObjectName("deepSeekKeyStatus"); layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto *save = buttons->button(QDialogButtonBox::Save); save->setText(tr("Save on %1").arg(machine)); save->setEnabled(false);
    layout->addWidget(buttons);
    connect(key, &QLineEdit::textChanged, &dialog, [=](const QString &text) { save->setEnabled(!text.trimmed().isEmpty()); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    quint64 pending = 0;
    connect(save, &QPushButton::clicked, &dialog, [&] {
        dialog.saving = true; key->setEnabled(false); buttons->setEnabled(false); status->setText(tr("Saving on %1…").arg(machine));
        QJsonObject data{{"api_key", key->text().trimmed()}};
        if (!accountLabel.isEmpty()) data["label"] = accountLabel;
        pending = m_client.requestAccounts(host, {"set-key", "native-dsh", "--json", "--revision", m_catalogs[host]["revision"].toString()}, QJsonDocument(data).toJson(QJsonDocument::Compact));
        m_requests[pending] = {host, "set-key"}; selectionChanged();
    });
    connect(&m_client, &HgsClient::accountsReady, &dialog, [&](quint64 id, const QString &, const QJsonObject &) {
        if (id != pending || !pending) return;
        m_activeProfile = m_requestedProfile = profileKey(host, "native-dsh");
        { const QSignalBlocker blocker(m_machine); m_machine->setCurrentIndex(0); }
        dialog.saving = false; key->clear(); dialog.accept(); render();
    });
    connect(&m_client, &HgsClient::accountsFailed, &dialog, [&](quint64 id, const QString &, const QString &error) {
        if (id != pending || !pending) return;
        pending = 0; dialog.saving = false; key->setEnabled(true); buttons->setEnabled(true); status->setText(error);
    });
    key->setFocus(); dialog.exec(); key->clear();
}
void AccountsPage::remove()
{
    const auto p = selected(); if (p.isEmpty() || !m_requests.isEmpty() || !online(p["host"].toString())) return;
    const QString host = p["host"].toString(), machine = p["machine"].toString();
    const QString revision = m_catalogs[host]["revision"].toString();
    QMessageBox dialog(QMessageBox::Question, tr("Remove account from Zerus"),
        tr("Remove “%1” from %2?").arg(selectedAccount()["label"].toString(), machine), QMessageBox::Cancel, this);
    dialog.setObjectName("removeAccountDialog"); dialog.setTextFormat(Qt::PlainText);
    dialog.setInformativeText(p["provider"]=="opencode"
        ? tr("OpenCode will disappear from Accounts and New session for %1 in every Zerus connected to %1, including Zerus running on %1.\n\nIts native configuration, provider credentials and existing sessions on %1 are kept.\n\nTo use it here again, choose Add account.").arg(machine)
        : tr("This account will disappear from Accounts and New session for %1 in every Zerus connected to %1, including Zerus running on %1.\n\nSign-in credentials and existing sessions on %1 are kept. The account stays available on its other machines.\n\nTo use it here again, choose Add account or Add to %1. Zerus can reuse the saved sign-in.").arg(machine));
    auto *confirm = dialog.addButton(tr("Remove from %1").arg(machine), QMessageBox::DestructiveRole);
    dialog.setDefaultButton(QMessageBox::Cancel); dialog.setEscapeButton(QMessageBox::Cancel);
    dialog.exec();
    if (dialog.clickedButton() != confirm) return;
    request(host, {"rm", p["id"].toString(), "--revision", revision});
}

void AccountsPage::useSavedProfile(const QJsonObject &p)
{
    const auto host=p["host"].toString();
    if(p["id"].toString().isEmpty() || !online(host) || !m_requests.isEmpty())return;
    m_activeProfile=m_requestedProfile=profileKey(host,p["id"].toString());
    {const QSignalBlocker blocker(m_machine);m_machine->setCurrentIndex(0);}
    request(host,{"restore",p["id"].toString(),"--revision",m_catalogs[host]["revision"].toString()});
}

void AccountsPage::permissions()
{
    const auto p = selected(); if (p.isEmpty() || !online(p["host"].toString())) return;
    QDialog dialog(this); dialog.setObjectName("accountPermissionsDialog"); dialog.setWindowTitle(tr("Account permissions")); dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);layout->setContentsMargins(18,18,18,18);layout->setSpacing(12);layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->addWidget(label(tr("%1 on %2").arg(selectedAccount()["label"].toString(),p["machine"].toString())));
    auto *mode = new QComboBox; mode->setObjectName("accountPermissionMode");
    mode->addItem(tr("Default (provider settings)"),"provider"); mode->addItem(tr("Bypass permissions"),"bypass");mode->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    mode->setCurrentIndex(p["permission_mode"] == "bypass" ? 1 : 0);layout->addWidget(mode);
    const auto provider = p["provider"].toString();
    auto *explanation = label(); explanation->setObjectName("permissionExplanation"); layout->addWidget(explanation);
    const auto explain = [=] {
        explanation->setText(mode->currentData() == "provider"
            ? tr("Inherit %1 settings on %2. %3").arg(providerName(provider), p["machine"].toString(),
                p["permission_mode"] == "bypass" ? QString() : p["permission_detail"].toString())
            : provider == "kimi" ? tr("Run tools and answer questions automatically (Never Ask).")
            : provider == "codex" || provider == "dsh" ? tr("Full access without approval prompts or a sandbox.")
            : tr("Run tools without permission prompts."));
    };
    connect(mode, &QComboBox::currentIndexChanged, &dialog, explain); explain();
    auto *scope = label(tr("New and resumed sessions only. Running sessions keep their mode.")); scope->setObjectName("muted"); layout->addWidget(scope);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    const auto revision = m_catalogs[p["host"].toString()]["revision"].toString();
    dialog.ensurePolished();mode->setMinimumHeight(mode->sizeHint().height());
    for(auto *button:buttons->buttons())button->setMinimumSize(button->sizeHint());
    connect(mode,&QComboBox::currentIndexChanged,&dialog,[&dialog]{QTimer::singleShot(0,&dialog,[&dialog]{dialog.adjustSize();});});
    dialog.adjustSize();
    if(dialog.exec()!=QDialog::Accepted || mode->currentData().toString() == p["permission_mode"].toString("provider"))return;
    request(p["host"].toString(),{"permissions",p["id"].toString(),"--mode",mode->currentData().toString(),"--revision",revision});
}
