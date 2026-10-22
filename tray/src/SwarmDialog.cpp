#include "SwarmDialog.h"
#include "SwarmController.h"
#include "SwarmConflictReview.h"
#include "FleetState.h"
#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
class CatalogDialog : public QDialog {
public:
    using QDialog::QDialog;
    bool changing = false;
    void reject() override { if(!changing)QDialog::reject(); }
protected:
    void closeEvent(QCloseEvent *event) override { if(changing)event->ignore();else QDialog::closeEvent(event); }
};
}

void showSwarmDialog(HgsClient *client,SwarmController *controller,const FleetState &fleet,QWidget *parent,const std::function<void()> &manageConnections, bool connectComputer, const QString &computer)
{
    CatalogDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Project swarm"));dialog.setObjectName("swarmDialog");dialog.resize(780,680);
    auto *layout=new QVBoxLayout(&dialog);layout->setContentsMargins(24,22,24,20);layout->setSpacing(12);
    auto *hint=new QLabel(QObject::tr("Projects are shared between connected computers. SSH settings and credentials stay on this computer. Joining through one member brings the whole project catalog."));
    hint->setWordWrap(true);layout->addWidget(hint);

    // Each task owns the same resizable area. A preview never inserts controls
    // into the machine list or pushes conflict review farther down the window.
    auto *tabs=new QTabWidget;tabs->setObjectName("swarmTabs");layout->addWidget(tabs,1);
    auto *computersPage=new QWidget;auto *computersLayout=new QVBoxLayout(computersPage);
    auto *filterRow=new QHBoxLayout;
    auto *filter=new QLineEdit;filter->setObjectName("swarmMachineFilter");filter->setPlaceholderText(QObject::tr("Find a computer or connection…"));filter->setClearButtonEnabled(true);
    auto *count=new QLabel;count->setObjectName("swarmMachineCount");filterRow->addWidget(filter,1);filterRow->addWidget(count);computersLayout->addLayout(filterRow);
    auto *machines=new QTableWidget;machines->setObjectName("swarmMachines");machines->setColumnCount(3);
    machines->setHorizontalHeaderLabels({QObject::tr("Computer"),QObject::tr("Local access"),QObject::tr("Sync connection")});
    machines->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    machines->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);machines->horizontalHeader()->setStretchLastSection(true);
    machines->setColumnWidth(0,210);machines->setColumnWidth(1,170);
    machines->verticalHeader()->hide();machines->verticalHeader()->setDefaultSectionSize(34);
    machines->setEditTriggers(QAbstractItemView::NoEditTriggers);machines->setSelectionBehavior(QAbstractItemView::SelectRows);machines->setSelectionMode(QAbstractItemView::SingleSelection);
    machines->setWordWrap(false);machines->setShowGrid(false);machines->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    machines->setSortingEnabled(true);machines->sortItems(0,Qt::AscendingOrder);computersLayout->addWidget(machines,1);
    auto *connectionRow=new QHBoxLayout;
    auto *manage=new QPushButton(QObject::tr("Manage connections…"));connectionRow->addWidget(manage);connectionRow->addStretch();
    auto *sync=new QPushButton(QObject::tr("Sync now"));sync->setObjectName("swarmSync");connectionRow->addWidget(sync);computersLayout->addLayout(connectionRow);
    tabs->addTab(computersPage,QObject::tr("Computers"));

    auto *connectPage=new QWidget;auto *connectLayout=new QVBoxLayout(connectPage);
    auto *joinRow=new QHBoxLayout;auto *peer=new QComboBox;peer->setObjectName("swarmPeer");
    peer->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);peer->setMinimumContentsLength(16);
    for(const auto &alias:fleet.peerNames())peer->addItem(alias);
    auto *preview=new QPushButton(QObject::tr("Preview connection"));preview->setObjectName("swarmPreview");joinRow->addWidget(peer,1);joinRow->addWidget(preview);connectLayout->addLayout(joinRow);
    auto *previewStack=new QStackedWidget;previewStack->setObjectName("swarmConnectionPreview");connectLayout->addWidget(previewStack,1);
    auto message=[](const QString &text){auto *label=new QLabel(text);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);label->setAlignment(Qt::AlignCenter);label->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);return label;};
    auto *previewEmpty=message(peer->count()?QObject::tr("Choose a computer and preview its project catalog before connecting."):QObject::tr("Add a computer in Manage connections first."));previewStack->addWidget(previewEmpty);
    auto *sameCatalog=message(QString());previewStack->addWidget(sameCatalog);
    auto *mappingPage=new QWidget;auto *mappingLayout=new QVBoxLayout(mappingPage);mappingLayout->setContentsMargins(0,0,0,0);
    auto *mappingHint=new QLabel(QObject::tr("Choose which projects are the same. Projects stay separate unless you match them below."));mappingHint->setWordWrap(true);mappingLayout->addWidget(mappingHint);
    auto *mapping=new QTableWidget;mapping->setObjectName("swarmProjectMapping");mapping->setColumnCount(2);
    mapping->setHorizontalHeaderLabels({QObject::tr("Project on this computer"),QObject::tr("Project in the other catalog")});mapping->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);mapping->verticalHeader()->hide();mapping->verticalHeader()->setDefaultSectionSize(38);mapping->setEditTriggers(QAbstractItemView::NoEditTriggers);mappingLayout->addWidget(mapping,1);previewStack->addWidget(mappingPage);
    auto *keepPlacement=new QCheckBox(QObject::tr("Keep swarm placement for sessions already in its catalog"));keepPlacement->setObjectName("swarmKeepPlacement");keepPlacement->setChecked(true);
    auto placementPolicy=keepPlacement->sizePolicy();placementPolicy.setRetainSizeWhenHidden(true);keepPlacement->setSizePolicy(placementPolicy);keepPlacement->hide();connectLayout->addWidget(keepPlacement);
    auto *join=new QPushButton(QObject::tr("Connect this member"));join->setObjectName("swarmJoin");connectLayout->addWidget(join,0,Qt::AlignRight);
    tabs->addTab(connectPage,QObject::tr("Connect computer"));
    if (connectComputer) {
        tabs->setCurrentWidget(connectPage);
        const int index = peer->findText(computer); if(index>=0) peer->setCurrentIndex(index);
    }

    auto *conflictStack=new QStackedWidget;conflictStack->setObjectName("swarmConflictReview");
    auto *noConflicts=message(QObject::tr("No conflicting changes.\nChanges from connected computers are merged automatically."));conflictStack->addWidget(noConflicts);
    auto *conflictPage=new QWidget;auto *conflictLayout=new QVBoxLayout(conflictPage);
    auto *conflictTitle=new QLabel(QObject::tr("These changes need your decision. Review one item at a time; each choice is shared with the swarm."));conflictTitle->setWordWrap(true);conflictLayout->addWidget(conflictTitle);
    auto *conflictSplit=new QSplitter;conflictSplit->setObjectName("swarmConflictSplit");conflictSplit->setHandleWidth(14);conflictLayout->addWidget(conflictSplit,1);
    auto *conflicts=new QListWidget;conflicts->setObjectName("swarmConflicts");conflicts->setWordWrap(true);conflicts->setResizeMode(QListView::Adjust);conflicts->setMinimumWidth(190);conflicts->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);conflicts->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);conflictSplit->addWidget(conflicts);
    conflicts->setItemDelegate(SwarmConflictReview::createDelegate(conflicts));conflicts->setUniformItemSizes(false);
    auto *review=new SwarmConflictReview(fleet);review->setObjectName("swarmConflictDetails");conflictSplit->addWidget(review);conflictSplit->setChildrenCollapsible(false);conflictSplit->setStretchFactor(1,1);conflictSplit->setSizes({210,480});
    conflictStack->addWidget(conflictPage);
    const int conflictTab=tabs->addTab(conflictStack,QObject::tr("Conflicts (0)"));

    auto *status=new QLabel;status->setWordWrap(true);status->setTextFormat(Qt::PlainText);status->setObjectName("swarmStatus");
    status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);status->setFixedHeight(status->fontMetrics().lineSpacing()*2+4);layout->addWidget(status);
    auto setStatus=[&](const QString &text){status->setText(text);status->setToolTip(text);};
    setStatus(QObject::tr("This computer: %1").arg(fleet.local().host));
    hint->setToolTip(QObject::tr("Swarm ID: %1").arg(controller->snapshot().value("swarm_id").toString()));
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(buttons);QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    auto filterMachines=[&]{
        const auto query=filter->text().trimmed();int visible=0;
        for(int row=0;row<machines->rowCount();++row){
            bool match=query.isEmpty();for(int col=0;col<machines->columnCount();++col)match=match||machines->item(row,col)->text().contains(query,Qt::CaseInsensitive);
            machines->setRowHidden(row,!match);if(match)++visible;
        }
        count->setText(query.isEmpty()?QObject::tr("%1 computers").arg(machines->rowCount()):QObject::tr("%1 of %2 computers").arg(visible).arg(machines->rowCount()));
    };
    QObject::connect(filter,&QLineEdit::textChanged,&dialog,filterMachines);
    quint64 request=0;QString action,previewPeer;QJsonObject previewData,conflictSnapshot;QJsonArray conflictData;
    auto render=[&](const QJsonObject &snapshot){
        const auto selected=machines->currentRow()>=0?machines->item(machines->currentRow(),0)->data(Qt::UserRole).toString():QString();
        const int scroll=machines->verticalScrollBar()->value();
        machines->setUpdatesEnabled(false);machines->setSortingEnabled(false);
        // Update existing rows by identity, including after user sorting. Keep
        // focus and scroll position when the five-second local snapshot arrives.
        const auto entries=snapshot.value("machines").toArray();QSet<QString> ids;
        for(const auto &entry:entries)ids.insert(entry.toObject().value("id").toString());
        for(int row=machines->rowCount()-1;row>=0;--row)if(!ids.contains(machines->item(row,0)->data(Qt::UserRole).toString()))machines->removeRow(row);
        const auto peers=snapshot.value("peers").toObject();
        for(const auto &entry:entries){
            const auto machine=entry.toObject();const auto id=machine.value("id").toString();const auto alias=machine.value("connection").toString();int row=0;
            while(row<machines->rowCount()&&machines->item(row,0)->data(Qt::UserRole).toString()!=id)++row;
            if(row==machines->rowCount()){machines->insertRow(row);for(int col=0;col<3;++col)machines->setItem(row,col,new QTableWidgetItem);}
            auto *name=machines->item(row,0);name->setText(machine.value("name").toString());name->setData(Qt::UserRole,id);name->setToolTip(name->text()+"\n"+id);
            auto *access=machines->item(row,1);access->setText(machine.value("local").toBool()?QObject::tr("This computer"):alias.isEmpty()?QObject::tr("Not configured"):alias);access->setToolTip(access->text());
            const auto state=peers.value(alias).toObject();QString text=machine.value("local").toBool()?QObject::tr("Local catalog"):QObject::tr("Shared through swarm");
            if(!state.isEmpty()){text=state.value("error").toString();if(text.isEmpty()){const auto timestamp=qint64(state.value("last_sync").toDouble());text=timestamp?QObject::tr("Synced %1").arg(QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime().toString("HH:mm:ss")):QObject::tr("Waiting to sync");}}
            auto *item=machines->item(row,2);item->setText(text);item->setToolTip(text);
        }
        machines->setSortingEnabled(true);filterMachines();
        if(!selected.isEmpty())for(int row=0;row<machines->rowCount();++row)if(machines->item(row,0)->data(Qt::UserRole).toString()==selected){machines->setCurrentCell(row,0);break;}
        machines->verticalScrollBar()->setValue(scroll);machines->setUpdatesEnabled(true);
        QSignalBlocker blockSelection(conflicts);
        const auto selectedConflict=conflicts->currentItem()?conflicts->currentItem()->data(Qt::UserRole).toString():QString();
        conflictSnapshot=snapshot;conflictData=snapshot.value("conflicts").toArray();conflicts->clear();
        for(const auto &entry:conflictData){const auto c=entry.toObject();
            const auto label=review->conflictLabel(c,snapshot);auto *item=new QListWidgetItem(label,conflicts);item->setToolTip(label);item->setData(Qt::UserRole,c.value("key").toString());item->setData(SwarmConflictReview::PresentationRole,review->conflictPresentation(c,snapshot));if(c.value("key").toString()==selectedConflict)conflicts->setCurrentItem(item);}
        if(!conflicts->currentItem()&&conflicts->count())conflicts->setCurrentRow(0);
        review->setConflict(conflicts->currentRow()<0?QJsonObject{}:conflictData[conflicts->currentRow()].toObject(),snapshot);
        tabs->setTabText(conflictTab,QObject::tr("Conflicts (%1)").arg(conflicts->count()));
        conflictStack->setCurrentWidget(conflicts->count()?conflictPage:static_cast<QWidget*>(noConflicts));
    };
    QObject::connect(conflicts,&QListWidget::currentRowChanged,&dialog,[&](int row){
        review->setConflict(row<0||row>=conflictData.size()?QJsonObject{}:conflictData[row].toObject(),conflictSnapshot);
    });
    auto updateButtons=[&]{const bool busy=request!=0;preview->setEnabled(!busy&&peer->count());peer->setEnabled(!busy);sync->setEnabled(!busy&&controller->settled());join->setEnabled(!busy&&controller->settled()&&!previewData.isEmpty()&&peer->currentText()==previewPeer);review->setBusy(busy||!controller->settled());conflicts->setEnabled(!dialog.changing);manage->setEnabled(!dialog.changing);buttons->setEnabled(!dialog.changing);mapping->setEnabled(!dialog.changing);};
    QTimer stateTimer;stateTimer.setInterval(200);QObject::connect(&stateTimer,&QTimer::timeout,&dialog,updateButtons);stateTimer.start();
    QObject::connect(manage,&QPushButton::clicked,&dialog,[&]{dialog.accept();manageConnections();});
    QObject::connect(peer,&QComboBox::currentIndexChanged,&dialog,[&]{previewData={};mapping->setRowCount(0);previewStack->setCurrentWidget(previewEmpty);keepPlacement->hide();join->setText(QObject::tr("Connect this member"));updateButtons();});
    QObject::connect(preview,&QPushButton::clicked,&dialog,[&]{action="preview";previewPeer=peer->currentText();previewData={};previewStack->setCurrentWidget(previewEmpty);keepPlacement->hide();request=client->requestSwarm({"preview",previewPeer});setStatus(QObject::tr("Reading the other project catalog…"));updateButtons();});
    QObject::connect(sync,&QPushButton::clicked,&dialog,[&]{action="sync";request=client->requestSwarm({"sync"});setStatus(QObject::tr("Synchronizing projects…"));updateButtons();});
    QObject::connect(join,&QPushButton::clicked,&dialog,[&]{
        if(!controller->settled())return;QJsonObject map;
        for(int row=0;row<mapping->rowCount();++row){auto *choice=qobject_cast<QComboBox*>(mapping->cellWidget(row,1));if(!choice->currentData().toString().isEmpty())map.insert(mapping->item(row,0)->data(Qt::UserRole).toString(),choice->currentData().toString());}
        dialog.changing=true;controller->suspend(true);action="join";request=client->requestSwarm({"join",previewPeer},{{"node_id",previewData.value("node_id")},{"swarm_id",previewData.value("swarm_id")},{"project_map",map},{"prefer_peer_memberships",keepPlacement->isChecked()}});setStatus(QObject::tr("Connecting and merging catalogs…"));updateButtons();
    });
    QObject::connect(review,&SwarmConflictReview::resolutionRequested,&dialog,[&](const QJsonObject &payload){
        if(request||!controller->settled()||conflicts->currentRow()<0)return;
        dialog.changing=true;controller->suspend(true);action="resolve";request=client->requestSwarm({"resolve"},payload);setStatus(QObject::tr("Saving your choice to the shared catalog…"));updateButtons();
    });
    QObject::connect(client,&HgsClient::swarmReady,&dialog,[&](quint64 id,const QJsonObject &result){
        if(id!=request)return;request=0;dialog.changing=false;
        if(action=="preview"){
            previewData=result;mapping->setRowCount(0);const bool same=result.value("same_swarm").toBool();
            if(!same)for(const auto &v:controller->snapshot().value("organization").toObject().value("projects").toArray()){
                const auto p=v.toObject();const int row=mapping->rowCount();mapping->insertRow(row);auto *name=new QTableWidgetItem(p.value("name").toString());name->setData(Qt::UserRole,p.value("id").toString());mapping->setItem(row,0,name);
                auto *choice=new QComboBox;choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);choice->setMinimumContentsLength(12);choice->addItem(QObject::tr("Keep as a separate project"),QString());
                for(const auto &remote:result.value("projects").toArray()){const auto r=remote.toObject();choice->addItem(r.value("name").toString()+" ("+r.value("id").toString().left(8)+")",r.value("id").toString());if(r.value("id")==p.value("id"))choice->setCurrentIndex(choice->count()-1);}
                mapping->setCellWidget(row,1,choice);
            }
            const bool initialized=result.value("initialized").toBool();
            keepPlacement->setVisible(initialized&&!same);
            join->setText(same?QObject::tr("Connect this member"):QObject::tr("Join and merge projects"));
            if(initialized){
                sameCatalog->setText(QObject::tr("%1 already shares this project catalog.\nConnect directly to synchronize through this computer.").arg(previewPeer));
                previewStack->setCurrentWidget(same?static_cast<QWidget*>(sameCatalog):mappingPage);
                setStatus(same?QObject::tr("Ready to connect to %1.").arg(previewPeer):QObject::tr("Review the project mapping, then connect to %1.").arg(previewPeer));
            }else{
                setStatus(QObject::tr("Open Projects in Zerus on %1 first to import its catalog.").arg(previewPeer));previewData={};
            }
        }else if(action=="join"||action=="resolve"){
            controller->acceptExternal(action=="join"?result.value("snapshot").toObject():result);controller->suspend(false);previewData={};mapping->setRowCount(0);previewStack->setCurrentWidget(previewEmpty);keepPlacement->hide();join->setText(QObject::tr("Connect this member"));setStatus(action=="join"?QObject::tr("Connected to %1.").arg(previewPeer):result.value("conflicts").toArray().isEmpty()?QObject::tr("Choice saved. No conflicts remain."):QObject::tr("Choice saved. Review the remaining conflicts."));
        }else{controller->refresh();setStatus(QObject::tr("Synchronization finished. Connection details are on the Computers tab."));}
        updateButtons();
    });
    QObject::connect(client,&HgsClient::swarmFailed,&dialog,[&](quint64 id,const QString &error){if(id!=request)return;request=0;dialog.changing=false;setStatus(error);controller->suspend(false);updateButtons();});
    QObject::connect(controller,&SwarmController::snapshotChanged,&dialog,render);
    render(controller->snapshot());updateButtons();dialog.exec();
    controller->suspend(false);
    // If closed while a remote action is in progress, subsequent local reads
    // still see its durable result; no native session is attached or restarted.
}
