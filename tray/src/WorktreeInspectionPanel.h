#pragma once
#include "HgsClient.h"
#include "WorktreeReviewUi.h"
#include "WorkspaceIcons.h"
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QLabel>
#include <QHeaderView>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QScrollArea>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

class WorktreeInspectionPanel : public QWidget {
public:
    explicit WorktreeInspectionPanel(HgsClient *client, QWidget *parent = nullptr) : QWidget(parent), m_client(client) {
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(12,8,8,8); layout->setSpacing(8);
        auto *heading = new QWidget; heading->setStyleSheet("QPushButton { padding:4px 8px; min-height:18px; }");
        auto *headingRow = new QHBoxLayout(heading); headingRow->setContentsMargins(0,0,0,0); headingRow->setSpacing(6);
        m_title = new QLabel(tr("Select a worktree")); m_title->setObjectName("heading"); m_title->setTextFormat(Qt::PlainText);
        m_title->setMinimumWidth(0); m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); headingRow->addWidget(m_title,1);
        auto *details = new QPushButton(tr("Details")); details->setObjectName("toggleWorktreeDetails"); details->setCheckable(true); details->setAutoDefault(false); headingRow->addWidget(details);
        m_inspect = new QPushButton; m_inspect->setObjectName("inspectWorktreeChanges"); configureWorkspaceIconButton(m_inspect,"refresh",tr("Read files and commits"),palette().color(QPalette::Window).lightness()<128); headingRow->addWidget(m_inspect);
        layout->addWidget(heading);
        m_brief = new QLabel; m_brief->setObjectName("worktreeBrief"); m_brief->setTextFormat(Qt::PlainText); m_brief->setWordWrap(true); layout->addWidget(m_brief);
        m_summary = new QLabel; m_summary->setObjectName("worktreeOverviewDetails"); m_summary->setWordWrap(true); m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse); m_summary->setTextFormat(Qt::PlainText);
        auto *summaryArea = new QScrollArea; summaryArea->setObjectName("worktreeSummaryArea"); summaryArea->setWidgetResizable(true); summaryArea->setFrameShape(QFrame::NoFrame); summaryArea->setMinimumHeight(76); summaryArea->setMaximumHeight(130); summaryArea->setWidget(m_summary); layout->addWidget(summaryArea); summaryArea->hide();
        connect(details,&QPushButton::toggled,summaryArea,&QWidget::setVisible);
        m_summary->setAlignment(Qt::AlignTop);
        m_tabs = new QTabWidget; m_tabs->setObjectName("worktreeInspectionTabs"); m_tabs->setMinimumHeight(230); layout->addWidget(m_tabs,1);
        auto *changes = new QWidget; auto *changesLayout = new QVBoxLayout(changes); changesLayout->setContentsMargins(10,10,10,10);
        m_mode = new QComboBox; m_mode->addItems({tr("Uncommitted changes"),tr("Compare files with base")}); changesLayout->addWidget(m_mode);
        m_filesState = new QLabel(tr("Select a folder to load its changed files."));m_filesState->setObjectName("worktreeChangedFilesState");m_filesState->setWordWrap(true);changesLayout->addWidget(m_filesState);
        auto *split = new QSplitter(Qt::Vertical); m_files = new QTreeWidget; m_files->setObjectName("worktreeChangedFiles"); m_files->setHeaderLabels({tr("Changed file"),tr("State")}); m_files->setRootIsDecorated(false);m_files->setMinimumHeight(60);
        m_files->header()->setStretchLastSection(false); m_files->header()->setSectionResizeMode(0,QHeaderView::Stretch); m_files->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
        split->setChildrenCollapsible(false); split->addWidget(m_files); m_diff = editor("worktreeChangesDiff");m_diff->setMinimumHeight(100); split->addWidget(m_diff);split->setSizes({120,300});changesLayout->addWidget(split,1);m_tabs->addTab(changes,tr("Changes"));
        auto *commitsPage=new QWidget;auto *commitsLayout=new QVBoxLayout(commitsPage);commitsLayout->setContentsMargins(10,10,10,10);
        m_commits = editor("worktreeCommits");commitsLayout->addWidget(m_commits);m_tabs->addTab(commitsPage,tr("Commits"));
        auto *local = new QWidget; auto *localLayout = new QVBoxLayout(local);localLayout->setContentsMargins(10,10,10,10);
        auto *localHint = new QLabel(tr("Outside Git. Removing this folder deletes these files too.")); localHint->setObjectName("worktreeLocalWarning"); localHint->setWordWrap(true);localLayout->addWidget(localHint);
        auto *navigation = new QWidget; navigation->setObjectName("worktreeFileNavigation"); navigation->setStyleSheet("QPushButton { padding:4px 8px; min-height:18px; }");
        auto *navigationRow = new QHBoxLayout(navigation); navigationRow->setContentsMargins(0,0,0,0); navigationRow->setSpacing(6);
        m_browse = new QPushButton(tr("Browse all files")); m_browse->setObjectName("browseAllWorktreeFiles"); m_browse->setAutoDefault(false); navigationRow->addWidget(m_browse);
        connect(m_browse,&QPushButton::clicked,this,[this]{preview(".");});
        m_more = new QPushButton(tr("Next")); m_more->setAccessibleName(tr("Next files")); m_more->setObjectName("nextWorktreeFiles"); m_more->setAutoDefault(false); m_more->hide(); navigationRow->addWidget(m_more);
        connect(m_more,&QPushButton::clicked,this,[this]{preview(m_file,m_nextOffset);});
        m_up = new QPushButton; m_up->setObjectName("showLocalWorktreeFiles"); configureWorkspaceIconButton(m_up,"folder",tr("Back to local files"),palette().color(QPalette::Window).lightness()<128); m_up->hide(); navigationRow->addWidget(m_up); navigationRow->addStretch(); localLayout->addWidget(navigation);
        auto *localSplit = new QSplitter(Qt::Vertical); localSplit->setObjectName("worktreeLocalSplit"); localSplit->setChildrenCollapsible(false);
        m_local = new QTreeWidget;m_local->setObjectName("worktreeLocalFiles");m_local->setHeaderLabels({tr("Local file / folder"),tr("Size")});m_local->setRootIsDecorated(false);
        m_localState = new QLabel;m_localState->setObjectName("worktreeLocalFilesState");m_localState->setWordWrap(true);localLayout->addWidget(m_localState);
        m_local->setMinimumHeight(90); m_local->setTextElideMode(Qt::ElideMiddle); m_local->header()->setStretchLastSection(false); m_local->header()->setSectionResizeMode(0,QHeaderView::Stretch); m_local->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents); localSplit->addWidget(m_local);
        m_preview=editor("worktreeLocalFilePreview");m_preview->setMinimumHeight(100);localSplit->addWidget(m_preview);localSplit->setSizes({220,180});localSplit->setStretchFactor(0,3);localSplit->setStretchFactor(1,2);m_preview->hide();localLayout->addWidget(localSplit,1);m_tabs->addTab(local,tr("Local"));m_tabs->setTabToolTip(2,tr("Local files outside Git and folder browser"));
        auto *sessions = new QWidget;auto *sessionLayout = new QVBoxLayout(sessions);sessionLayout->setContentsMargins(10,10,10,10);
        m_sessionsState=new QLabel(tr("No related sessions."));m_sessionsState->setWordWrap(true);sessionLayout->addWidget(m_sessionsState);
        m_sessions = new QListWidget;m_sessions->setObjectName("worktreeRelatedSessions");sessionLayout->addWidget(m_sessions,1);
        m_openSession = new QPushButton(tr("Open session"));m_openSession->setAutoDefault(false);sessionLayout->addWidget(m_openSession);m_tabs->addTab(sessions,tr("Sessions"));
        m_status = new QLabel;m_status->setObjectName("worktreeInspectionStatus");m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);layout->addWidget(m_status);
        m_load = new QTimer(this);m_load->setSingleShot(true);m_load->setInterval(150);
        connect(m_load,&QTimer::timeout,this,[this]{inspect(false);});
        connect(m_inspect,&QPushButton::clicked,this,[this]{inspect(true);});
        connect(m_client,&HgsClient::worktreeCacheInvalidated,this,[this](const QString &host,const QString &common){
            if(host!=m_host||common!=m_common)return;
            m_request=0;m_fileRequest=0;m_data={};m_filesState->setText(tr("Folder changed. Updating files…"));
            if(m_available)m_load->start();
        });
        connect(m_mode,&QComboBox::currentIndexChanged,this,[this]{showDiff();});
        connect(m_up,&QPushButton::clicked,this,[this]{showLocalRoots();});
        connect(m_local,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem *item){preview(item->data(0,Qt::UserRole).toString());});
        connect(m_files,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem *item){preview(item->data(0,Qt::UserRole).toString());m_tabs->setCurrentIndex(2);});
        connect(m_openSession,&QPushButton::clicked,this,[this]{openSession();});
        connect(m_sessions,&QListWidget::itemActivated,this,[this]{openSession();});
        connect(m_sessions,&QListWidget::currentRowChanged,this,[this]{m_openSession->setEnabled(m_sessions->currentItem()!=nullptr);});
        connect(m_client,&HgsClient::worktreeInspectionFinished,this,[this](quint64 request,const QString &host,const QString &path,const QString &file,const QJsonObject &data,const QString &error){
            if(file.isEmpty()&&request==m_readingMetadata) {
                m_readingMetadata=0;
                if(request!=m_request) {if(m_available&&m_data.isEmpty())m_load->start();return;}
            }
            if(host!=m_host||path!=m_path)return;
            if(file.isEmpty()){
                if(request!=m_request)return;m_request=0;m_inspect->setEnabled(m_available);
                if(!error.isEmpty()){
                    m_filesState->setText(m_data.isEmpty()?tr("Could not load changed files. Use Refresh to retry."):tr("Could not refresh files. Showing previous data."));
                    if(m_data.isEmpty()){m_diff->setPlainText(tr("Could not load changes. Use Refresh to retry."));m_commits->setPlainText(tr("Could not load commits. Use Refresh to retry."));}
                    m_status->setText(error);return;
                }
                m_data=data;m_review=data["review"].toObject();m_files->clear();
                for(const auto &value:data["files"].toArray()){const auto entry=value.toObject();auto *item=new QTreeWidgetItem(m_files,{entry["path"].toString(),entry["status"].toString()});item->setData(0,Qt::UserRole,entry["path"].toString());}
                m_filesState->setText(m_files->topLevelItemCount()?(m_files->topLevelItemCount()==1?tr("1 changed file"):tr("%1 changed files").arg(m_files->topLevelItemCount()))+(data["files_partial"].toBool()?tr(" · First 200 shown"):QString()):tr("No changed files."));
                m_commits->setPlainText(!data.contains("commits")?tr("No verified base is available for comparing commits."):tr("Original commits absent from the base history (up to 30). Equivalent changes may already be integrated.\n\n")+text(data["commits"].toObject(),tr("No additional commits.")));
                showDiff();m_status->setText(tr("Updated %1 · %2 · Local Git refs").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(qint64(data["received_at"].toDouble())).time(),QLocale::ShortFormat),m_machine)+(data["files_partial"].toBool()?tr(" Only the first 200 changed files are shown."):QString()));if(m_file.isEmpty())showLocalRoots();
            }else{
                if(request!=m_fileRequest||file!=m_file)return;m_fileRequest=0;
                if(!error.isEmpty()){m_localState->setText(tr("Could not load files. Browse again to retry."));m_preview->setPlainText(error);m_preview->show();return;}
                const auto result=data["file"].toObject();m_nextOffset=result["next_offset"].toInt(-1);m_more->setVisible(m_nextOffset>=0);
                if(result.contains("entries")){
                    m_preview->hide();
                    m_status->setText(tr("Read files from %1").arg(m_machine));
                    m_local->clear();for(const auto &value:result["entries"].toArray()){const auto entry=value.toObject();auto *item=new QTreeWidgetItem(m_local,{entry["path"].toString(),entry["directory"].toBool()?tr("Folder"):entry["symlink"].toBool()?tr("Link"):QString()});item->setData(0,Qt::UserRole,entry["path"].toString());}
                    m_localState->setText(m_local->topLevelItemCount()?tr("%1 entries%2").arg(m_local->topLevelItemCount()).arg(result["truncated"].toBool()?tr(" · More available"):QString()):tr("This folder is empty."));
                    m_preview->setPlainText(result["truncated"].toBool()?tr("More entries are available. Use Next files to continue."):tr("Open a file to read it."));
                }else {m_localState->setText(tr("Preview: %1").arg(file));m_preview->show();m_preview->setPlainText(file+'\n'+(result["binary"].toBool()?tr("Binary file; text preview is unavailable."):result["text"].toString())+(result["truncated"].toBool()?tr("\n[Preview truncated]"):QString()));}
            }
        });
        m_inspect->setEnabled(false);m_openSession->setEnabled(false);
    }
    std::function<void(const QString &,const QString &)> sessionRequested;
    void setContext(const QString &host,const QString &machine,const QString &path,const QString &common,const QJsonObject &review,const QJsonArray &sessions,bool available) {
        const bool changed=host!=m_host||path!=m_path||common!=m_common;
        const bool evidenceChanged=review["fingerprint"]!=m_contextFingerprint;
        if(changed)m_refreshPending=false;
        // Invalidate immediately, before the user can start a file read under the new proof.
        if(!changed&&evidenceChanged)m_client->invalidateWorktreeInspection(host,path,common);
        m_contextFingerprint=review["fingerprint"];
        m_host=host;m_machine=machine;m_path=path;m_common=common;if(changed||evidenceChanged||m_data.isEmpty())m_review=review;m_available=available;
        m_title->setText(path.isEmpty()?tr("Select a worktree"):QFileInfo(path).fileName());
        m_title->setToolTip(path);
        m_brief->setText(path.isEmpty()?QString():machine+" · "+(review["verdict"].toString().isEmpty()?review["context"].toString():WorktreeReviewUi::summary(review)));
        QStringList summary;
        if(!path.isEmpty()){summary<<WorktreeReviewUi::summary(review)<<path<<machine+" · "+review["branch"].toString()<<review["context"].toString()<<tr("Folder size: %1").arg(WorktreeReviewUi::size(review));}
        if(!review["verdict"].toString().isEmpty())summary<<WorktreeReviewUi::decision(review);
        if(review["last_activity"].toDouble()>0)summary<<tr("Last activity: %1").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(qint64(review["last_activity"].toDouble())),QLocale::ShortFormat));
        if(review["sampled_at"].toDouble()>0)summary<<tr("Checked: %1").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(qint64(review["sampled_at"].toDouble())).time(),QLocale::ShortFormat));
        m_summary->setText(summary.join('\n'));m_summary->setToolTip(path);
        if(changed||evidenceChanged||!available){m_file.clear();m_request=0;m_fileRequest=0;m_data={};m_files->clear();m_diff->clear();m_commits->clear();m_preview->clear();m_status->setText(available?tr("Loading folder details…"):tr("Connect this computer and refresh to inspect its files."));
            m_filesState->setText(path.isEmpty()?tr("Select a worktree."):available?tr("Loading changed files…"):tr("Offline · Changed files are not loaded."));
            m_commits->setPlainText(available?tr("Loading commits…"):tr("Commits are not loaded."));
            m_diff->setPlainText(available?tr("Loading changes…"):tr("Changes are not loaded."));
            m_load->stop();if(available&&!path.isEmpty())m_load->start();
        }
        m_inspect->setEnabled(available&&!m_request);m_browse->setEnabled(available);m_more->setEnabled(available);
        const auto selected=m_sessions->currentItem()?m_sessions->currentItem()->data(Qt::UserRole).toString():QString();m_sessions->clear();
        for(const auto &value:sessions){const auto row=value.toObject();auto *item=new QListWidgetItem(workspaceIcon("sessions",m_sessions->palette().color(QPalette::Text)),row["label"].toString(),m_sessions);item->setData(Qt::UserRole,row["name"].toString());if(row["name"]==selected)m_sessions->setCurrentItem(item);}
        if(!m_sessions->currentItem()&&m_sessions->count())m_sessions->setCurrentRow(0);
        m_sessionsState->setVisible(m_sessions->count()==0);
        m_openSession->setEnabled(available&&m_sessions->currentItem());
        if(changed||evidenceChanged)showLocalRoots();
    }
