#include "AnalyticsSettings.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QFileDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString timeText(const QJsonValue &value) {
    return value.isDouble() ? QDateTime::fromMSecsSinceEpoch(qint64(value.toDouble()*1000),QTimeZone::UTC).toString("yyyy-MM-dd HH:mm:ss 'UTC'") : QObject::tr("Unknown");
}
QString measure(const QJsonObject &values, const QString &key) {
    if (!values.contains(key)) return QObject::tr("Unavailable");
    const auto n = values.value(key).toDouble();
    if (key.endsWith("_ms")) return QObject::tr("%1 min").arg(QLocale().toString(n/60000.,'f',2));
    return QLocale().toString(qint64(n));
}
QString codes(const QJsonArray &values) {
    QStringList text; for (const auto &value : values) { auto code = value.toString(); code.replace('_',' '); text.append(code); }
    return text.isEmpty() ? QObject::tr("None reported") : text.join(", ");
}
bool fresh(const QJsonObject &agent) {
    const auto age = QDateTime::currentMSecsSinceEpoch()/1000. - agent.value("generated_at").toDouble();
    return !agent.isEmpty() && age >= -60 && age <= 300;
}
QString describeCounters(const QJsonObject &counts, const QStringList &names) {
    QStringList lines;
    for (const auto &name : names) {
        QString label = name;if(label.endsWith("_ms"))label.chop(3);label.replace('_',' ');
        if(name=="audio_ms")label=QObject::tr("Accepted audio");
        else if(name=="queue_ms")label=QObject::tr("Total queue time");
        else if(name=="processing_ms")label=QObject::tr("Total processing time");
        lines << label + ": " + measure(counts,name);
    }
    return lines.join('\n');
}
}

