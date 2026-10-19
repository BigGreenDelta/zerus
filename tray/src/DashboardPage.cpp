#include "DashboardPage.h"
#include "SessionCardDelegate.h"
#include "SessionPresentation.h"
#include <QStandardItemModel>
#include "WorkspaceIcons.h"
#include "WorkspacePageHeader.h"
#include "MachineAppearance.h"
#include "IdentityBadge.h"
#include "AccountUsage.h"
#include "AccountCatalog.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
QString bytes(double value)
{
    return value >= 1024. * 1024. * 1024.
        ? QStringLiteral("%1 GiB").arg(value / (1024. * 1024. * 1024.), 0, 'f', 1)
        : QStringLiteral("%1 MiB").arg(value / (1024. * 1024.), 0, 'f', 0);
}
QString age(double timestamp)
{
    if (timestamp <= 0) return DashboardPage::tr("No sample yet");
    const auto seconds = qMax<qint64>(0, QDateTime::currentSecsSinceEpoch() - qint64(timestamp));
    if (seconds < 10) return DashboardPage::tr("Sampled just now");
    if (seconds < 60) return DashboardPage::tr("Sampled %1s ago").arg(seconds);
    if (seconds < 3600) return DashboardPage::tr("Sampled %1m ago").arg(seconds / 60);
    return DashboardPage::tr("Last sample %1").arg(QDateTime::fromSecsSinceEpoch(qint64(timestamp)).toString("d MMM HH:mm"));
}
QLabel *label(const QString &text, const QString &name, QWidget *parent = nullptr)
{
    auto *result = new QLabel(text, parent); result->setObjectName(name); result->setTextFormat(Qt::PlainText);
    return result;
}
// Polls mostly repeat values. Re-polish, relayout or repaint only on a change.
void setStyleProperty(QWidget *widget, const char *name, const QVariant &value)
{
    if (widget->property(name) == value) return;
    widget->setProperty(name, value); widget->style()->unpolish(widget); widget->style()->polish(widget); widget->update();
}
void setBadge(IdentityBadge *badge, const QString &value, bool dark)
{
    if (badge->value() != value || (value.isEmpty() && !badge->isHidden())) badge->setValue(value);
    if (badge->dark() != dark) badge->setTheme(dark);
}
void updateLoadBar(QProgressBar *bar, double percentage, bool fresh, bool available)
{
    available = available && std::isfinite(percentage) && percentage >= 0;
    bar->setValue(available ? qRound(qBound(0., percentage, 100.) * 10) : 0);
    bar->setEnabled(fresh && available);
    const QString level = !fresh || !available ? "unknown" : percentage >= 90 ? "critical" : percentage >= 70 ? "warning" : "normal";
    if (bar->property("loadLevel").toString() != level) {
        bar->setProperty("loadLevel", level); bar->style()->unpolish(bar); bar->style()->polish(bar); bar->update();
    }
    bar->setToolTip(!available ? DashboardPage::tr("No sample available") : !fresh ? DashboardPage::tr("Last known value; waiting for a fresh sample.")
        : DashboardPage::tr("Orange from 70%; red from 90%."));
}
// Fills itself with the stylesheet background, which the stylesheet keeps in its
// palette. Being opaque lets Qt scroll by moving pixels and skip the page and
// viewport backgrounds beneath it; a styled QWidget would repaint them all.
class DashboardContent : public QWidget {
public:
    DashboardContent() { setObjectName("dashboardContent"); setAttribute(Qt::WA_OpaquePaintEvent); }
protected:
    void paintEvent(QPaintEvent *event) override { QPainter(this).fillRect(event->rect(), palette().window()); }
};
}

