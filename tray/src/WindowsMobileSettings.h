#pragma once
#include "ChildProcess.h"
#include "RelayConfiguration.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

// The bundled connector owns Windows ACLs and lifecycle. This widget never
// writes credentials or persists them in QSettings. All I/O is asynchronous.
namespace WindowsMobileSettings {
class Panel : public QWidget {
public:
    explicit Panel(QWidget *parent=nullptr):Panel(
        QCoreApplication::applicationDirPath()+"/zerus-mobile-connector.exe",
        RelaySettings::configPath(),parent){}
    Panel(QString connector,QString configuration,QWidget *parent=nullptr)
        :QWidget(parent),program(std::move(connector)),path(std::move(configuration)){
        setObjectName("windowsMobileSettings");
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,12,0);layout->setSpacing(16);
        auto label=[&](const QString &text){auto *v=new QLabel(text);v->setWordWrap(true);v->setTextFormat(Qt::PlainText);layout->addWidget(v);return v;};
        label(tr("Mobile connection"))->setObjectName("heading");
        label(tr("Connect this Windows PC directly to the relay. Enable local sessions in Machines to control its Codex, Claude and Kimi sessions from a phone."));
        label(tr("Enter the computer credential issued by your relay. Saving does not start the connector. Start and Stop affect only the connector; closing Zerus keeps it and native sessions running."));
        label(tr("Relay (HTTPS)"));url=new QLineEdit;url->setObjectName("relayUrl");url->setText(RelaySettings::canonicalRelay());layout->addWidget(url);
        label(tr("Replacement computer credential"));token=new QLineEdit;token->setObjectName("relayReplacementToken");token->setEchoMode(QLineEdit::Password);token->setPlaceholderText(tr("Leave blank to keep the configured credential"));layout->addWidget(token);
        state=label(tr("Connector: checking…"));state->setObjectName("connectorServiceStatus");
        notice=label({});notice->setObjectName("relaySaveNotice");
        auto *row=new QHBoxLayout;layout->addLayout(row);
        save=new QPushButton(tr("Save configuration"));save->setObjectName("saveRelayConfiguration");row->addWidget(save);
        start=new QPushButton(tr("Start connector"));start->setObjectName("startMobileConnector");row->addWidget(start);
        stop=new QPushButton(tr("Stop connector"));stop->setObjectName("stopMobileConnector");row->addWidget(stop);
        reload=new QPushButton(tr("Reload status"));reload->setObjectName("reloadMobileConnector");row->addWidget(reload);row->addStretch();
        label(tr("Start the connector after each Windows login. It uses a private receipt journal. Stop waits for its current phone request; reload status to confirm it has stopped before changing configuration."));
        label(tr("This Windows gateway controls existing local sessions. Mobile session creation, folder/worktree operations, peer routing, DeepSeek, OpenCode and voice are unavailable."));
        connect(reload,&QPushButton::clicked,this,[this]{request("status");});
        connect(save,&QPushButton::clicked,this,[this]{
            if(!RelaySettings::validUrl(url->text())){notice->setText(tr("Enter an HTTPS relay URL without credentials, query or fragment."));return;}
            if(!token->text().isEmpty()&&!RelaySettings::validToken(token->text())){notice->setText(tr("Supply the computer credential issued by this relay."));return;}
            request("save",{{"server_url",RelaySettings::normalizedUrl(url->text())},{"node_token",token->text()}});
        });
        connect(start,&QPushButton::clicked,this,[this]{request("start");});
        connect(stop,&QPushButton::clicked,this,[this]{request("stop",{{"generation",snapshot.value("generation")}});});
        request("status");
    }
    ~Panel() override {
        if(process){process->disconnect();process->kill();process->waitForFinished(1000);}
    }
private:
    static bool hex(const QJsonValue &value,int size){
        if(!value.isString()||value.toString().size()!=size)return false;
        for(const auto c:value.toString())if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        return true;
    }
    static bool statusShape(const QJsonObject &result){
        if((result.contains("ok")&&result.value("ok")!=QJsonValue(true))
            ||!result.value("configured").isBool()||!result.value("running").isBool()
            ||result.value("startup_at_login")!=QJsonValue(false)||result.contains("stopping"))return false;
        const bool configured=result.value("configured").toBool(),running=result.value("running").toBool();
        if(!configured)return !running&&result.value("revision")==QJsonValue(QString())
            &&(result.value("generation").isUndefined()||result.value("generation").isNull())
            &&(result.value("server_url").isUndefined()||result.value("server_url").isNull())
            &&(result.value("state_dir").isUndefined()||result.value("state_dir").isNull());
        return hex(result.value("revision"),64)&&result.value("server_url").isString()
            &&RelaySettings::validUrl(result.value("server_url").toString())
            // A manually supplied configuration may use the connector's
            // default state directory, reported as null or an empty string.
            &&(result.value("state_dir").isNull()||result.value("state_dir").isString())
            &&(running?hex(result.value("generation"),32):result.value("generation").isNull());
    }
    static bool confirmed(const QString &action,const QJsonObject &result,const QJsonObject &sent,const QJsonObject &previous){
        if(action=="stop"&&result.value("stopping")==QJsonValue(true)){
            return result.keys().size()==2&&hex(result.value("generation"),32)
                &&result.value("generation")==sent.value("generation");
        }
        if(!statusShape(result))return false;
        if(action=="status")return true;
        if(action=="save")return result.value("configured")==QJsonValue(true)
            &&result.value("running")==QJsonValue(false)&&result.value("server_url")==sent.value("server_url");
        if(result.value("revision")!=sent.value("revision")||result.value("configured")!=QJsonValue(true)
            ||result.value("state_dir")!=previous.value("state_dir")
            ||result.value("server_url")!=previous.value("server_url"))return false;
        return action=="start"?result.value("running")==QJsonValue(true):action=="stop"&&result.value("running")==QJsonValue(false);
    }
    QString program,path;QJsonObject snapshot;QProcess *process=nullptr;
    QLineEdit *url,*token;QLabel *state,*notice;QPushButton *save,*start,*stop,*reload;
    void controls(bool busy){
        const bool running=snapshot.value("running").toBool();
        url->setReadOnly(busy||running);token->setReadOnly(busy||running);
        save->setEnabled(!busy&&!running);start->setEnabled(!busy&&!running&&snapshot.value("configured").toBool());
        stop->setEnabled(!busy&&running&&!snapshot.value("generation").toString().isEmpty());reload->setEnabled(!busy);
    }
    void request(const QString &action,QJsonObject payload={}){
        if(process)return;
        payload.insert("revision",snapshot.value("revision").toString());
        auto *p=new QProcess(this);process=p;controls(true);
        auto environment=QProcessEnvironment::systemEnvironment();
        for(const auto &key:{"HGS_SESSION","HGS_RUN_ID","HGS_EXPECTED_ID","HGS_AGENT","HGS_HOST_PANE","HGS_HOST_SESSION","TMUX","TMUX_PANE"})environment.remove(QLatin1String(key));
        p->setProcessEnvironment(environment);p->setProcessChannelMode(QProcess::SeparateChannels);
        auto *deadline=new QTimer(p);deadline->setSingleShot(true);
        auto output=std::make_shared<QByteArray>();
        const auto previous=snapshot;
        auto finish=[this,p,deadline,output,action,payload,previous](bool success){
            if(process!=p)return;
            deadline->stop();process=nullptr;
            const auto bytes=*output+p->readAllStandardOutput();
            const auto doc=QJsonDocument::fromJson(bytes);const auto result=doc.object();
            if(!success||!doc.isObject()||bytes.size()>65536||!confirmed(action,result,payload,previous)){
                notice->setText(tr("Connector operation was not confirmed. Reload status before retrying. Check that the portable ZIP includes zerus-mobile-connector.exe and private configuration is available."));
                state->setText(tr("Connector: status unknown"));
                // An uncertain start/save/stop grants no retry authority. Only
                // explicit status reload can unlock another operation.
                controls(true);reload->setEnabled(true);
            }else if(result.value("stopping").toBool()){
                notice->setText(tr("Stop requested. The current phone request will finish before the connector exits. Reload status to confirm."));controls(true);reload->setEnabled(true);
            }else{
                snapshot=result;
                if(action=="save"||action=="status"){
                    if(result.value("configured").toBool())url->setText(result.value("server_url").toString());
                    token->clear();
                }
                state->setText(result.value("running").toBool()?tr("Connector: running. Relay connectivity is not verified here."):tr("Connector: stopped"));
                notice->setText(action=="save"?tr("Configuration saved privately. Start the connector, then refresh Phones below."):QString());controls(false);
            }
            p->deleteLater();
        };
        connect(deadline,&QTimer::timeout,p,[p]{p->kill();});
        connect(p,&QProcess::readyReadStandardOutput,p,[p,output]{*output+=p->readAllStandardOutput();if(output->size()>65536)p->kill();});
        connect(p,&QProcess::readyReadStandardError,p,[p]{p->readAllStandardError();});
        connect(p,&QProcess::started,p,[p,payload]{p->write(QJsonDocument(payload).toJson(QJsonDocument::Compact));p->closeWriteChannel();});
        connect(p,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[finish](int code,QProcess::ExitStatus status){finish(code==0&&status==QProcess::NormalExit);});
        connect(p,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(false);});
        ChildProcess::inheritStdioOnly(*p);p->start(program,{"--manage",action,"--config",path});deadline->start(12000);
    }
};
}
