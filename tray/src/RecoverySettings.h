#pragma once
#include "HgsClient.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>

// Shared policy replication is independent of the visible Settings page.
// Workers keep executing their durable local copy while the GUI is closed.
class RecoverySync : public QObject {
public:
    std::function<void()> changed;
    explicit RecoverySync(const QString &executable,QObject *parent):QObject(parent),client(executable,this) {
        best=QJsonDocument::fromJson(QSettings().value("recovery/sharedPolicy").toByteArray()).object();
        if(!best.value("policy").isObject()||best.value("order").toArray().size()!=2)best={};
        connect(&client,&HgsClient::recoveryFinished,this,[this](quint64 id,bool ok,const QJsonObject &data,const QString &error){
            if(!pending.contains(id))return;const auto request=pending.take(id);
            if(!ok){errors[request.host]=error;if(request.op=="set")saveError=error;}
            else if(!data.value("policy").isObject()||data.value("order").toArray().size()!=2){errors[request.host]=tr("Update HGS on this machine to synchronize recovery settings.");if(request.op=="set")saveError=errors[request.host];}
            else {
                errors.remove(request.host);copies[request.host]=data;
                if(best.isEmpty()||compare(data,best)>0){best=data;persist();}
                if(request.op=="set")saveError.clear();
            }
            if(pending.isEmpty())reconcile();
            if(changed)changed();
        });
        timer.setInterval(15000);connect(&timer,&QTimer::timeout,this,[this]{refresh();});timer.start();
    }
    static int compare(const QJsonObject &a,const QJsonObject &b){
        const auto x=a.value("order").toArray(),y=b.value("order").toArray();
        if(x.size()!=2||y.size()!=2)return x.size()==2?1:y.size()==2?-1:0;
        if(x.at(0).toDouble()!=y.at(0).toDouble())return x.at(0).toDouble()>y.at(0).toDouble()?1:-1;
        return QString::compare(x.at(1).toString(),y.at(1).toString(),Qt::CaseSensitive);
    }
    void setPeers(QStringList peers){peers.removeAll(QString());peers.removeDuplicates();if(peers==machines)return;machines=peers;refresh();}
    // A control-only computer (hgs.exe on Windows) keeps no recovery policy of its
    // own: only the remote machines are read, and a save goes to one of them.
    void setLocalSessions(bool enabled){if(enabled==local)return;local=enabled;copies.remove({});errors.remove({});refresh();}
    // Machines offers Enable local sessions for this console (spec D16), which the
    // status then names besides adding a remote machine.
    void setLocalSessionsOffered(bool offered){if(offered==enableOffered)return;enableOffered=offered;if(changed)changed();}
    QJsonObject envelope()const{return best;}
    // Machines that keep a recovery policy: none on a control-only computer
    // without remote machines.
    bool hasMachines()const{return !hosts().isEmpty();}
    // A save has a machine to go to: this one, or a reachable remote machine.
    bool canSave()const{return !busy()&&!best.isEmpty()&&(local||!saveTarget().isEmpty());}
    QString status()const {
        if(!hasMachines())return enableOffered?tr("To configure automatic recovery, enable local sessions for this computer in Machines or add a remote machine.")
                                              :tr("Add a remote machine in Machines to configure automatic recovery; this computer runs no agents.");
        QStringList missing;for(const auto &host:hosts())if(errors.contains(host)||!copies.contains(host))missing<<label(host);
        if(!pending.isEmpty())return tr("Synchronizing recovery settings…");
        if(!missing.isEmpty())return tr("Waiting for %1. Changes will sync when connected; those machines keep their last settings.").arg(missing.join(", "));
        return tr("Synchronized across %1 machines. Changes from any Zerus are shared.").arg(hosts().size());
    }
    bool busy()const{return !pending.isEmpty();}
    QString error()const{return saveError;}
    void refresh(){if(busy())return;for(const auto &host:hosts())send(host,"get");if(changed)changed();}
    void save(QJsonObject policy,const QJsonObject &base){
        if(busy()){saveError=tr("Wait for synchronization to finish before saving.");if(changed)changed();return;}
        if(best.isEmpty()||compare(base,best)!=0){saveError=tr("Settings changed in another Zerus. Reload before saving.");if(changed)changed();return;}
        // Any reachable machine accepts the change; reconcile() copies it to the rest.
        const QString target=local?QString():saveTarget();
        if(!local&&target.isEmpty()){saveError=tr("Connect a remote machine to save recovery settings.");if(changed)changed();return;}
        saveError.clear();send(target,"set",policy);if(changed)changed();
    }
private:
    struct Request{QString host,op;};
    QString saveTarget()const{for(const auto &host:machines)if(!errors.contains(host)&&copies.contains(host))return host;return {};}
    QStringList hosts()const{QStringList result;if(local)result<<QString();result.append(machines);return result;}
    QString label(const QString &host)const{return host.isEmpty()?tr("this machine"):host;}
    void send(const QString &host,const QString &op,const QJsonObject &data={}){pending.insert(client.requestRecovery(host,op,data),{host,op});}
    void persist(){QSettings().setValue("recovery/sharedPolicy",QJsonDocument(best).toJson(QJsonDocument::Compact));}
    void reconcile(){
        if(best.isEmpty())return;
        for(const auto &host:hosts())if(!errors.contains(host)&&copies.contains(host)&&compare(copies[host],best)<0)
            send(host,"sync",best.value("policy").toObject());
    }
    HgsClient client;QTimer timer;QStringList machines;QJsonObject best;bool local=true,enableOffered=false;
    QMap<QString,QJsonObject> copies;QMap<QString,QString> errors;QMap<quint64,Request> pending;QString saveError;
};

