#include <QtTest>
#include "AnalyticsData.h"
#include "AnalyticsClient.h"
#include "AnalyticsSettings.h"
// Compile the ordinary Settings integration without constructing its unrelated
// native-preference and service panels in this isolated component suite.
#include "SettingsPage.h"
#include "FakeCli.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

namespace {
const QString machine = "10000000-0000-4000-8000-000000000001";
const QString epoch = "20000000-0000-4000-8000-000000000001";
const QString resetEpoch = "20000000-0000-4000-8000-000000000002";
const QDate first(2026,1,1),last(2026,1,30);
QByteArray bytes(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject bucket(QDate date=first,const QString &provider="codex",int completed=3) {
    return {{"date",date.toString(Qt::ISODate)},{"provider",provider},{"producer","main"},
        {"counters",QJsonObject{{"turn_started",completed},{"turn_completed",completed},{"work_ms",60000},{"input_tokens",17},{"failure_network",1}}},
        {"histograms",QJsonObject{{"work_ms",QJsonObject{{"version",1},{"bounds",QJsonArray{100,1000,10000,60000,600000}},
            {"counts",QJsonArray{0,0,0,1,0,0}},{"count",1},{"sum",60000}}}}}};
}
QJsonObject workBucket(const QJsonArray &counts, qint64 count, qint64 sum, const QString &provider="codex") {
    auto row=bucket(first,provider);
    row.insert("histograms",QJsonObject{{"work_ms",QJsonObject{{"version",1},{"bounds",QJsonArray{100,1000,10000,60000,600000}},
        {"counts",counts},{"count",count},{"sum",sum}}}});
    return row;
}
QJsonObject summary(QJsonArray rows=QJsonArray{bucket()}) {
    return {{"schema",1},{"machine_id",machine},{"source","agent"},{"source_epoch",epoch},{"revision",7},
        {"generated_at",QDateTime::currentMSecsSinceEpoch()/1000.},{"range",QJsonObject{{"from",first.toString(Qt::ISODate)},{"through",last.toString(Qt::ISODate)}}},
        {"collection",QJsonObject{{"enabled",false},{"event_retention_days",14},{"retention_days",90}}},
        {"coverage",QJsonObject{{"start",1767225600.},{"end",1767225660.},{"partial",true},{"gaps",QJsonArray{"collection_disabled"}},
            {"unsupported",QJsonArray{"cost","tokens"}},{"providers",QJsonObject{{"codex",QJsonObject{{"supported",QJsonArray{"turns","work_ms"}},{"unsupported",QJsonArray{"tokens"}},{"partial",true}}}}}}},
        {"buckets",rows}};
}
QJsonObject voice() {
    QJsonObject row{{"day",first.toString(Qt::ISODate)},{"profile_id","private-profile-sentinel"},{"error_codes",QJsonObject{{"http_timeout",1}}}};
    for(const auto &name:AnalyticsData::speechCounters())row.insert(name,name.endsWith("_ms")?60000:2);
    return {{"schema",1},{"enabled",true},{"retention_days",90},{"days",QJsonArray{row}},{"totals",QJsonObject{{"private","never forward"}}}};
}
const QByteArray cli=R"PY(#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
p=Path(__file__)
config=json.loads(Path(str(p)+'.json').read_text())
args=sys.argv[1:]
host=args.pop(0)[1:] if args and args[0].startswith('@') else ''
payload=json.loads(sys.stdin.read() or '{}') if '--help' not in args else {}
with open(str(p)+'.calls','a') as log:
    log.write(json.dumps({'host':host,'args':args,'payload':payload,'inherited_session':'HGS_SESSION' in os.environ})+'\n')
if host in config.get('offline',[]):
    print('private-stderr-sentinel',file=sys.stderr);sys.exit(1)
if '--help' in args:
    print('hgs [@host] ls' if host in config.get('old',[]) else 'hgs [@host] analytics --json\nhgs [@host] speech --json')
    sys.exit(0)
if config.get('oversized'):
    sys.stdout.write('x'*(1024*1024+1));sys.exit(0)
if args==['analytics','--json']:
    value=config['agents'].get(host,config['agents'][''])
    action=payload['action']
    if action in ['configure','purge']:
        assert host==''
        assert payload['expected_epoch']==value['source_epoch']
        if action=='purge':
            assert payload['confirm'] is True
            value['source_epoch']='20000000-0000-4000-8000-000000000002'
            value['buckets']=[]
        else:
            value['collection']['enabled']=payload['enabled'];value['collection']['retention_days']=payload['retention_days']
        Path(str(p)+'.json').write_text(json.dumps(config))
    else:
        value['range']={'from':payload['from'],'through':payload['through']}
        value['buckets']=[r for r in value['buckets'] if payload['from']<=r['date']<=payload['through']]
    if not config.get('stale'):value['generated_at']=time.time()
    print(json.dumps(value))
elif args==['speech','--json']:print(json.dumps(config['voice']))
else:sys.exit(5)
)PY";
}