// Stable widgets survive background polls, including keyboard focus and pointer clicks.
class DashboardMachineCard : public QFrame {
public:
    explicit DashboardMachineCard(QWidget *parent) : QFrame(parent)
    {
        setObjectName("dashboardMachineCard");
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(18, 16, 18, 16); layout->setSpacing(12);
        auto *top = new QHBoxLayout; indicator = new IdentityBadge(IdentityBadges::Machine); indicator->setObjectName("dashboardMachineBadge"); top->addWidget(indicator); title = label({}, "dashboardMachineTitle"); title->setMinimumWidth(0);
        status = label({}, "dashboardMachineStatus");
        settings = new QPushButton; settings->setObjectName("dashboardMachineSettings"); settings->setFixedSize(28, 28);
        settings->setToolTip(tr("Machine settings")); settings->setAccessibleName(tr("Machine settings"));
        top->addWidget(title, 1); top->addWidget(status); top->addWidget(settings); layout->addLayout(top);
        auto *grid = new QGridLayout; grid->setHorizontalSpacing(24); grid->setVerticalSpacing(6);
        grid->addWidget(label(tr("CPU"), "dashboardMuted"), 0, 0); grid->addWidget(label(tr("Memory"), "dashboardMuted"), 0, 1);
        cpu = label({}, "dashboardMetric"); memory = label({}, "dashboardMetric");
        grid->addWidget(cpu, 1, 0); grid->addWidget(memory, 1, 1);
        cpuBar = new QProgressBar; ramBar = new QProgressBar;
        cpuBar->setObjectName("dashboardCpuBar"); ramBar->setObjectName("dashboardMemoryBar");
        for (auto *bar : {cpuBar, ramBar}) { bar->setRange(0, 1000); bar->setTextVisible(false); bar->setFixedHeight(5); }
        grid->addWidget(cpuBar, 2, 0); grid->addWidget(ramBar, 2, 1);
        cpuDetail = label({}, "dashboardMuted"); memoryDetail = label({}, "dashboardMuted");
        grid->addWidget(cpuDetail, 3, 0); grid->addWidget(memoryDetail, 3, 1);
        grid->setColumnStretch(0, 1); grid->setColumnStretch(1, 1); layout->addLayout(grid);
        auto *bottom = new QHBoxLayout;
        sessions = new QPushButton; sessions->setObjectName("dashboardMachineSessions"); bottom->addWidget(sessions); bottom->addStretch();
        sampled = label({}, "dashboardMuted"); bottom->addWidget(sampled); layout->addLayout(bottom);
    }
    void update(const BoxState &box, const QString &alias, bool local, bool fresh)
    {
        title->setText(local ? tr("This machine") : QString());
        setBadge(indicator, alias, indicator->dark());
        title->setToolTip(alias); status->setText(fresh ? tr("Connected") : box.ok ? tr("Stale") : tr("Offline"));
        setStyleProperty(status, "online", fresh);
        const auto metrics = box.metrics;
        const bool hasCpu = metrics.value("cpu_percent").isDouble();
        const auto percentage = metrics.value("cpu_percent").toDouble();
        cpu->setText(hasCpu ? tr("%1%").arg(percentage, 0, 'f', 0) : QStringLiteral("—"));
        updateLoadBar(cpuBar, percentage, fresh, hasCpu);
        const auto cores = metrics.value("cpu_count").toInt();
        cpuDetail->setText(hasCpu ? tr("%1 cores, load %2").arg(cores).arg(metrics.value("load_1").toDouble(), 0, 'f', 2)
            : metrics.isEmpty() ? tr("Update hgs for metrics") : metrics.value("cpu_state").toString() == "unavailable" ? tr("CPU metrics unavailable") : tr("Waiting for second sample"));
        cpu->setToolTip(hasCpu ? tr("Average CPU use over the last %1 seconds, across all logical cores.").arg(metrics.value("cpu_interval_seconds").toDouble(), 0, 'f', 1) : tr("CPU use needs two samples. Refresh again after a moment."));
        const auto total = metrics.value("memory_total_bytes").toDouble(); const auto used = metrics.value("memory_used_bytes").toDouble();
        memory->setText(total > 0 ? bytes(used) : QStringLiteral("—"));
        memoryDetail->setText(total > 0 ? tr("of %1 (%2%)").arg(bytes(total)).arg(100. * used / total, 0, 'f', 0) : tr("Memory unavailable"));
        memory->setToolTip(metrics.value("platform").toString() == "macos"
            ? tr("App, wired and compressed memory. Reclaimable file cache is excluded.")
            : tr("Total memory minus available memory, including reclaimable cache."));
        updateLoadBar(ramBar, total > 0 ? 100. * used / total : 0, fresh, total > 0);
        int active = 0, needs = 0;
        for (const auto &session : box.sessions) { if (session.state != "archived") ++active; if (!session.muted && session.needsAttention()) ++needs; }
        const auto countLabel = active == 1 ? tr("1 session") : tr("%1 sessions").arg(active);
        sessions->setText(countLabel + (needs ? tr(", %1 need attention").arg(needs) : QString()));
        sessions->setToolTip(fresh ? tr("View sessions on %1").arg(alias) : tr("View last known sessions on %1").arg(alias));
        sampled->setText(age(metrics.value("sampled_at").toDouble()));
        sampled->setToolTip(fresh ? tr("Metrics update with the normal machine poll. Use Refresh for a new sample.") : tr("Last known values. This machine is unavailable or has not replied recently."));
    }
    QLabel *title, *status, *cpu, *memory, *cpuDetail, *memoryDetail, *sampled;
    IdentityBadge *indicator;
    QProgressBar *cpuBar, *ramBar;
    QPushButton *settings, *sessions;
};

