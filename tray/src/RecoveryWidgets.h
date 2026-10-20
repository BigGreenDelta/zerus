#pragma once
#include "HgsClient.h"
#include "SessionPresentation.h"
#include "ComposerToolbar.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace RecoveryUi {
// The agent accepted the continuation and the provider served it again. The
// episode stays open until the turn ends only to keep its attempt budget.
inline bool resumed(const QJsonObject &job)
{
    return job.value("state") == "retrying" && job.value("acknowledged").toBool() && job.value("resumed_at").toDouble() > 0;
}
inline QString status(const QJsonObject &job)
{
    const auto state = job.value("state").toString();
    if (state == "waiting") {
        if(job.value("class")=="session_limit" && job.value("session_limit_mode")=="reset")
            return QObject::tr("Waiting for reset at %1").arg(QDateTime::fromMSecsSinceEpoch(qint64(job.value("due_at").toDouble()*1000)).toLocalTime().toString("d MMM HH:mm"));
        const auto remaining = qMax(0, int(std::ceil(job.value("due_at").toDouble() - QDateTime::currentMSecsSinceEpoch()/1000.0)));
        return remaining > 0 ? QObject::tr("Retry in %1 s").arg(remaining) : QObject::tr("Waiting to retry");
    }
    if (state == "dispatching") return QObject::tr("Sending recovery request…");
    if (state == "retrying") return !job.value("acknowledged").toBool() ? QObject::tr("Waiting for delivery confirmation…")
        : resumed(job) ? QObject::tr("Agent resumed after recovery") : QObject::tr("Recovery in progress");
    if (state == "exhausted") return QObject::tr("Automatic attempts exhausted");
    if (state == "uncertain") return QObject::tr("Check the agent before retrying");
    if (state == "blocked") return QObject::tr("Automatic recovery needs attention");
    if (state == "cancelled") return QObject::tr("Automatic recovery cancelled");
    if (state == "succeeded") return QObject::tr("Recovered");
    return QObject::tr("Automatic recovery is not scheduled");
}