class TestAnalytics : public QObject {
    Q_OBJECT
private slots:
    void init();
    void parserKeepsOnlyReviewedFieldsAndUnknowns();
    void parserRejectsInvalidAndUnboundedSummaries();
    void fleetReplacesSnapshotsAndEpochsInsteadOfSumming();
    void histogramsMergeSamplesBeforeComputingMean();
    void invalidMissingAndOverflowingHistogramsStayUnavailable();
    void retiredEpochNeverRevivesWithNewTimestamp();
    void voiceCollapsesPrivateProfilesAndPreservesUnits();
    void diagnosticsExcludeIdentityContentAndDetailedHistory();
    void oldCliIsProbedWithoutLaunchingUnknownVerb();
    void responseCapAndErrorsNeverEchoPrivateText();
    void pageReadsWithoutEnablingAndPagesDailyRows();
    void repeatedRefreshAndDuplicateRoutesDoNotAddTotals();
    void fleetOverviewWeightsSamplesAndExcludesStaleComputers();
    void failedReadKeepsLastKnownButExcludesTotals();
    void invalidRangeAndRemoteFailureKeepLocalDataUseful();
    void controlsStayLocalAndRequireExplicitApply();
    void retentionReductionRequiresConfirmation();
    void purgeCancelDoesNothingAndConfirmUsesFreshEpoch();
    void diagnosticReviewDoesNotExportAutomatically();
private:
    void save(const QJsonObject &value) { QVERIFY(FakeCli::replace(script()+".json",bytes(value))); }
    QJsonObject config() const { QFile f(script()+".json");if(!f.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(f.readAll()).object(); }
    QList<QJsonObject> calls() const { QFile f(script()+".calls");if(!f.open(QIODevice::ReadOnly))return {};QList<QJsonObject> result;for(const auto &line:f.readAll().split('\n'))if(!line.isEmpty())result.append(QJsonDocument::fromJson(line).object());return result; }
    int mutations() const { int n=0;for(const auto &call:calls()){const auto action=call.value("payload").toObject().value("action").toString();if(action=="configure"||action=="purge")++n;}return n; }
    QString script() const { return m_dir->filePath("hgs"); }
    QString program() const { return FakeCli::program(script()); }
    void range(AnalyticsSettings &page,QDate through=last) { page.findChild<QDateEdit *>("analyticsFrom")->setDate(first);page.findChild<QDateEdit *>("analyticsThrough")->setDate(through); }
    std::unique_ptr<QTemporaryDir> m_dir;
};
void TestAnalytics::init() {
    m_dir=std::make_unique<QTemporaryDir>();QVERIFY(m_dir->isValid());
    // This suite never constructs the full GUI or touches native preferences.
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,m_dir->path());
    QVERIFY(FakeCli::install(script(),cli));save({{"agents",QJsonObject{{"",summary()}}},{"voice",voice()}});
}