class DashboardSessionRow : public QPushButton {
public:
    explicit DashboardSessionRow(QWidget *parent) : QPushButton(parent), m_model(this), m_delegate(this)
    {
        setObjectName("dashboardSessionRow");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); setFixedHeight(104);
        setAttribute(Qt::WA_Hover);
        m_model.setObjectName("dashboardSessionModel");
        m_model.appendRow(new QStandardItem);
    }
    void setTheme(bool dark) { if (property("hgsDark") != QVariant(dark)) { setProperty("hgsDark", dark); QWidget::update(); } }
    void update(const SessionInfo &session, const QString &alias, bool includeCompleted, bool online, bool draft)
    {
        using namespace SessionPresentation;
        auto *item = m_model.item(0); bool changed = false;
        const auto set = [&](int role, const QVariant &value) { if (item->data(role) != value) { item->setData(value, role); changed = true; } };
        set(SessionRoles::Title, sessionLabel(session));
        set(SessionRoles::Meta, checkoutLine(session));
        set(SessionRoles::MetaIcon, checkoutIcon(session));
        set(SessionRoles::GitStatus, session.gitStatus);
        set(SessionRoles::Status, status(session, online));
        set(SessionRoles::Failure, session.phase=="error");
        set(SessionRoles::Detail, currentAction(session, online).simplified());
        set(SessionRoles::Agent, session.cmd);
        set(SessionRoles::Host, alias);
        set(SessionRoles::MachineName, alias);
        set(SessionRoles::MachineColor, MachineAppearance::color(alias));
        set(SessionRoles::Model, session.model.isEmpty() ? (session.cmd == "sh" ? QString() : tr("Model unknown")) : session.model);
        set(SessionRoles::Effort, session.effort);
        set(SessionRoles::Attention, session.reviewLater || (online && !session.attentionAcknowledged && session.needsAction()));
        set(SessionRoles::ReviewLater, session.reviewLater);
        set(SessionRoles::Muted, session.muted);
        set(SessionRoles::Unread, session.visibleUnreadReply(online));
        set(SessionRoles::Working, working(session, online));
        set(SessionRoles::Draft, draft);
        set(SessionRoles::WorkingSince, SessionPresentation::backgroundIdle(session) ? session.backgroundStarted : session.phase == "compacting" ? session.compactionStarted : session.turnStarted);
        set(SessionRoles::Scheduled, scheduled(session, online));
        set(SessionRoles::OptionalQuestions, optionalQuestionCount(session, online));
        set(SessionRoles::Children, childCount(session, false, online, includeCompleted));
        // Overview cards navigate to a session; the roster opens in its list.
        set(SessionRoles::HasChildren, false);
        const QStringList description{displayTitle(session), alias, session.cmd, status(session, online),
            projectContext(session), item->data(SessionRoles::Detail).toString(), session.model, session.effort,
            childCount(session, true, online, includeCompleted)};
        QStringList detailLines;
        if (const auto git = GitStatusBadge::tooltip(session.gitStatus); !git.isEmpty()) detailLines << git;
        if (const int optional = optionalQuestionCount(session, online); optional > 0) detailLines << optionalQuestionsLabel(optional);
        const auto details = detailLines.join('\n');
        const auto name = description.join(" / ");
        if (accessibleName() != name) setAccessibleName(name);
        if (accessibleDescription() != details) setAccessibleDescription(details);
        // Background polls repeat most cards unchanged; repaint only changed ones.
        if (changed) QWidget::update();
    }
    void updateElapsed() { if (m_model.item(0)->data(SessionRoles::Working).toBool()) QWidget::update(); }
protected:
    bool event(QEvent *event) override
    {
        // The card already shows its fields; like Sessions cards, only the Git and
        // optional-question badges explain themselves.
        if (event->type() == QEvent::ToolTip) {
            const auto *help = static_cast<QHelpEvent *>(event);
            const auto index = m_model.index(0, 0);
            const auto question = SessionDelegate::optionalQuestionRect(rect(), index, font(), false);
            if (question.contains(help->pos())) {
                QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(
                    OptionalQuestionBadge::tooltip(index.data(SessionRoles::OptionalQuestions).toInt())), this, question);
                return true;
            }
            const auto badge = SessionDelegate::gitStatusRect(rect(), index, font(), false);
            const auto text = badge.contains(help->pos()) ? GitStatusBadge::tooltip(index.data(SessionRoles::GitStatus).toJsonObject()) : QString();
            if (text.isEmpty()) QToolTip::hideText();
            else QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(text), this, badge);
            return true;
        }
        return QPushButton::event(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const bool dark = property("hgsDark").toBool();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(dark ? "#33414c" : "#d7e0e6"));
        painter.setBrush(QColor(dark ? "#1d252c" : "#ffffff"));
        painter.drawRoundedRect(sessionCardRect(rect()), 8, 8);
        QStyleOptionViewItem option; option.initFrom(this); option.widget = this; option.rect = rect();
        option.font = font();
        if (isDown()) option.state |= QStyle::State_Selected;
        m_delegate.paint(&painter, option, m_model.index(0, 0));
    }