private:
    static QPlainTextEdit *editor(const QString &name){auto *e=new QPlainTextEdit;e->setObjectName(name);e->setReadOnly(true);return e;}
    static QString text(const QJsonObject &data,const QString &empty){return !data["error"].toString().isEmpty()?data["error"].toString():data["text"].toString().isEmpty()?empty:data["text"].toString()+(data["truncated"].toBool()?QObject::tr("\n[Preview truncated]"):QString());}
    void inspect(bool refresh){if(!m_available||m_request||m_path.isEmpty())return;
        if(m_readingMetadata){m_refreshPending|=refresh;m_load->start();return;}
        refresh|=m_refreshPending;m_refreshPending=false;m_load->stop();m_status->setText(tr("Reading files and commits on %1…").arg(m_machine));m_filesState->setText(m_data.isEmpty()?tr("Loading changed files…"):tr("Updating changed files…"));m_request=m_client->inspectWorktree(m_host,m_path,m_common,{},0,refresh);m_readingMetadata=m_request;m_inspect->setEnabled(false);}
    void showDiff(){if(m_data.isEmpty()){m_diff->setPlainText(tr("Changes are not loaded yet."));return;}m_diff->setPlainText(m_mode->currentIndex()==0?text(m_data["changes"].toObject(),tr("No tracked changes. Open an untracked file from the list to inspect it.")):!m_data.contains("comparison")?tr("No verified base is available for comparing files."):tr("Comparison includes changes made later in the base.\n\n")+text(m_data["comparison"].toObject(),tr("No file differences from the base.")));}
    void showLocalRoots(){m_file.clear();m_local->clear();m_fileRequest=0;m_more->hide();m_up->hide();m_preview->clear();m_preview->hide();for(const auto &value:m_review["ignored"].toArray()){const auto entry=value.toObject();auto *item=new QTreeWidgetItem(m_local,{entry["path"].toString(),QLocale().formattedDataSize(qint64(entry["bytes"].toDouble()))});item->setData(0,Qt::UserRole,entry["path"].toString());item->setToolTip(0,entry["path"].toString());}m_up->setEnabled(m_available);
        const bool loaded=m_review.contains("ignored_count");
        m_localState->setText(!loaded?tr("Local files have not been checked. Use Browse all files."):m_review["ignored_count"].toInt()==0?tr("No local files outside Git."):tr("%1 local entries outside Git").arg(m_review["ignored_count"].toInt()));
        if(m_review["ignored_count"].toInt()>m_review["ignored"].toArray().size())m_status->setText(tr("Showing the largest %1 of %2 local entries. Use Browse all files for the rest.").arg(m_review["ignored"].toArray().size()).arg(m_review["ignored_count"].toInt()));}
    void preview(const QString &file,int offset=0){if(!m_available||file.isEmpty()||m_fileRequest)return;m_file=file;m_up->show();m_localState->setText(tr("Loading files…"));m_preview->setVisible(file!=".");m_preview->setPlainText(tr("Reading %1…").arg(file));m_fileRequest=m_client->inspectWorktree(m_host,m_path,m_common,file,offset);}
    void openSession(){if(m_available&&m_sessions->currentItem()&&sessionRequested)sessionRequested(m_host,m_sessions->currentItem()->data(Qt::UserRole).toString());}
    HgsClient *m_client;QString m_host,m_machine,m_path,m_common,m_file;QJsonObject m_review,m_data;QJsonValue m_contextFingerprint;bool m_available=false;
    quint64 m_request=0,m_fileRequest=0,m_readingMetadata=0;bool m_refreshPending=false;QTimer *m_load;QLabel *m_title,*m_summary,*m_status,*m_brief,*m_filesState,*m_localState,*m_sessionsState;QPushButton *m_inspect,*m_up,*m_openSession,*m_more,*m_browse;int m_nextOffset=-1;QComboBox *m_mode;
    QTabWidget *m_tabs;QTreeWidget *m_files,*m_local;QListWidget *m_sessions;QPlainTextEdit *m_diff,*m_commits,*m_preview;
};
