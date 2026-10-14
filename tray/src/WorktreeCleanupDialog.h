#pragma once
#include "HgsClient.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>

// Explicit cleanup of linked worktrees. The owning machine reviews them and
// repeats every check before removing one; this dialog only selects reviewed
// rows, asks for confirmation and removes them one at a time.
class WorktreeCleanupDialog : public QDialog {
public:
    WorktreeCleanupDialog(HgsClient *client,const QString &host,const QString &machine,const QString &path,const QString &focus={},QWidget *parent=nullptr)
        :QDialog(parent),m_client(client),m_host(host),m_machine(machine),m_path(path),m_focus(focus) {
        setObjectName("worktreeCleanupDialog");setWindowTitle(focus.isEmpty()?tr("Clean up worktrees"):tr("Remove worktree"));setMinimumWidth(620);resize(820,520);
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(24,22,24,22);layout->setSpacing(12);
        auto label=[](const QString &text={}){auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);return l;};
        auto *heading=label(windowTitle());heading->setObjectName("heading");layout->addWidget(heading);
        m_context=label();m_context->setObjectName("worktreeCleanupContext");m_context->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_context);describe();
        auto *hint=label(tr("Removal uses git worktree remove without force. Ignored files in a removed folder, such as build output, are deleted with it. Blocked worktrees explain what to finish first."));
        hint->setObjectName("muted");layout->addWidget(hint);
        m_list=new QTreeWidget;m_list->setObjectName("worktreeCleanupList");m_list->setColumnCount(3);m_list->setHeaderLabels({tr("Worktree"),tr("State"),tr("Details")});
        m_list->setRootIsDecorated(false);m_list->setTextElideMode(Qt::ElideMiddle);m_list->setMinimumHeight(140);
        m_list->header()->setSectionResizeMode(0,QHeaderView::Interactive);m_list->setColumnWidth(0,280);m_list->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
        layout->addWidget(m_list,1);
        m_details=label();m_details->setObjectName("worktreeCleanupDetails");m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_details);
        m_branches=new QCheckBox(tr("Also delete merged branches"));m_branches->setObjectName("deleteMergedBranches");
        m_branches->setToolTip(tr("Uses git branch -d after the worktree is removed. Branches with commits that are not merged are always kept."));
        m_branches->setChecked(QSettings().value("workspace/worktreeCleanupDeleteBranches",false).toBool());layout->addWidget(m_branches);
        m_status=label();m_status->setObjectName("worktreeCleanupStatus");layout->addWidget(m_status);
        auto *buttons=new QDialogButtonBox;m_review=buttons->addButton(tr("Review again"),QDialogButtonBox::ResetRole);m_review->setObjectName("reviewWorktreesAgain");
        m_close=buttons->addButton(QDialogButtonBox::Close);m_remove=buttons->addButton(tr("Remove selected…"),QDialogButtonBox::ActionRole);m_remove->setObjectName("removeSelectedWorktrees");
        for(auto *button:{m_review,m_close,m_remove})button->setAutoDefault(false);layout->addWidget(buttons);
        connect(m_close,&QPushButton::clicked,this,&QDialog::reject);
        connect(m_review,&QPushButton::clicked,this,[this]{m_results.clear();review();});
        connect(m_remove,&QPushButton::clicked,this,[this]{confirm();});
        connect(m_branches,&QCheckBox::toggled,this,[this](bool on){QSettings().setValue("workspace/worktreeCleanupDeleteBranches",on);});
        connect(m_list,&QTreeWidget::itemChanged,this,[this]{updateActions();});
        connect(m_list,&QTreeWidget::currentItemChanged,this,[this]{showDetails();});
        connect(m_client,&HgsClient::worktreeReviewReady,this,[this](quint64 request,const QString &,const QString &,const QJsonObject &data){
            if(request!=m_request)return;m_request=0;m_data=data;m_error.clear();render();
        });
        connect(m_client,&HgsClient::worktreeReviewFailed,this,[this](quint64 request,const QString &,const QString &,const QString &error){
            if(request!=m_request)return;m_request=0;m_data={};m_error=error;render();
        });
        connect(m_client,&HgsClient::worktreeRemoved,this,[this](quint64 request,bool ok,const QJsonObject &result,const QString &error){
            if(request!=m_removal)return;m_removal=0;removed(ok,result,error);
        });
        review();
    }
    bool changed() const {return m_changed;}
    // Removal cannot be abandoned half way; the current Git call always finishes.
    void reject() override {if(!busy())QDialog::reject();}
