#include <QtTest>
#include <algorithm>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QVBoxLayout>
#include "DashboardPage.h"
#include "SessionPresentation.h"

class PaintCounter : public QObject {
public:
    QHash<QObject *, int> paints;
    bool eventFilter(QObject *watched, QEvent *event) override { if (event->type() == QEvent::Paint) ++paints[watched]; return false; }
    int total() const { int result = 0; for (const auto count : paints) result += count; return result; }
};

class TestDashboard : public QObject {
    Q_OBJECT
private:
    QJsonArray accounts() const {
        QJsonArray result;
        for(const auto &provider:{"codex","claude","kimi"}) {
            const auto usage=QJsonObject{{"provider",provider},{"status","ok"},{"checked_at",QDateTime::currentSecsSinceEpoch()},
                {"identity",QJsonObject{{"email","work@example.test"},{"plan","Pro"}}},
                {"windows",QJsonArray{QJsonObject{{"used_percent",78},{"window_minutes",300}},QJsonObject{{"used_percent",94},{"window_minutes",10080}}}}};
            result.append(QJsonObject{{"id",QString("native-")+provider},{"label","Work"},{"provider",provider},{"host","mac"},{"machine","mac"},{"home",QString("/home/")+provider},{"installed",true},{"usage",usage}});
        }
        return result;
    }
    FleetState fixture() const
    {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        SessionInfo work; work.name = "codex/hgs/dashboard"; work.cmd = "codex"; work.project = "hgs"; work.tag = "dashboard";
        work.activity = "busy"; work.phase = "tool"; work.model = "gpt-6-astra"; work.effort = "high";
        work.activityDetail = "Building the machines overview"; work.lastEventAt = now / 1000.;
        auto attention = work; attention.name = "kimi/docs/research"; attention.cmd = "kimi"; attention.project = "docs";
        attention.tag = "research"; attention.phase = "input"; attention.activityDetail = "Choose the deployment target";
        auto ready = work; ready.name = "codex/hgs/ready"; ready.phase = "idle"; ready.activity = "idle";
        auto archived = work; archived.name = "codex/hgs/done"; archived.state = "archived";
        BoxState local; local.host = "arch"; local.ok = true; local.sessions = {work, ready, archived};
        local.metrics = QJsonObject{{"sampled_at", now / 1000.}, {"cpu_percent", 27.4}, {"cpu_interval_seconds", 2.},
            {"cpu_count", 16}, {"load_1", 2.8}, {"memory_total_bytes", 32. * 1024 * 1024 * 1024},
            {"memory_used_bytes", 12. * 1024 * 1024 * 1024}, {"platform", "linux"}};
        BoxState remote; remote.host = "mac"; remote.ok = true; remote.sessions = {attention}; remote.metrics = local.metrics;
        remote.metrics["cpu_percent"] = 12.8; remote.metrics["platform"] = "macos";
        BoxState offline = remote; offline.host = "studio"; offline.sessions = {work, attention}; offline.metrics["sampled_at"] = now / 1000. - 7200;
        FleetState fleet; fleet.setLocal(local, now); fleet.setPeer(remote, now); fleet.setPeer(offline, now - 7200000);
        offline.ok = false; fleet.setPeer(offline, now); return fleet;
    }
private slots:
    void filtersFollowSharedAndroidContract()
    {
        QFile file(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
            "../../mobile/android/app/src/test/resources/session-filter-contract.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto contract = QJsonDocument::fromJson(file.readAll()).object();
        const QStringList filters{"all", "attention", "active", "paused", "archived"};
        QCOMPARE(QJsonArray::fromStringList(filters), contract.value("filters").toArray());
        for (const auto &value : contract.value("cases").toArray()) {
            const auto example = value.toObject(); auto raw = example.value("raw").toObject();
            raw["name"] = "codex/contract/" + example.value("name").toString();
            QString error;
            const auto box = HgsClient::parseBox(QJsonDocument(QJsonObject{{"host", "fixture"}, {"ok", true},
                {"sessions", QJsonArray{raw}}}).toJson(), &error);
            QVERIFY2(error.isEmpty(), qPrintable(error)); QCOMPARE(box.sessions.size(), 1);
            auto session = box.sessions.first(); const bool online = example.value("online").toBool();
            // Desktop overlays these device/read marks after parsing CLI rows.
            session.reviewLater = raw.value("review_later").toBool();
            session.attentionAcknowledged = raw.value("attention_acknowledged").toBool();
            QStringList matches;
            if (session.state != "archived") matches << "all";
            if (SessionPresentation::needsAttention(session, online)) matches << "attention";
            if (SessionPresentation::active(session, online, example.value("draft").toBool())) matches << "active";
            if (session.state != "running" && session.state != "archived") matches << "paused";
            if (session.state == "archived") matches << "archived";
            QVERIFY2(QJsonArray::fromStringList(matches) == example.value("matches").toArray(), qPrintable(example.value("name").toString()));
        }
    }
    void draftsJoinActiveOnceAndStayOffline()
    {
        auto state = fixture(); const QSet<QString> drafts{"\ncodex/hgs/ready", "\ncodex/hgs/dashboard",
            "\ncodex/hgs/done", "mac\nkimi/docs/research", "studio\ncodex/hgs/dashboard"};
        DashboardPage page; page.setFleet(state, drafts);
        QCOMPARE(page.findChild<QPushButton *>("dashboardActive")->text(), QString("4\nActive"));
        QCOMPARE(page.findChild<QPushButton *>("dashboardAttention")->text(), QString("1\nNeeds attention"));
        QCOMPARE(page.findChildren<QPushButton *>("dashboardSessionRow").size(), 4);
        page.setFleet(state); QCOMPARE(page.findChild<QPushButton *>("dashboardActive")->text(), QString("2\nActive"));
    }
    void pendingNativeIdentityDoesNotCreateAnotherAccount()
    {
        auto profiles=accounts();auto pending=profiles.last().toObject();
        pending["host"]="";pending["machine"]="arch";pending["native"]=true;pending["label"]="Default account";
        pending["usage"]=QJsonObject{{"status","loading"}};profiles.append(pending);
        DashboardPage page;page.setAccounts(profiles);
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(),3);
        auto known=profiles[2].toObject()["usage"].toObject();pending["usage"]=known;profiles[3]=pending;page.setAccounts(profiles);
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(),3);
        // A confirmed sign-out or a different identity is a real separate profile.
        pending["usage"]=QJsonObject{{"status","signed_out"},{"identity",QJsonObject{}}};profiles[3]=pending;page.setAccounts(profiles);
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(),4);
        known["identity"]=QJsonObject{{"email","other@example.test"}};pending["usage"]=known;profiles[3]=pending;page.setAccounts(profiles);
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(),4);
        // Named API profiles remain visible even when their usage is unavailable.
        pending["label"]="API work";pending["usage"]=QJsonObject{{"status","unavailable"}};profiles[3]=pending;page.setAccounts(profiles);
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(),4);
    }
    void oneCardPerAccountAcrossMachines()
    {
        auto rows = accounts(); auto other = rows[0].toObject(); other["host"] = ""; other["machine"] = "arch"; other["id"] = "copied-codex"; rows.append(other);
        DashboardPage page; page.setFleet(fixture()); page.setAccounts(rows); page.resize(1120, 900); page.show();
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(), 3);
        QSignalSpy opened(&page, &DashboardPage::accountRequested);
        rows.removeAt(0); page.setAccounts(rows);
        QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(), 3);
        for (auto *card : page.findChildren<QPushButton *>("dashboardAccountCard")) if (card->property("profile") == "copied-codex") card->click();
        QCOMPARE(opened.takeFirst(), QVariantList({QString(), QString("copied-codex")}));
    }
    void accountSummaryKeepsCardsAndNavigatesToExactProfile() {
        DashboardPage page;page.setFleet(fixture());page.setAccounts(accounts());page.resize(1120,1100);page.show();QTest::qWait(20);
        auto cards=page.findChildren<QPushButton *>("dashboardAccountCard");QCOMPARE(cards.size(),3);
        auto *first=cards.first();QVERIFY(first->height()<=64);QVERIFY(first->toolTip().contains("work@example.test"));
        page.setAccounts(accounts());QVERIFY(page.findChildren<QPushButton *>("dashboardAccountCard").contains(first));
        QSignalSpy opened(&page,&DashboardPage::accountRequested);first->click();QCOMPARE(opened.first(),QVariantList({QString("mac"),QString("native-codex")}));
        QVERIFY(first->width()<page.width()/2);
        auto changed=accounts();changed.removeLast();page.setAccounts(changed);QCOMPARE(page.findChildren<QPushButton *>("dashboardAccountCard").size(),2);
    }
    void onlineCountsExcludeArchivedAndOffline()
    {
        DashboardPage page; page.setFleet(fixture()); page.resize(1120, 900); page.show();
        QCOMPARE(page.findChild<QPushButton *>("dashboardAttention")->text(), QString("1\nNeeds attention"));
        QCOMPARE(page.findChild<QPushButton *>("dashboardActive")->text(), QString("2\nActive"));
        QCOMPARE(page.findChild<QPushButton *>("dashboardConnected")->text(), QString("2 / 3\nMachines online"));
        QCOMPARE(page.findChild<QPushButton *>("dashboardSessions")->text(),QString("5\nSessions"));
        QCOMPARE(page.findChildren<QPushButton *>("dashboardSessionRow").size(), 2);
        QStringList statuses;
        for (const auto *status : page.findChildren<QLabel *>("dashboardMachineStatus")) statuses << status->text();
        QCOMPARE(statuses.count("Offline"), 1); QCOMPARE(statuses.count("Connected"), 2);
        QVERIFY(page.findChildren<QLabel *>("dashboardMuted").size() > 5);
    }
    void controlOnlyComputerHasNoMachineCard()
    {
        auto fleet = fixture(); auto local = fleet.local(); local.sessions.clear(); local.localSessions = false;
        fleet.setLocal(local, QDateTime::currentMSecsSinceEpoch());
        DashboardPage page; page.setFleet(fleet); page.resize(1120, 900); page.show();
        QCOMPARE(page.findChild<QPushButton *>("dashboardConnected")->text(), QString("1 / 2\nMachines online"));
        QStringList machines;
        for (const auto *title : page.findChildren<QLabel *>("dashboardMachineTitle")) machines << title->toolTip();
        machines.sort();
        QCOMPARE(machines, QStringList({"mac", "studio"}));
    }
    void reviewLaterStaysActiveOnOfflineMachine()
    {
        auto state = fixture(); auto box = *state.peer("studio"); box.sessions[0].reviewLater = true;
        state.setPeer(box, QDateTime::currentMSecsSinceEpoch());
        QVERIFY(state.setReviewLater("studio", box.sessions[0], true));
        DashboardPage page; page.setFleet(state);
        QCOMPARE(page.findChild<QPushButton *>("dashboardActive")->text(), QString("3\nActive"));
        QCOMPARE(page.findChild<QPushButton *>("dashboardAttention")->text(), QString("2\nNeeds attention"));
        const auto rows = page.findChildren<QPushButton *>("dashboardSessionRow"); QCOMPARE(rows.size(), 3);
        const auto reminder = std::find_if(rows.cbegin(), rows.cend(), [](const QPushButton *row) {
            return row->accessibleName().contains("studio");
        });
        QVERIFY(reminder != rows.cend()); QVERIFY((*reminder)->accessibleName().contains("Offline"));
    }
    void navigationAndPollsKeepFocusedRow()
    {
        DashboardPage page; page.setFleet(fixture()); page.resize(1000, 850); page.show();
        QSignalSpy filters(&page, &DashboardPage::filterRequested), sessions(&page, &DashboardPage::sessionRequested), machines(&page, &DashboardPage::machinesRequested);
        page.findChild<QPushButton *>("dashboardAttention")->click();
        QCOMPARE(filters.takeFirst(), QVariantList({QString("@all"), QString("attention")}));
        page.findChild<QPushButton *>("dashboardActive")->click();
        QCOMPARE(filters.takeFirst(), QVariantList({QString("@all"), QString("active")}));
        page.findChild<QPushButton *>("dashboardConnected")->click(); QCOMPARE(machines.count(), 1);
        const auto rows = page.findChildren<QPushButton *>("dashboardSessionRow");
        auto *row = *std::find_if(rows.cbegin(), rows.cend(), [](const QPushButton *button) {return button->accessibleName().contains("docs / research");});
        row->setFocus(); const auto id = row;
        page.setFleet(fixture()); QVERIFY(page.findChildren<QPushButton *>("dashboardSessionRow").contains(id));
        row->click(); QCOMPARE(sessions.takeFirst(), QVariantList({QString("mac"), QString("kimi/docs/research")}));
    }
    void unchangedPollsDoNotRepaint()
    {
        // Background polls usually repeat the same values; they must not repaint the page.
        const auto fleet = fixture(); const auto rows = accounts();
        DashboardPage page; page.setFleet(fleet); page.setAccounts(rows); page.resize(1120, 1100); page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page)); QTest::qWait(50);
        PaintCounter counter; QPushButton *attention = nullptr;
        // The working row repaints its turn clock every second; watch every other card.
        for (auto *row : page.findChildren<QPushButton *>("dashboardSessionRow"))
            if (row->accessibleName().contains("docs / research")) { attention = row; row->installEventFilter(&counter); }
        for (auto *card : page.findChildren<QPushButton *>("dashboardAccountCard")) card->installEventFilter(&counter);
        for (auto *card : page.findChildren<QFrame *>("dashboardMachineCard")) card->installEventFilter(&counter);
        for (const auto &name : {"dashboardAttention", "dashboardActive", "dashboardConnected", "dashboardSessions"})
            page.findChild<QPushButton *>(name)->installEventFilter(&counter);
        QVERIFY(attention);
        page.setFleet(fleet); page.setAccounts(rows); QTest::qWait(60);
        QCOMPARE(counter.total(), 0);
        auto changed = fleet; auto remote = *changed.peer("mac"); remote.sessions[0].activityDetail = "Choose another deployment target";
        changed.setPeer(remote, changed.peerPolledAt("mac")); page.setFleet(changed);
        QTRY_VERIFY(counter.paints.value(attention) > 0);
    }
    void unknownCpuIsNotZeroAndStalePeersAreExcluded()
    {
        const auto now = QDateTime::currentMSecsSinceEpoch(); auto fleet = fixture(); auto local = fleet.local();
        local.metrics["cpu_percent"] = QJsonValue::Null; fleet.setLocal(local, now);
        const auto remote = *fleet.peer("mac"); fleet.setPeer(remote, now - FleetState::kPeerStaleMs - 1000);
        DashboardPage page; page.setFleet(fleet);
        QCOMPARE(page.findChild<QPushButton *>("dashboardConnected")->text(), QString("1 / 3\nMachines online"));
        QCOMPARE(page.findChild<QPushButton *>("dashboardAttention")->text(), QString("0\nNeeds attention"));
        bool warmup = false, dash = false, stale = false;
        for (const auto *label : page.findChildren<QLabel *>()) {
            warmup |= label->text() == "Waiting for second sample";
            dash |= label->objectName() == "dashboardMetric" && label->text() == "—";
            stale |= label->text() == "Stale";
        }
        QVERIFY(warmup); QVERIFY(dash); QVERIFY(stale);
    }
    void summaryLabelsFitInsideParentStyles()
    {
        // SessionsWindow sets a global button minimum: child QSS must override it,
        // otherwise Qt replaces the C++ minimum and clips the second summary line.
        QWidget parent; parent.setStyleSheet("QWidget {font-size:13px;} QPushButton {padding:8px 13px; min-height:18px;}");
        auto *layout = new QVBoxLayout(&parent); auto *page = new DashboardPage;
        layout->addWidget(page); page->setFleet(fixture()); parent.resize(840,600); parent.show();
        QTest::qWait(20);
        for (const auto &name : {"dashboardAttention", "dashboardActive", "dashboardConnected", "dashboardSessions"}) {
            const auto *button = page->findChild<QPushButton *>(name);
            QVERIFY(button); QVERIFY2(button->height() >= 70, name);
            QVERIFY(button->text().contains('\n'));
        }
    }
    void preview()
    {
        const auto directory = qEnvironmentVariable("HGS_DASHBOARD_PREVIEW");
        if (directory.isEmpty()) QSKIP("Set HGS_DASHBOARD_PREVIEW to write screenshots");
        QDir().mkpath(directory);
        auto fleet = fixture(); auto local = fleet.local(); auto remote = *fleet.peer("mac");
        local.metrics["cpu_percent"] = 70.; local.metrics["memory_used_bytes"] = local.metrics["memory_total_bytes"].toDouble() * .9;
        remote.metrics["memory_used_bytes"] = remote.metrics["memory_total_bytes"].toDouble() * .8;
        const auto now = QDateTime::currentMSecsSinceEpoch(); fleet.setLocal(local, now); fleet.setPeer(remote, now);
        auto profiles=accounts();
        for(int i=0;i<profiles.size();++i){auto profile=profiles[i].toObject();auto usage=profile["usage"].toObject();usage["windows"]=QJsonArray{QJsonObject{{"used_percent",i==0?69:i==1?78:100},{"window_minutes",300},{"resets_at",QDateTime::currentSecsSinceEpoch()+3600}},QJsonObject{{"used_percent",23},{"window_minutes",10080}}};profile["usage"]=usage;profiles[i]=profile;}
        profiles.append(QJsonObject{{"id","api"},{"label","DeepSeek API"},{"provider","deepseek"},{"machine","arch"},{"installed",true},{"usage",QJsonObject{{"status","configured"}}}});
        DashboardPage page; page.setFleet(fleet);page.setAccounts(profiles); page.resize(1120, 1100); page.show();
        QTest::qWait(80); QVERIFY(page.grab().save(directory + "/dashboard-dark.png"));
        page.setTheme(false); QTest::qWait(30); QVERIFY(page.grab().save(directory + "/dashboard-light.png"));
        remote.sessions.clear(); fleet.setPeer(remote, now); page.setFleet(fleet); page.setTheme(true);
        QTest::qWait(30); QVERIFY(page.grab().save(directory + "/dashboard-single.png"));
        page.resize(620, 1100); QTest::qWait(30); QVERIFY(page.grab().save(directory + "/dashboard-narrow.png"));
    }
};
QTEST_MAIN(TestDashboard)
#include "test_dashboard.moc"
