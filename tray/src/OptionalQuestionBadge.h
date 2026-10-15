#pragma once

#include "CachedText.h"
#include "GitStatusBadge.h"
#include "SessionPresentation.h"
#include "WorkspaceIcons.h"
#include <QPainter>

// The chip for pending optional Codex questions on session and Overview cards.
// It has the geometry of a Git mark and the accent of the Optional question
// heading in Activity, with a stronger fill and a border because it asks for an
// answer. Never amber: amber means attention, and these questions are not.
namespace OptionalQuestionBadge {
inline QString countText(int count) { return GitStatusBadge::countText(count); }
inline int width(int count, QFont base) { return 27 + CachedText::width(GitStatusBadge::font(base), countText(count)); }
// The glyph and border use the theme accent. The count is darker in the light
// theme so it keeps a 4.5:1 contrast on hovered and selected rows.
inline QColor ink(bool dark) { return QColor(dark ? "#8bdfc0" : "#167357"); }
inline QColor text(bool dark) { return QColor(dark ? "#8bdfc0" : "#0d5641"); }
inline QColor fill(bool dark) { QColor result = ink(dark); result.setAlpha(dark ? 40 : 32); return result; }
inline QString tooltip(int count) {
    return SessionPresentation::optionalQuestionsLabel(count) + '\n'
        + QObject::tr("The agent can continue without an answer. Answer or skip it in Activity.") + '\n'
        + QObject::tr("Click to open.");
}
inline void paint(QPainter *p, const QRect &rect, int count, QFont base, bool dark) {
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(ink(dark), 1)); p->setBrush(fill(dark));
    p->drawRoundedRect(QRectF(rect).adjusted(.5, .5, -.5, -.5), 4, 4);
    workspaceIcon("question", ink(dark)).paint(p, QRect(rect.x() + 4, rect.y() + 1, 16, 16));
    p->setFont(GitStatusBadge::font(base)); p->setPen(text(dark));
    CachedText::draw(p, QRect(rect.x() + 23, rect.y(), rect.width() - 27, 18), Qt::AlignVCenter, countText(count));
    p->restore();
}
// Collapsed strip tiles have no metadata row: a small mark on the provider
// badge's outer corner, opposite the unread dot on the status badge.
inline QRect stripMarkRect(const QRect &provider) { return {provider.right() - 7, provider.y() - 5, 12, 12}; }
inline void paintStripMark(QPainter *p, const QRect &rect, QFont base, bool dark, const QColor &surface) {
    p->save(); p->setRenderHint(QPainter::Antialiasing); p->setPen(Qt::NoPen);
    p->setBrush(surface); p->drawEllipse(QRectF(rect).adjusted(-1.5, -1.5, 1.5, 1.5));
    p->setBrush(ink(dark)); p->drawEllipse(QRectF(rect));
    base.setPixelSize(9); base.setWeight(QFont::Bold); p->setFont(base);
    p->setPen(QColor(dark ? "#10231b" : "#ffffff"));
    CachedText::draw(p, rect, Qt::AlignCenter, QStringLiteral("?"));
    p->restore();
}
}
