#pragma once
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QPushButton>
#include "WorkspaceIcons.h"
#include <cmath>

namespace SessionUsage {
class ContextButton : public QPushButton {
public:
    explicit ContextButton(QWidget *parent=nullptr):QPushButton(parent) {
        setFixedHeight(24);setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);setCursor(Qt::PointingHandCursor);setIconSize(QSize(12,12));setData({});
    }
    void setTheme(bool dark) {if(m_initialized && m_dark==dark)return;m_dark=dark;render();}
    void setData(const QJsonObject &usage,bool recorded=false) {
        const auto context=usage.value("context").toObject();
        if(m_initialized && m_context==context && m_recorded==recorded)return;
        m_context=context;m_recorded=recorded;render();
    }
private:
    static bool valid(const QJsonValue &v) {return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble()>=0 && v.toDouble()<=9007199254740991.;}
    static QString compact(double n) {
        if(n<1000)return QLocale().toString(qint64(n));
        QString number=QLocale().toString(n/(n>=1000000?1000000.:1000.),'f',1);
        if(number.endsWith(QLocale().zeroDigit())){number.chop(QLocale().zeroDigit().size());number.chop(QLocale().decimalPoint().size());}
        return number+(n>=1000000?"M":"k");
    }
    void render() {
        m_initialized=true;
        const bool known=valid(m_context.value("used"));
        const bool capacity=valid(m_context.value("limit")) && m_context.value("limit").toDouble()>0;
        const double used=m_context.value("used").toDouble(),limit=m_context.value("limit").toDouble();
        const double percent=known && capacity ? 100.*used/limit : -1.;
        const QString percentage=percent>=0?QLocale().toString(percent,'f',1)+"%":QStringLiteral("—%");
        const QString approximate=m_context.value("estimated").toBool()?QStringLiteral("≈"):QString();
        setText(!known ? QString() : capacity
            ? QString("%1%2% (%3/%4)").arg(approximate,QLocale().toString(percent,'f',0),compact(used),compact(limit))
            : approximate+compact(used));
        setIcon(known && !capacity ? workspaceIcon("context-warning",QColor(m_dark?"#edbd77":"#9b6216")) : QIcon());
        setEnabled(known); setFocusPolicy(known ? Qt::StrongFocus : Qt::NoFocus);
        QStringList hint;
        if(known) {
            hint<<tr("Latest reported context: %1 tokens").arg(QLocale().toString(qint64(used)));
            if(capacity)hint<<tr("Context window: %1 tokens / %2 used").arg(QLocale().toString(qint64(limit)),percentage);
            else hint<<tr("The provider has not reported the window size. Percentage is unavailable.");
            if(m_context.value("estimated").toBool())hint<<tr("Approximate context size reported by the provider.");
            if(m_context.value("limit_source")=="model_config")hint<<tr("Window size from this model's native configuration.");
            if(capacity && m_context.value("limit_source")=="native_statusline")
                hint<<tr("Window size that Claude Code's status line reports for %1.").arg(m_context.value("limit_model").toString());
            if(m_recorded)hint<<tr("Last recorded values. Live context is not confirmed.");
        } else hint<<tr("The provider has not reported context usage for this conversation yet.");
        hint<<tr("Open session usage in Details.");setToolTip(hint.join('\n'));setAccessibleName(tr("Context: %1").arg(text())+(known && !capacity ? tr(" (capacity unknown)") : QString()));setAccessibleDescription(toolTip());
        setProperty("contextPercent",percent>=0?QVariant(percent):QVariant());setProperty("contextRecorded",m_recorded);
        const QString color=!known || !capacity || m_recorded ? (m_dark?"#a1adbb":"#647386")
            : percent>=90 ? (m_dark?"#f07878":"#ce3d47") : percent>=70 ? (m_dark?"#f0a35b":"#bd6519") : (m_dark?"#72cdb2":"#237a62");
        setStyleSheet(QString("QPushButton{font-size:11px;text-align:right;padding:0 6px;min-height:0;color:%1;background:transparent;border:1px solid transparent;border-radius:5px;} QPushButton:hover{background:%2;border-color:%3;} QPushButton:pressed{background:%3;} QPushButton:focus[keyboardFocus=\"true\"]{border-color:%1;}")
            .arg(color,m_dark?"#27313b":"#e8edf3",m_dark?"#36434e":"#d7e0e6"));
    }
    QJsonObject m_context;bool m_dark=true,m_recorded=false,m_initialized=false;
};
inline QString html(const QJsonObject &usage, const QString &muted, bool includeContext = true) {
    const auto tr=[](const char *text){return QObject::tr(text);};
    QString out="<h3>"+tr("Session usage")+"</h3>";
    if (usage.value("status")!="ok")
        return out+QString("<p style='color:%1'>%2</p>").arg(muted,tr("No native usage reported for this conversation yet."));
    const auto totals=usage.value("totals").toObject();
    const auto format=[](QJsonValue value) {
        return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble()>=0
            ? QLocale().toString(qint64(value.toDouble())) : QObject::tr("Not reported");
    };
    out+="<table width='100%' cellspacing='0' cellpadding='3'>";
    const auto row=[&](const QString &label,const QString &value){out+="<tr><td>"+label.toHtmlEscaped()+"</td><td align='right'>"+value.toHtmlEscaped()+"</td></tr>";};
    row(tr("Total tokens"),format(totals.value("total")));
    for (const auto &field: {qMakePair("input","Input (including cache)"),qMakePair("uncached_input","Uncached input"),
            qMakePair("cache_read","Cache read"),qMakePair("cache_write","Cache write"),qMakePair("output","Output"),
            qMakePair("reasoning","Reasoning (in output)"),qMakePair("web_searches","Web searches"),qMakePair("web_fetches","Web fetches")})
        if (totals.value(field.first).isDouble()) row(tr(field.second),format(totals.value(field.first)));
    if (usage.value("requests").isDouble()) row(tr("Recorded requests"),format(usage.value("requests")));
    row(usage.value("cost_usd").isDouble()?tr("Native cost estimate"):tr("Cost"),usage.value("cost_usd").isDouble() ? QLocale().toCurrencyString(usage.value("cost_usd").toDouble(),"USD ",4) : tr("Not reported"));
    const auto extras=usage.value("extras").toObject();
    for (const auto &field:{qMakePair("api_ms","API time"),qMakePair("tool_ms","Tool time")})
        if(extras.value(field.first).isDouble())row(tr(field.second),tr("%1 s").arg(QLocale().toString(extras.value(field.first).toDouble()/1000,'f',1)));
    for (const auto &field:{qMakePair("lines_added","Lines added"),qMakePair("lines_removed","Lines removed")})
        if(extras.value(field.first).isDouble())row(tr(field.second),format(extras.value(field.first)));
    out+="</table>";
    if(usage.value("cost_usd").isDouble())out+=QString("<p style='font-size:11px;color:%1'>%2</p>").arg(muted,
        usage.value("cost_incomplete").toBool()?tr("The provider reports incomplete cost data."):
        usage.value("cost_stale").toBool()?tr("Last saved native cost snapshot. Newer requests are not included yet."):tr("Provider estimate from its latest saved snapshot; may differ from the bill."));
    const auto context=usage.value("context").toObject();
    if (includeContext && context.value("used").isDouble()) {
        QString text=format(context.value("used"));
        if (context.value("limit").toDouble()>0) text+=QString(" / %1 (%2%)").arg(format(context.value("limit")),QLocale().toString(100*context.value("used").toDouble()/context.value("limit").toDouble(),'f',1));
        out+="<p><b>"+tr("Latest context")+"</b><br>"+(context.value("estimated").toBool()?tr("Approx. "):QString())+text+"</p>";
    }
    out+=QString("<p style='font-size:11px;color:%1'>%2</p>").arg(muted,usage.value("partial").toBool()
        ? tr("Reading session history. Totals are incomplete.") : tr("This conversation only. Subagent usage is separate. Cache is included in input; reasoning is included in output."));
    return out;
}
}