private:
    QStandardItemModel m_model;
    SessionDelegate m_delegate;
};

class DashboardAccountCard : public QPushButton {
public:
    explicit DashboardAccountCard(QWidget *parent):QPushButton(parent) {
        setObjectName("dashboardAccountCard");setFixedHeight(62);
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(12,8,12,8);layout->setSpacing(3);
        auto *top=new QHBoxLayout;machineRow=top;title=label({},"dashboardSessionTitle");title->setMinimumWidth(0);top->addWidget(title,1);
        provider=new IdentityBadge(IdentityBadges::Provider);host=new IdentityBadge(IdentityBadges::Machine);top->addWidget(provider);top->addWidget(host);layout->addLayout(top);
        auto *bottom=new QHBoxLayout;bottom->setSpacing(8);
        identity=label({},"dashboardMuted");identity->setProperty("accountIdentity",true);identity->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);identity->setMinimumWidth(0);bottom->addWidget(identity,1);
        usage=new AccountUsage::Button;usage->setObjectName("dashboardAccountUsage");usage->setShowProvider(false);usage->setCompact(true);bottom->addWidget(usage);layout->addLayout(bottom);
        for(auto *child:findChildren<QWidget *>()){child->setAttribute(Qt::WA_TransparentForMouseEvents);child->setFocusPolicy(Qt::NoFocus);}
    }
    void setUsageIndicator(AccountUsage::Indicator indicator){usage->setIndicator(indicator);}
    void setTheme(bool dark){if(m_themed&&m_dark==dark)return;m_themed=true;m_dark=dark;provider->setTheme(dark);host->setTheme(dark);for(auto *badge:extraHosts)badge->setTheme(dark);usage->setTheme(dark);QPushButton::update();}
    void update(const QJsonObject &profile) {
        // Polls repeat profiles. Refresh time-based text; repaint only on a visible change.
        if(m_hasData&&profile==data){describe();if(stripe()!=m_stripe)QPushButton::update();return;}
        m_hasData=true;data=profile;setBadge(provider,profile["provider"].toString(),m_dark);setBadge(host,profile["machine"].toString(),m_dark);
        const auto machines=profile["machines"].toArray();
        while(extraHosts.size()>qMax(0,int(machines.size())-1))delete extraHosts.takeLast();
        while(extraHosts.size()<machines.size()-1){auto *badge=new IdentityBadge(IdentityBadges::Machine);badge->setAttribute(Qt::WA_TransparentForMouseEvents);machineRow->addWidget(badge);extraHosts<<badge;}
        for(int i=0;i<extraHosts.size();++i)setBadge(extraHosts[i],machines[i+1].toString(),m_dark);
        m_usage=profile["usage"].toObject();if(!profile["installed"].toBool())m_usage["status"]="unavailable";usage->setData(m_usage);
        const auto info=m_usage["identity"].toObject();identityText=info["email"].toString(info["name"].toString());
        if(!info["plan"].toString().isEmpty())identityText+=(identityText.isEmpty()?QString():" / ")+info["plan"].toString();
        if(identityText.isEmpty())identityText=profile["installed"].toBool()?QString():tr("Agent not installed");
        describe();QPushButton::update();elide();
    }
protected:
    void resizeEvent(QResizeEvent *event) override {QPushButton::resizeEvent(event);elide();}
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);
        m_stripe=stripe();
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Qt::NoPen);painter.setBrush(m_stripe);
        painter.drawRoundedRect(QRectF(2,10,3,height()-20),1.5,1.5);
    }
