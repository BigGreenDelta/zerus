#pragma once

#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

// A fixed-size journal resource: changing playback never edits or lays out text.
namespace VoicePlayerImage {
inline QString time(qint64 milliseconds) {
    const auto seconds=qMax<qint64>(0,milliseconds)/1000;
    return QStringLiteral("%1:%2").arg(seconds/60).arg(seconds%60,2,10,QLatin1Char('0'));
}
inline QRectF waveformRect(qreal width,qreal scale) {
    return {56*scale,2*scale,qMax<qreal>(1,width-60*scale),27*scale};
}
inline QJsonArray waveform(const QJsonObject &audio) {
    const auto values=audio.value("waveform").toArray();
    if(values.size()!=64)return {};
    for(const auto &value:values){const auto number=value.toDouble(-1);if(!value.isDouble() || number<0 || number>255 || number!=qFloor(number))return {};}
    return values;
}
inline QImage draw(qreal width,qreal scale,qreal ratio,bool dark,const QJsonObject &audio,
                   qint64 position,bool playing,bool loading) {
    ratio=qMax<qreal>(1,ratio);
    QImage image(qCeil(width*ratio),qCeil(52*scale*ratio),QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);image.fill(Qt::transparent);
    QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent(dark?"#8bdfc0":"#167357"),ink(dark?"#152d24":"#ffffff");
    const QColor muted(dark?"#9eabba":"#657487"),remaining(dark?"#739b8c":"#638475");
    const QRectF button(0,0,44*scale,44*scale);
    painter.setPen(Qt::NoPen);painter.setBrush(accent);painter.drawEllipse(button);
    painter.setBrush(ink);
    if(loading){
        painter.setBrush(Qt::NoBrush);painter.setPen(QPen(ink,2.2*scale,Qt::SolidLine,Qt::RoundCap));
        painter.drawArc(button.adjusted(12*scale,12*scale,-12*scale,-12*scale),30*16,260*16);
    }else if(playing){
        painter.drawRoundedRect(QRectF(15*scale,13*scale,5*scale,18*scale),scale,scale);
        painter.drawRoundedRect(QRectF(24*scale,13*scale,5*scale,18*scale),scale,scale);
    }else{
        QPainterPath triangle;triangle.moveTo(17*scale,12*scale);triangle.lineTo(31*scale,22*scale);triangle.lineTo(17*scale,32*scale);triangle.closeSubpath();painter.drawPath(triangle);
    }
    const auto duration=qMax<qint64>(0,audio.value("duration_ms").toInteger());
    const qreal progress=duration?qBound<qreal>(0,qreal(position)/duration,1):0;
    const auto area=waveformRect(width,scale);const auto values=waveform(audio);
    const auto bars=[&](const QColor &color){
        painter.setPen(Qt::NoPen);painter.setBrush(color);
        if(values.isEmpty()){painter.drawRoundedRect(QRectF(area.left(),area.center().y()-scale,area.width(),2*scale),scale,scale);return;}
        const int count=qBound(1,int(area.width()/(3*scale)),64);
        const qreal step=area.width()/count,barWidth=qMax<qreal>(scale,step-qMin<qreal>(2*scale,step*.38));
        for(int i=0;i<count;++i){
            int amplitude=0;for(int j=i*64/count;j<(i+1)*64/count;++j)amplitude=qMax(amplitude,values[j].toInt());
            const qreal height=qMax<qreal>(2*scale,area.height()*amplitude/255.);
            painter.drawRoundedRect(QRectF(area.left()+i*step,area.bottom()-height,barWidth,height),barWidth/2,barWidth/2);
        }
    };
    bars(remaining);painter.save();painter.setClipRect(QRectF(area.left(),area.top(),area.width()*progress,area.height()));bars(accent);painter.restore();
    auto font=painter.font();font.setPixelSize(qRound(12*scale));painter.setFont(font);painter.setPen(muted);
    const QString label=loading?QObject::tr("Loading…"):time(position)+QStringLiteral(" / ")+time(duration);
    const QRectF caption(area.left(),33*scale,area.width(),17*scale);
    painter.drawText(caption,Qt::AlignLeft|Qt::AlignVCenter,QFontMetrics(font).elidedText(label,Qt::ElideRight,int(caption.width())));
    return image;
}
}