class Panel : public QFrame {
public:
    std::function<void(const QJsonObject &)> action;
    std::function<void()> openSettings;
    std::function<void()> openTerminal;
    std::function<void()> refreshUsage;
    // The toolbar chip follows the panel: called on every refresh, also while hidden.
    std::function<void()> summaryChanged;
    struct Summary { QString label, shortLabel; ChipTone tone; };
    explicit Panel(QWidget *parent=nullptr):QFrame(parent) {
        setObjectName("recoveryPanel"); setFrameShape(QFrame::StyledPanel);setFocusPolicy(Qt::StrongFocus);
        auto *layout=new QVBoxLayout(this); layout->setContentsMargins(14,10,14,10); layout->setSpacing(8);
        m_status=new QLabel; m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true); layout->addWidget(m_status);
        m_detail=new QLabel; m_detail->setTextFormat(Qt::PlainText);m_detail->setWordWrap(true); layout->addWidget(m_detail);
        auto *row=new QHBoxLayout; m_now=new QPushButton(tr("Retry now"));m_now->setObjectName("recoveryNow");m_cancel=new QPushButton(tr("Cancel retry"));m_cancel->setObjectName("recoveryCancel");
        m_history=new QPushButton(tr("Attempts…"));m_history->setObjectName("recoveryHistory");
        m_terminal=new QPushButton(tr("Open Terminal"));m_terminal->setObjectName("providerErrorTerminal");
        m_usage=new QPushButton(tr("Refresh usage"));m_usage->setObjectName("providerErrorRefresh");
        m_help=new QLabel;m_help->setObjectName("providerErrorHelp");m_help->setWordWrap(true);m_help->setTextFormat(Qt::PlainText);layout->addWidget(m_help);
        auto *settings=new QPushButton(tr("Settings…"));m_settings=settings;
        row->addWidget(m_now);row->addWidget(m_cancel);row->addWidget(m_history);row->addWidget(m_terminal);row->addWidget(m_usage);row->addStretch();row->addWidget(settings);layout->addLayout(row);
        connect(m_terminal,&QPushButton::clicked,this,[this]{if(openTerminal)openTerminal();});
        connect(m_usage,&QPushButton::clicked,this,[this]{if(refreshUsage)refreshUsage();});
        connect(m_history,&QPushButton::clicked,this,[this]{
            // Close the popover first and centre the dialog on the window: a dialog parented to a
            // hidden popup has no place, and reopening a popup without user input is unreliable.
            auto *popover=dynamic_cast<ChipPopover *>(window());
            QDialog dialog(popover&&popover->parentWidget()?popover->parentWidget()->window():static_cast<QWidget *>(this));
            if(popover)popover->hide();
            dialog.setObjectName("recoveryHistoryDialog");dialog.setWindowTitle(tr("Recovery attempts — Zerus"));dialog.resize(560,360);
            auto *layout=new QVBoxLayout(&dialog);layout->setContentsMargins(20,20,20,20);layout->setSpacing(12);
            auto *history=new QPlainTextEdit;history->setReadOnly(true);QStringList entries;
            for(const auto &value:m_job.value("history").toArray()) {
                const auto entry=value.toObject();const auto state=entry.value("state").toString();
                const auto label=state=="waiting"?tr("Retry scheduled"):RecoveryUi::status(entry);
                entries<<tr("%1  %2 (%3 attempts)").arg(QDateTime::fromMSecsSinceEpoch(qint64(entry.value("at").toDouble()*1000)).toLocalTime().toString("d MMM HH:mm:ss"),label).arg(entry.value("attempt").toInt());
                if(!entry.value("reason").toString().isEmpty())entries<<entry.value("reason").toString();
            }
            history->setPlainText(entries.join('\n'));layout->addWidget(history);
            auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);dialog.exec();
        });
        connect(settings,&QPushButton::clicked,this,[this]{if(openSettings)openSettings();});
        for(auto *button:{m_now,m_cancel})connect(button,&QPushButton::clicked,this,[this,button]{
            if(!action)return; m_pending=true;refresh(); action({{"name",m_job.value("name")},{"id",m_job.value("id")},{"action",button==m_now?"now":"cancel"}});
        });
        auto *timer=new QTimer(this);connect(timer,&QTimer::timeout,this,[this]{if(m_active)refresh();});timer->start(1000);
    }
    void setState(const QJsonObject &details,bool online) {
        const auto job=details.value("recovery").toObject();
        if(job.value("id")!=m_job.value("id")) {m_pending=false;m_error.clear();}
        m_job=job;m_online=online;
        m_failure=details.value("phase")=="error"?details.value("provider_error").toObject():QJsonObject();
        m_active=(!job.isEmpty() && job.value("state")!="succeeded" && !RecoveryUi::resumed(job)) || !m_failure.isEmpty(); refresh();
    }
    void finished(const QString &error) { m_pending=false;m_error=error;refresh(); }
    void setUsageNotice(const QString &notice) { if(m_usageNotice!=notice){m_usageNotice=notice;refresh();} }
    bool active() const { return m_active; }
    QString detailText() const { return QStringList{m_status->text(),m_detail->text()}.join('\n').trimmed(); }
    Summary summary() const {
        if(isFailure())return {tr("Provider error"),tr("Error"),ChipTone::Danger};
        const auto state=m_job.value("state").toString();const auto label=RecoveryUi::status(m_job);
        if(state=="waiting"||state=="dispatching"||state=="retrying") {
            const int remaining=qMax(0,int(std::ceil(m_job.value("due_at").toDouble()-QDateTime::currentMSecsSinceEpoch()/1000.0)));
            if(state=="waiting" && m_job.value("class")=="session_limit" && m_job.value("session_limit_mode")=="reset")
                return {label,tr("Reset %1").arg(QDateTime::fromMSecsSinceEpoch(qint64(m_job.value("due_at").toDouble()*1000)).toLocalTime().toString("HH:mm")),ChipTone::Warning};
            return {label,state=="waiting"&&remaining>0?tr("%1 s").arg(remaining):QStringLiteral("…"),ChipTone::Warning};
        }
        if(state=="cancelled")return {label,tr("Off"),ChipTone::Warning};
        return {label,tr("Error"),ChipTone::Danger};
    }
