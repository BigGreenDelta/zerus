#pragma once
#include "ChildProcess.h"
#include "RelayConfiguration.h"
#include <QApplication>
#include <QClipboard>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace RelaySettings {
class Panel : public QWidget {
public:
    explicit Panel(QWidget *parent=nullptr):Panel(configPath(),parent){}
    // An explicit path supports isolated tests; production uses the native file.
    explicit Panel(const QString &path,QWidget *parent=nullptr):QWidget(parent),store(path){
        setObjectName("relaySettings");
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,12,0);layout->setSpacing(16);
        auto *heading=new QLabel(tr("Mobile connection"));heading->setObjectName("heading");layout->addWidget(heading);
        auto *intro=new QLabel(tr("The computer connector uses this private configuration. Saving does not restart it or change the active connection."));
        intro->setWordWrap(true);layout->addWidget(intro);
        configured=new QLabel;configured->setObjectName("configuredRelay");configured->setTextFormat(Qt::PlainText);
        configured->setWordWrap(true);configured->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(configured);
        auto *pathLabel=new QLabel(tr("Configuration: %1").arg(store.path()));pathLabel->setTextFormat(Qt::PlainText);
        pathLabel->setWordWrap(true);pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(pathLabel);
        status=new QLabel(tr("Connector service: checking…"));status->setObjectName("connectorServiceStatus");status->setWordWrap(true);layout->addWidget(status);
        auto *form=new QFormLayout;form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        url=new QLineEdit;url->setObjectName("relayUrl");url->setPlaceholderText(canonicalRelay());form->addRow(tr("Relay (HTTPS)"),url);
        token=new QLineEdit;token->setObjectName("relayReplacementToken");token->setEchoMode(QLineEdit::Password);
        token->setPlaceholderText(tr("Leave blank to keep the configured credential"));form->addRow(tr("Replacement computer credential"),token);
        directory=new QLineEdit;directory->setObjectName("relayStateDirectory");form->addRow(tr("Private receipt directory"),directory);layout->addLayout(form);
        auto *bindingHint=new QLabel(tr("The official relay migration keeps the existing credential and receipts. A different relay or replacement credential requires a new, unused private receipt directory. Old receipts are never copied or deleted."));bindingHint->setWordWrap(true);layout->addWidget(bindingHint);
        auto *buttons=new QHBoxLayout;
        auto *official=new QPushButton(tr("Use official relay"));official->setObjectName("useOfficialRelay");buttons->addWidget(official);
        auto *freshDirectory=new QPushButton(tr("New receipt directory"));freshDirectory->setObjectName("newRelayStateDirectory");buttons->addWidget(freshDirectory);buttons->addStretch();layout->addLayout(buttons);
        connect(official,&QPushButton::clicked,this,[this]{url->setText(canonicalRelay());});
        connect(freshDirectory,&QPushButton::clicked,this,[this]{directory->setText(newStateDirectory());});
        auto *actions=new QHBoxLayout;saveButton=new QPushButton(tr("Save configuration"));saveButton->setObjectName("saveRelayConfiguration");actions->addWidget(saveButton);
        auto *reload=new QPushButton(tr("Reload configuration"));reload->setObjectName("reloadRelayConfiguration");actions->addWidget(reload);actions->addStretch();layout->addLayout(actions);
        notice=new QLabel;notice->setObjectName("relaySaveNotice");notice->setTextFormat(Qt::PlainText);notice->setWordWrap(true);layout->addWidget(notice);
        auto *instructions=new QLabel(tr("Changes apply when the connector restarts. Restart only after pending connector requests and receipts have drained."));instructions->setWordWrap(true);layout->addWidget(instructions);
#ifdef Q_OS_LINUX
        auto addCommand=[&](const QString &name,const QString &command,const QString &label){
            auto *row=new QWidget;row->setObjectName(name);auto *rowLayout=new QVBoxLayout(row);rowLayout->setContentsMargins(0,0,0,0);
            auto *text=new QLabel(command);text->setTextFormat(Qt::PlainText);text->setTextInteractionFlags(Qt::TextSelectableByMouse);text->setWordWrap(true);rowLayout->addWidget(text);
            auto *copy=new QPushButton(label);rowLayout->addWidget(copy,0,Qt::AlignLeft);
            connect(copy,&QPushButton::clicked,this,[command]{QApplication::clipboard()->setText(command);});
            layout->addWidget(row);return row;
        };
        addCommand(QStringLiteral("connectorStartCommand"),QStringLiteral("systemctl --user start zerus-mobile-connector.service"),tr("Copy start command"));
        killMode=new QLabel;killMode->setObjectName("connectorKillMode");killMode->setTextFormat(Qt::PlainText);killMode->setWordWrap(true);killMode->hide();layout->addWidget(killMode);
        // The default control-group kill mode would also stop the tmux servers that
        // phone launches start inside the service. Offer the restart only once the
        // unit reports KillMode=process; until then offer the non-restarting drop-in.
        protectCommand=addCommand(QStringLiteral("connectorProtectCommand"),QStringLiteral("mkdir -p ~/.config/systemd/user/zerus-mobile-connector.service.d && printf '[Service]\\nKillMode=process\\n' > ~/.config/systemd/user/zerus-mobile-connector.service.d/50-zerus-killmode.conf && systemctl --user daemon-reload"),tr("Copy protection command"));protectCommand->hide();
        restartCommand=addCommand(QStringLiteral("connectorRestartCommand"),QStringLiteral("systemctl --user restart zerus-mobile-connector.service"),tr("Copy restart command"));restartCommand->hide();
#endif
        auto *docs=new QLabel(tr("Connector setup is documented in docs/mobile-deployment.md in the Zerus source checkout."));docs->setWordWrap(true);layout->addWidget(docs);
        layout->addStretch();
        connect(reload,&QPushButton::clicked,this,[this]{loadConfiguration();probeService();});
        connect(saveButton,&QPushButton::clicked,this,[this]{saveConfiguration();});
        loadConfiguration();probeService();
    }
    ~Panel() override{
        // QWidget destroys labels before later-created child processes. Cancel
        // this read-only probe while every callback target is still alive.
        const auto processes=findChildren<QProcess*>(QString{},Qt::FindDirectChildrenOnly);
        probe=nullptr;
        for(auto *p:processes){
            for(auto *timer:p->findChildren<QTimer*>()){timer->stop();timer->disconnect();}
            p->disconnect();
            if(p->state()!=QProcess::NotRunning){p->kill();p->waitForFinished(1000);}
            delete p;
        }
    }
