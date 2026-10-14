#pragma once
#include <QDateTime>
#include <QFrame>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLocale>
#include <QPainterPath>
#include <QSettings>
#include <cmath>
#include <optional>
#include <QTimer>
#include "IdentityBadge.h"
#include "WorkspaceIcons.h"

namespace AccountUsage {
inline QString credits(const QString &value) {
    bool ok=false;const double number=value.toDouble(&ok);
    return ok && std::isfinite(number)?QLocale().toString(number,'f',2):QStringLiteral("—");
}
inline QString period(const QJsonObject &window) {
    const int minutes=window.value("window_minutes").toInt();
    const QString duration=minutes==10080 ? QStringLiteral("7d") : minutes && minutes%1440==0 ? QString::number(minutes/1440)+"d"
        : minutes && minutes%60==0 ? QString::number(minutes/60)+"h" : minutes ? QString::number(minutes)+"m" : QString();
    const QString name=window.value("label").toString();
    return name.isEmpty() ? (duration.isEmpty() ? QObject::tr("Usage") : duration) : name+(duration.isEmpty()?QString():" "+duration);
}
inline QDateTime reset(const QJsonValue &value) {
    if (value.isDouble()) return QDateTime::fromSecsSinceEpoch(qint64(value.toDouble()));
    return QDateTime::fromString(value.toString(),Qt::ISODate).toLocalTime();
}
inline bool current(const QJsonObject &w) { const auto at=reset(w.value("resets_at"));return !at.isValid() || at>QDateTime::currentDateTime(); }
inline QString remaining(const QDateTime &at) {
    if(!at.isValid())return QObject::tr("reset unknown");
    const qint64 minutes=qMax<qint64>(0,(QDateTime::currentDateTime().secsTo(at)+59)/60);
    if(!minutes)return QObject::tr("refresh needed");
    if(minutes>=1440)return QObject::tr("%1d %2h").arg(minutes/1440).arg((minutes%1440)/60);
    if(minutes>=60)return QObject::tr("%1h %2m").arg(minutes/60).arg(minutes%60);
    return QObject::tr("%1m").arg(minutes);
}
// Footer countdown: only the largest whole unit fits next to the percentage.
inline QString countdown(qint64 seconds) {
    const qint64 minutes=qMax<qint64>(1,(seconds+59)/60);
    if(minutes<60)return QObject::tr("%1m").arg(minutes);
    if(minutes<2880)return QObject::tr("%1h").arg(minutes/60);
    return QObject::tr("%1d").arg(minutes/1440);
}
inline QString approximately(qint64 seconds) {
    const qint64 minutes=qMax<qint64>(1,seconds/60),days=minutes/1440,hours=minutes%1440/60,rest=minutes%60;
    if(days)return hours?QObject::tr("%1d %2h").arg(days).arg(hours):QObject::tr("%1d").arg(days);
    if(hours)return rest?QObject::tr("%1h %2m").arg(hours).arg(rest):QObject::tr("%1h").arg(hours);
    return QObject::tr("%1m").arg(minutes);
}
enum class Tone { Normal, Warning, Critical };
inline Tone thresholdTone(double used) {return used>=90?Tone::Critical:used>=70?Tone::Warning:Tone::Normal;}
inline QColor toneColor(Tone tone,bool dark) {
    return QColor(tone==Tone::Critical ? (dark?"#f07878":"#ce3d47") : tone==Tone::Warning ? (dark?"#f0a35b":"#bd6519") : (dark?"#72cdb2":"#237a62"));
}
// Pace compares used quota with the share of the limit window that has passed.
// Spending faster than that share exhausts the limit before it resets.
struct Pace {
    bool known=false;      // the window length and a future reset time are reported
    bool early=false;      // the first 8% of a window is too short to extrapolate
    double used=0;
    double elapsed=0;      // share of the window that has passed, 0..1
    qint64 secondsLeft=-1;
    qint64 runsOutIn=-1;   // seconds until 100% at the average pace; -1 when it lasts until reset
    double projected=-1;   // percent at reset at the average pace
    Tone tone=Tone::Normal;
};
inline Pace pace(const QJsonObject &w,const QDateTime &now=QDateTime::currentDateTime()) {
    Pace result;result.used=qBound(0.,w.value("used_percent").toDouble(),100.);result.tone=thresholdTone(result.used);
    const qint64 length=qint64(w.value("window_minutes").toInt())*60;const auto at=reset(w.value("resets_at"));
    if(length<=0 || !at.isValid() || at<=now)return result;
    result.known=true;result.secondsLeft=now.secsTo(at);
    const qint64 passed=qMax<qint64>(0,length-result.secondsLeft);
    result.elapsed=double(passed)/length;result.early=result.elapsed<0.08;
    // Exhausted and fresh windows keep the fixed 70/90% thresholds.
    if(result.used>=100 || result.early)return result;
    result.projected=result.used/result.elapsed;
    if(result.projected>100)result.runsOutIn=qint64((100-result.used)*passed/result.used);
    result.tone=result.runsOutIn<0 ? Tone::Normal : result.runsOutIn>=result.secondsLeft/2 ? Tone::Warning : Tone::Critical;
    if(result.used>=90 && result.tone==Tone::Normal)result.tone=Tone::Warning;
    return result;
}
inline QColor paceColor(const Pace &pace,bool dark) {return toneColor(pace.tone,dark);}
// The most urgent current window, or nothing when no window reports current usage.
inline std::optional<Tone> severest(const QJsonObject &data) {
    std::optional<Tone> tone;
    for(const auto &value:data.value("windows").toArray()){const auto w=value.toObject();if(current(w)&&w.value("used_percent").isDouble())tone=qMax(tone.value_or(Tone::Normal),pace(w).tone);}
    return tone;
}
inline QString forecast(const QJsonObject &w) {
    if(!current(w))return {};
    const auto p=pace(w);
    if(!p.known || p.early || p.used>=100)return {};
    return p.runsOutIn>=0 ? QObject::tr("At the current pace, the limit runs out in about %1").arg(approximately(p.runsOutIn))
        : QObject::tr("At the current pace, about %1% by reset").arg(qRound(p.projected));
}
// Footer indicator chosen in Settings → Appearance.
enum class Indicator { Rails, Marker, MarkerCountdown };
inline Indicator indicator() {
    const auto value=QSettings().value("workspace/usageIndicator").toString();
    return value=="marker" ? Indicator::Marker : value=="marker-countdown" ? Indicator::MarkerCountdown : Indicator::Rails;
}
inline QString windowText(const QJsonObject &w,bool withReset=true) {
    if(!current(w))return period(w)+QObject::tr(" refresh");
    const double used=w.value("used_percent").toDouble();
    return period(w)+" "+QString::number(used,'f',0)+"%"+(withReset&&used>=100?" ("+remaining(reset(w.value("resets_at")))+")":QString());
}
// New session only presents existing snapshots; unavailable windows add no detail.
inline QJsonObject launchMeterData(const QJsonObject &data) {
    QJsonArray windows;
    for(const auto &value:data["windows"].toArray()){
        auto window=value.toObject();
        if(!current(window)||!window["used_percent"].isDouble()||!std::isfinite(window["used_percent"].toDouble()))continue;
        window["used_percent"]=qBound(0.,window["used_percent"].toDouble(),100.);
        if(window["window_minutes"].toInt()>0)window.remove("label");
        windows.append(window);
    }
    auto result=data;result["windows"]=windows;
    if(!windows.isEmpty())result["status"]="ok";
    return result;
}
inline QString launchSummary(const QJsonObject &data) {
    QStringList limits;
    for(const auto &value:launchMeterData(data)["windows"].toArray()){
        const auto window=value.toObject();
        limits.append(period(window)+QObject::tr(" %1% used").arg(qRound(qBound(0.,window["used_percent"].toDouble(),100.))));
    }
    for(const auto &value:data["balances"].toArray()){
        const auto balance=value.toObject();bool valid=false;
        const auto amount=balance["balance"].toString();const double number=amount.toDouble(&valid);
        if(valid&&std::isfinite(number))limits.append(QObject::tr("Balance: %1 %2").arg(amount,balance["currency"].toString()).trimmed());
    }
    return limits.isEmpty()?QObject::tr("limits n/a"):limits.join("  ");
}
inline double highest(const QJsonObject &data) {
    double used=-1;for(const auto &value:data.value("windows").toArray()){const auto w=value.toObject();if(current(w)&&w.value("used_percent").isDouble())used=qMax(used,w.value("used_percent").toDouble());}return used;
}
inline QString exhausted(const QJsonObject &data) {
    QStringList lines;
    for(const auto &value:data.value("windows").toArray()){
        const auto w=value.toObject();if(w.value("used_percent").toDouble()<100)continue;const auto at=reset(w.value("resets_at"));
        lines<<(!current(w)?QObject::tr("%1 limit window ended; refresh usage").arg(period(w)):
            at.isValid()?QObject::tr("%1 limit reached. Resets in %2").arg(period(w),remaining(at)):QObject::tr("%1 limit reached. Reset time is unavailable").arg(period(w)));
    }
    const auto text=lines.join("\n");
    return !text.isEmpty() && (data.value("offline").toBool() || data.value("refresh_error").toBool())
        ? QObject::tr("Last reported usage: %1. Refresh to check current limits.").arg(text) : text;
}
// Chip label: the nearest reset of an exhausted current window, or a reminder to
// refresh when only windows that already ended still report 100%.
inline QString exhaustedSummary(const QJsonObject &data) {
    QDateTime nearest;bool reached=false,ended=false;
    for(const auto &value:data.value("windows").toArray()){
        const auto w=value.toObject();if(w.value("used_percent").toDouble()<100)continue;
        if(!current(w)){ended=true;continue;}
        reached=true;const auto at=reset(w.value("resets_at"));
        if(at.isValid()&&(!nearest.isValid()||at<nearest))nearest=at;
    }
    if(reached)return nearest.isValid()?QObject::tr("Limit reached · %1").arg(remaining(nearest)):QObject::tr("Limit reached");
    return ended?QObject::tr("Refresh usage"):QString();
}
inline QColor color(double value,bool dark) {return toneColor(thresholdTone(value),dark);}
inline QString state(const QJsonObject &data) {
    return data.isEmpty() || data.value("status")=="loading" ? QObject::tr("Loading account details…")
        : data.value("status")=="configured" && data.value("provider")=="opencode" ? QObject::tr("OpenCode uses its native provider configuration. Usage is not available here.")
        : data.value("status")=="configured" ? QObject::tr("API key configured. Usage is available in the DeepSeek console.")
        : data.value("offline").toBool() || data.value("status")=="offline" ? QObject::tr("Machine is offline. Showing the last available account data.") : data.value("status")=="expired" ? QObject::tr("Native sign-in has expired. Open the agent or sign in again, then refresh usage.")
        : data.value("status")=="signed_out" && data.value("provider")=="dsh" ? QObject::tr("DeepSeek account is signed out. API-key connections are configured separately in the native settings.")
        : data.value("status")=="signed_out" ? QObject::tr("No native subscription sign-in found. API-key sessions may still work.")
        : data.value("status")=="credentials_locked" ? QObject::tr("Saved sign-in is in the locked macOS Keychain. Unlock it in Keychain Access, then refresh.")
        : data.value("status")=="desktop_session_unavailable" ? QObject::tr("Saved sign-in requires the Mac desktop session. Log in to macOS and check the HGS installation, then refresh.")
        : data.value("status")=="credentials_unavailable" ? QObject::tr("The saved Keychain sign-in could not be used. Check account access or sign in again.")
        : QObject::tr("Usage is unavailable from this provider. Open its native account page or refresh after signing in.");
}
inline QString tooltip(const QJsonObject &data) {
    QStringList lines;
    const auto provider=IdentityBadges::providerName(data.value("provider").toString());
    const auto label=data.value("label").toString();
    if(!provider.isEmpty())lines<<provider;
    if(!label.isEmpty()&&label!=provider)lines<<label;
    const auto identity=data.value("identity").toObject();
    for(const auto *key:{"name","email","plan","organization"}) if(!identity.value(key).toString().isEmpty())lines<<identity.value(key).toString();
    for(const auto &value:data.value("windows").toArray()) {
        const auto w=value.toObject();const auto at=reset(w.value("resets_at"));
        lines<<QObject::tr("%1: %2% used").arg(period(w)).arg(w.value("used_percent").toDouble(),0,'f',0);
        if(at.isValid())lines<<QObject::tr("Resets %1").arg(at.toString("d MMM HH:mm"))+(current(w)?QObject::tr(" (in %1)").arg(remaining(at)):QString());
        if(const auto line=forecast(w);!line.isEmpty())lines<<line;
    }
    for(const auto &value:data.value("balances").toArray()) {const auto b=value.toObject();lines<<b.value("kind").toString()+": "+b.value("balance").toString()+" "+b.value("currency").toString();}
    if(data.value("checked_at").isDouble())lines<<QObject::tr("Updated %1").arg(QDateTime::fromSecsSinceEpoch(qint64(data.value("checked_at").toDouble())).toString("HH:mm"));
    if(data.value("refreshing").toBool())lines<<QObject::tr("Updating… Previous values remain visible.");
    else if(data.value("refresh_error").toBool())lines<<QObject::tr("Update failed. Showing the last available values.");
    if(data.value("offline").toBool())lines<<QObject::tr("Machine is offline.");
    if(data.value("windows").toArray().isEmpty() && data.value("balances").toArray().isEmpty())lines<<state(data);
    lines<<QObject::tr("Updates every 5 minutes. Open account details.");
    return lines.join('\n');
}
class RefreshButton : public QPushButton {
public:
    explicit RefreshButton(QWidget *parent=nullptr):QPushButton(parent) {
        setObjectName("sessionUsageRefresh");setFixedSize(22,22);setIconSize(QSize(14,14));setCursor(Qt::PointingHandCursor);
        setAccessibleName(tr("Refresh account usage"));setToolTip(tr("Refresh account usage (automatically every 5 minutes)"));m_timer.setInterval(40);
        connect(&m_timer,&QTimer::timeout,this,[this]{m_angle=(m_angle+18)%360;update();});setTheme(true);
    }
    void setTheme(bool dark) {
        m_dark=dark;m_icon=workspaceIcon("refresh",QColor(dark?"#b8c8d6":"#506278"));m_busyIcon=workspaceIcon("refresh",QColor(dark?"#8ce3c9":"#237a62"));if(!m_refreshing)setIcon(m_icon);
        setStyleSheet(QString("QPushButton{padding:0;min-height:20px;min-width:20px;border:1px solid transparent;border-radius:4px;background:transparent;} QPushButton:hover{background:%1;border-color:%2;} QPushButton:pressed{background:%3;} QPushButton:focus[keyboardFocus=true]{border-color:%2;}").arg(dark?"#34434d":"#dde8ef",dark?"#799da5":"#64868f",dark?"#49616b":"#bfd3df"));update();
    }
    void setRefreshing(bool refreshing) {
        if(m_refreshing==refreshing)return;m_refreshing=refreshing;setProperty("refreshing",refreshing);m_angle=0;
        if(refreshing){setIcon({});m_timer.start();}else{m_timer.stop();setIcon(m_icon);}update();
        setToolTip(refreshing?tr("Updating account usage…"):tr("Refresh account usage (automatically every 5 minutes)"));
    }
    bool isRefreshing() const {return m_refreshing;}
protected:
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);if(!m_refreshing)return;
        QPainter p(this);p.setRenderHint(QPainter::SmoothPixmapTransform);p.translate(width()/2.,height()/2.);p.rotate(m_angle);
        p.drawPixmap(QRect(-8,-8,16,16),m_busyIcon.pixmap(32,32));
    }
