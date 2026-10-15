#include "ArchiveBrowser.h"
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

ArchiveBrowser::ArchiveBrowser(HgsClient &client,QWidget *parent):QWidget(parent),m_client(client) {
    setObjectName("archiveBrowser");
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
    auto *dates=new QHBoxLayout;dates->setSpacing(6);
    m_period=new QComboBox;m_period->setObjectName("archivePeriod");m_period->setAccessibleName(tr("Archived date"));
    m_period->addItems({tr("All dates"),tr("Today"),tr("Last 7 days"),tr("Last 30 days"),tr("Date range…")});
    dates->addWidget(m_period,1);m_refresh=new QPushButton(tr("Refresh"));m_refresh->setObjectName("archiveRefresh");dates->addWidget(m_refresh);layout->addLayout(dates);
    auto *range=new QWidget;auto *rangeLayout=new QVBoxLayout(range);rangeLayout->setContentsMargins(0,0,0,0);rangeLayout->setSpacing(6);
    m_from=new QDateEdit(QDate::currentDate().addDays(-29));m_until=new QDateEdit(QDate::currentDate());
    for(auto *date:{m_from,m_until}){date->setMinimumDate(QDate(1970,1,2));date->setMaximumDate(QDate(9999,12,30));date->setCalendarPopup(true);date->setDisplayFormat("dd MMM yyyy");auto *row=new QHBoxLayout;row->addWidget(new QLabel(date==m_from?tr("From"):tr("Through")));row->addWidget(date,1);rangeLayout->addLayout(row);}
    m_from->setObjectName("archiveFrom");m_from->setAccessibleName(tr("Archived from, inclusive"));m_until->setObjectName("archiveUntil");m_until->setAccessibleName(tr("Archived through, inclusive"));range->hide();layout->addWidget(range);
    m_status=new QLabel;m_status->setObjectName("archiveStatus");m_status->setWordWrap(true);layout->addWidget(m_status);
    m_more=new QPushButton(tr("Load more"));m_more->setObjectName("archiveMore");layout->addWidget(m_more);
    m_newer=new QPushButton(tr("Newer sessions"));m_newer->setObjectName("archiveNewer");layout->addWidget(m_newer);
    connect(m_period,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,range](int i){range->setVisible(i==4);refresh();});
    connect(m_from,&QDateEdit::dateChanged,this,[this]{if(m_period->currentIndex()==4)refresh();});
    connect(m_until,&QDateEdit::dateChanged,this,[this]{if(m_period->currentIndex()==4)refresh();});
    connect(m_refresh,&QPushButton::clicked,this,&ArchiveBrowser::refresh);
    connect(m_more,&QPushButton::clicked,this,&ArchiveBrowser::loadMore);
    connect(m_newer,&QPushButton::clicked,this,[this]{navigate(true);});
    connect(&m_client,&HgsClient::archivePageReady,this,[this](quint64 id,const QString &host,const QJsonObject &data,const QString &error){
        auto i=m_pages.find(host);if(i==m_pages.end()||i->request!=id)return;
        i->request=0;i->error=error;
        if(error.isEmpty()){
            const auto rows=HgsClient::parseSessions(data["sessions"].toArray());
            if(rows.isEmpty()&&!i->rows.isEmpty())i->error=tr("Archive changed. Refresh to start a new view.");
            else {i->loaded=true;i->cursor=data["next_cursor"].toString();i->previous=data["previous_cursor"].toString();i->total=data["total"].toInt();i->position=data.contains("position")?data["position"].toInt():-1;i->rows=rows;}
        }
        const bool accepted=i->error.isEmpty();updateStatus();emit changed();if(accepted)emit windowChanged();
    });
    m_debounce=new QTimer(this);m_debounce->setSingleShot(true);m_debounce->setInterval(300);connect(m_debounce,&QTimer::timeout,this,&ArchiveBrowser::refresh);
    hide();
}
QJsonObject ArchiveBrowser::query(const QString &cursor)const {
    qint64 from=0,until=253402300800LL;const auto today=QDate::currentDate();const int p=m_period->currentIndex();
    QDate first,last;
    if(p>0&&p<4){first=today.addDays(p==1?0:p==2?-6:-29);last=today;}
    if(p==4){first=m_from->date();last=m_until->date();}
    if(first.isValid()){from=first.startOfDay().toSecsSinceEpoch();until=last.addDays(1).startOfDay().toSecsSinceEpoch();}
    return {{"from",double(from)},{"until",double(until)},{"limit",50},{"cursor",cursor},{"query",m_query}};
}
bool ArchiveBrowser::includes(double at)const{const auto q=query({});return at>=q["from"].toDouble()&&at<q["until"].toDouble();}
void ArchiveBrowser::configure(const QMap<QString,QString>&hosts,bool active){
    setVisible(active);const bool different=hosts!=m_hosts;
    if(!active&&m_active){m_client.cancelArchivePages();for(auto &page:m_pages)page.request=0;}
    m_active=active;
    if(different){m_client.cancelArchivePages();m_hosts=hosts;m_pages.clear();}
    bool initial=false;for(auto h=m_hosts.begin();h!=m_hosts.end();++h){const auto p=m_pages.value(h.key());initial|=!p.loaded&&!p.request&&p.error.isEmpty();}
    updateStatus();
    if(active&&!m_debounce->isActive()&&initial)QTimer::singleShot(0,this,[this]{if(m_active)loadMore();});
}
void ArchiveBrowser::setQuery(const QString &query){const auto text=query.trimmed();if(text==m_query)return;m_query=text;m_client.cancelArchivePages();m_pages.clear();m_debounce->start();updateStatus();}
void ArchiveBrowser::refresh(){m_debounce->stop();m_client.cancelArchivePages();m_pages.clear();if(m_active)loadMore();updateStatus();emit changed();}
void ArchiveBrowser::loadMore(){navigate(false);}
void ArchiveBrowser::navigate(bool newer){
    if(!m_active)return;
    if(query({})["from"].toDouble()>=query({})["until"].toDouble()){updateStatus();return;}
    for(auto h=m_hosts.begin();h!=m_hosts.end();++h){auto &p=m_pages[h.key()];const auto cursor=newer?p.previous:p.cursor;if(p.request||(newer&&cursor.isEmpty())||(!newer&&p.loaded&&cursor.isEmpty()))continue;p.error.clear();p.newer=newer;p.request=m_client.requestArchivePage(h.key(),query(cursor));}
    updateStatus();
}
void ArchiveBrowser::updateStatus(){
    bool busy=false,more=false,newer=false;QStringList errors,windows;
    for(auto i=m_pages.begin();i!=m_pages.end();++i){busy|=i->request!=0;more|=!i->loaded||!i->cursor.isEmpty();newer|=!i->previous.isEmpty();if(!i->error.isEmpty())errors.append(m_hosts.value(i.key())+": "+i->error);
        if(i->loaded)windows.append(m_hosts.value(i.key())+": "+(i->position>=0&&!i->rows.isEmpty()?tr("%1–%2 of %3").arg(i->position+1).arg(i->position+i->rows.size()).arg(i->total):tr("%1 shown of %2").arg(i->rows.size()).arg(i->total)));}
    const bool valid=query({})["from"].toDouble()<query({})["until"].toDouble();
    m_status->setText(!valid?tr("Choose an end date on or after the start date."):!errors.isEmpty()?errors.join('\n').left(240):busy?tr("Loading archive…"):m_hosts.isEmpty()?tr("Archived date · loaded sessions"):windows.join('\n'));
    m_status->setToolTip(errors.join('\n'));
    m_more->setVisible(!m_hosts.isEmpty()&&(more||busy));m_more->setEnabled(valid&&!busy);m_more->setText(tr("Older sessions"));m_newer->setVisible(newer);m_newer->setEnabled(valid&&!busy);m_refresh->setEnabled(!busy);
}
QList<SessionInfo> ArchiveBrowser::sessions(const QString&host)const{return m_pages.value(host).rows;}
int ArchiveBrowser::total()const{int n=0;for(const auto&p:m_pages)n+=p.total;return n;}

bool ArchiveBrowser::totalsKnown()const{if(m_hosts.isEmpty())return false;for(auto h=m_hosts.begin();h!=m_hosts.end();++h)if(!m_pages.value(h.key()).loaded)return false;return true;}
bool ArchiveBrowser::hasMore()const{for(auto h=m_hosts.begin();h!=m_hosts.end();++h){const auto p=m_pages.value(h.key());if(!p.loaded||!p.cursor.isEmpty()||!p.previous.isEmpty()||p.rows.size()<p.total||!p.error.isEmpty())return true;}return false;}