private:
    void refresh() {
        const bool failure=isFailure();
        const bool sessionLimit=m_failure.value("error_kind")=="session_limit";
        m_terminal->setVisible(failure);m_usage->setVisible(failure && (m_failure.value("error_kind")=="quota"||sessionLimit));
        m_terminal->setEnabled(m_online);m_usage->setEnabled(m_online);
        m_help->setVisible(failure);m_settings->setVisible(!failure||sessionLimit);m_history->setVisible(!failure);
        if(failure) {
            m_status->setText(SessionPresentation::providerFailure(m_failure));
            const auto detail=m_failure.value("detail").toString();
            m_detail->setText(detail.size()>500?detail.left(500)+QStringLiteral("…"):detail);m_detail->setToolTip(detail);
            m_help->setText(sessionLimit
                ? tr("The session usage limit stopped this turn. Automatic recovery can wait for reset or retry on a schedule when enabled in Settings. Your draft is kept.")
                : m_failure.value("error_kind")=="quota"
                ? tr("The provider stopped this turn. Wait for the limit to reset or restore account access, then refresh usage and send a message to continue. Your draft is kept; Zerus will not retry automatically.")
                : tr("The provider stopped this turn. Check Terminal and resolve the error, then send a message to continue. Your draft is kept."));
            if(!m_usageNotice.isEmpty() && (sessionLimit || m_failure.value("error_kind")=="quota"))m_help->setText(m_usageNotice+"\n"+m_help->text());
            m_now->hide();m_cancel->setVisible(m_job.value("state")=="waiting");m_cancel->setEnabled(m_online&&!m_pending);notify();return;
        }
        const auto state=m_job.value("state").toString(); const bool waiting=state=="waiting";
        if(!waiting&&(m_now->hasFocus()||m_cancel->hasFocus()))setFocus(Qt::OtherFocusReason);
        const auto delays=m_job.value("delays").toArray();const bool infinite=!delays.isEmpty()&&delays.last().toInt()==0;
        const int count=int(delays.size())-(!delays.isEmpty()&&delays.last().toInt()<=0?1:0);
        const bool reset=m_job.value("class")=="session_limit"&&m_job.value("session_limit_mode")=="reset";
        m_status->setText(RecoveryUi::status(m_job)+(reset?tr(" (%1 attempts)").arg(m_job.value("attempt").toInt()):infinite?tr(" (%1 attempts, repeats until stopped)").arg(m_job.value("attempt").toInt()):tr(" (%1/%2 attempts)").arg(m_job.value("class_attempt").toInt(m_job.value("attempt").toInt())).arg(count)));
        m_detail->setText(!m_error.isEmpty()?m_error:(!m_job.value("reason").toString().isEmpty()?m_job.value("reason").toString():
            (m_job.value("action")=="retry_request"?tr("Retry the failed provider request"):tr("Send a continuation message in this conversation"))));
        if(sessionLimit) m_detail->setText(m_failure.value("detail").toString()+"\n"+m_detail->text());
        m_now->setVisible(waiting);m_cancel->setVisible(waiting);
        m_history->setEnabled(!m_job.value("history").toArray().isEmpty());
        m_now->setEnabled(waiting&&m_online&&!m_pending&&m_job.value("not_before").toDouble()<=QDateTime::currentMSecsSinceEpoch()/1000.0);
        m_cancel->setEnabled(waiting&&m_online&&!m_pending);
        notify();
    }
    bool isFailure() const {
        return !m_failure.isEmpty() && (m_job.isEmpty() || m_job.value("state")=="succeeded" || RecoveryUi::resumed(m_job) || m_failure.value("error_kind")=="quota"
            || (m_failure.value("error_kind")=="session_limit" && m_job.value("class")!="session_limit"));
    }
    void notify() { if(summaryChanged)summaryChanged(); }
    QJsonObject m_job,m_failure; bool m_online=false,m_pending=false,m_active=false; QString m_error,m_usageNotice;
    QLabel *m_status,*m_detail,*m_help; QPushButton *m_now,*m_cancel,*m_history,*m_terminal,*m_usage,*m_settings;
};
}