private:QTimer m_timer;QIcon m_icon,m_busyIcon;bool m_dark=true,m_refreshing=false;int m_angle=0;
};
class Button : public QPushButton {
public:
    explicit Button(QWidget *parent=nullptr):QPushButton(parent) {
        setObjectName("sessionAccountUsage");setFixedHeight(22);setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
        setStyleSheet("QPushButton{padding:0;min-height:20px;border:0;background:transparent;text-align:right;}");
        m_indicator=AccountUsage::indicator();setFixedWidth(contentWidth());
        auto *timer=new QTimer(this);timer->setInterval(30000);connect(timer,&QTimer::timeout,this,[this]{if(!isVisible())return;setFixedWidth(contentWidth());setToolTip(tooltip(m_data));setAccessibleName(tr("Account usage")+"\n"+exhausted(m_data)+"\n"+toolTip());update();});timer->start();
    }
    void setData(const QJsonObject &data) {if(m_data==data)return;m_data=data;setFixedWidth(contentWidth());setToolTip(tooltip(data));setAccessibleName(QObject::tr("Account usage")+"\n"+exhausted(data)+"\n"+toolTip());update();}
    void setTheme(bool dark) {m_dark=dark;update();}
    void setShowProvider(bool show) {m_showProvider=show;setFixedWidth(contentWidth());update();}
    void setCompact(bool compact) {m_compact=compact;setFixedWidth(contentWidth());update();}
    void setIndicator(Indicator indicator) {if(m_indicator==indicator)return;m_indicator=indicator;setFixedWidth(contentWidth());update();}
    Indicator indicator() const {return m_indicator;}
    QSize sizeHint() const override {return {contentWidth(),22};}
protected:
    void changeEvent(QEvent *event) override {
        QPushButton::changeEvent(event);
        if(event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange)setFixedWidth(contentWidth());
    }
public:
    // Delegates can use the same widget's paint path without re-rendering its
    // hidden parent hierarchy or introducing another quota presentation.
    void paintContents(QPainter &p,const QPoint &position = {}) const {
        p.save();p.translate(position);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);
        if(underMouse() || hasFocus()){p.setBrush(QColor(m_dark?"#27313b":"#e8edf3"));p.drawRoundedRect(rect(),5,5);}
        QFont f=font();f.setPixelSize(11);p.setFont(f);const auto muted=QColor(m_dark?"#a1adbb":"#647386");
        const auto windows=m_data.value("windows").toArray();const bool loading=m_data.isEmpty() || m_data.value("status")=="loading";
        const int slotCount=loading?2:qMin(2,int(windows.size()));
        const int provider=providerWidth();
        int x=m_compact?4:6;
        if(provider){IdentityBadges::paint(&p,QRect(x,(height()-18)/2,provider-6,18),IdentityBadges::Provider,m_data.value("provider").toString(),m_dark);x+=provider;}
        for(int i=0;i<slotCount;++i) {
            const auto w=loading?QJsonObject():windows[i].toObject();const bool fresh=!loading && current(w);
            const auto state=fresh?pace(w):Pace();
            const int barWidth=m_compact?20:34, textOffset=barWidth+6;
            const bool withCountdown=showsCountdown(w);
            const QString text=windowText(w,!withCountdown);
            const int textWidth=indicatorTextWidth(w,loading);
            paintMeter(p,x,barWidth,fresh,state);
            if(loading){p.setPen(Qt::NoPen);p.setBrush(QColor(m_dark?"#2b3740":"#e3e8ed"));p.drawRoundedRect(QRectF(x+textOffset,7,textWidth,8),3,3);}
            else {p.setPen(fresh?paceColor(state,m_dark):muted);p.drawText(QRect(x+textOffset,0,textWidth,height()),Qt::AlignVCenter,p.fontMetrics().elidedText(text,Qt::ElideRight,textWidth));}
            if(withCountdown)paintCountdown(p,x+textOffset+textWidth,w,muted);
            x+=textOffset+textWidth+countdownWidth(w)+(m_compact?10:16);
        }
        if(!slotCount) {
            const int available=width()-x-(m_compact?4:6);
            p.setPen(muted);p.drawText(QRect(x,0,available,height()),Qt::AlignVCenter|Qt::AlignRight,p.fontMetrics().elidedText(summaryText(),Qt::ElideRight,available));
        }
        p.restore();
    }
