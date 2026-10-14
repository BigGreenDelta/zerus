#pragma once
#include "FleetState.h"
#include "MachinePlatform.h"
#include "WorktreeCleanupDialog.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QMenu>
#include <QDir>
#include <QJsonDocument>
#include <QScrollBar>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

// Shared, on-demand folder/catalog view. Responses never change its address.
class WorktreePanel : public QWidget {
public:
    enum class Layout { Standard, Compact };
    WorktreePanel(HgsClient *client,bool picker=false,QWidget *parent=nullptr,Layout mode=Layout::Standard):QWidget(parent),m_client(client),m_picker(picker),m_compact(mode==Layout::Compact&&!picker) {
        setObjectName("worktreePanel");auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,6,0,0);layout->setSpacing(8);
        auto *row=new QHBoxLayout;auto *title=new QLabel(tr("Folder and worktrees"));title->setObjectName("heading");row->addWidget(title);row->addStretch();
        m_refresh=new QPushButton(tr("Refresh"));m_refresh->setObjectName("refreshWorktrees");m_refresh->setAutoDefault(false);row->addWidget(m_refresh);layout->addLayout(row);
        m_status=new QLabel;m_status->setObjectName("worktreeStatus");m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);layout->addWidget(m_status);
        m_tree=new QTreeWidget;m_tree->setObjectName("worktreeCatalog");m_tree->setHeaderHidden(true);m_tree->setColumnCount(1);m_tree->setMinimumHeight(m_compact?60:120);m_tree->setTextElideMode(Qt::ElideMiddle);layout->addWidget(m_tree,1);
        auto *actions=new QHBoxLayout;
        m_all=new QPushButton(tr("All sessions"));m_all->setObjectName("allWorktreeSessions");m_all->setAutoDefault(false);m_all->setVisible(!picker);
        m_launch=new QPushButton(picker?tr("Use folder"):tr("New session…"));m_launch->setObjectName("useWorktreeFolder");m_launch->setAutoDefault(false);
        m_cleanup=new QPushButton(tr("Clean up…"));m_cleanup->setObjectName("cleanUpWorktrees");m_cleanup->setAutoDefault(false);m_cleanup->setVisible(!picker);
        if(m_compact){row->insertWidget(row->count()-1,m_all);row->insertWidget(row->count()-1,m_cleanup);row->addWidget(m_launch);}
        else{actions->addWidget(m_all);actions->addWidget(m_cleanup);actions->addStretch();actions->addWidget(m_launch);}
        if(picker){auto *cancel=new QPushButton(tr("Cancel"));cancel->setAutoDefault(false);actions->addWidget(cancel);connect(cancel,&QPushButton::clicked,this,[this]{if(cancelRequested)cancelRequested();});}
        if(m_compact)delete actions;else layout->addLayout(actions);
        connect(m_refresh,&QPushButton::clicked,this,[this]{request(true);});
        connect(m_all,&QPushButton::clicked,this,[this]{if(filterRequested)filterRequested({},false);});
        connect(m_launch,&QPushButton::clicked,this,[this]{auto *item=rootItem();if(item&&m_launch->isEnabled()&&launchRequested)launchRequested(item->data(0,Qt::UserRole).toString());});
        connect(m_tree,&QTreeWidget::itemSelectionChanged,this,[this]{updateActions();});
        connect(m_cleanup,&QPushButton::clicked,this,[this]{cleanUp({});});
        if(!picker){m_tree->setContextMenuPolicy(Qt::CustomContextMenu);connect(m_tree,&QTreeWidget::customContextMenuRequested,this,[this](const QPoint &position){showMenu(position);});}
        connect(m_tree,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem *item,int){
            if(m_picker)return;
            if(item->parent()){if(sessionRequested)sessionRequested(item->data(0,Qt::UserRole).toString(),item->data(0,Qt::UserRole+1).toString());}
            else if(filterRequested)filterRequested(item->data(0,Qt::UserRole).toString(),item->data(0,Qt::UserRole+2).toBool());
        });
        connect(m_client,&HgsClient::worktreesReady,this,[this](quint64 id,const QString &host,const QString &path,const QJsonObject &data){
            if(id!=m_request||host!=m_host||path!=m_path)return;m_request=0;m_data=data;m_error.clear();render();
        });
        connect(m_client,&HgsClient::worktreesFailed,this,[this](quint64 id,const QString &host,const QString &path,const QString &error){
            if(id!=m_request||host!=m_host||path!=m_path)return;m_request=0;m_error=error;render();
        });
        connect(m_client,&HgsClient::worktreeReviewReady,this,[this](quint64 id){if(id!=m_review)return;m_review=0;updateCleanup();});
        connect(m_client,&HgsClient::worktreeReviewFailed,this,[this](quint64 id){if(id!=m_review)return;m_review=0;updateCleanup();});
    }
    std::function<void()> cancelRequested;
    std::function<void(const QString &,bool)> filterRequested;
    std::function<void(const QString &,const QString &)> sessionRequested;
    std::function<void(const QString &)> launchRequested;
    void setContext(const QString &host,const QString &path,const FleetState &fleet) {
        const bool changed=host!=m_host||path!=m_path;const bool wasOnline=online();m_fleet=fleet;
        if(changed){m_host=host;m_path=path;m_request=0;m_error.clear();m_data=m_client->worktreeSnapshot(host,path);m_requested=false;m_rendered.clear();}
        render();if(isVisible()&&(changed||(!wasOnline&&online())||!m_requested))request();
    }
    QString selectedPath() const {auto *item=rootItem();return item?item->data(0,Qt::UserRole).toString():QString();}
    void refreshCatalog() { request(true); }