private:
    bool busy() const {return m_removal||!m_queue.isEmpty();}
    static QString size(qint64 bytes) {return QLocale().formattedDataSize(bytes);}
    static QString ago(double seconds) {
        if(seconds<=0)return {};const qint64 age=qMax<qint64>(0,QDateTime::currentSecsSinceEpoch()-qint64(seconds));
        if(age<3600)return tr("used within the last hour");
        if(age<2*86400){const qint64 hours=age/3600;return hours==1?tr("used 1 hour ago"):tr("used %1 hours ago").arg(hours);}
        return tr("used %1 days ago").arg(age/86400);
    }
    static QString state(const QString &verdict) {
        return verdict=="ready"?tr("Ready"):verdict=="review"?tr("Review"):verdict=="missing"?tr("Missing"):tr("Blocked");
    }
    QString base() const {const auto bases=m_data["base"].toArray();return bases.isEmpty()?tr("the base branch"):bases.first().toString();}
    QString summary(const QJsonObject &row) const {
        const auto reasons=row["reasons"].toArray();
        if(!reasons.isEmpty())return reasons.first().toObject()["message"].toString()+(reasons.size()>1?tr(" (+%1 more)").arg(reasons.size()-1):QString());
        if(row["verdict"]=="missing")return tr("The folder is gone; only Git metadata remains.");
        QStringList parts;if(row["merged"]==true)parts<<tr("Merged into %1").arg(base());
        for(const auto &note:row["notes"].toArray())parts<<note.toObject()["message"].toString();
        if(row["ignored_bytes"].toDouble()>0)parts<<tr("%1 of ignored files").arg(size(qint64(row["ignored_bytes"].toDouble())));
        if(const auto used=ago(row["last_activity"].toDouble());!used.isEmpty())parts<<used;
        return parts.join(" · ");
    }
    QString details(const QJsonObject &row) const {
        QStringList lines;
        for(const auto &value:row["reasons"].toArray())lines<<"• "+value.toObject()["message"].toString();
        for(const auto &value:row["notes"].toArray())lines<<"• "+value.toObject()["message"].toString();
        if(row["verdict"]=="missing")lines<<tr("• Forgetting it removes only Git's record; the branch stays.");
        QStringList facts;
        if(row["merged"]==true)facts<<tr("merged into %1").arg(base());
        if(!row["upstream"].toString().isEmpty())facts<<(row["pushed"]==true?tr("pushed to %1").arg(row["upstream"].toString()):tr("not pushed to %1").arg(row["upstream"].toString()));
        if(const auto used=ago(row["last_activity"].toDouble());!used.isEmpty())facts<<used;
        if(!facts.isEmpty())lines<<facts.join(", ");
        QStringList ignored;for(const auto &value:row["ignored"].toArray()){const auto entry=value.toObject();ignored<<entry["path"].toString()+" "+size(qint64(entry["bytes"].toDouble()))+(entry["complete"].toBool()?QString():tr(" or more"));}
        if(!ignored.isEmpty())lines<<tr("Ignored files deleted with the folder: %1").arg(ignored.mid(0,5).join(", ")+(ignored.size()>5?tr(", …"):QString()));
        return lines.join('\n');
    }
    // `path` may be the repository's common directory; show its main checkout.
    void describe() {
        QString repository=m_path;
        for(const auto &value:m_data["worktrees"].toArray())if(value.toObject()["kind"]=="main"&&value.toObject()["available"].toBool())repository=value.toObject()["path"].toString();
        m_context->setText(m_focus.isEmpty()?tr("Computer: %1\nRepository: %2").arg(m_machine,repository):tr("Computer: %1\nWorktree: %2").arg(m_machine,m_focus));
    }
    void review() {
        if(busy())return;m_status->setText(tr("Reviewing worktrees on %1…").arg(m_machine));
        m_request=m_client->reviewWorktrees(m_host,m_path,m_focus);updateActions();
    }
    void render() {
        describe();const QSignalBlocker blocker(m_list);m_list->clear();int ready=0,review=0,blocked=0,missing=0;
        const auto muted=palette().color(QPalette::Disabled,QPalette::Text);
        for(const auto &value:m_data["worktrees"].toArray()) {
            const auto row=value.toObject();if(row["kind"]!="linked")continue;
            const auto path=row["path"].toString(),verdict=row["verdict"].toString();
            // Keep a single-checkout operation scoped even if the response
            // contains a wider repository review.
            if(!m_focus.isEmpty()&&path!=m_focus)continue;
            const auto branch=row["branch"].toString().isEmpty()?tr("detached %1").arg(row["head"].toString().left(8)):row["branch"].toString();
            auto *item=new QTreeWidgetItem(m_list,{QFileInfo(path).fileName()+" · "+branch,state(verdict),summary(row)});
            item->setData(0,Qt::UserRole,path);item->setData(0,Qt::UserRole+1,row);item->setToolTip(0,path);item->setToolTip(2,details(row));
            if(verdict=="blocked"){++blocked;item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable);for(int column=0;column<3;++column)item->setForeground(column,muted);continue;}
            (verdict=="ready"?ready:verdict=="missing"?missing:review)++;
            item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable|Qt::ItemIsUserCheckable);
            // Only verdicts without attention notes start selected, unless this
            // dialog was opened for exactly this worktree.
            item->setCheckState(0,verdict!="review"||!m_focus.isEmpty()?Qt::Checked:Qt::Unchecked);
        }
        QString status;const auto catalog=m_data["state"].toString();
        if(!m_error.isEmpty())status=tr("Review failed: %1").arg(m_error);
        else if(catalog!="ok")status=catalog=="not_repo"?tr("This folder is not in a Git repository."):tr("Worktrees are unavailable: %1").arg(m_data["detail"].toString(catalog));
        else if(!m_list->topLevelItemCount())status=m_focus.isEmpty()?tr("This repository has no linked worktrees."):tr("Git no longer lists this worktree.");
        else {
            QStringList parts;if(ready)parts<<tr("%1 ready").arg(ready);if(review)parts<<tr("%1 to review").arg(review);if(missing)parts<<tr("%1 missing").arg(missing);if(blocked)parts<<tr("%1 blocked").arg(blocked);
            status=parts.join(", ")+'.';if(m_data["partial"].toBool())status+=tr(" Only part of the repository was reviewed.");
        }
        m_status->setText(m_results.isEmpty()?status:m_results+'\n'+status);
        if(m_list->topLevelItemCount())m_list->setCurrentItem(m_list->topLevelItem(0));
        showDetails();updateActions();
    }
    void showDetails() {
        const auto *item=m_list->currentItem();
        m_details->setText(item?item->data(0,Qt::UserRole).toString()+'\n'+details(item->data(0,Qt::UserRole+1).toJsonObject()):QString());
    }
    QList<QJsonObject> selected() const {
        QList<QJsonObject> rows;
        for(int i=0;i<m_list->topLevelItemCount();++i){const auto *item=m_list->topLevelItem(i);if((item->flags()&Qt::ItemIsUserCheckable)&&item->checkState(0)==Qt::Checked)rows<<item->data(0,Qt::UserRole+1).toJsonObject();}
        return rows;
    }
    void updateActions() {
        const int count=selected().size();const bool idle=!busy()&&!m_request;
        m_remove->setEnabled(idle&&count>0&&m_data["state"]=="ok");m_remove->setText(count>1?tr("Remove %1 worktrees…").arg(count):tr("Remove selected…"));
        for(QWidget *widget:{static_cast<QWidget *>(m_review),static_cast<QWidget *>(m_branches),static_cast<QWidget *>(m_list)})widget->setEnabled(idle);
        m_close->setEnabled(!busy());
    }
    void confirm() {
        const auto rows=selected();if(rows.isEmpty()||busy()||m_request)return;
        qint64 ignored=0;QStringList names,branches;int missing=0;
        for(const auto &row:rows){
            names<<QFileInfo(row["path"].toString()).fileName();ignored+=qint64(row["ignored_bytes"].toDouble());missing+=row["verdict"]=="missing";
            if(m_branches->isChecked()&&row["merged"]==true&&!row["branch"].toString().isEmpty())branches<<row["branch"].toString();
        }
        QStringList lines{names.join(", ")};
        if(ignored>0)lines<<tr("Ignored files deleted with them: %1.").arg(size(ignored));
        if(missing)lines<<tr("Missing folders: only Git's record is removed.");
        lines<<(branches.isEmpty()?tr("All branches are kept."):tr("Merged branches deleted: %1.").arg(branches.join(", ")));
        lines<<tr("Each worktree is checked again before removal. This cannot be undone.");
        QMessageBox box(QMessageBox::Warning,windowTitle(),rows.size()==1?tr("Remove 1 worktree on %1?").arg(m_machine):tr("Remove %1 worktrees on %2?").arg(rows.size()).arg(m_machine),QMessageBox::Cancel,this);
        box.setObjectName("confirmWorktreeCleanup");box.setInformativeText(lines.join('\n'));
        auto *accept=box.addButton(rows.size()==1?tr("Remove worktree"):tr("Remove %1 worktrees").arg(rows.size()),QMessageBox::DestructiveRole);accept->setObjectName("confirmRemoveWorktrees");
        box.setDefaultButton(QMessageBox::Cancel);box.setEscapeButton(QMessageBox::Cancel);box.exec();if(box.clickedButton()!=accept)return;
        m_queue=rows;m_results.clear();m_done=0;m_failures.clear();m_branchNotes.clear();next();
    }
    void setRowState(const QString &path,const QString &text) {
        for(int i=0;i<m_list->topLevelItemCount();++i)if(auto *item=m_list->topLevelItem(i);item->data(0,Qt::UserRole).toString()==path){const QSignalBlocker blocker(m_list);item->setText(1,text);}
    }
    void next() {
        if(m_queue.isEmpty()) {
            QStringList lines;lines<<(m_done==1?tr("Removed 1 worktree."):tr("Removed %1 worktrees.").arg(m_done));
            lines<<m_branchNotes<<m_failures;m_results=lines.join('\n');updateActions();review();return;
        }
        m_current=m_queue.takeFirst();setRowState(m_current["path"].toString(),tr("Removing…"));
        m_removal=m_client->removeWorktree(m_host,m_current["path"].toString(),m_data["common_dir"].toString(),m_current["fingerprint"].toString(),
            m_branches->isChecked()&&m_current["merged"]==true);
        m_status->setText(tr("Removing %1…").arg(m_current["path"].toString()));updateActions();
    }
    void removed(bool ok,const QJsonObject &result,const QString &error) {
        const auto path=m_current["path"].toString(),name=QFileInfo(path).fileName();
        if(ok) {
            ++m_done;m_changed=true;setRowState(path,result["status"]=="forgotten"?tr("Forgotten"):tr("Removed"));
            if(!result["branch_error"].toString().isEmpty())m_branchNotes<<tr("%1: %2").arg(name,result["branch_error"].toString());
        } else {m_failures<<tr("%1 was not removed: %2").arg(name,error);setRowState(path,tr("Not removed"));}
        next();
    }
    HgsClient *m_client;QString m_host,m_machine,m_path,m_focus,m_error,m_results;QJsonObject m_data,m_current;
    QTreeWidget *m_list;QLabel *m_context,*m_details,*m_status;QCheckBox *m_branches;QPushButton *m_review,*m_close,*m_remove;
    QList<QJsonObject> m_queue;QStringList m_failures,m_branchNotes;quint64 m_request=0,m_removal=0;int m_done=0;bool m_changed=false;
};