protected:
    void paintEvent(QPaintEvent *) override {QPainter p(this);paintContents(p);}
private:
    // Rails put usage above the elapsed share of the window. The marker shows
    // that share on the usage bar, and usage beyond it takes the warning color.
    // Without a window length or reset time both draw the plain usage bar.
    void paintMeter(QPainter &p,int x,int width,bool fresh,const Pace &state) const {
        const QColor track(m_dark?"#36434e":"#d7e0e6");p.setPen(Qt::NoPen);
        if(fresh && state.known && m_indicator==Indicator::Rails) {
            p.setBrush(track);p.drawRoundedRect(QRectF(x,7,width,3),1.5,1.5);p.drawRoundedRect(QRectF(x,12,width,2),1,1);
            p.setBrush(paceColor(state,m_dark));p.drawRoundedRect(QRectF(x,7,width*state.used/100.,3),1.5,1.5);
            p.setBrush(QColor(m_dark?"#8395a5":"#7f90a1"));p.drawRoundedRect(QRectF(x,12,width*state.elapsed,2),1,1);
            return;
        }
        const QRectF bar(x,9,width,4);p.setBrush(track);p.drawRoundedRect(bar,2,2);
        if(!fresh)return;
        if(!state.known){p.setBrush(paceColor(state,m_dark));p.drawRoundedRect(QRectF(x,9,width*state.used/100.,4),2,2);return;}
        QPainterPath clip;clip.addRoundedRect(bar,2,2);p.save();p.setClipPath(clip);
        const double mark=state.elapsed*100;
        if(state.used<100 && !state.early && state.used>mark) {
            p.setBrush(toneColor(Tone::Normal,m_dark));p.drawRect(QRectF(x,9,width*mark/100.,4));
            p.setBrush(paceColor(state,m_dark));p.drawRect(QRectF(x+width*mark/100.,9,width*(state.used-mark)/100.,4));
        } else {p.setBrush(paceColor(state,m_dark));p.drawRect(QRectF(x,9,width*state.used/100.,4));}
        p.restore();
        p.setBrush(QColor(m_dark?"#e3eaf0":"#2b3946"));p.drawRoundedRect(QRectF(x+width*mark/100.-0.75,6,1.5,10),0.75,0.75);
    }
    bool showsCountdown(const QJsonObject &w) const {
        return m_indicator==Indicator::MarkerCountdown && !m_compact && current(w) && reset(w.value("resets_at")).isValid();
    }
    // An hourglass, as on Android, and the time until reset after the percentage.
    void paintCountdown(QPainter &p,int x,const QJsonObject &w,const QColor &muted) const {
        const qreal left=x+5;QPen pen(muted,1);pen.setJoinStyle(Qt::RoundJoin);p.setPen(pen);p.setBrush(Qt::NoBrush);
        p.drawPolygon(QPolygonF({QPointF(left+.5,6.5),QPointF(left+6.5,6.5),QPointF(left+3.5,11),QPointF(left+6.5,15.5),QPointF(left+.5,15.5),QPointF(left+3.5,11)}));
        p.setPen(muted);p.drawText(QRect(x+15,0,countdownWidth(w)-15,height()),Qt::AlignVCenter,countdown(QDateTime::currentDateTime().secsTo(reset(w.value("resets_at")))));
    }
    // Countdowns keep a stable slot like percentages; the hourglass and its gaps take 15 px.
    int countdownWidth(const QJsonObject &w) const {
        if(!showsCountdown(w))return 0;
        return 15+qMax(compactTextWidth(countdown(QDateTime::currentDateTime().secsTo(reset(w.value("resets_at"))))),compactTextWidth("00m"));
    }
    int compactTextWidth(const QString &text) const {
        QFont f=font();f.setPixelSize(11);
        // Fractional glyph advances can exceed the rounded integer width.
        // Leave room for the complete percentage, including its last glyph.
        return qMin(150,int(std::ceil(QFontMetricsF(f).horizontalAdvance(text)))+2);
    }
    int providerWidth() const {const auto name=m_data.value("provider").toString();return m_showProvider&&!m_compact&&!name.isEmpty()?IdentityBadges::width(IdentityBadges::Provider,name,font(),95)+6:0;}
    int indicatorTextWidth(const QJsonObject &window,bool loading) const {
        if(loading)return m_compact?40:48;
        const int text=compactTextWidth(windowText(window));
        // Percent changes keep a small stable slot; only additional text, such
        // as a reset countdown, expands it.
        return m_compact?text:qMax(text,compactTextWidth(period(window)+" 100%"));
    }
    QString summaryText() const {
        const auto balances=m_data.value("balances").toArray();const auto b=balances.isEmpty()?QJsonObject():balances.first().toObject();
        return !b.isEmpty()?b.value("balance").toString()+" "+b.value("currency").toString():m_data.value("status")=="configured"?(m_data.value("provider")=="opencode"?tr("Configured"):tr("API key")):m_data.value("status")=="signed_out"?tr("Sign in required"):m_data.value("status")=="expired"?tr("Sign-in expired"):tr("Unavailable");
    }
    int contentWidth() const {
        const auto windows=m_data.value("windows").toArray();const bool loading=m_data.isEmpty() || m_data.value("status")=="loading";
        int width=(m_compact?8:12)+providerWidth();
        if(!loading&&windows.isEmpty())return width+compactTextWidth(summaryText());
        for(int i=0;i<(loading?2:qMin(2,int(windows.size())));++i)
            width+=(m_compact?26:40)+indicatorTextWidth(loading?QJsonObject():windows[i].toObject(),loading)+countdownWidth(loading?QJsonObject():windows[i].toObject())+(i?(m_compact?10:16):0);
        return width;
    }
    QJsonObject m_data;bool m_dark=true,m_showProvider=true,m_compact=false;Indicator m_indicator=Indicator::Rails;
};
class Panel : public QFrame {
public:
    explicit Panel(QWidget *parent=nullptr):QFrame(parent) {setObjectName("accountUsagePanel");m_layout=new QVBoxLayout(this);m_layout->setContentsMargins(14,12,14,12);m_layout->setSpacing(8);setMaximumWidth(620);}
    void setTheme(bool dark){m_dark=dark;setStyleSheet(QString("QFrame#accountUsagePanel{background:%1;border:1px solid %2;border-radius:9px;} QLabel{background:transparent;border:0;color:%3;} QProgressBar{border:0;border-radius:2px;background:%2;}").arg(dark?"#1d252c":"#ffffff",dark?"#33414c":"#d7e0e6",dark?"#a1adbb":"#647386"));const auto data=m_data;m_data={};setData(data);}
    void setData(const QJsonObject &data) {
        if(m_data==data && m_layout->count())return;m_data=data;while(auto *item=m_layout->takeAt(0)){delete item->widget();delete item;}
        auto addText=[this](const QString &text){auto *label=new QLabel(text);label->setTextFormat(Qt::PlainText);label->setWordWrap(true);m_layout->addWidget(label);};
        addText(QObject::tr("Account usage"));addText(QObject::tr("Shared by sessions using this account."));const auto windows=data.value("windows").toArray();const auto balances=data.value("balances").toArray();
        if(windows.isEmpty() && balances.isEmpty())addText(state(data));
        for(const auto &value:windows){const auto w=value.toObject();const double used=w.value("used_percent").toDouble();const bool fresh=current(w);const auto at=reset(w.value("resets_at"));
            addText(period(w)+"   "+(fresh?QObject::tr("%1% used").arg(used,0,'f',0):QObject::tr("Window ended; refresh for current usage")));
            auto *bar=new QProgressBar;bar->setRange(0,1000);bar->setValue(fresh?qRound(qBound(0.,used,100.)*10):0);bar->setTextVisible(false);bar->setFixedHeight(5);bar->setStyleSheet(QString("QProgressBar::chunk{background:%1;border-radius:2px;}").arg(color(used,m_dark).name()));m_layout->addWidget(bar);
            if(at.isValid())addText(QObject::tr("Resets %1").arg(at.toString("d MMM HH:mm")));
        }
        for(const auto &value:balances){const auto b=value.toObject();addText(b.value("kind").toString()+": "+b.value("balance").toString()+" "+b.value("currency").toString());}
        const auto credits=data.value("credits").toObject();
        if(!credits.isEmpty())addText(credits.value("unlimited").toBool()?QObject::tr("Credits: unlimited"):QObject::tr("Credits available: %1").arg(AccountUsage::credits(credits.value("balance").toString())));
        if(data.value("checked_at").isDouble())addText(QObject::tr("Updated %1").arg(QDateTime::fromSecsSinceEpoch(qint64(data.value("checked_at").toDouble())).toString("HH:mm")));
    }
private:QVBoxLayout *m_layout;QJsonObject m_data;bool m_dark=true;
};
}