private:
    ConfigStore store;Snapshot loaded;bool loadedOk=false;
    QLabel *configured,*status,*notice,*killMode=nullptr;
    QWidget *protectCommand=nullptr,*restartCommand=nullptr;
    QLineEdit *url,*token,*directory;
    QPushButton *saveButton;
    QProcess *probe=nullptr;QByteArray probeOutput;
    void loadConfiguration(){
        QString error;loadedOk=store.load(&loaded,&error);saveButton->setEnabled(loadedOk);
        token->clear();
        if(!loadedOk){configured->setText(tr("Configured relay: unavailable"));notice->setText(error);return;}
        const auto relay=loaded.object.value("server_url").toString();
        // Invalid existing URLs may contain secrets. Never render them.
        configured->setText(loaded.exists?(validUrl(relay)?tr("Configured relay: %1").arg(relay):tr("Configured relay: invalid configuration")):tr("Configured relay: not configured"));
        url->setText(validUrl(relay)?relay:canonicalRelay());
        auto state=loaded.object.value("state_dir").toString(defaultStateDirectory());if(state.isEmpty())state=defaultStateDirectory();
        directory->setText(loaded.exists?state:newStateDirectory());
        notice->setText(loaded.exists?tr("The configured computer credential stays hidden. Leave replacement blank to retain it for this relay."):tr("Set up this computer on the relay, then enter its issued computer credential. A phone pairing credential cannot be used here."));
    }
    void saveConfiguration(){
        if(!loadedOk)return;
        QString error;auto prepared=prepare(loaded,{url->text(),token->text(),directory->text()},&error);
        if(!prepared){notice->setText(error);return;}
        Snapshot saved;
        if(!store.save(loaded,*prepared,&saved,&error)){notice->setText(error);return;}
        loaded=std::move(saved);token->clear();
        configured->setText(tr("Configured relay: %1").arg(loaded.object.value("server_url").toString()));
        notice->setText(tr("Configuration saved privately. Changes apply when the connector restarts. Service status does not verify relay connectivity or that this configuration is active."));
    }
    void showKillMode(const QByteArray &mode,bool loaded){
        if(!killMode)return;
        const bool safe=loaded&&mode=="process";
        killMode->setVisible(loaded);restartCommand->setVisible(safe);protectCommand->setVisible(loaded&&!safe);
        static const QList<QByteArray> known{"control-group","mixed","none"};
        killMode->setText(safe?tr("A restart stops only the connector process (KillMode=process). Native agents keep running.")
            :tr("The service reports KillMode=%1, not process: restarting it can also stop agents launched from the phone. Protect them first. This command adds a drop-in and reloads the user service manager without restarting anything; then reload this panel.").arg(known.contains(mode)?QString::fromLatin1(mode):tr("unknown")));
    }
    void probeService(){
#ifdef Q_OS_LINUX
        if(probe)return;
        showKillMode({},false);
        auto *p=new QProcess(this);probe=p;probeOutput.clear();
        auto *deadline=new QTimer(p);deadline->setSingleShot(true);
        connect(deadline,&QTimer::timeout,this,[this,p]{if(probe==p){status->setText(tr("Connector service: unknown (status check timed out)"));p->kill();}});
        connect(p,&QProcess::errorOccurred,this,[this,p](QProcess::ProcessError){
            if(probe==p)status->setText(tr("Connector service: unknown (systemctl is unavailable)"));
            if(p->state()==QProcess::NotRunning){probe=nullptr;p->deleteLater();}
        });
        connect(p,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,p,deadline](int,QProcess::ExitStatus exit){
            deadline->stop();
            if(probe!=p)return;
            const auto output=probeOutput+p->readAllStandardOutput();
            if(exit==QProcess::NormalExit&&output.size()<=16384){
                QMap<QByteArray,QByteArray> values;
                for(const auto &line:output.split('\n')){const auto at=line.indexOf('=');if(at>0)values.insert(line.left(at),line.mid(at+1));}
                showKillMode(values.value("KillMode"),values.value("LoadState")=="loaded");
                if(values.value("LoadState")=="not-found")status->setText(tr("Connector service: not installed. Follow the setup documentation."));
                else if(values.value("LoadState")=="loaded"){
                    const auto active=values.value("ActiveState");
                    if(active=="active")status->setText(tr("Connector service: running. Relay connectivity is not verified here."));
                    else if(active=="inactive")status->setText(tr("Connector service: stopped"));
                    else if(active=="failed")status->setText(tr("Connector service: failed. Review its private service logs."));
                    else status->setText(tr("Connector service: changing state or unknown"));
                }else status->setText(tr("Connector service: unknown (user service manager unavailable)"));
            }
            probe=nullptr;p->deleteLater();
        });
        connect(p,&QProcess::readyReadStandardOutput,this,[this,p]{
            if(probe!=p)return;
            const auto chunk=p->readAllStandardOutput();
            if(probeOutput.size()+chunk.size()>16384){status->setText(tr("Connector service: unknown (status response exceeded its limit)"));p->kill();}
            else probeOutput+=chunk;
        });
        connect(p,&QProcess::readyReadStandardError,p,[p]{p->readAllStandardError();});
        p->setProcessChannelMode(QProcess::SeparateChannels);ChildProcess::inheritStdioOnly(*p);
        p->start(QStringLiteral("systemctl"),{QStringLiteral("--user"),QStringLiteral("show"),QStringLiteral("zerus-mobile-connector.service"),QStringLiteral("--property=LoadState,ActiveState,SubState,KillMode"),QStringLiteral("--no-pager")});
        deadline->start(3000);
#else
        status->setText(tr("Connector service: unknown (automatic status checks are available on Linux). Follow the setup documentation."));
#endif
    }
};
}