void TestAnalytics::parserKeepsOnlyReviewedFieldsAndUnknowns() {
    auto raw=summary();raw.insert("prompt","private-content-sentinel");
    auto rows=raw.value("buckets").toArray();auto row=rows[0].toObject();row.insert("path","private-path-sentinel");
    auto counts=row.value("counters").toObject();counts.insert("arbitrary_counter_sentinel",123);row.insert("counters",counts);rows[0]=row;raw.insert("buckets",rows);
    auto coverage=raw.value("coverage").toObject();coverage.insert("gaps",QJsonArray{"private-error-sentinel"});raw.insert("coverage",coverage);
    const auto parsed=AnalyticsData::summary(bytes(raw),first,last);QVERIFY(parsed);
    QVERIFY(!bytes(*parsed).contains("sentinel"));QCOMPARE(parsed->value("coverage").toObject().value("gaps").toArray(),QJsonArray{"unknown"});
    const auto kept=parsed->value("buckets").toArray()[0].toObject().value("counters").toObject();QCOMPARE(kept.value("input_tokens").toInt(),17);QVERIFY(!kept.contains("output_tokens"));
    raw.insert("source_epoch","00000000-0000-0000-0000-000000000000");QVERIFY(AnalyticsData::summary(bytes(raw),first,last));
}

void TestAnalytics::parserRejectsInvalidAndUnboundedSummaries() {
    QVERIFY(!AnalyticsData::summary(QByteArray(AnalyticsData::MaximumBytes+1,'x'),first,last));
    QVERIFY(!AnalyticsData::summary(bytes(summary()),first,first.addDays(90)));
    for(const auto &key:{"machine_id","source_epoch","generated_at","revision"}){auto invalid=summary();invalid.insert(key,"private");QVERIFY2(!AnalyticsData::summary(bytes(invalid),first,last),key);}
    auto duplicate=summary(QJsonArray{bucket(),bucket()});QVERIFY(!AnalyticsData::summary(bytes(duplicate),first,last));
    auto bad=bucket();auto c=bad.value("counters").toObject();c.insert("work_ms",-1);bad.insert("counters",c);QVERIFY(!AnalyticsData::summary(bytes(summary(QJsonArray{bad})),first,last));
    bad=bucket();bad.insert("provider","private-account-name");QVERIFY(!AnalyticsData::summary(bytes(summary(QJsonArray{bad})),first,last));
    auto histogram=bucket().value("histograms").toObject();auto work=histogram.value("work_ms").toObject();work.insert("count",2);histogram.insert("work_ms",work);bad=bucket();bad.insert("histograms",histogram);QVERIFY(!AnalyticsData::summary(bytes(summary(QJsonArray{bad})),first,last));
    QJsonArray excessive;for(int n=0;n<=AnalyticsData::MaximumBuckets;++n)excessive.append(bucket());QVERIFY(!AnalyticsData::summary(bytes(summary(excessive)),first,last));
}

void TestAnalytics::fleetReplacesSnapshotsAndEpochsInsteadOfSumming() {
    AnalyticsData::Observation a{"route-a",summary(),{},true},b{"route-b",summary(),{},true};
    auto unique=AnalyticsData::uniqueComputers({a,b});QCOMPARE(unique.size(),1);QCOMPARE(AnalyticsData::totals(unique[0].agent.value("buckets").toArray()).value("turn_completed").toInt(),3);
    b.agent.insert("revision",8);b.agent.insert("buckets",QJsonArray{bucket(first,"codex",5)});unique=AnalyticsData::uniqueComputers({a,b});QCOMPARE(unique[0].agent.value("revision").toInt(),8);
    b.agent.insert("source_epoch",resetEpoch);b.agent.insert("revision",0);b.agent.insert("generated_at",a.agent.value("generated_at").toDouble()+1);b.agent.insert("buckets",QJsonArray{});b.current=false;
    unique=AnalyticsData::uniqueComputers({b,a});QCOMPARE(unique.size(),1);QCOMPARE(unique[0].agent.value("source_epoch").toString(),resetEpoch);QVERIFY(!unique[0].current);QVERIFY(unique[0].agent.value("buckets").toArray().isEmpty());
}

