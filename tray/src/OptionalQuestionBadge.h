#pragma once

#include "CachedText.h"
#include "GitStatusBadge.h"
#include "SessionPresentation.h"
#include "SessionStatusBadge.h"
#include "WorkspaceIcons.h"
#include <QPainter>

// The chip for pending optional Codex questions on session and Overview cards.
// It follows the status badge like the separate new reply badge and shares the
// amber of attention, as the owner asked (2026-10-16): an open question is
// worth a look. It never changes a session's status, its attention count or
// its notifications. Muted sessions keep the same question count in neutral colors.
namespace OptionalQuestionBadge {
inline QString countText(int count) { return GitStatusBadge::countText(count); }
inline int width(int count, QFont base) { return 27 + CachedText::width(GitStatusBadge::font(base), countText(count)); }
// The status badge's attention colours: dark ink on amber in both themes.
inline QColor ink(bool dark, bool muted = false) { return muted ? SessionStatusBadge::mutedTone(dark) : QColor("#392900"); }
inline QColor text(bool dark, bool muted = false) { return ink(dark, muted); }
inline QColor fill(bool dark, bool muted = false) { return QColor(muted ? (dark ? "#2b343e" : "#e7edf2") : (dark ? "#ffda76" : "#f4ce65")); }
inline QString tooltip(int count) {
    return SessionPresentation::optionalQuestionsLabel(count) + '\n'
        + QObject::tr("The agent can continue without an answer. Answer or skip it in Activity.") + '\n'
        + QObject::tr("Click to open.");
}
inline void paint(QPainter *p, const QRect &rect, int count, QFont base, bool dark, bool muted = false) {
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen); p->setBrush(fill(dark, muted)); p->drawRoundedRect(rect, 5, 5);
    const int top = rect.y() + (rect.height() - 16) / 2;
    workspaceIcon("question", ink(dark, muted)).paint(p, QRect(rect.x() + 4, top, 16, 16));
    p->setFont(GitStatusBadge::font(base)); p->setPen(text(dark, muted));
    CachedText::draw(p, QRect(rect.x() + 23, rect.y(), rect.width() - 27, rect.height()), Qt::AlignVCenter, countText(count));
    p->restore();
}
// Collapsed strip tiles have no metadata row: a small mark on the provider
// badge's outer corner, opposite the unread dot on the status badge.
inline QRect stripMarkRect(const QRect &provider) { return {provider.right() - 7, provider.y() - 5, 12, 12}; }
inline void paintStripMark(QPainter *p, const QRect &rect, QFont base, bool dark, const QColor &surface, bool muted = false) {
    p->save(); p->setRenderHint(QPainter::Antialiasing); p->setPen(Qt::NoPen);
    p->setBrush(surface); p->drawEllipse(QRectF(rect).adjusted(-1.5, -1.5, 1.5, 1.5));
    p->setBrush(fill(dark, muted)); p->drawEllipse(QRectF(rect));
    base.setPixelSize(9); base.setWeight(QFont::Bold); p->setFont(base);
    p->setPen(ink(dark, muted));
    CachedText::draw(p, rect, Qt::AlignCenter, QStringLiteral("?"));
    p->restore();
}
}
