#include "ArchiveBrowser.h"
#include "ArchiveDatePicker.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

ArchiveBrowser::ArchiveBrowser(HgsClient &client,QWidget *parent):QWidget(parent),m_client(client) {
    setObjectName("archiveBrowser");
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(6);
    auto *dates=new QHBoxLayout;dates->setSpacing(6);
    m_period=new ArchiveDatePicker;
    dates->addWidget(m_period,1);
    m_refresh=new QPushButton;m_refresh->setObjectName("archiveRefresh");
    m_refresh->setAccessibleName(tr("Refresh archive"));m_refresh->setToolTip(tr("Refresh archive"));
    dates->addWidget(m_refresh);layout->addLayout(dates);
    auto *navigation=new QHBoxLayout;navigation->setSpacing(6);
    m_status=new QLabel;m_status->setObjectName("archiveStatus");
    m_status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);navigation->addWidget(m_status,1);
    m_newer=new QPushButton;m_newer->setObjectName("archiveNewer");
    m_more=new QPushButton;m_more->setObjectName("archiveMore");
    for(auto *button:{m_newer,m_more}){button->setAccessibleName(button==m_more?tr("Older sessions"):tr("Newer sessions"));button->setToolTip(button->accessibleName());navigation->addWidget(button);}
    layout->addLayout(navigation);
    m_error=new QLabel;m_error->setObjectName("archiveError");m_error->setWordWrap(true);layout->addWidget(m_error);
    connect(m_period,&ArchiveDatePicker::rangeChanged,this,&ArchiveBrowser::refresh);
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
    setTheme(true);hide();
}
QJsonObject ArchiveBrowser::query(const QString &cursor)const {
    const auto [from,until]=m_period->range();
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
    bool busy=false,more=false,newer=false;QStringList errors,windows;QString singleWindow;
    for(auto i=m_pages.begin();i!=m_pages.end();++i){busy|=i->request!=0;more|=!i->loaded||!i->cursor.isEmpty();newer|=!i->previous.isEmpty();if(!i->error.isEmpty())errors.append(m_hosts.value(i.key())+": "+i->error);
        if(i->loaded){singleWindow=i->position>=0&&!i->rows.isEmpty()?tr("%1–%2 of %3").arg(i->position+1).arg(i->position+i->rows.size()).arg(i->total):tr("%1 shown of %2").arg(i->rows.size()).arg(i->total);windows.append(m_hosts.value(i.key())+": "+singleWindow);}}
    const bool valid=query({})["from"].toDouble()<query({})["until"].toDouble();
    int shown=0;for(const auto &page:m_pages)shown+=page.rows.size();
    const auto summary=busy?tr("Loading archive…"):m_hosts.isEmpty()?tr("Loaded sessions"):m_hosts.size()==1&&!singleWindow.isEmpty()?singleWindow:totalsKnown()?tr("%1 loaded of %2").arg(shown).arg(total()):tr("%1 loaded").arg(shown);
    m_status->setText(summary);m_status->setToolTip(windows.join('\n'));m_status->setAccessibleDescription(windows.join('\n'));
    m_error->setText(errors.join('\n').left(240));m_error->setToolTip(errors.join('\n'));m_error->setVisible(!errors.isEmpty());
    m_more->setVisible(!m_hosts.isEmpty()&&(more||busy));m_more->setEnabled(valid&&!busy);m_newer->setVisible(newer);m_newer->setEnabled(valid&&!busy);m_refresh->setEnabled(!busy);
}
void ArchiveBrowser::resizeEvent(QResizeEvent *event){QWidget::resizeEvent(event);m_more->setText(width()<310?QString():tr("Older"));m_newer->setText(width()<310?QString():tr("Newer"));}
void ArchiveBrowser::setTheme(bool dark){
    m_period->setTheme(dark);const QColor ink(dark?"#a2adbc":"#627082");
    m_refresh->setIcon(workspaceIcon("refresh",ink));m_more->setIcon(workspaceIcon("chevron-right",ink));m_newer->setIcon(workspaceIcon("chevron-left",ink));
    m_refresh->setFixedSize(m_period->sizeHint().height(),m_period->sizeHint().height());
    setWorkspaceStyle(this,QString("QWidget#archiveBrowser QPushButton{background:transparent;color:%1;border:1px solid %2;border-radius:7px;padding:5px 7px;min-height:20px;} QWidget#archiveBrowser QPushButton:hover{background:%3;} QWidget#archiveBrowser QPushButton:focus{border-color:%4;} QLabel#archiveStatus{color:%1;} QLabel#archiveError{color:%5;}").arg(ink.name(),dark?"#333c47":"#dce2e8",dark?"#2b3540":"#edf2f5",dark?"#8bdfc0":"#167357",dark?"#ff9ca8":"#b52d48"));
}
QList<SessionInfo> ArchiveBrowser::sessions(const QString&host)const{return m_pages.value(host).rows;}
int ArchiveBrowser::total()const{int n=0;for(const auto&p:m_pages)n+=p.total;return n;}

bool ArchiveBrowser::totalsKnown()const{if(m_hosts.isEmpty())return false;for(auto h=m_hosts.begin();h!=m_hosts.end();++h)if(!m_pages.value(h.key()).loaded)return false;return true;}
bool ArchiveBrowser::hasMore()const{for(auto h=m_hosts.begin();h!=m_hosts.end();++h){const auto p=m_pages.value(h.key());if(!p.loaded||!p.cursor.isEmpty()||!p.previous.isEmpty()||p.rows.size()<p.total||!p.error.isEmpty())return true;}return false;}