AnalyticsSettings::AnalyticsSettings(const QString &executable,QWidget *parent) : QWidget(parent),m_client(executable,this) {
    setObjectName("analyticsSettings"); setMaximumWidth(1200);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,12,0); layout->setSpacing(12);
    auto *heading = new QLabel(tr("Private analytics")); heading->setObjectName("heading"); layout->addWidget(heading);
    auto *hint = new QLabel(tr("Content-free daily counters stay on each computer. Reads do not enable collection. Unknown measures are unavailable, not zero. Dates are inclusive UTC days (up to 90).")); hint->setWordWrap(true); layout->addWidget(hint);
    auto *scope = new QHBoxLayout;
    m_computer = new QComboBox; m_computer->setObjectName("analyticsComputer"); m_computer->setAccessibleName(tr("Selected computers"));
    m_computer->addItem(tr("This computer"),QString()); m_computer->addItem(tr("All configured computers"),QStringLiteral("*")); scope->addWidget(m_computer);
    m_refresh = new QPushButton(tr("Apply and refresh")); m_refresh->setObjectName("refreshAnalytics"); scope->addWidget(m_refresh); scope->addStretch(); layout->addLayout(scope);
    auto *dates = new QHBoxLayout;
    m_from = new QDateEdit(QDateTime::currentDateTimeUtc().date().addDays(-29)); m_from->setObjectName("analyticsFrom");
    m_through = new QDateEdit(QDateTime::currentDateTimeUtc().date()); m_through->setObjectName("analyticsThrough");
    for (auto *date : {m_from,m_through}) { date->setDisplayFormat("yyyy-MM-dd"); date->setCalendarPopup(true); }
    m_from->setAccessibleName(tr("From UTC day")); m_through->setAccessibleName(tr("Through UTC day"));
    dates->addWidget(new QLabel(tr("From"))); dates->addWidget(m_from); dates->addWidget(new QLabel(tr("Through"))); dates->addWidget(m_through);dates->addStretch();layout->addLayout(dates);
    m_status = new QLabel; m_status->setObjectName("analyticsStatus"); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    m_overview = new QLabel(tr("Choose a range and refresh to read analytics.")); m_overview->setObjectName("analyticsOverview"); m_overview->setWordWrap(true); m_overview->setTextFormat(Qt::PlainText); layout->addWidget(m_overview);
    m_coverage = new QPlainTextEdit; m_coverage->setObjectName("analyticsCoverage"); m_coverage->setReadOnly(true); m_coverage->setMaximumHeight(170); layout->addWidget(m_coverage);
    auto *filter = new QHBoxLayout; filter->addWidget(new QLabel(tr("Daily agent work")));
    m_provider = new QComboBox; m_provider->setObjectName("analyticsProvider"); m_provider->setAccessibleName(tr("Provider")); m_provider->addItem(tr("All providers"),QString());
    for (const auto &provider : AnalyticsData::providers()) m_provider->addItem(provider,provider);
    filter->addWidget(m_provider); filter->addStretch(); layout->addLayout(filter);
    m_table = new QTableWidget(0,8); m_table->setObjectName("analyticsDaily");
    m_table->setHorizontalHeaderLabels({tr("UTC day"),tr("Computer"),tr("Provider"),tr("Producer"),tr("Completed turns"),tr("Work"),tr("Approval wait"),tr("Input wait")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setWordWrap(false); m_table->verticalHeader()->hide(); m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->setMinimumHeight(230); layout->addWidget(m_table);
    auto *pages = new QHBoxLayout; m_previous = new QPushButton(tr("Previous")); m_previous->setObjectName("analyticsPrevious"); m_next = new QPushButton(tr("Next")); m_next->setObjectName("analyticsNext");
    m_page = new QLabel; m_page->setObjectName("analyticsPage"); pages->addWidget(m_previous); pages->addWidget(m_next); pages->addWidget(m_page); pages->addStretch(); layout->addLayout(pages);
    m_detail = new QPlainTextEdit; m_detail->setObjectName("analyticsDetail"); m_detail->setReadOnly(true); m_detail->setPlaceholderText(tr("Select a day to see all reported counts, token usage, typed failures and histogram means.")); m_detail->setMaximumHeight(190); layout->addWidget(m_detail);
    auto *voiceHeading = new QLabel(tr("Voice on processing computers")); layout->addWidget(voiceHeading);
    m_voice = new QPlainTextEdit; m_voice->setObjectName("analyticsVoice"); m_voice->setReadOnly(true); m_voice->setMaximumHeight(190); layout->addWidget(m_voice);
    auto *voiceHint = new QLabel(tr("Voice counts are separate from agent turns. Accepted audio and total queue/processing time have units; percentile latency is unavailable. Speech collection is managed independently on its processing host.")); voiceHint->setWordWrap(true); layout->addWidget(voiceHint);
    auto *localHeading = new QLabel(tr("Collection on this computer")); localHeading->setObjectName("heading"); layout->addWidget(localHeading);
    auto *local = new QHBoxLayout;
    m_enabled = new QCheckBox(tr("Collect content-free agent analytics")); m_enabled->setObjectName("analyticsEnabled"); layout->addWidget(m_enabled);
    m_retention = new QSpinBox; m_retention->setObjectName("analyticsRetention"); m_retention->setRange(1,90); m_retention->setValue(90); m_retention->setSuffix(tr(" days")); m_retention->setAccessibleName(tr("Daily aggregate retention")); local->addWidget(m_retention);
    m_apply = new QPushButton(tr("Apply locally")); m_apply->setObjectName("applyAnalyticsCollection"); local->addWidget(m_apply);
    m_purge = new QPushButton(tr("Purge local history…")); m_purge->setObjectName("purgeAnalytics"); local->addWidget(m_purge); local->addStretch(); layout->addLayout(local);
    auto *localHint = new QLabel(tr("Disabling collection keeps existing history. Purge removes only this computer's agent analytics, not speech statistics or durable delivery receipts. Remote collection controls are read-only here.")); localHint->setWordWrap(true); layout->addWidget(localHint);
    m_export = new QPushButton(tr("Review aggregate diagnostics…")); m_export->setObjectName("exportAnalyticsDiagnostics"); layout->addWidget(m_export,0,Qt::AlignLeft); layout->addStretch();
    connect(m_refresh,&QPushButton::clicked,this,[this]{refresh();});
    connect(m_provider,&QComboBox::currentIndexChanged,this,[this]{m_offset=0;renderRows();});
    connect(m_previous,&QPushButton::clicked,this,[this]{m_offset=qMax(0,m_offset-AnalyticsData::PageSize);renderRows();});
    connect(m_next,&QPushButton::clicked,this,[this]{m_offset+=AnalyticsData::PageSize;renderRows();});
    connect(m_table,&QTableWidget::itemSelectionChanged,this,[this]{showDetail();});
    connect(m_apply,&QPushButton::clicked,this,[this]{configure();}); connect(m_purge,&QPushButton::clicked,this,[this]{purge();});
    connect(m_export,&QPushButton::clicked,this,[this]{exportDiagnostics();});
    auto *age = new QTimer(this); age->setInterval(60000); connect(age,&QTimer::timeout,this,[this]{if(isVisible()&&!m_busy)render();}); age->start();
    controls();
}

QString AnalyticsSettings::displayRoute(const QString &route) const { return route.isEmpty() ? tr("This computer") : route; }
void AnalyticsSettings::setPeers(const QStringList &peers) {
    QStringList kept;
    m_peerLimit=false;
    for (const auto &peer : peers) if (!peer.isEmpty() && peer != "*" && peer.size() <= 256 && !kept.contains(peer)) {
        if (kept.size() == AnalyticsData::MaximumComputers-1) {m_peerLimit=true;break;} kept.append(peer);
    }
    m_computer->setItemText(1,m_peerLimit?tr("First 32 configured computers"):tr("All configured computers"));
    if (kept == m_peers) return;
    m_peers = kept; const auto selected = m_computer->currentData().toString(); const QSignalBlocker block(m_computer);
    while (m_computer->count()>2) m_computer->removeItem(2);
    for (const auto &peer : kept) m_computer->addItem(peer,peer);
    m_computer->setCurrentIndex(qMax(0,m_computer->findData(selected)));
    // Forget disconnected routes instead of growing an unbounded peer cache.
    for (auto i=m_snapshots.begin();i!=m_snapshots.end();) if (!i.key().isEmpty()&&!m_peers.contains(i.key())) i=m_snapshots.erase(i); else ++i;
}

void AnalyticsSettings::refresh() {
    if (m_busy || m_mutating) return;
    if (!AnalyticsData::validRange(m_from->date(),m_through->date())) { m_status->setText(tr("Choose an inclusive UTC range of at most 90 days.")); return; }
    const auto from=m_from->date(),through=m_through->date();
    if (from!=m_appliedFrom||through!=m_appliedThrough) m_snapshots.clear();
    m_appliedFrom=from; m_appliedThrough=through; m_offset=0;
    m_routes = m_computer->currentData().toString()=="*" ? QStringList{QString()}+m_peers : QStringList{m_computer->currentData().toString()};
    m_queue=m_routes; m_busy=true; m_local={};
    for (const auto &route : m_routes) { auto &o=m_snapshots[route];o.route=route;o.current=false;o.voiceCurrent=false;o.error=tr("Reading…"); }
    controls(); render(); m_status->setText(tr("Reading %1 configured route(s)…").arg(m_routes.size())); nextComputer();
}

void AnalyticsSettings::nextComputer() {
    if (m_queue.isEmpty()) {
        m_busy=false; m_status->setText(tr("Read finished. Last-known or missing data is excluded from current totals.")); render(); controls(); return;
    }
    const auto route=m_queue.takeFirst();
    if(!route.isEmpty()&&!m_peers.contains(route)){nextComputer();return;}
    // One route at a time bounds child processes and pending parse work even for
    // a large peer catalog. Scope edits are staged until the next refresh.
    m_client.summary(route,m_appliedFrom,m_appliedThrough,[this,route](const QJsonObject &reply,const QString &error) {
        auto &o=m_snapshots[route];o.route=route;o.error=error;o.current=false;
        if (error.isEmpty() && !m_epochs.accept(reply)) {
            o.error=tr("Retired, older or ambiguous analytics source. Excluded from current totals.");
        } else if (error.isEmpty()) {
            // A lower revision from the same epoch cannot replace a newer cached
            // source. A new epoch always replaces this exact route (purge/reset).
            if (o.agent.isEmpty()||o.agent.value("source_epoch")!=reply.value("source_epoch")||reply.value("revision").toDouble()>=o.agent.value("revision").toDouble()) o.agent=reply;
            o.current=fresh(o.agent)&&o.agent==reply;
            if (!o.current) o.error=tr("Summary is stale or its clock is unconfirmed.");
            if (route.isEmpty()&&o.current) {
                m_local=reply; const QSignalBlocker block(m_enabled);
                m_enabled->setChecked(reply.value("collection").toObject().value("enabled").toBool());
                m_retention->setValue(reply.value("collection").toObject().value("retention_days").toInt());
            }
        }
        m_client.speech(route,m_appliedFrom,m_appliedThrough,[this,route](const QJsonObject &voice,const QString &failure) {
            auto &o=m_snapshots[route];o.voiceCurrent=failure.isEmpty();o.voiceError=failure;
            if (failure.isEmpty()) {o.voice=voice;o.voiceReadAt=QDateTime::currentMSecsSinceEpoch();}
            nextComputer();
        });
    });
}

QList<AnalyticsData::Observation> AnalyticsSettings::observations() const {
    QList<AnalyticsData::Observation> values;
    for (const auto &route : m_routes) { auto o=m_snapshots.value(route);o.route=route;if(o.agent.isEmpty()&&o.error.isEmpty())o.error=tr("No current read available.");o.current=o.current&&fresh(o.agent);o.voiceCurrent=o.voiceCurrent&&QDateTime::currentMSecsSinceEpoch()-o.voiceReadAt<=300000; values.append(o); }
    return values;
}

void AnalyticsSettings::controls() {
    const bool idle=!m_busy&&!m_mutating;
    m_refresh->setEnabled(idle); m_export->setEnabled(idle&&!m_routes.isEmpty());
    const bool local=idle&&!m_local.isEmpty()&&fresh(m_local);
    m_enabled->setEnabled(local);m_retention->setEnabled(local);m_apply->setEnabled(local);m_purge->setEnabled(local);
}

void AnalyticsSettings::render() {
    const auto all=observations(); const auto computers=AnalyticsData::uniqueComputers(all);
    QJsonArray current; QStringList coverage,voice;int present=0;
    if(m_peerLimit)coverage<<tr("This view includes at most 32 configured computers. Additional configured routes are omitted.");
    m_rows.clear();
    for (const auto &o : all) if (o.agent.isEmpty()) coverage << displayRoute(o.route)+": "+o.error;
    for (const auto &o : computers) {
        const auto &a=o.agent; const auto c=a.value("coverage").toObject();
        coverage << tr("%1 (%2); generated %3; revision %4\nCoverage %5 to %6; %7\nGaps: %8\nUnsupported: %9\nCollection: %10")
            .arg(displayRoute(o.route),o.current?tr("Current"):tr("Last known (excluded)"),timeText(a.value("generated_at")),QString::number(a.value("revision").toInteger()),
                timeText(c.value("start")),timeText(c.value("end")),c.value("partial").toBool()?tr("partial"):tr("no partial flag reported"),codes(c.value("gaps").toArray()),
                codes(c.value("unsupported").toArray()),a.value("collection").toObject().value("enabled").toBool()?tr("enabled"):tr("disabled"));
        if(!o.error.isEmpty())coverage<<o.error;
        const auto waits=a.value("unresolved_waits").toObject();
        if(!waits.isEmpty())coverage<<tr("Current unresolved waits observed: %1 (%2)").arg(waits.value("observed").toInteger()).arg(waits.value("complete").toBool()?tr("reported complete"):tr("incomplete observation"));
        const auto providerCoverage=c.value("providers").toObject();
        for(const auto &provider:AnalyticsData::providers()) {
            const auto pc=providerCoverage.value(provider).toObject();
            coverage<<tr("%1 coverage: %2; unsupported: %3").arg(provider,pc.isEmpty()?tr("unknown"):pc.value("partial").toBool()?tr("partial"):tr("reported"),pc.isEmpty()?tr("unknown"):codes(pc.value("unsupported").toArray()));
        }
        if(o.current)++present;
        for(const auto &v:a.value("buckets").toArray()) {
            auto row=v.toObject();row.insert("computer",displayRoute(o.route));row.insert("current",o.current);m_rows.append(row);
            if(o.current)current.append(v);
        }
    }
    auto voiceComputers=computers;for(const auto &o:all)if(o.agent.isEmpty())voiceComputers.append(o);
    for(const auto &o:voiceComputers) {
        if(!o.voice.isEmpty()) {
            const auto sums=AnalyticsData::speechTotals(o.voice.value("days").toArray());const auto errors=sums.value("error_codes").toObject();
            voice<<tr("%1 (%2; speech collection %3)\n%4\nTyped errors: %5")
                .arg(displayRoute(o.route),o.voiceCurrent?tr("latest read"):tr("last known"),o.voice.value("enabled").toBool()?tr("enabled"):tr("disabled"),describeCounters(sums,AnalyticsData::speechCounters()),QString::fromUtf8(QJsonDocument(errors).toJson(QJsonDocument::Compact)));
            if(o.agent.isEmpty())voice<<tr("Computer identity unavailable; this route is not included in any fleet total.");
        } else voice<<displayRoute(o.route)+": "+o.voiceError;
    }
    std::sort(m_rows.begin(),m_rows.end(),[](const QJsonObject &a,const QJsonObject &b){
        for(const auto &key:{"date","computer","provider","producer"})if(a.value(key)!=b.value(key))return a.value(key).toString()<b.value(key).toString();return false;
    });
    const auto sum=AnalyticsData::totals(current);
    const auto workHistogram=AnalyticsData::histogramTotals(current).value("work_ms").toObject();
    const auto sampleCount=workHistogram.value("count").toInteger();
    QString histogramText=tr("Recorded fleet work histogram: Unavailable (missing, incompatible or overflowing samples).");
    if(!workHistogram.isEmpty()) {
        const auto mean=sampleCount>0 ? tr("%1 ms").arg(QLocale().toString(workHistogram.value("sum").toDouble()/sampleCount,'f',1)) : tr("Unavailable (no samples)");
        histogramText=tr("Recorded fleet work histogram: %1 samples; weighted mean %2.").arg(QLocale().toString(sampleCount),mean);
    }
    m_overview->setText(tr("%1 through %2 UTC (%3 selected route(s), %4 current unique computer(s)). Recorded current totals: %5 completed turns; %6 work.\n%7\nCoverage below applies to every total and recorded sample.")
        .arg(m_appliedFrom.toString(Qt::ISODate),m_appliedThrough.toString(Qt::ISODate)).arg(m_routes.size()).arg(present).arg(measure(sum,"turn_completed"),measure(sum,"work_ms"),histogramText));
    m_coverage->setPlainText(coverage.join("\n\n"));m_voice->setPlainText(voice.join("\n\n"));renderRows();controls();
}

void AnalyticsSettings::renderRows() {
    QList<QJsonObject> filtered;const auto provider=m_provider->currentData().toString();
    for(const auto &row:m_rows)if(provider.isEmpty()||row.value("provider")==provider)filtered.append(row);
    if(m_offset>=filtered.size())m_offset=qMax(0,((filtered.size()-1)/AnalyticsData::PageSize)*AnalyticsData::PageSize);
    const auto page=filtered.mid(m_offset,AnalyticsData::PageSize);const QSignalBlocker block(m_table);
    m_table->clearContents();m_table->setRowCount(page.size());int index=0;
    for(const auto &row:page) {
        const auto counts=row.value("counters").toObject();
        QStringList text{row.value("date").toString(),row.value("computer").toString()+(row.value("current").toBool()?QString():tr(" (last known)")),row.value("provider").toString(),row.value("producer").toString(),
            measure(counts,"turn_completed"),measure(counts,"work_ms"),measure(counts,"approval_wait_ms"),measure(counts,"input_wait_ms")};
        for(int column=0;column<text.size();++column){auto *item=new QTableWidgetItem(text[column]);if(column==0)item->setData(Qt::UserRole,row);m_table->setItem(index,column,item);}++index;
    }
    m_previous->setEnabled(m_offset>0);m_next->setEnabled(m_offset+page.size()<filtered.size());
    m_page->setText(tr("%1–%2 of %3 daily buckets (up to 50 per page)").arg(page.isEmpty()?0:m_offset+1).arg(m_offset+page.size()).arg(filtered.size()));m_detail->clear();
}

void AnalyticsSettings::showDetail() {
    const auto *item=m_table->item(m_table->currentRow(),0);if(!item)return;
    const auto row=item->data(Qt::UserRole).toJsonObject();QString text=describeCounters(row.value("counters").toObject(),AnalyticsData::counters());
    const auto histograms=row.value("histograms").toObject();
    for(auto i=histograms.begin();i!=histograms.end();++i){const auto h=i.value().toObject();if(h.value("count").toDouble()>0)text+=tr("\n%1 histogram mean: %2 ms (%3 samples)").arg(i.key()).arg(h.value("sum").toDouble()/h.value("count").toDouble(),0,'f',1).arg(h.value("count").toInteger());}
    m_detail->setPlainText(text);
}

void AnalyticsSettings::configure() {
    if(m_busy||m_mutating||m_local.isEmpty()||!fresh(m_local))return;
    const auto epoch=m_local.value("source_epoch").toString();const auto enabled=m_enabled->isChecked();const auto retention=m_retention->value();m_mutating=true;controls();
    if(retention<m_local.value("collection").toObject().value("retention_days").toInt()) {
        QMessageBox confirm(QMessageBox::Warning,tr("Shorten local analytics history?"),tr("Keep only the latest %1 UTC days of agent analytics on this computer. Older daily aggregates will be deleted. Speech statistics and durable delivery receipts are unaffected.").arg(retention),QMessageBox::NoButton,this);
        confirm.setObjectName("confirmAnalyticsRetention");auto *apply=confirm.addButton(tr("Shorten retention"),QMessageBox::DestructiveRole);auto *cancel=confirm.addButton(QMessageBox::Cancel);confirm.setDefaultButton(cancel);confirm.setEscapeButton(cancel);confirm.exec();
        if(confirm.clickedButton()!=apply){m_mutating=false;controls();return;}
    }
    m_status->setText(tr("Applying local collection settings…"));
    m_client.configure(epoch,enabled,retention,[this](const QJsonObject &,const QString &error){
        m_mutating=false;m_local={};controls();
        if(!error.isEmpty()){m_status->setText(error);return;}refresh();
    });
}

void AnalyticsSettings::purge() {
    if(m_busy||m_mutating||m_local.isEmpty()||!fresh(m_local))return;
    // Fetch the latest epoch before presenting the destructive scope. The
    // backend also compares it atomically after the person confirms.
    m_mutating=true;controls();
    m_client.summary({},m_appliedFrom,m_appliedThrough,[this](const QJsonObject &reply,const QString &error){
        if(!error.isEmpty()){m_mutating=false;controls();m_status->setText(error);return;}
        if(!fresh(reply)||!m_epochs.accept(reply)){m_mutating=false;controls();m_status->setText(tr("Local analytics epoch is stale or ambiguous. Refresh before purging."));return;}
        QMessageBox confirm(QMessageBox::Warning,tr("Purge local agent analytics?"),tr("Permanently delete all retained agent analytics on this computer. Speech statistics, other computers and durable delivery receipts are unaffected. Collection keeps its current enabled/disabled setting."),QMessageBox::NoButton,this);
        confirm.setObjectName("confirmAnalyticsPurge");auto *remove=confirm.addButton(tr("Purge local history"),QMessageBox::DestructiveRole);auto *cancel=confirm.addButton(QMessageBox::Cancel);confirm.setDefaultButton(cancel);confirm.setEscapeButton(cancel);
        confirm.exec();
        if(confirm.clickedButton()!=remove){m_mutating=false;controls();return;}
        m_client.purge(reply.value("source_epoch").toString(),[this](const QJsonObject &,const QString &failure){
            m_mutating=false;m_local={};m_snapshots.clear();controls();
            if(!failure.isEmpty()){m_status->setText(failure);render();return;}refresh();
        });
    });
}

void AnalyticsSettings::exportDiagnostics() {
    if(m_busy||m_mutating)return;
    // Only this closed projection enters the preview. No raw reply, stderr,
    // configured machine alias, native UUID or speech profile ID is exported.
    const auto bytes=QJsonDocument(AnalyticsData::diagnostic(observations(),m_appliedFrom,m_appliedThrough)).toJson(QJsonDocument::Indented);
    QDialog review(this);review.setObjectName("analyticsDiagnosticsReview");review.setWindowTitle(tr("Review aggregate diagnostics"));review.resize(760,560);
    auto *layout=new QVBoxLayout(&review);auto *hint=new QLabel(tr("Review these aggregate fields before saving a local JSON file. Computer numbers are anonymous within this file. This export includes no logs, message content, account details or native identifiers, and is never uploaded automatically."));hint->setWordWrap(true);layout->addWidget(hint);
    auto *preview=new QPlainTextEdit(QString::fromUtf8(bytes));preview->setReadOnly(true);preview->setObjectName("analyticsDiagnosticsPreview");layout->addWidget(preview);
    auto *buttons=new QHBoxLayout;buttons->addStretch();auto *save=new QPushButton(tr("Save file…"));save->setObjectName("saveAnalyticsDiagnostics");auto *cancel=new QPushButton(tr("Cancel"));cancel->setDefault(true);buttons->addWidget(save);buttons->addWidget(cancel);layout->addLayout(buttons);
    connect(save,&QPushButton::clicked,&review,&QDialog::accept);connect(cancel,&QPushButton::clicked,&review,&QDialog::reject);
    if(review.exec()!=QDialog::Accepted)return;
    const auto path=QFileDialog::getSaveFileName(this,tr("Save aggregate diagnostics"),QStringLiteral("zerus-aggregate-diagnostics.json"),tr("JSON files (*.json)"));if(path.isEmpty())return;
    const QPointer<AnalyticsSettings> guard(this);
    QThreadPool::globalInstance()->start(QRunnable::create([guard,path,bytes]{
        QSaveFile file(path);bool ok=file.open(QIODevice::WriteOnly);if(ok)ok=file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner)&&file.write(bytes)==bytes.size()&&file.commit();
        if(guard)QMetaObject::invokeMethod(guard,[guard,ok]{if(guard)guard->m_status->setText(ok?tr("Aggregate diagnostics saved locally."):tr("Could not save aggregate diagnostics."));},Qt::QueuedConnection);
    }));
}