class RecoverySettings : public QWidget {
public:
    explicit RecoverySettings(RecoverySync *sync,QWidget *parent=nullptr):QWidget(parent),m_sync(sync){
        setObjectName("recoverySettingsPage");auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(16);
        auto *title=new QLabel(tr("Automatic recovery"));title->setObjectName("heading");layout->addWidget(title);
        auto *scope=new QLabel(tr("Shared by all connected machines. Agents finish their own retries first; recovery continues the same task."));scope->setWordWrap(true);layout->addWidget(scope);
        enable=new QCheckBox(tr("Recover automatically after temporary failures"));enable->setObjectName("recoveryEnabled");layout->addWidget(enable);
        for(const auto &rule:QList<QPair<QString,QString>>{{"service",tr("Overload / server unavailable")},{"rate_limit",tr("Temporary rate limit")},{"session_limit",tr("Session usage limit")},{"network",tr("Connection / timeout")}}){
            auto *frame=new QFrame;frame->setObjectName("settingsCard");auto *box=new QVBoxLayout(frame);box->setContentsMargins(16,14,16,14);box->setSpacing(10);
            auto *on=new QCheckBox(rule.second);on->setObjectName("recoveryRule_"+rule.first);choices.insert(rule.first,on);box->addWidget(on);
            if(rule.first=="session_limit") {
                sessionMode=new QComboBox;sessionMode->setObjectName("recoverySessionLimitMode");
                sessionMode->addItem(tr("Wait for reset"),"reset");sessionMode->addItem(tr("Retry on a schedule"),"interval");box->addWidget(sessionMode);
                auto *hint=new QLabel(tr("Wait until the provider's reset time by default. If no reset time is available, recovery needs your attention. Choose a schedule to retry at your own intervals."));hint->setWordWrap(true);box->addWidget(hint);
                connect(sessionMode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{dirty=true;refreshControls();});
            }
            auto *row=new QHBoxLayout;row->addWidget(new QLabel(tr("Retry after (seconds)")));auto *delays=new QLineEdit;delays->setObjectName("recoveryDelays_"+rule.first);delays->setPlaceholderText("15, 30, 60, 300, -1");row->addWidget(delays,1);box->addLayout(row);schedules.insert(rule.first,delays);layout->addWidget(frame);
            connect(on,&QCheckBox::toggled,this,[this]{dirty=true;refreshControls();});
            connect(delays,&QLineEdit::textEdited,this,[this]{dirty=true;});
        }
        auto *help=new QLabel(tr("Each positive number is a delay before the next attempt. End with −1 to stop, or 0 to repeat the previous delay forever.\nExample: 300, 0 retries every 5 minutes until stopped.\n\nNew messages and Stop cancel recovery. Sign-in, balance, other quota and context errors require your action."));help->setWordWrap(true);layout->addWidget(help);
        state=new QLabel;state->setWordWrap(true);state->setObjectName("recoverySyncStatus");state->setMinimumHeight(48);layout->addWidget(state);
        message=new QLabel;message->setWordWrap(true);message->setMinimumHeight(36);message->setObjectName("recoverySettingsStatus");layout->addWidget(message);
        auto *row=new QHBoxLayout;save=new QPushButton(tr("Save recovery settings"));save->setObjectName("recoverySave");auto *reload=new QPushButton(tr("Reload"));reload->setObjectName("recoveryReload");row->addWidget(save);row->addWidget(reload);row->addStretch();layout->addLayout(row);layout->addStretch();
        connect(enable,&QCheckBox::toggled,this,[this]{dirty=true;refreshControls();});
        connect(reload,&QPushButton::clicked,this,[this]{dirty=false;load();m_sync->refresh();});
        connect(save,&QPushButton::clicked,this,[this]{
            auto policy=base.value("policy").toObject();QJsonObject delays;
            for(auto i=schedules.begin();i!=schedules.end();++i){QJsonArray list;const auto parts=i.value()->text().split(',');bool valid=!parts.isEmpty()&&parts.size()<=16;
                for(int n=0;n<parts.size();++n){bool ok;const int v=parts[n].trimmed().toInt(&ok);valid&=ok&&((v>=1&&v<=86400)||(n==parts.size()-1&&(v==-1||(v==0&&n>0))));list.append(v);}
                if(!valid){message->setText(tr("Use 1–16 values: delays from 1 to 86400 seconds; −1 or 0 may only be last, and 0 needs a preceding delay."));return;}delays[i.key()]=list;policy[i.key()]=choices[i.key()]->isChecked();
            }
            policy["version"]=3;policy["enabled"]=enable->isChecked();policy["schedules"]=delays;policy["session_limit_mode"]=sessionMode->currentData().toString();
            saving=true;m_sync->save(policy,base);
        });
        m_sync->changed=[this]{
            state->setText(m_sync->status());
            if(saving&&!m_sync->busy()){saving=false;if(m_sync->error().isEmpty()){dirty=false;message->setText(tr("Saved."));}else message->setText(m_sync->error());}
            if(!dirty&&!saving)load();refreshControls();
        };
        load();refreshControls();
    }
    void load(){
        base=m_sync->envelope();if(base.isEmpty())return;const auto policy=base.value("policy").toObject();
        enable->setChecked(policy.value("enabled").toBool());
        sessionMode->setCurrentIndex(qMax(0,sessionMode->findData(policy.value("session_limit_mode").toString("reset"))));
        for(auto i=schedules.begin();i!=schedules.end();++i){const bool session=i.key()=="session_limit";choices[i.key()]->setChecked(policy.value(i.key()).toBool(session));const auto patterns=policy.value("schedules").toObject();auto sequence=patterns.contains(i.key())?patterns.value(i.key()).toArray():session?QJsonArray{300,0}:policy.value("delays").toArray();if(!sequence.isEmpty()&&sequence.last().toInt()>0)sequence.append(-1);QStringList text;for(const auto &v:sequence)text<<QString::number(v.toInt());i.value()->setText(text.join(", "));}dirty=false;
    }
private:
    void refreshControls(){
        // Without a machine (a control-only computer alone) nothing can be edited.
        const bool usable=!base.isEmpty()&&m_sync->hasMachines(),on=usable&&enable->isChecked();
        enable->setEnabled(usable);for(auto i=choices.begin();i!=choices.end();++i){i.value()->setEnabled(on);schedules[i.key()]->setEnabled(on&&i.value()->isChecked()&&(i.key()!="session_limit"||sessionMode->currentData()=="interval"));}
        sessionMode->setEnabled(on&&choices["session_limit"]->isChecked());save->setEnabled(usable&&m_sync->canSave());
    }
    QComboBox *sessionMode=nullptr;
    RecoverySync *m_sync;QJsonObject base;bool dirty=false,saving=false;QCheckBox *enable;QMap<QString,QCheckBox *> choices;QMap<QString,QLineEdit *> schedules;QLabel *state,*message;QPushButton *save;
};