void TestAnalytics::histogramsMergeSamplesBeforeComputingMean() {
    const auto one=workBucket({1,0,0,0,0,0},1,100);
    const auto nine=workBucket({0,9,0,0,0,0},9,9000,"claude");
    const auto merged=AnalyticsData::histogramTotals({one,nine}).value("work_ms").toObject();
    QCOMPARE(merged.value("version").toInt(),1);
    QCOMPARE(merged.value("bounds").toArray(),QJsonArray({100,1000,10000,60000,600000}));
    QCOMPARE(merged.value("counts").toArray(),QJsonArray({1,9,0,0,0,0}));
    QCOMPARE(merged.value("count").toInteger(),10);
    QCOMPARE(merged.value("sum").toInteger(),9100);
    // The mean of machine means would be 550 ms; pooled samples weigh 910 ms.
    QCOMPARE(merged.value("sum").toDouble()/merged.value("count").toDouble(),910.);
    QCOMPARE(AnalyticsData::histogramTotals({nine,one}).value("work_ms").toObject(),merged);
    QCOMPARE(AnalyticsData::histogramTotals({one,nine},"codex").value("work_ms").toObject().value("count").toInteger(),1);
    QVERIFY(AnalyticsData::histogramTotals({one,nine},"kimi").isEmpty());
    QVERIFY(AnalyticsData::histogramTotals({}).isEmpty());
    const auto zero=workBucket({0,0,0,0,0,0},0,0);
    QCOMPARE(AnalyticsData::histogramTotals({zero,one,nine}).value("work_ms").toObject(),merged);
}

void TestAnalytics::invalidMissingAndOverflowingHistogramsStayUnavailable() {
    const auto valid=workBucket({1,0,0,0,0,0},1,100);
    const auto validHistogram=valid.value("histograms").toObject().value("work_ms").toObject();
    QList<QJsonObject> malformed;
    auto bad=validHistogram;bad.insert("version",2);malformed.append(bad);
    bad=validHistogram;bad.insert("bounds",QJsonArray{100,1000,10000,60000,600001});malformed.append(bad);
    bad=validHistogram;bad.insert("count",2);malformed.append(bad);
    bad=validHistogram;bad.insert("counts",QJsonArray{1,0,0,0,0});malformed.append(bad);
    bad=validHistogram;bad.insert("counts",QJsonArray{-1,0,0,0,0,0});malformed.append(bad);
    bad=validHistogram;bad.insert("sum",1.5);malformed.append(bad);
    bad=validHistogram;bad.insert("sum",9007199254740992.);malformed.append(bad);
    bad=validHistogram;bad.insert("counts",QJsonArray{0,0,0,0,0,0});bad.insert("count",0);malformed.append(bad);
    for(const auto &h:malformed) {
        auto invalid=valid;invalid.insert("histograms",QJsonObject{{"work_ms",h}});
        QVERIFY(!AnalyticsData::summary(bytes(summary({invalid})),first,last));
        QVERIFY(!AnalyticsData::histogramTotals({valid,invalid,valid}).contains("work_ms"));
    }
    auto missing=valid;missing.insert("histograms",QJsonObject{});
    QVERIFY(AnalyticsData::summary(bytes(summary({missing})),first,last));
    QVERIFY(!AnalyticsData::histogramTotals({valid,missing,valid}).contains("work_ms"));
    QVERIFY(!AnalyticsData::histogramTotals({missing,valid}).contains("work_ms"));
    constexpr qint64 maximum=9007199254740991LL;
    const auto countLimit=workBucket({maximum,0,0,0,0,0},maximum,0);
    QVERIFY(AnalyticsData::summary(bytes(summary({countLimit})),first,last));
    // Both same-bin overflow and total-count overflow across different bins fail closed.
    QVERIFY(!AnalyticsData::histogramTotals({countLimit,valid}).contains("work_ms"));
    QVERIFY(!AnalyticsData::histogramTotals({countLimit,workBucket({0,1,0,0,0,0},1,101)}).contains("work_ms"));
    const auto sumLimit=workBucket({0,0,0,0,0,1},1,maximum);
    QVERIFY(AnalyticsData::summary(bytes(summary({sumLimit})),first,last));
    QVERIFY(!AnalyticsData::histogramTotals({sumLimit,valid}).contains("work_ms"));
    const auto justBelow=workBucket({0,0,0,0,0,1},1,maximum-100);
    QCOMPARE(AnalyticsData::histogramTotals({justBelow,valid}).value("work_ms").toObject().value("sum").toInteger(),maximum);
}