protected:
    void showEvent(QShowEvent *event) override {QWidget::showEvent(event);request();}
private:
    const BoxState *box() const {return m_host.isEmpty()?&m_fleet.local():m_fleet.peer(m_host);}
    bool online() const {const auto *b=box();return b&&b->ok&&(m_host.isEmpty()||QDateTime::currentMSecsSinceEpoch()-m_fleet.peerPolledAt(m_host)<FleetState::kPeerStaleMs);}
    QTreeWidgetItem *rootItem() const {auto *item=m_tree->currentItem();return item&&item->parent()?item->parent():item;}
    QString commonDir() const {return m_data["common_dir"].toString();}
    bool hasLinked() const {for(const auto &value:m_data["worktrees"].toArray())if(value.toObject()["kind"]=="linked")return true;return false;}
    // Cleanup needs a current catalog: verdicts and removals are always computed
    // again on the owning machine, but the panel must not offer stale rows.
    bool cleanupAvailable() const {return online()&&!m_request&&m_error.isEmpty()&&m_data["state"]=="ok"&&!m_data["stale"].toBool()&&!commonDir().isEmpty()&&hasLinked();}
    void cleanUp(const QString &focus){
        if(!cleanupAvailable())return;
        WorktreeCleanupDialog dialog(m_client,m_host,m_host.isEmpty()?m_fleet.local().host:m_host,commonDir(),focus,window());dialog.exec();
        if(dialog.changed()){m_reviewedAt.remove(m_host+'\n'+commonDir());request(true);}else updateCleanup();
    }
    void showMenu(const QPoint &position){
        auto *item=m_tree->itemAt(position);if(item&&item->parent())item=item->parent();if(!item)return;m_tree->setCurrentItem(item);
        const auto path=item->data(0,Qt::UserRole).toString();QJsonObject row;
        for(const auto &value:m_data["worktrees"].toArray())if(value.toObject()["path"]==path)row=value.toObject();
        auto *menu=new QMenu(this);menu->setObjectName("worktreeMenu");menu->setToolTipsVisible(true);
        if(row["kind"]=="linked"){
            auto *remove=menu->addAction(row["available"].toBool()?tr("Remove worktree…"):tr("Forget missing worktree…"));remove->setObjectName("removeWorktreeAction");
            remove->setEnabled(cleanupAvailable());remove->setToolTip(tr("Review this worktree on its computer before anything is removed."));
            connect(remove,&QAction::triggered,this,[this,path]{cleanUp(path);});menu->addSeparator();
        }
        auto *copy=menu->addAction(tr("Copy path"));copy->setObjectName("copyWorktreePath");connect(copy,&QAction::triggered,this,[path]{QApplication::clipboard()->setText(path);});
        connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);menu->popup(m_tree->viewport()->mapToGlobal(position));
    }
    // The reminder only counts; it never removes. One review per repository and
    // machine every five minutes while the panel is visible.
    void updateCleanup(){
        if(m_picker)return;
        const bool available=cleanupAvailable();const auto review=available?m_client->worktreeReview(m_host,commonDir()):QJsonObject();int ready=0;
        for(const auto &value:review["worktrees"].toArray()){const auto verdict=value.toObject()["verdict"].toString();ready+=verdict=="ready"||verdict=="missing";}
        m_cleanup->setEnabled(available);m_cleanup->setText(ready?tr("Clean up (%1)").arg(ready):tr("Clean up…"));
        m_cleanup->setToolTip(!available?(m_data["state"]!="ok"?tr("Cleanup is available for Git repositories with linked worktrees."):hasLinked()?tr("Waiting for a current worktree catalog."):tr("This repository has no linked worktrees.")):
            ready==1?tr("1 worktree can be removed without losing work. Review it first."):ready?tr("%1 worktrees can be removed without losing work. Review them first.").arg(ready):tr("Review linked worktrees and remove the ones you no longer need."));
        const auto key=m_host+'\n'+commonDir();const auto now=QDateTime::currentSecsSinceEpoch();
        if(available&&review.isEmpty()&&isVisible()&&!m_review&&now-m_reviewedAt.value(key,0)>=300){m_reviewedAt.insert(key,now);m_review=m_client->reviewWorktrees(m_host,commonDir());}
    }
    void request(bool force=false){
        if(m_path.isEmpty()||!online()||m_request)return;
        m_requested=true;m_error.clear();m_request=m_client->requestWorktrees(m_host,m_path,force);render();
    }
    void updateActions(){
        const auto *item=rootItem();m_refresh->setEnabled(online()&&!m_request&&!m_path.isEmpty());
        m_launch->setEnabled(item&&item->data(0,Qt::UserRole+1).toBool()&&online()&&!m_request&&m_error.isEmpty()&&!m_data.value("stale").toBool());
    }
    void render(){
        const auto state=m_data.value("state").toString();const bool stale=!online()||m_data.value("stale").toBool()||!m_error.isEmpty();
        QString status;
        if(m_path.isEmpty())status=tr("Select a folder.");
        else if(!online())status=tr("Offline. Showing the last snapshot.");
        else if(!m_error.isEmpty())status=tr("Catalog unavailable: %1").arg(m_error);
        else if(m_request)status=tr("Reading folder…");
        else if(state=="not_repo")status=tr("Ordinary folder (no Git).");
        else if(state!="ok")status=state=="folder_unavailable"?tr("Folder is missing or inaccessible."):tr("Git information unavailable. You can still use an accessible folder.");
        else if(m_data.value("partial").toBool())status=tr("Partial catalog (first 512 worktrees).");
        else status=tr("Existing working folders on %1").arg(m_host.isEmpty()?m_fleet.local().host:m_host);
        if(m_data["sampled_at"].toDouble()>0)status+=(m_compact?tr(" (%1%2)"):tr("\nSnapshot: %1%2")).arg(QDateTime::fromSecsSinceEpoch(qint64(m_data["sampled_at"].toDouble())).toLocalTime().toString("d MMM HH:mm:ss"),stale?(m_compact?tr(", outdated"):tr(" (outdated)")):QString());
        m_status->setText(status);m_status->setToolTip(status);
        QJsonArray roots=m_data["worktrees"].toArray();
        if(roots.isEmpty()&&!m_path.isEmpty())roots.append(QJsonObject{{"path",m_data["path"].toString(m_path)},{"kind","folder"},{"available",state!="folder_unavailable"&&!m_data.isEmpty()}});
        QJsonArray rows;
        for(const auto &value:roots){auto row=value.toObject();QJsonArray sessions;const auto path=row["path"].toString();const bool checkout=row["kind"]!="folder";
            if(const auto *b=box())for(const auto &s:b->sessions){const auto confirmed=checkout?s.gitRoot:s.canonicalCwd;if(!MachinePlatform::same(confirmed,path,MachinePlatform::of(b->metrics,m_host.isEmpty())))continue;
                sessions.append(QJsonObject{{"name",s.name},{"archive",s.archiveId},{"state",s.state}});
            }
            row["sessions"]=sessions;rows.append(row);
        }
        const QByteArray fingerprint=QJsonDocument(rows).toJson(QJsonDocument::Compact)+QByteArray::number(stale);
        if(fingerprint!=m_rendered){
            m_rendered=fingerprint;const auto selected=selectedPath();QSet<QString> expanded;for(int i=0;i<m_tree->topLevelItemCount();++i){auto *item=m_tree->topLevelItem(i);if(item->isExpanded())expanded.insert(item->data(0,Qt::UserRole).toString());}
            const int scroll=m_tree->verticalScrollBar()->value();const QSignalBlocker blocker(m_tree);m_tree->clear();
            for(const auto &value:rows){const auto row=value.toObject();const auto path=row["path"].toString();const auto kind=row["kind"].toString();const auto sessions=row["sessions"].toArray();
                QString label=kind=="main"?tr("Main checkout"):kind=="linked"?tr("Linked worktree"):kind=="bare"?tr("Bare repository"):tr("Folder");
                const auto branch=row["branch"].toString();if(!branch.isEmpty())label+=" / "+branch;else if(row["detached"].toBool())label+=tr(" / detached %1").arg(row["head"].toString().left(8));
                if(row["locked"].toBool())label+=tr(" (locked)");if(row["prunable"].toBool())label+=tr(" (prunable)");
                if(!row["available"].toBool())label+=tr(" (unavailable)");label+=(sessions.size()==1?tr(" (1 session)"):tr(" (%1 sessions)").arg(sessions.size()));
                auto *item=new QTreeWidgetItem(m_tree,{label+'\n'+path});item->setData(0,Qt::UserRole,path);item->setData(0,Qt::UserRole+1,row["available"].toBool()&&kind!="bare"&&!stale);item->setData(0,Qt::UserRole+2,kind!="folder");
                item->setToolTip(0,path+'\n'+row["locked_reason"].toString()+'\n'+row["prunable_reason"].toString());
                if(!m_picker)for(const auto &s:sessions){const auto session=s.toObject();auto *child=new QTreeWidgetItem(item,{session["name"].toString()+" ("+(stale?tr("last recorded "):QString())+session["state"].toString()+')'});child->setData(0,Qt::UserRole,session["name"].toString());child->setData(0,Qt::UserRole+1,session["archive"].toString());}
                item->setExpanded(expanded.contains(path));
                if(path==selected|| (selected.isEmpty()&&(path==m_path||path==m_data["selected_root"].toString())))m_tree->setCurrentItem(item);
            }
            if(!m_tree->currentItem()&&m_tree->topLevelItemCount())m_tree->setCurrentItem(m_tree->topLevelItem(0));m_tree->verticalScrollBar()->setValue(scroll);
        }
        updateActions();updateCleanup();
    }
    HgsClient *m_client;bool m_picker=false,m_compact=false,m_requested=false;FleetState m_fleet;QString m_host,m_path,m_error;QJsonObject m_data;QByteArray m_rendered;quint64 m_request=0;
    QLabel *m_status;QTreeWidget *m_tree;QPushButton *m_refresh,*m_launch,*m_all,*m_cleanup;quint64 m_review=0;QHash<QString,qint64> m_reviewedAt;
};
