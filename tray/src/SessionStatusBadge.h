#pragma once
#include "CachedText.h"
#include <QPainter>

namespace SessionStatusBadge {
enum Kind { Neutral, Working, Attention, Error, Paused, Unread, Draft, Scheduled, Background, Waiting, MutedAttention, MutedUnread };
inline QColor mutedTone(bool dark) { return QColor(dark ? "#c4ccd5" : "#637080"); }
inline QFont font(QFont base) { base.setPixelSize(11); base.setWeight(QFont::Medium); return base; }
// The state's color for a frame around the session; invalid for Neutral.
inline QColor edge(Kind kind, bool dark) {
    switch (kind) {
    case Working: return QColor(dark ? "#4cc98a" : "#2a9d63");
    case Background: return QColor(dark ? "#86a9d3" : "#466b99");
    case Attention: case Unread: return QColor(dark ? "#ffda76" : "#e0a82e");
    case MutedAttention: case MutedUnread: return mutedTone(dark);
    case Error: return QColor(dark ? "#ff8fa0" : "#c7405a");
    case Paused: return QColor(dark ? "#a988d6" : "#9474c0");
    case Draft: return QColor(dark ? "#6b9bd8" : "#5b8cc9");
    case Scheduled: return QColor(dark ? "#5ab8cf" : "#2b8aa1");
    case Waiting: return QColor(dark ? "#d4ae65" : "#a37a24");
    case Neutral: break;
    }
    return {};
}
inline int width(const QString &text, Kind kind, const QFont &base) {
    // Leave room for fractional glyph advances before elidedText rounds them.
    return CachedText::width(font(base), text) + 16 + (kind == Neutral ? 0 : 16);
}
// The maximum halo is a 5px ellipse. Include its antialiasing fringe.
inline QPointF indicatorCenter(const QRect &rect) { return {qreal(rect.x() + 11), rect.center().y() + .5}; }
inline QRect workingIndicatorRect(const QRect &rect) {
    const auto c = indicatorCenter(rect);
    return QRectF(c.x() - 6, c.y() - 6, 12, 12).toAlignedRect().intersected(rect);
}
// labelOpacity fades the caption while the collapsed session strip grows into cards.
inline void paint(QPainter *p, const QRect &rect, const QString &text, Kind kind, bool dark, qreal pulse = 0, qreal labelOpacity = 1, bool indicatorOnly = false) {
    p->save(); p->setRenderHint(QPainter::Antialiasing); p->setClipRect(rect, Qt::IntersectClip);
    const QColor background = kind == Working ? QColor(dark ? "#194d36" : "#ccefdc")
        : kind == Waiting ? QColor(dark ? "#443a25" : "#f5e8c7")
        : kind == Background ? QColor(dark ? "#273544" : "#e9f0f8")
        : (kind == Attention || kind == Unread) ? QColor(dark ? "#ffda76" : "#f4ce65")
        : kind == Error ? QColor(dark ? "#ffabb6" : "#b73750")
        : kind == Paused ? QColor(dark ? "#443654" : "#e8ddf5")
        : kind == Draft ? QColor(dark ? "#263446" : "#e3edf9")
        : kind == Scheduled ? QColor(dark ? "#173e48" : "#d3edf4") : QColor(dark ? "#2b343e" : "#e7edf2");
    const QColor foreground = kind == Working ? QColor(dark ? "#97f0ba" : "#145b37")
        : kind == Waiting ? QColor(dark ? "#edd096" : "#795913")
        : kind == Background ? QColor(dark ? "#aac0df" : "#466b99")
        : (kind == Attention || kind == Unread) ? QColor("#392900")
        : kind == Error ? QColor(dark ? "#46202a" : "#ffffff")
        : kind == Paused ? QColor(dark ? "#dec3ff" : "#634187")
        : kind == Draft ? QColor(dark ? "#9cc0ec" : "#2f5f93")
        : kind == Scheduled ? QColor(dark ? "#9fe0f0" : "#165a6b") : QColor(dark ? "#b6c2d0" : "#566575");
    p->setPen(Qt::NoPen); p->setBrush(background); p->drawRoundedRect(rect, 5, 5);
    const QPointF center = indicatorCenter(rect);
    p->setFont(font(p->font())); p->setPen(foreground);
    if (kind == Working) {
        p->setPen(Qt::NoPen); QColor halo(foreground); halo.setAlphaF(.08 + .25 * pulse);
        p->setBrush(halo); p->drawEllipse(center, 4 + pulse, 4 + pulse);
        p->setBrush(foreground); p->drawEllipse(center, 2.5, 2.5);
    } else if (kind == Background) {
        p->setPen(Qt::NoPen);p->setBrush(foreground);p->drawEllipse(center,2.5,2.5);
    } else if (kind == Attention || kind == Error || kind == MutedAttention) {
        auto bold = p->font(); bold.setBold(true); p->setFont(bold);
        CachedText::draw(p, QRectF(center.x() - 5, rect.y(), 10, rect.height()), Qt::AlignCenter, QStringLiteral("!"));
    } else if (kind == Paused) {
        p->setPen(QPen(foreground, 1.5, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(center + QPointF(-2, -3), center + QPointF(-2, 3));
        p->drawLine(center + QPointF(2, -3), center + QPointF(2, 3));
    } else if (kind == Draft) {
        // A pencil: an unsent message waits in the composer.
        p->setPen(QPen(foreground, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawLine(center + QPointF(-3, 3), center + QPointF(2.5, -2.5));
        p->drawLine(center + QPointF(1.5, -3.5), center + QPointF(3.5, -1.5));
        p->drawLine(center + QPointF(-3, 3), center + QPointF(-3.5, 3.5));
    } else if (kind == Scheduled || kind == Waiting) {
        // A clock: the agent's own schedule starts its next turn.
        p->setPen(QPen(foreground, 1.3, Qt::SolidLine, Qt::RoundCap)); p->setBrush(Qt::NoBrush);
        p->drawEllipse(center, 4.5, 4.5);
        p->drawLine(center, center + QPointF(0, -2.8));
        p->drawLine(center, center + QPointF(2.2, 0));
    } else if (kind == Unread || kind == MutedUnread) {
        p->setPen(QPen(foreground, 1)); p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(center.x() - 4, center.y() - 3, 8, 6), 1, 1);
        p->drawLine(center + QPointF(-4, -3), center); p->drawLine(center, center + QPointF(4, -3));
    }
    if (indicatorOnly) { p->restore(); return; }
    p->setPen(foreground); p->setFont(font(p->font())); p->setOpacity(p->opacity() * labelOpacity);
    const QRect label = rect.adjusted(kind == Neutral ? 7 : 23, 0, -7, 0);
    // A growing badge reveals its caption under the clip instead of re-eliding it.
    CachedText::draw(p, label, Qt::AlignVCenter, labelOpacity < 1 ? text : CachedText::elided(p->font(), text, Qt::ElideRight, qMax(0, label.width())));
    p->restore();
}
}