void TestAnalytics::voiceCollapsesPrivateProfilesAndPreservesUnits() {
    auto raw=voice();auto days=raw.value("days").toArray();auto row=days[0].toObject();row.insert("profile_id","second-private-profile");row.insert("queue_ms",1.5);row.insert("error_codes",QJsonObject{{"private-provider-message",2}});days.append(row);raw.insert("days",days);
    const auto parsed=AnalyticsData::speech(bytes(raw),first,last);QVERIFY(parsed);QVERIFY(!bytes(*parsed).contains("private"));QCOMPARE(parsed->value("days").toArray().size(),1);
    const auto result=parsed->value("days").toArray()[0].toObject();QCOMPARE(result.value("submitted").toInt(),4);QCOMPARE(result.value("queue_ms").toDouble(),60001.5);QCOMPARE(result.value("error_codes").toObject().value("unknown").toInt(),2);
    days.append(days[0]);raw.insert("days",days);QVERIFY(!AnalyticsData::speech(bytes(raw),first,last));
}

void TestAnalytics::retiredEpochNeverRevivesWithNewTimestamp() {
    AnalyticsData::EpochGuard guard;auto original=summary();QVERIFY(guard.accept(original));
    auto reset=original;reset.insert("source_epoch",resetEpoch);reset.insert("generated_at",original.value("generated_at").toDouble()+1);reset.insert("revision",0);QVERIFY(guard.accept(reset));
    original.insert("generated_at",reset.value("generated_at").toDouble()+100);original.insert("revision",999);QVERIFY(!guard.accept(original));
    reset.insert("revision",5);QVERIFY(guard.accept(reset));reset.insert("revision",4);QVERIFY(!guard.accept(reset));
    auto next=reset;
    for(int n=0;n<63;++n){next.insert("source_epoch",QString("30000000-0000-4000-8000-%1").arg(n,12,10,QChar('0')));next.insert("generated_at",original.value("generated_at").toDouble()+n+1);QVERIFY(guard.accept(next));}
    next.insert("source_epoch","40000000-0000-4000-8000-000000000001");next.insert("generated_at",original.value("generated_at").toDouble()+1000);QVERIFY(!guard.accept(next));
}

void TestAnalytics::diagnosticsExcludeIdentityContentAndDetailedHistory() {
    const auto agent=AnalyticsData::summary(bytes(summary()),first,last),v=AnalyticsData::speech(bytes(voice()),first,last);QVERIFY(agent&&v);
    const auto exported=bytes(AnalyticsData::diagnostic({{"private-host-sentinel",*agent,*v,true,true}},first,last));
    QVERIFY(!exported.contains(machine.toUtf8()));QVERIFY(!exported.contains(epoch.toUtf8()));QVERIFY(!exported.contains("private"));QVERIFY(!exported.contains("profile_id"));QVERIFY(!exported.contains("buckets"));QVERIFY(!exported.contains("\"days\":"));QVERIFY(exported.contains("reported_totals"));
}

