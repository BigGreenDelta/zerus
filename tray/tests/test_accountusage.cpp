#include <QtTest>
#include "AccountUsageStore.h"
#include "AccountUsage.h"
#include "CacheStatus.h"

static QJsonObject limitWindow(int minutes,double used,qint64 resetIn,const QDateTime &now) {
    return {{"window_minutes",minutes},{"used_percent",used},{"resets_at",double(now.toSecsSinceEpoch()+resetIn)}};
}

class TestAccountUsage : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("hgs-tests");QCoreApplication::setApplicationName("account-usage");
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_settings.path());
    }
    void paceSeparatesEqualUsageByTimeUntilReset() {
        const auto now=QDateTime::currentDateTime();
        const auto tomorrow=AccountUsage::pace(limitWindow(10080,48,26*3600,now),now);
        QVERIFY(tomorrow.known);QCOMPARE(tomorrow.tone,AccountUsage::Tone::Normal);
        QVERIFY(qAbs(tomorrow.elapsed-142./168)<0.001);QCOMPARE(tomorrow.runsOutIn,qint64(-1));QCOMPARE(qRound(tomorrow.projected),57);
        const auto later=AccountUsage::pace(limitWindow(10080,48,145*3600,now),now);
        QVERIFY(later.known);QCOMPARE(later.tone,AccountUsage::Tone::Critical);
        // 48% in the first 23 hours leaves 52%, enough for about 24h 55m at that pace.
        QCOMPARE(later.runsOutIn/60,qint64(1495));
        QVERIFY(AccountUsage::paceColor(later,true)!=AccountUsage::paceColor(tomorrow,true));
    }
    void paceToneFollowsForecastWithThresholdFallbacks() {
        using AccountUsage::Tone;
        const auto now=QDateTime::currentDateTime();
        auto tone=[&](int minutes,double used,qint64 resetIn){return AccountUsage::pace(limitWindow(minutes,used,resetIn,now),now).tone;};
        QCOMPARE(tone(10080,92,3*3600),Tone::Warning);
        QCOMPARE(tone(10080,61,76*3600),Tone::Warning);
        QCOMPARE(tone(300,35,120*60),Tone::Normal);
        QCOMPARE(tone(300,100,47*60),Tone::Critical);
        // The first 8% of a window is too short to extrapolate; the fixed thresholds apply.
        const auto early=AccountUsage::pace(limitWindow(10080,6,165*3600,now),now);
        QVERIFY(early.early);QCOMPARE(early.tone,Tone::Normal);QCOMPARE(early.runsOutIn,qint64(-1));
        QCOMPARE(tone(10080,75,165*3600),Tone::Warning);
        QCOMPARE(tone(10080,95,165*3600),Tone::Critical);
        QJsonObject unknown{{"used_percent",78},{"window_minutes",300}};
        const auto withoutReset=AccountUsage::pace(unknown,now);QVERIFY(!withoutReset.known);QCOMPARE(withoutReset.tone,Tone::Warning);
        unknown.remove("window_minutes");unknown["resets_at"]=double(now.toSecsSinceEpoch()+3600);
        QVERIFY(!AccountUsage::pace(unknown,now).known);
    }
    void severestToneFollowsPaceAcrossWindows() {
        using AccountUsage::Tone;
        const auto now=QDateTime::currentDateTime();
        // 80% with half an hour left lasts until reset; 48% a day into the week does not.
        QJsonObject data{{"windows",QJsonArray{limitWindow(300,80,30*60,now)}}};
        QCOMPARE(AccountUsage::severest(data),std::optional(Tone::Normal));
        data["windows"]=QJsonArray{limitWindow(300,80,30*60,now),limitWindow(10080,48,145*3600,now)};
        QCOMPARE(AccountUsage::severest(data),std::optional(Tone::Critical));
        data["windows"]=QJsonArray{QJsonObject{{"used_percent",100},{"window_minutes",300},{"resets_at",double(now.toSecsSinceEpoch()-60)}}};
        QVERIFY(!AccountUsage::severest(data));
    }
    void countdownUsesTheLargestWholeUnit() {
        QCOMPARE(AccountUsage::countdown(30),QString("1m"));
        QCOMPARE(AccountUsage::countdown(47*60),QString("47m"));
        QCOMPARE(AccountUsage::countdown(3*3600+20*60),QString("3h"));
        QCOMPARE(AccountUsage::countdown(26*3600),QString("26h"));
        QCOMPARE(AccountUsage::countdown(145*3600),QString("6d"));
    }
    void tooltipForecastsPaceUntilReset() {
        const auto now=QDateTime::currentDateTime();
        QJsonObject data{{"provider","claude"},{"status","ok"},{"windows",QJsonArray{limitWindow(10080,48,145*3600+30,now)}}};
        QVERIFY(AccountUsage::tooltip(data).contains("(in 6d 1h)"));
        QVERIFY(AccountUsage::tooltip(data).contains("At the current pace, the limit runs out in about 1d"));
        data["windows"]=QJsonArray{limitWindow(10080,48,26*3600,now)};
        QVERIFY(AccountUsage::tooltip(data).contains("At the current pace, about 57% by reset"));
        data["windows"]=QJsonArray{limitWindow(10080,6,165*3600,now)};
        QVERIFY(!AccountUsage::tooltip(data).contains("current pace"));
    }
    void indicatorPreferenceDefaultsToRails() {
        using AccountUsage::Indicator;
        QSettings().remove("workspace/usageIndicator");QCOMPARE(AccountUsage::indicator(),Indicator::Rails);
        QSettings().setValue("workspace/usageIndicator","marker");QCOMPARE(AccountUsage::indicator(),Indicator::Marker);
        QSettings().setValue("workspace/usageIndicator","marker-countdown");QCOMPARE(AccountUsage::indicator(),Indicator::MarkerCountdown);
        AccountUsage::Button saved;QCOMPARE(saved.indicator(),Indicator::MarkerCountdown);
        QSettings().setValue("workspace/usageIndicator","unknown");QCOMPARE(AccountUsage::indicator(),Indicator::Rails);
        QSettings().remove("workspace/usageIndicator");
        AccountUsage::Button button;QCOMPARE(button.indicator(),Indicator::Rails);
    }
    void footerIndicatorsShareLayoutExceptTheCountdown() {
        using AccountUsage::Indicator;
        const auto now=QDateTime::currentDateTime();
        AccountUsage::Button button;button.show();
        const QJsonObject data{{"provider","claude"},{"status","ok"},{"windows",QJsonArray{limitWindow(300,10,200*60,now),limitWindow(10080,48,145*3600,now)}}};
        button.setData(data);
        button.setIndicator(Indicator::Rails);const auto rails=button.size();const auto railsImage=button.grab().toImage();
        button.setIndicator(Indicator::Marker);QCOMPARE(button.size(),rails);
        const auto markerImage=button.grab().toImage();QVERIFY(markerImage!=railsImage);
        button.setIndicator(Indicator::MarkerCountdown);QVERIFY(button.width()>rails.width());
        QCOMPARE(button.height(),22);QCOMPARE(button.sizeHint(),button.size());
        QVERIFY(button.accessibleName().contains("At the current pace"));
        // The compact dashboard footer has no room for the countdown; the marker remains.
        button.setCompact(true);const auto compactCountdown=button.size();
        button.setIndicator(Indicator::Marker);QCOMPARE(button.size(),compactCountdown);
        button.setCompact(false);
        const auto directory=qEnvironmentVariable("HGS_DASHBOARD_PREVIEW");
        if(!directory.isEmpty()) {
            QDir().mkpath(directory);
            for(const auto &[indicator,name]:{std::pair{Indicator::Rails,"rails"},std::pair{Indicator::Marker,"marker"},std::pair{Indicator::MarkerCountdown,"marker-countdown"}})
                for(const bool dark:{false,true}) {
                    button.setIndicator(indicator);button.setTheme(dark);
                    QVERIFY(button.grab().save(directory+"/pace-"+name+(dark?"-dark":"-light")+".png"));
                }
            button.setTheme(true);
        }
        // Without a reset time there is no pace, so every indicator draws the plain bar.
        button.setData({{"status","ok"},{"windows",QJsonArray{QJsonObject{{"used_percent",48},{"window_minutes",10080}}}}});
        button.setIndicator(Indicator::Rails);const auto plain=button.grab().toImage();
        button.setIndicator(Indicator::Marker);QCOMPARE(button.grab().toImage(),plain);
        button.setIndicator(Indicator::MarkerCountdown);QCOMPARE(button.grab().toImage(),plain);
    }
    void savedCredentialsAreDistinctFromSignedOut() {
        for(const auto &status:{"credentials_locked","desktop_session_unavailable","credentials_unavailable"}) {
            const auto text=AccountUsage::state({{"status",status},{"provider","claude"}});
            QVERIFY(!text.contains("No native subscription"));
            QVERIFY(text.contains("Keychain") || text.contains("desktop"));
        }
    }
    void expiredLimitsAndUnknownCacheNeverImplyAvailableCapacity() {
        QJsonObject window{{"window_minutes",300},{"used_percent",100},{"resets_at",QDateTime::currentSecsSinceEpoch()+3600}};
        QJsonObject data{{"windows",QJsonArray{window}}};
        QVERIFY(AccountUsage::windowText(window).contains("1h"));QCOMPARE(AccountUsage::highest(data),100);
        QVERIFY(AccountUsage::exhausted(data).contains("Resets in"));
        window.remove("resets_at");data["windows"]=QJsonArray{window};
        QVERIFY(AccountUsage::exhausted(data).contains("unavailable"));QCOMPARE(AccountUsage::highest(data),100);
        window["resets_at"]=QDateTime::currentSecsSinceEpoch()-60;data["windows"]=QJsonArray{window};
        QVERIFY(AccountUsage::windowText(window).contains("refresh"));QCOMPARE(AccountUsage::highest(data),-1);
        QVERIFY(AccountUsage::exhausted(data).contains("refresh"));
        QJsonObject session{{"session_usage",QJsonObject{{"status","ok"},{"prompt_cache",QJsonObject{{"status","unknown"},{"cache_read",10000}}}}}};
        QVERIFY(!CacheStatus::expired(session));QVERIFY(CacheStatus::warning(session).isEmpty());
        session["cache_hint"]=QJsonObject{{"status","saving_hint"},{"tokens",484800}};
        QVERIFY(!CacheStatus::expired(session));QVERIFY(CacheStatus::warning(session).contains("still be warm"));
        session["cache_hint"]=QJsonObject{{"status","cold"},{"tokens",756000}};
        QVERIFY(CacheStatus::expired(session));QVERIFY(CacheStatus::warning(session).contains("cold cache"));
    }
    void cacheChipStateFollowsHintAndCountdown() {
        QJsonObject data;
        QVERIFY(!CacheStatus::chipState(data).visible);
        data["session_usage"]=QJsonObject{{"prompt_cache",QJsonObject{{"status","warm"},{"expires_at",double(QDateTime::currentSecsSinceEpoch()+240)}}}};
        auto state=CacheStatus::chipState(data);QVERIFY(state.visible);QCOMPARE(state.tone,ChipTone::Success);
        QCOMPARE(state.label,QString("Cache ~4m"));QCOMPARE(state.shortLabel,QString("~4m"));
        data["cache_hint"]=QJsonObject{{"status","saving_hint"},{"tokens",484800}};
        state=CacheStatus::chipState(data);QCOMPARE(state.tone,ChipTone::Warning);
        QCOMPARE(state.label,QString("Clear suggested"));QCOMPARE(state.shortLabel,QString("/clear"));
        data["cache_hint"]=QJsonObject{{"status","cold"},{"tokens",756000}};
        state=CacheStatus::chipState(data);QCOMPARE(state.tone,ChipTone::Danger);
        QCOMPARE(state.label,QString("Cold cache"));QCOMPARE(state.shortLabel,QString("Cold"));
        QVERIFY(!CacheStatus::details(data,true).contains("cold cache"));QVERIFY(CacheStatus::details(data,true).contains("Last recorded"));
    }
    void exhaustedSummaryNamesNearestReset() {
        const auto now=QDateTime::currentSecsSinceEpoch();
        QJsonObject data{{"windows",QJsonArray{QJsonObject{{"window_minutes",300},{"used_percent",80}}}}};
        QVERIFY(AccountUsage::exhaustedSummary(data).isEmpty());
        data["windows"]=QJsonArray{QJsonObject{{"window_minutes",10080},{"used_percent",100},{"resets_at",now+3*86400}},
            QJsonObject{{"window_minutes",300},{"used_percent",100},{"resets_at",now+2*3600+30}}};
        QCOMPARE(AccountUsage::exhaustedSummary(data),QString("Limit reached · 2h 1m"));
        data["windows"]=QJsonArray{QJsonObject{{"window_minutes",300},{"used_percent",100}}};
        QCOMPARE(AccountUsage::exhaustedSummary(data),QString("Limit reached"));
        data["windows"]=QJsonArray{QJsonObject{{"window_minutes",300},{"used_percent",100},{"resets_at",now-60}}};
        QCOMPARE(AccountUsage::exhaustedSummary(data),QString("Refresh usage"));
    }
    void failedRefreshKeepsQuotaWarningExplicitlyHistorical() {
        QJsonObject data{{"status","ok"},{"windows",QJsonArray{QJsonObject{{"window_minutes",10080},
            {"used_percent",100},{"resets_at",QDateTime::currentSecsSinceEpoch()+3600}}}}};
        QVERIFY(!AccountUsage::exhausted(data).contains("Last reported"));
        data["refresh_error"]=true;QVERIFY(AccountUsage::exhausted(data).contains("Last reported"));
        data.remove("refresh_error");data["offline"]=true;QVERIFY(AccountUsage::exhausted(data).contains("Last reported"));
    }
    void cacheIsSharedByAccountWithTimedAndManualRefresh() {
        qint64 now=1000;AccountUsageStore store("/nonexistent/hgs",nullptr,[&]{return now;});
        auto *client=store.findChild<HgsClient *>();
        AccountUsageRef a{{},"work","codex","/accounts/work","Work","codex/a","run-a"};auto b=a;b.session="codex/b";b.run="run-b";
        const QJsonObject data{{"id","work"},{"home","/accounts/work"},{"provider","codex"},{"status","ok"},{"run_id","run-a"},{"identity",QJsonObject{{"email","work@example.test"}}},{"windows",QJsonArray{QJsonObject{{"used_percent",78},{"window_minutes",300}}}}};
        store.ensure(a);QVERIFY(store.data(a)["refreshing"].toBool());
        client->accountsReady(1,{},data);QCOMPARE(store.data(a)["windows"],data["windows"]);
        store.ensure(b);QVERIFY(!store.data(b)["refreshing"].toBool());QCOMPARE(store.data(b)["windows"],data["windows"]);
        now+=AccountUsageStore::RefreshInterval-1;store.ensure(b);QVERIFY(!store.data(b)["refreshing"].toBool());
        ++now;store.ensure(b);QVERIFY(store.data(b)["refreshing"].toBool());QCOMPARE(store.data(b)["windows"],data["windows"]);
        client->accountsFailed(2,{},"offline");QVERIFY(store.data(b)["refresh_error"].toBool());QCOMPARE(store.data(b)["windows"],data["windows"]);
        store.ensure(b);QVERIFY(!store.data(b)["refreshing"].toBool());
        store.ensure(b,true);store.ensure(a,true);QVERIFY(store.data(b)["refreshing"].toBool());
        auto fresh=data;fresh["run_id"]="run-b";client->accountsReady(3,{},fresh);QVERIFY(!store.data(a)["refreshing"].toBool());QVERIFY(!store.data(a)["refresh_error"].toBool());
        AccountUsageRef catalog=a;catalog.session.clear();catalog.run.clear();store.ensure(catalog);QVERIFY(!store.data(catalog)["refreshing"].toBool());
        auto remote=catalog;remote.host="mac";store.ensure(remote);QVERIFY(store.data(remote)["windows"].toArray().isEmpty());
        auto otherHome=catalog;otherHome.home="/different/home";store.ensure(otherHome);QVERIFY(store.data(otherHome)["windows"].toArray().isEmpty());
        client->accountsReady(4,"mac",fresh);QCOMPARE(store.data(remote)["windows"],fresh["windows"]);
        client->accountsReady(5,{},fresh);QVERIFY(store.data(otherHome)["windows"].toArray().isEmpty());
    }
    void credentialChangeRefreshesIdentityWithoutWaitingFiveMinutes() {
        AccountUsageStore store("/nonexistent/hgs");auto *client=store.findChild<HgsClient *>();
        AccountUsageRef ref{{},"work","claude","/accounts/work","Work",{},{},"before"};
        store.ensure(ref);client->accountsReady(1,{},{{"id","work"},{"home",ref.home},{"provider","claude"},{"status","signed_out"}});
        store.ensure(ref);QVERIFY(!store.data(ref)["refreshing"].toBool());
        ref.authRevision="after";store.ensure(ref);QVERIFY(store.data(ref)["refreshing"].toBool());
        client->accountsReady(2,{},{{"id","work"},{"home",ref.home},{"provider","claude"},{"status","ok"},{"identity",QJsonObject{{"email","work@example.test"}}}});
        QCOMPARE(store.data(ref)["status"].toString(),QString("ok"));
    }
    void refreshButtonShowsMotionUntilComplete() {
        AccountUsage::RefreshButton button;button.show();button.setRefreshing(true);
        const auto before=button.grab().toImage();QTRY_VERIFY(button.grab().toImage()!=before);QVERIFY(button.isRefreshing());
        button.setRefreshing(false);QVERIFY(!button.isRefreshing());QVERIFY(!button.icon().isNull());QVERIFY(button.toolTip().contains("Refresh"));
    }
    void footerExpandsOnlyForExtraText() {
        AccountUsage::Button button;button.show();
        QJsonObject first{{"used_percent",69},{"window_minutes",300},{"resets_at",QDateTime::currentSecsSinceEpoch()+3600}};
        const QJsonObject second{{"used_percent",23},{"window_minutes",10080}};
        auto setUsage=[&]{button.setData({{"status","ok"},{"provider","claude"},{"windows",QJsonArray{first,second}}});};
        const auto directory=qEnvironmentVariable("HGS_DASHBOARD_PREVIEW");
        auto preview=[&](const QString &name){if(!directory.isEmpty()){QDir().mkpath(directory);QVERIFY(button.grab().save(directory+"/quota-"+name+".png"));}};
        setUsage();const auto size=button.size();QVERIFY(size.width()<300);QCOMPARE(size.height(),22);preview("normal");
        first["used_percent"]=9;setUsage();QCOMPARE(button.size(),size);
        first["used_percent"]=100;setUsage();QVERIFY(button.width()>size.width());QCOMPARE(button.height(),size.height());preview("exhausted");
        first["used_percent"]=69;setUsage();QCOMPARE(button.size(),size);
        button.setData({{"status","unavailable"}});QVERIFY(button.width()<size.width());QCOMPARE(button.height(),size.height());
    }
    void footerProviderBadgesPreserveCompactLayoutAndAccessibleIdentity() {
        AccountUsage::Button button;button.show();
        const QJsonArray windows{QJsonObject{{"used_percent",15},{"window_minutes",300}},
            QJsonObject{{"used_percent",4},{"window_minutes",10080}}};
        const auto directory=qEnvironmentVariable("HGS_DASHBOARD_PREVIEW");
        for(const auto &provider:{"claude","codex","kimi","dsh","example-provider"}) {
            const QString name=IdentityBadges::providerName(provider);
            for(const auto &status:{"ok","loading","configured","signed_out"}) {
                QJsonObject data{{"provider",provider},{"label","Work"},{"status",status}};
                if(QString(status)=="ok")data["windows"]=windows;
                button.setCompact(false);button.setShowProvider(false);button.setData(data);
                const auto withoutProvider=button.size();
                button.setShowProvider(true);const auto withProvider=button.size();
                QVERIFY(withProvider.width()>withoutProvider.width());
                QCOMPARE(withProvider.height(),22);QCOMPARE(button.sizeHint(),withProvider);
                QVERIFY(button.toolTip().contains(name));QVERIFY(button.toolTip().contains("Work"));
                QVERIFY(button.accessibleName().contains(name));
                button.setTheme(false);QCOMPARE(button.size(),withProvider);
                button.setTheme(true);QCOMPARE(button.size(),withProvider);
                button.setCompact(true);const auto compactSize=button.size();
                QVERIFY(compactSize.width()<withoutProvider.width());
                button.setShowProvider(false);QCOMPARE(button.size(),compactSize);
                QVERIFY(button.accessibleName().contains(name));
                button.setCompact(false);QCOMPARE(button.size(),withoutProvider);
                data.remove("provider");button.setData(data);button.setShowProvider(true);
                QCOMPARE(button.size(),withoutProvider);
            }
            // Render both themes for visual review without checking platform-specific pixels.
            if(!directory.isEmpty()) {
                QDir().mkpath(directory);
                button.setData({{"provider",provider},{"status","ok"},{"windows",windows}});
                for(const bool dark:{false,true}) {
                    button.setTheme(dark);
                    QVERIFY(button.grab().save(directory+"/quota-"+provider+(dark?"-dark":"-light")+".png"));
                }
            }
        }
        QFont font=button.font();font.setFamily("DejaVu Sans");font.setWeight(QFont::Light);button.setFont(font);
        button.setData({{"provider","dsh"},{"status","ok"},{"windows",windows}});
        QCOMPARE(button.size(),button.sizeHint());
        font.setFamily("DejaVu Sans Mono");button.setFont(font);QCOMPARE(button.size(),button.sizeHint());
        QVERIFY(button.toolTip().contains("DeepSeek"));QVERIFY(button.accessibleName().contains("DeepSeek"));
    }
private:
    QTemporaryDir m_settings;
};
QTEST_MAIN(TestAccountUsage)
#include "test_accountusage.moc"