private:
    // Usage windows expire with time, so the stripe and tooltip can change without new data.
    QColor stripe() const {const auto tone=AccountUsage::severest(data["usage"].toObject());return tone?AccountUsage::toneColor(*tone,m_dark):QColor(m_dark?"#9aaaba":"#64788a");}
    void describe(){const auto tip=AccountUsage::tooltip(m_usage);if(tip==toolTip()&&!accessibleName().isEmpty())return;setToolTip(tip);setAccessibleName(data["label"].toString()+" / "+data["machine"].toString()+"\n"+tip);}
    void elide(){int extra=0;for(auto *badge:extraHosts)extra+=badge->sizeHint().width()+6;title->setText(title->fontMetrics().elidedText(data["label"].toString(),Qt::ElideRight,qMax(20,width()-provider->sizeHint().width()-host->sizeHint().width()-extra-52)));identity->setText(identity->fontMetrics().elidedText(identityText,Qt::ElideRight,qMax(0,width()-usage->width()-32)));}
    QJsonObject data,m_usage;QString identityText;QLabel *title,*identity;IdentityBadge *provider,*host;AccountUsage::Button *usage;
    QHBoxLayout *machineRow;QList<IdentityBadge *> extraHosts;QColor m_stripe;bool m_dark=false,m_themed=false,m_hasData=false;
};