void TestAnalytics::oldCliIsProbedWithoutLaunchingUnknownVerb() {
    auto data=config();data.insert("old",QJsonArray{"old"});save(data);
    AnalyticsClient client(program());bool done=false;QString error;
    client.summary("old",first,last,[&](const QJsonObject &,const QString &why){done=true;error=why;});QTRY_VERIFY(done);QVERIFY(!error.isEmpty());
    QCOMPARE(calls().size(),1);QCOMPARE(calls()[0].value("host").toString(),"old");QCOMPARE(calls()[0].value("args").toArray(),QJsonArray{"--help"});
}

void TestAnalytics::responseCapAndErrorsNeverEchoPrivateText() {
    AnalyticsClient client(program());auto data=config();data.insert("oversized",true);save(data);bool done=false;QString error;
    client.summary({},first,last,[&](const QJsonObject &result,const QString &why){done=true;error=why;QVERIFY(result.isEmpty());});QTRY_VERIFY(done);QVERIFY(!error.isEmpty());
    data.remove("oversized");data.insert("offline",QJsonArray{"peer"});save(data);done=false;
    client.summary("peer",first,last,[&](const QJsonObject &,const QString &why){done=true;error=why;});QTRY_VERIFY(done);QVERIFY(!error.contains("sentinel"));
}

void TestAnalytics::pageReadsWithoutEnablingAndPagesDailyRows() {
    QJsonArray rows;for(int n=0;n<60;++n)rows.append(bucket(first.addDays(n)));
    auto data=config();data.insert("agents",QJsonObject{{"",summary(rows)}});save(data);
    AnalyticsSettings page(program());range(page,first.addDays(59));page.show();QVERIFY(calls().isEmpty());page.refresh();
    auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    auto *table=page.findChild<QTableWidget *>("analyticsDaily");QCOMPARE(table->rowCount(),50);QCOMPARE(mutations(),0);QVERIFY(!page.findChild<QCheckBox *>("analyticsEnabled")->isChecked());
    page.findChild<QPushButton *>("analyticsNext")->click();QCOMPARE(table->rowCount(),10);QCOMPARE(table->item(0,0)->text(),first.addDays(50).toString(Qt::ISODate));
    table->selectRow(0);QVERIFY(page.findChild<QPlainTextEdit *>("analyticsDetail")->toPlainText().contains("output tokens: Unavailable"));
    for(const auto &call:calls())QVERIFY(!call.value("inherited_session").toBool());
}

void TestAnalytics::repeatedRefreshAndDuplicateRoutesDoNotAddTotals() {
    AnalyticsSettings page(program());range(page);page.setPeers({"same-computer"});page.findChild<QComboBox *>("analyticsComputer")->setCurrentIndex(1);page.refresh();
    auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    auto *overview=page.findChild<QLabel *>("analyticsOverview");QVERIFY(overview->text().contains("1 current unique computer"));QVERIFY(overview->text().contains("3 completed turns"));QCOMPARE(page.findChild<QTableWidget *>("analyticsDaily")->rowCount(),1);
    QVERIFY(overview->text().contains("Recorded fleet work histogram: 1 samples"));
    page.refresh();QTRY_VERIFY(refresh->isEnabled());QVERIFY(overview->text().contains("3 completed turns"));QVERIFY(overview->text().contains("Recorded fleet work histogram: 1 samples"));QCOMPARE(mutations(),0);
}

void TestAnalytics::fleetOverviewWeightsSamplesAndExcludesStaleComputers() {
    auto local=summary({workBucket({1,0,0,0,0,0},1,100)});
    auto peer=summary({workBucket({0,9,0,0,0,0},9,9000)});peer.insert("machine_id","10000000-0000-4000-8000-000000000002");
    auto data=config();data.insert("agents",QJsonObject{{"",local},{"peer",peer}});save(data);
    AnalyticsSettings page(program());range(page);page.setPeers({"same-computer","peer"});page.findChild<QComboBox *>("analyticsComputer")->setCurrentIndex(1);page.refresh();
    auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    auto *overview=page.findChild<QLabel *>("analyticsOverview");
    const auto pooled=QString("10 samples; weighted mean %1 ms").arg(QLocale().toString(910.,'f',1));
    QVERIFY(overview->text().contains("2 current unique computer"));QVERIFY(overview->text().contains(pooled));
    page.refresh();QTRY_VERIFY(refresh->isEnabled());QVERIFY(overview->text().contains(pooled));
    data.insert("offline",QJsonArray{"peer"});save(data);page.refresh();QTRY_VERIFY(refresh->isEnabled());
    QVERIFY(overview->text().contains("1 current unique computer"));
    QVERIFY(overview->text().contains(QString("1 samples; weighted mean %1 ms").arg(QLocale().toString(100.,'f',1))));
    QVERIFY(!overview->text().contains(pooled));QCOMPARE(mutations(),0);
}

void TestAnalytics::failedReadKeepsLastKnownButExcludesTotals() {
    AnalyticsSettings page(program());range(page);page.refresh();auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    auto data=config();data.insert("offline",QJsonArray{""});save(data);page.refresh();QTRY_VERIFY(refresh->isEnabled());
    QVERIFY(page.findChild<QLabel *>("analyticsOverview")->text().contains("0 current unique computer"));QVERIFY(page.findChild<QLabel *>("analyticsOverview")->text().contains("Unavailable completed turns"));
    QVERIFY(page.findChild<QLabel *>("analyticsOverview")->text().contains("Recorded fleet work histogram: Unavailable"));
    QVERIFY(page.findChild<QPlainTextEdit *>("analyticsCoverage")->toPlainText().contains("Last known (excluded)"));QCOMPARE(page.findChild<QTableWidget *>("analyticsDaily")->rowCount(),1);
    QVERIFY(!page.findChild<QPushButton *>("applyAnalyticsCollection")->isEnabled());
}

void TestAnalytics::controlsStayLocalAndRequireExplicitApply() {
    AnalyticsSettings page(program());range(page);page.setPeers({"peer"});page.refresh();auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    page.findChild<QCheckBox *>("analyticsEnabled")->setChecked(true);QCOMPARE(mutations(),0);
    page.findChild<QPushButton *>("applyAnalyticsCollection")->click();QTRY_VERIFY(refresh->isEnabled());QCOMPARE(mutations(),1);
    for(const auto &call:calls())if(call.value("payload").toObject().value("action")=="configure"){
        QVERIFY(call.value("host").toString().isEmpty());const auto p=call.value("payload").toObject();QCOMPARE(p.value("expected_epoch").toString(),epoch);QCOMPARE(p.value("retention_days").toInt(),90);QVERIFY(p.value("enabled").toBool());}
    page.findChild<QComboBox *>("analyticsComputer")->setCurrentIndex(2);page.refresh();QTRY_VERIFY(refresh->isEnabled());QVERIFY(!page.findChild<QPushButton *>("applyAnalyticsCollection")->isEnabled());
}

void TestAnalytics::retentionReductionRequiresConfirmation() {
    AnalyticsSettings page(program());range(page);page.refresh();auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    page.findChild<QSpinBox *>("analyticsRetention")->setValue(45);bool saw=false,accept=false;
    QTimer poll;poll.setInterval(10);connect(&poll,&QTimer::timeout,&page,[&]{for(auto *w:QApplication::topLevelWidgets())if(w->objectName()=="confirmAnalyticsRetention"){
        auto *box=qobject_cast<QMessageBox *>(w);saw=true;QCOMPARE(box->defaultButton(),box->button(QMessageBox::Cancel));
        if(accept){for(auto *button:box->buttons())if(box->buttonRole(button)==QMessageBox::DestructiveRole){button->click();break;}}
        else box->button(QMessageBox::Cancel)->click();
    }});poll.start();page.findChild<QPushButton *>("applyAnalyticsCollection")->click();QVERIFY(saw);QCOMPARE(mutations(),0);
    saw=false;accept=true;page.findChild<QPushButton *>("applyAnalyticsCollection")->click();QVERIFY(saw);QTRY_VERIFY(refresh->isEnabled());QCOMPARE(mutations(),1);
    for(const auto &call:calls())if(call.value("payload").toObject().value("action")=="configure")QCOMPARE(call.value("payload").toObject().value("retention_days").toInt(),45);
}