DashboardPage::DashboardPage(QWidget *parent) : QWidget(parent)
{
    setObjectName("dashboardPage");
    auto *outer = new QVBoxLayout(this); outer->setContentsMargins(20, 18, 0, 18); outer->setSpacing(14);
    auto *scroll = new QScrollArea; scroll->setObjectName("dashboardScroll"); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
    auto *content = new DashboardContent; scroll->setWidget(content);
    auto *layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 20, 0); layout->setSpacing(14);
    auto *title = label(tr("Overview"), "dashboardTitle");
    auto *refresh = new QPushButton(tr("Refresh")); refresh->setObjectName("dashboardRefresh");
    connect(refresh, &QPushButton::clicked, this, &DashboardPage::refreshRequested);
    auto *header = WorkspacePageHeader::row(title, refresh, nullptr); header->setContentsMargins(0,0,20,0);
    outer->addLayout(header); outer->addWidget(scroll);
    auto *summary = new QHBoxLayout; summary->setSpacing(12);
    m_attention = new QPushButton; m_attention->setObjectName("dashboardAttention");
    m_active = new QPushButton; m_active->setObjectName("dashboardActive");
    m_connected = new QPushButton; m_connected->setObjectName("dashboardConnected");
    m_sessions=new QPushButton;m_sessions->setObjectName("dashboardSessions");
    connect(m_sessions,&QPushButton::clicked,this,[this]{emit filterRequested("@all","all");});
    for (auto *button : {m_attention, m_active, m_connected, m_sessions}) { button->setMinimumHeight(72); summary->addWidget(button, 1); }
    connect(m_attention, &QPushButton::clicked, this, [this] { emit filterRequested("@all", "attention"); });
    connect(m_active, &QPushButton::clicked, this, [this] { emit filterRequested("@all", "active"); });
    connect(m_connected, &QPushButton::clicked, this, [this] { emit machinesRequested({}); }); layout->addLayout(summary);
    layout->addWidget(label(tr("Machines"), "dashboardSection"));
    m_machineGrid = new QGridLayout; m_machineGrid->setSpacing(14); layout->addLayout(m_machineGrid);
    auto *accountsHeading=new QHBoxLayout;accountsHeading->addWidget(label(tr("Accounts"),"dashboardSection"));accountsHeading->addStretch();
    auto *allAccounts=new QPushButton(tr("Manage accounts →"));allAccounts->setObjectName("dashboardAllAccounts");accountsHeading->addWidget(allAccounts);layout->addLayout(accountsHeading);
    connect(allAccounts,&QPushButton::clicked,this,&DashboardPage::accountsRequested);
    m_accountGrid=new QGridLayout;m_accountGrid->setSpacing(12);layout->addLayout(m_accountGrid);
    m_accountsEmpty=label(tr("Loading accounts…"),"dashboardMuted");layout->addWidget(m_accountsEmpty);
    auto *sessionsHeading = new QHBoxLayout; sessionsHeading->addWidget(label(tr("Active sessions"), "dashboardSection")); sessionsHeading->addStretch();
    auto *allSessions = new QPushButton(tr("All sessions →")); allSessions->setObjectName("dashboardAllSessions"); sessionsHeading->addWidget(allSessions);
    connect(allSessions, &QPushButton::clicked, this, [this] { emit filterRequested("@all", "all"); }); layout->addLayout(sessionsHeading);
    m_sessionGrid = new QGridLayout; m_sessionGrid->setSpacing(12); layout->addLayout(m_sessionGrid);
    m_empty = label(tr("No sessions are working, need attention or have drafts."), "dashboardEmpty"); m_empty->setWordWrap(true); layout->addWidget(m_empty);
    m_inactive = label({}, "dashboardMuted"); m_inactive->setWordWrap(true); layout->addWidget(m_inactive); layout->addStretch();
    content->installEventFilter(this);
    auto *timer = new QTimer(this); timer->setInterval(15000); connect(timer, &QTimer::timeout, this, [this] { if (isVisible()) render(); }); timer->start();
    auto *elapsed = new QTimer(this); elapsed->setInterval(1000);
    connect(elapsed, &QTimer::timeout, this, [this] {
        if (isVisible()) for (auto *row : m_rows) row->updateElapsed();
    }); elapsed->start();
    setTheme(true);
}
void DashboardPage::setFleet(const FleetState &fleet, const QSet<QString> &drafts) { m_fleet = fleet; m_drafts = drafts; render(); }
void DashboardPage::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); arrangeMachines(); arrangeSessions();arrangeAccounts(); }
bool DashboardPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize && watched == m_sessionGrid->parentWidget()) {arrangeSessions();arrangeAccounts();}
    return QWidget::eventFilter(watched, event);
}
void DashboardPage::setUsageIndicator(AccountUsage::Indicator indicator)
{
    m_usageIndicator = indicator;
    for (auto *card : m_accountCards) card->setUsageIndicator(indicator);
}
void DashboardPage::setAccounts(const QJsonArray &profiles)
{
    QStringList keys;
    QJsonArray identified;
    bool loading = false;
    for (const auto &value : profiles) {
        const auto profile = value.toObject(); const auto usage = profile["usage"].toObject();
        const auto status = usage["status"].toString(); loading |= status == "loading";
        // A native profile awaiting its identity is not another account. Keep
        // setup/error details in Manage accounts until it can be identified.
        if (profile["native"].toBool() && !AccountCatalog::named(profile["label"].toString())
            && AccountCatalog::key(profile).startsWith("profile:") && (status == "loading" || status == "unavailable" || status == "offline")) continue;
        identified.append(profile);
    }
    for(const auto &value:AccountCatalog::group(identified)) {
        const auto p=value.toObject();const auto key=p["account_key"].toString();keys<<key;
        auto *card=m_accountCards.value(key);
        if(!card){card=new DashboardAccountCard(this);m_accountCards[key]=card;connect(card,&QPushButton::clicked,this,[this,card]{emit accountRequested(card->property("host").toString(),card->property("profile").toString());});}
        card->setProperty("host", p["host"].toString()); card->setProperty("profile", p["id"].toString());
        card->setTheme(m_dark);card->setUsageIndicator(m_usageIndicator);card->update(p);
    }
    for(auto it=m_accountCards.begin();it!=m_accountCards.end();)if(!keys.contains(it.key())){delete it.value();it=m_accountCards.erase(it);}else ++it;
    if(keys!=m_accountOrder){m_accountOrder=keys;m_accountColumns=0;}
    m_accountsEmpty->setText(loading ? tr("Loading account details…") : tr("Open Manage accounts to view or set up accounts."));
    m_accountsEmpty->setVisible(keys.isEmpty());arrangeAccounts();
}
void DashboardPage::arrangeAccounts()
{
    const auto *content=m_accountGrid->parentWidget();const auto margins=content->layout()->contentsMargins();
    const int columns=qMax(1,(content->width()-margins.left()-margins.right()+12)/352);
    if(columns==m_accountColumns)return;
    while(auto *item=m_accountGrid->takeAt(0))delete item;
    for(int c=0;c<m_accountGrid->columnCount();++c)m_accountGrid->setColumnStretch(c,0);
    for(int c=0;c<columns;++c)m_accountGrid->setColumnStretch(c,1);
    for(int i=0;i<m_accountOrder.size();++i)m_accountGrid->addWidget(m_accountCards[m_accountOrder[i]],i/columns,i%columns);
    m_accountColumns=columns;
}
void DashboardPage::arrangeSessions()
{
    const auto *content = m_sessionGrid->parentWidget();
    const auto margins = content->layout()->contentsMargins();
    const int available = content->width() - margins.left() - margins.right();
    const int columns = qMax(1, (available + 12) / (340 + 12));
    if (columns == m_sessionColumns) return;
    // Reserve all columns even for a single card; the last row never stretches.
    while (auto *item = m_sessionGrid->takeAt(0)) delete item;
    for (int column = 0; column < m_sessionGrid->columnCount(); ++column) m_sessionGrid->setColumnStretch(column, 0);
    for (int column = 0; column < columns; ++column) m_sessionGrid->setColumnStretch(column, 1);
    for (int i = 0; i < m_sessionOrder.size(); ++i) m_sessionGrid->addWidget(m_rows.value(m_sessionOrder[i]), i / columns, i % columns);
    m_sessionColumns = columns;
}
void DashboardPage::arrangeMachines()
{
    const int columns = width() >= 880 ? 2 : 1;
    if (columns == m_columns) return;
    m_columns = columns;
    while (auto *item = m_machineGrid->takeAt(0)) delete item;
    for (int i = 0; i < m_machineOrder.size(); ++i) m_machineGrid->addWidget(m_machineCards.value(m_machineOrder[i]), i / columns, i % columns);
    m_machineGrid->setColumnStretch(0, 1); m_machineGrid->setColumnStretch(1, columns == 2 ? 1 : 0);
}
void DashboardPage::render()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const bool includeCompleted = QSettings().value("workspace/showCompletedSubagents", false).toBool();
    struct Machine { QString key, alias, host; BoxState box; bool fresh; };
    QList<Machine> machines;
    const auto local = m_fleet.local();
    // A control-only computer runs no agents; the overview covers remote machines.
    if (local.localSessions)
        machines.append({"@local", local.host.isEmpty() ? tr("This machine") : local.host, {}, local, local.ok});
    for (const auto &host : m_fleet.peerNames()) {
        const auto *box = m_fleet.peer(host); if (!box) continue;
        machines.append({host, host, host, *box, box->ok && now - m_fleet.peerPolledAt(host) < FleetState::kPeerStaleMs});
    }
    struct Active { QString key, host, alias; SessionInfo session; bool online; };
    QList<Active> active;
    QStringList keys;
    int attentionCount = 0, connectedCount = 0, offlineSessions = 0, knownSessions = 0;
    for (const auto &machine : machines) {
        keys << machine.key;
        if (machine.fresh) ++connectedCount;
        for (const auto &session : machine.box.sessions) {
            if (session.state == "archived") continue;
            ++knownSessions;
            if (!machine.fresh) ++offlineSessions;
            const bool needsAttention = SessionPresentation::needsAttention(session, machine.fresh);
            if (needsAttention && !session.muted) ++attentionCount;
            if (!SessionPresentation::active(session, machine.fresh, m_drafts.contains(machine.host + '\n' + session.name))) continue;
            active.append({machine.key + QChar(0x1f) + session.name, machine.host, machine.alias, session, machine.fresh});
        }
        auto *card = m_machineCards.value(machine.key);
        if (!card) {
            card = new DashboardMachineCard(this); m_machineCards.insert(machine.key, card);
            card->settings->setIcon(workspaceIcon("settings", QColor(m_dark ? "#9baaba" : "#627585")));
            connect(card->settings, &QPushButton::clicked, this, [this, host = machine.host] { emit machinesRequested(host); });
            connect(card->sessions, &QPushButton::clicked, this, [this, host = machine.host] { emit filterRequested(host.isEmpty() ? "@local" : host, "all"); });
        }
        setBadge(card->indicator, card->indicator->value(), m_dark);
        card->update(machine.box, machine.alias, machine.host.isEmpty(), machine.fresh);
    }
    for (auto it = m_machineCards.begin(); it != m_machineCards.end();) {
        if (!keys.contains(it.key())) { delete it.value(); it = m_machineCards.erase(it); } else ++it;
    }
    if (keys != m_machineOrder) { m_machineOrder = keys; m_columns = 0; } arrangeMachines();
    m_attention->setText(tr("%1\nNeeds attention").arg(attentionCount)); m_attention->setAccessibleName(tr("%1 sessions need attention").arg(attentionCount));
    setStyleProperty(m_attention, "active", attentionCount > 0);
    m_active->setText(tr("%1\nActive").arg(active.size())); m_active->setAccessibleName(tr("%1 active sessions").arg(active.size()));
    m_connected->setText(tr("%1 / %2\nMachines online").arg(connectedCount).arg(machines.size()));
    m_sessions->setText(tr("%1\nSessions").arg(knownSessions));m_sessions->setAccessibleName(tr("%1 sessions").arg(knownSessions));
    m_inactive->setText(offlineSessions ? tr("%1 sessions are on offline or stale machines. Their last known status is shown on the machine cards.").arg(offlineSessions) : QString());
    m_inactive->setVisible(offlineSessions > 0);
    std::sort(active.begin(), active.end(), [](const Active &a, const Active &b) {
        if (a.session.needsAttention() != b.session.needsAttention()) return a.session.needsAttention();
        if (a.session.lastEventAt != b.session.lastEventAt) return a.session.lastEventAt > b.session.lastEventAt;
        return a.key < b.key;
    });
    QSet<QString> rowKeys;
    QStringList rowOrder;
    for (const auto &item : active) {
        rowKeys.insert(item.key); rowOrder << item.key; auto *row = m_rows.value(item.key);
        if (!row) {
            row = new DashboardSessionRow(this); m_rows.insert(item.key, row);
            connect(row, &QPushButton::clicked, this, [this, host = item.host, name = item.session.name] { emit sessionRequested(host, name); });
        }
        row->setTheme(m_dark); row->update(item.session, item.alias, includeCompleted, item.online, m_drafts.contains(item.host + '\n' + item.session.name));
    }
    for (auto it = m_rows.begin(); it != m_rows.end();) {
        if (!rowKeys.contains(it.key())) { delete it.value(); it = m_rows.erase(it); } else ++it;
    }
    if (rowOrder != m_sessionOrder) { m_sessionOrder = rowOrder; m_sessionColumns = 0; }
    arrangeSessions();
    m_empty->setVisible(active.isEmpty());
}
void DashboardPage::setTheme(bool dark)
{
    m_dark = dark;
    const QString bg = dark ? "#161c21" : "#f3f5f7", card = dark ? "#1d252c" : "#ffffff";
    const QString border = dark ? "#33414c" : "#d7e0e6", text = dark ? "#e3e9ee" : "#23313c";
    const QString muted = dark ? "#9baaba" : "#627585", hover = dark ? "#27383e" : "#e8f3ef";
    setStyleSheet(QStringLiteral(
        "QWidget#dashboardPage,QWidget#dashboardContent,QScrollArea{background:%1;color:%4;border:0;}"
        "QLabel{color:%4;background:transparent;} QLabel#dashboardTitle{font-size:23px;font-weight:600;}"
        "QLabel#dashboardSection{font-size:15px;font-weight:600;} QLabel#dashboardMuted,QLabel#dashboardSessionHost{color:%5;font-size:11px;}"
        "QLabel#dashboardMachineTitle,QLabel#dashboardSessionTitle{font-size:14px;font-weight:600;}"
        "QLabel#dashboardMetric{font-size:24px;font-weight:500;} QLabel#dashboardMachineStatus{font-size:11px;color:%5;}"
        "QLabel#dashboardMachineStatus[online=true]{color:%10;}"
        "QFrame#dashboardMachineCard{background:%2;border:1px solid %3;border-radius:12px;}"
        "QPushButton{background:%2;border:1px solid %3;border-radius:7px;color:%4;padding:7px 12px;}"
        "QPushButton:hover{background:%6;border-color:#639b8d;} QPushButton:focus[keyboardFocus=\"true\"]{border-color:#79cdb5;}"
        "QPushButton#dashboardAttention,QPushButton#dashboardActive,QPushButton#dashboardConnected,QPushButton#dashboardSessions{text-align:left;font-size:16px;padding:12px 12px;min-height:46px;border-radius:10px;}"
        "QPushButton#dashboardAttention[active=true]{color:%7;background:%8;border-color:%9;}"
        // Keep shared card geometry under the application button stylesheet.
        "QPushButton#dashboardSessionRow{padding:0;min-height:104px;border:0;background:transparent;}"
        "QPushButton#dashboardAccountCard{text-align:left;padding:0;min-height:60px;border-radius:9px;}"
        "QPushButton#dashboardMachineSettings{padding:2px;background:transparent;border:0;}"
        "QPushButton#dashboardMachineSessions,QPushButton#dashboardAllSessions,QPushButton#dashboardAllAccounts{font-size:11px;background:transparent;padding:3px 0;border:0;text-align:left;color:%5;}"
        "QLabel#dashboardEmpty{color:%5;padding:24px;background:%2;border:1px solid %3;border-radius:10px;}"
        "QProgressBar{border:0;border-radius:2px;background:%3;} QProgressBar::chunk{background:%10;border-radius:2px;}"
        "QProgressBar[loadLevel=warning]::chunk{background:%11;} QProgressBar[loadLevel=critical]::chunk{background:%12;}"
        "QProgressBar::chunk:disabled{background:%5;}")
        .arg(bg, card, border, text, muted, hover, dark ? "#efc985" : "#835412", dark ? "#302b22" : "#fff3d9", dark ? "#705a35" : "#e1c38e", dark ? "#72cdb2" : "#237a62", dark ? "#f0a35b" : "#bd6519", dark ? "#f07878" : "#ce3d47"));
    WorkspacePageHeader::theme(findChild<QPushButton *>("dashboardRefresh"), nullptr, tr("Refresh overview"), dark);
    for (auto *row : m_rows) row->setTheme(dark);
    for(auto *card:m_accountCards)card->setTheme(dark);
    for (auto *cardWidget : m_machineCards) { cardWidget->settings->setIcon(workspaceIcon("settings", QColor(muted))); cardWidget->indicator->setTheme(dark); }
}