void TestAnalytics::invalidRangeAndRemoteFailureKeepLocalDataUseful() {
    AnalyticsSettings page(program());range(page,first.addDays(90));page.refresh();QVERIFY(calls().isEmpty());QVERIFY(page.findChild<QLabel *>("analyticsStatus")->text().contains("at most 90 days"));
    auto data=config();data.insert("offline",QJsonArray{"offline"});save(data);range(page);page.setPeers({"offline"});page.findChild<QComboBox *>("analyticsComputer")->setCurrentIndex(1);page.refresh();
    QTRY_VERIFY(page.findChild<QPushButton *>("refreshAnalytics")->isEnabled());QVERIFY(page.findChild<QLabel *>("analyticsOverview")->text().contains("3 completed turns"));
    QVERIFY(page.findChild<QPlainTextEdit *>("analyticsCoverage")->toPlainText().contains("offline: Computer unavailable"));QVERIFY(page.findChild<QPushButton *>("applyAnalyticsCollection")->isEnabled());
}

void TestAnalytics::purgeCancelDoesNothingAndConfirmUsesFreshEpoch() {
    AnalyticsSettings page(program());range(page);page.refresh();auto *refresh=page.findChild<QPushButton *>("refreshAnalytics");QTRY_VERIFY(refresh->isEnabled());
    QTimer poll;poll.setInterval(10);bool saw=false,accept=false;
    connect(&poll,&QTimer::timeout,&page,[&]{for(auto *w:QApplication::topLevelWidgets())if(w->objectName()=="confirmAnalyticsPurge"){
        auto *box=qobject_cast<QMessageBox *>(w);if(!box)return;saw=true;QCOMPARE(box->defaultButton(),box->button(QMessageBox::Cancel));
        if(accept){for(auto *button:box->buttons())if(box->buttonRole(button)==QMessageBox::DestructiveRole){button->click();break;}}
        else box->button(QMessageBox::Cancel)->click();
    }});poll.start();page.findChild<QPushButton *>("purgeAnalytics")->click();QTRY_VERIFY(saw&&refresh->isEnabled());QCOMPARE(mutations(),0);
    auto data=config();auto a=summary();a.insert("source_epoch",resetEpoch);data.insert("agents",QJsonObject{{"",a}});save(data);
    saw=false;accept=true;page.findChild<QPushButton *>("purgeAnalytics")->click();QTRY_VERIFY(saw&&refresh->isEnabled());QCOMPARE(mutations(),1);
    for(const auto &call:calls())if(call.value("payload").toObject().value("action")=="purge")QCOMPARE(call.value("payload").toObject().value("expected_epoch").toString(),resetEpoch);
    QCOMPARE(page.findChild<QTableWidget *>("analyticsDaily")->rowCount(),0);
}

void TestAnalytics::diagnosticReviewDoesNotExportAutomatically() {
    AnalyticsSettings page(program());range(page);page.refresh();QTRY_VERIFY(page.findChild<QPushButton *>("refreshAnalytics")->isEnabled());
    bool saw=false;QTimer::singleShot(0,&page,[&]{for(auto *w:QApplication::topLevelWidgets())if(w->objectName()=="analyticsDiagnosticsReview"){
        saw=true;const auto text=w->findChild<QPlainTextEdit *>("analyticsDiagnosticsPreview")->toPlainText();QVERIFY(!text.contains(machine));QVERIFY(!text.contains("profile_id"));qobject_cast<QDialog *>(w)->reject();}});
    page.findChild<QPushButton *>("exportAnalyticsDiagnostics")->click();QVERIFY(saw);QCOMPARE(mutations(),0);QVERIFY(!QFile::exists(m_dir->filePath("zerus-aggregate-diagnostics.json")));
}

QTEST_MAIN(TestAnalytics)
#include "test_analytics.moc"
