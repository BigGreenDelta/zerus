#pragma once

#include "SessionList.h"
#include "CachedText.h"
#include "IdentityBadge.h"
#include "ProjectAppearance.h"
#include "MachineAppearance.h"
#include "SessionStatusBadge.h"
#include "SessionElapsed.h"
#include "WorkspaceIcons.h"
#include "GitStatusBadge.h"
#include "OptionalQuestionBadge.h"
#include <QPainter>
#include <QStyledItemDelegate>
#include <QtMath>

// One renderer for session-list rows and overview cards.
inline QRect sessionCardRect(const QRect &row) { return row.adjusted(2, 4, -2, -6); }

// The collapsed session list keeps every row at its height; only the width
// changes. The list's "expansion" runs from 0 (strip) to 1 (full cards), and
// "expandedRowWidth" is the row width the cards grow into.
namespace SessionStrip {
inline QColor surface(bool dark) { return QColor(dark ? "#1c2229" : "#ffffff"); }
// A strip tile is as wide as a card is tall, so every session gets a square.
inline int cardSide(bool compact) { return (compact ? 94 : 104) - 10; }
// The panel adds its margins, the list's padding and the card inset.
inline int width(bool compact) { return cardSide(compact) + 14; }
// Controls above the tiles line up with them: a tile sits 2px inside the list,
// which pads its right side by another 2px.
inline QMargins tileColumn() { return {2, 0, 4, 0}; }
inline qreal expansion(const QWidget *list) {
    const auto value = list ? list->property("expansion") : QVariant();
    return value.isValid() ? qBound(0.0, value.toReal(), 1.0) : 1.0;
}
// Card text appears after a third of the way; strip marks are gone by then.
inline qreal fullOpacity(qreal expansion) { return qBound(0.0, (expansion - .32) * 1.9, 1.0); }
inline qreal stripOpacity(qreal expansion) { return qBound(0.0, 1 - expansion * 3.2, 1.0); }
inline QRect mix(const QRect &from, const QRect &to, qreal t) {
    const auto at = [t](int a, int b) { return qRound(a + (b - a) * t); };
    return {at(from.x(), to.x()), at(from.y(), to.y()), at(from.width(), to.width()), at(from.height(), to.height())};
}
// A tile title takes up to two lines; a word too long for a line is split.
inline QStringList twoLines(const QString &text, const QFontMetrics &metrics, int width) {
    const auto words = text.simplified().split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty() || width <= 0) return {};
    QString first; int used = 0;
    for (; used < words.size(); ++used) {
        const QString candidate = first.isEmpty() ? words[used] : first + ' ' + words[used];
        if (metrics.horizontalAdvance(candidate) > width) break;
        first = candidate;
    }
    QString rest = words.mid(used).join(' ');
    if (first.isEmpty()) {
        int fit = 1;
        while (fit < words[0].size() && metrics.horizontalAdvance(words[0].left(fit + 1)) <= width) ++fit;
        first = words[0].left(fit); rest = (words[0].mid(fit) + ' ' + words.mid(1).join(' ')).trimmed();
    }
    return rest.isEmpty() ? QStringList{first} : QStringList{first, metrics.elidedText(rest, Qt::ElideRight, width)};
}
}

class SessionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        // List rows are as wide as the view but never narrower than the hint.
        const int width = SessionStrip::expansion(option.widget) < 1 ? 1 : 280;
        if (rowCollapsed(index) && SessionStrip::expansion(option.widget) >= 1) return {width, 40};
        return {width, !index.data(SessionRoles::ChildId).toString().isEmpty() ? 64 : index.data(SessionRoles::Header).toBool() ? 40 : option.widget && option.widget->property("compact").toBool() ? 94 : 104};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const qreal expansion = SessionStrip::expansion(option.widget);
        if (paintIndicatorClip(p, option, index, expansion)) return;
        if (index.data(SessionRoles::Header).toBool()) paintHeader(p, option, index, expansion);
        else if (rowCollapsed(index) && expansion >= 1) paintCompactRow(p, option, index);
        else if (expansion >= 1) paintCard(p, option, index);
        else paintStripCard(p, option, index, expansion);
    }
    struct CardStatus {
        bool attention, unread, working, paused, draft, additionalUnread, muted;
        QString state, caption;
        SessionStatusBadge::Kind kind;
        IdentityBadges::RowEmphasis emphasis;
    };
    static bool rowCollapsed(const QModelIndex &index) {
        return index.data(SessionRoles::RowCollapsed).toBool() && !index.data(SessionRoles::Header).toBool()
            && index.data(SessionRoles::ChildId).toString().isEmpty() && index.data(SessionRoles::LaunchId).toString().isEmpty();
    }
    static bool densityControlAvailable(const QModelIndex &index) {
        return !index.data(SessionRoles::Header).toBool() && index.data(SessionRoles::ChildId).toString().isEmpty()
            && index.data(SessionRoles::LaunchId).toString().isEmpty();
    }
    static QRect rowDensityRect(const QStyleOptionViewItem &option, const QModelIndex &index) {
        if (!rowCollapsed(index) || SessionStrip::expansion(option.widget) < 1) return {};
        // Compact rows retain their accessible expand control.
        const QRect card = sessionCardRect(option.rect);
        return {card.right() - 28, option.rect.top() + 7, 24, 24};
    }
    struct CompactLayout { QRect title, status, provider, mute, expand; QString text, agent; };
    static CompactLayout compactLayout(const QStyleOptionViewItem &option, const QModelIndex &index) {
        const QRect card = sessionCardRect(option.rect);
        const QRect expand = rowDensityRect(option, index);
        const auto status = statusForView(option, index);
        QFont font = option.font; font.setPixelSize(14); font.setWeight(QFont::DemiBold);
        const int left = card.left() + 12, right = expand.left() - 6;
        const int muteWidth = index.data(SessionRoles::Muted).toBool() ? 20 : 0;
        const QString agent = index.data(SessionRoles::Agent).toString();
        const int naturalProvider = agent.isEmpty() ? 0 : IdentityBadges::width(IdentityBadges::Provider, agent, option.font, 95);
        const int naturalStatus = SessionStatusBadge::width(status.caption, status.kind, option.font);
        const int room = qMax(0, right - left - muteWidth);
        // Provider glyphs alone do not distinguish every agent (Codex/Shell).
        // Keep their short labels; a long status elides before the title does.
        const int providerWidth = naturalProvider;
        const int reservedTitle = qMin(60, CachedText::width(font, index.data(SessionRoles::Title).toString()));
        const int statusWidth = qMin(naturalStatus, qMax(26, room - providerWidth - 12 - reservedTitle));
        const QRect provider = providerWidth ? QRect(right - muteWidth - providerWidth, option.rect.center().y() - 9, providerWidth, 18) : QRect();
        const QRect mute = muteWidth ? QRect(right - 16, option.rect.center().y() - 7, 14, 14) : QRect();
        const int statusRight = right - muteWidth - (providerWidth ? providerWidth + 6 : 0);
        const QRect badge(statusRight - statusWidth, option.rect.center().y() - 10, statusWidth, 20);
        const QRect title(left, option.rect.center().y() - 11, qMax(0, badge.left() - left - 6), 22);
        return {title, badge, provider, mute, expand,
            CachedText::elided(font, index.data(SessionRoles::Title).toString(), Qt::ElideRight, title.width()), agent};
    }
    static QString compactDescription(const QStyleOptionViewItem &option, const QModelIndex &index) {
        return QStringList{index.data(SessionRoles::Title).toString(), statusForView(option, index).caption,
            IdentityBadges::providerName(index.data(SessionRoles::Agent).toString())}.join('\n');
    }
    static QString compactTooltip(const QStyleOptionViewItem &option, const QModelIndex &index) {
        if (!rowCollapsed(index) || SessionStrip::expansion(option.widget) < 1) return {};
        const auto layout = compactLayout(option, index);
        const auto status = statusForView(option, index);
        const auto providerFont = IdentityBadges::font(option.font, IdentityBadges::Provider);
        const int labelWidth = CachedText::width(providerFont, IdentityBadges::providerName(layout.agent));
        const bool elided = layout.text != index.data(SessionRoles::Title).toString()
            || layout.status.width() < SessionStatusBadge::width(status.caption, status.kind, option.font)
            || (!layout.agent.isEmpty() && layout.provider.width() - 28 < labelWidth);
        return elided ? compactDescription(option, index) : QString();
    }
    static QRect expandRowRect(const QStyleOptionViewItem &option, const QModelIndex &index) {
        return rowCollapsed(index) && SessionStrip::expansion(option.widget) >= 1 ? compactLayout(option, index).expand : QRect();
    }
    static int machineBadgeWidth(const QModelIndex &index, QFont base, int width) {
        base.setPixelSize(11); base.setWeight(QFont::Normal);
        return IdentityBadges::width(IdentityBadges::Machine, index.data(SessionRoles::Host).toString(), base, width / 3 + 10);
    }
    // Pending optional questions follow the status badge (and a separate new
    // reply badge) in the title row; the title elides first.
    static int optionalQuestionCount(const QModelIndex &index) {
        if (index.data(SessionRoles::Header).toBool() || !index.data(SessionRoles::ChildId).toString().isEmpty()
            || !index.data(SessionRoles::LaunchId).toString().isEmpty()) return 0;
        return qMax(0, index.data(SessionRoles::OptionalQuestions).toInt());
    }
    static QRect optionalQuestionRect(const QRect &row, const QModelIndex &index, QFont base, bool compact, bool selected = false) {
        if (rowCollapsed(index)) return {};
        return titleLayout(row, base, compact, index, statusOf(index, selected)).question;
    }
    static QRect gitStatusRect(const QRect &row, const QModelIndex &index, QFont base, bool compact) {
        if (rowCollapsed(index) || index.data(SessionRoles::Header).toBool() || !index.data(SessionRoles::ChildId).toString().isEmpty()) return {};
        const QRect card = sessionCardRect(row);
        const int textX = card.x() + 20, right = card.right() - 11;
        const int end = right - machineBadgeWidth(index, base, right - textX) - 8;
        const auto values = GitStatusBadge::fitted(GitStatusBadge::marks(index.data(SessionRoles::GitStatus).toJsonObject()), base, end - textX - 48);
        const int w = GitStatusBadge::width(values, base);
        return w ? QRect(end - w, card.y() + (compact ? 25 : 29), w, 18) : QRect();
    }
    // Priority: attention or error, work, an unsent draft (not for the open session,
    // which shows it in place), a new reply (then an extra badge), a schedule, a pause, Ready.
    static CardStatus statusOf(const QModelIndex &index, bool selected) {
        CardStatus s;
        s.muted = index.data(SessionRoles::Muted).toBool();
        s.attention = index.data(SessionRoles::Attention).toBool();
        s.unread = index.data(SessionRoles::Unread).toBool() && index.data(SessionRoles::Working).toInt() <= 0;
        s.state = index.data(SessionRoles::Status).toString();
        const bool error = index.data(SessionRoles::Failure).toBool() || s.state == "Error";
        s.working = index.data(SessionRoles::Working).toInt() > 0;
        s.paused = s.state == "Paused";
        const bool hasDraft = index.data(SessionRoles::Draft).toBool();
        s.draft = hasDraft && !selected && !s.attention && !s.working && s.state != "Offline";
        s.emphasis = s.attention ? (error ? IdentityBadges::Error : IdentityBadges::Attention)
            : hasDraft ? IdentityBadges::Draft : s.unread ? IdentityBadges::Unread : IdentityBadges::Normal;
        s.kind = s.attention ? (error ? SessionStatusBadge::Error : SessionStatusBadge::Attention)
            : s.working ? (s.state == "Background" ? SessionStatusBadge::Background : SessionStatusBadge::Working) : s.draft ? SessionStatusBadge::Draft : s.unread ? SessionStatusBadge::Unread
            : index.data(SessionRoles::Scheduled).toBool() ? SessionStatusBadge::Scheduled
            : index.data(SessionRoles::Waiting).toBool() ? SessionStatusBadge::Waiting
            : s.paused ? SessionStatusBadge::Paused : SessionStatusBadge::Neutral;
        if (s.muted) {
            if (s.emphasis == IdentityBadges::Attention || s.emphasis == IdentityBadges::Unread) s.emphasis = IdentityBadges::Muted;
            if (s.kind == SessionStatusBadge::Attention) s.kind = SessionStatusBadge::MutedAttention;
            if (s.kind == SessionStatusBadge::Unread) s.kind = SessionStatusBadge::MutedUnread;
        }
        s.caption = s.working ? SessionElapsed::status(s.state == "Compacting" ? tr("Compacting") : s.state == "Background" ? tr("Background") : tr("Working"), index.data(SessionRoles::WorkingSince).toDouble()) : s.draft ? tr("Draft") : s.unread && !s.attention ? tr("New reply")
            : s.state == "Not tracked" ? tr("Untracked") : s.state;
        if (s.kind == SessionStatusBadge::Waiting) {
            const auto items=index.data(SessionRoles::CoordinationWait).toJsonObject().value("items").toArray();
            s.caption=SessionElapsed::status(tr("Waiting"),items.isEmpty()?0:items.first().toObject().value("created_at").toDouble());
        }
        s.additionalUnread = s.unread && (s.working || s.attention || s.draft);
        return s;
    }
    // Selecting a list inspection row does not open its composer. Keep
    // its draft badge until the ordinary Sessions editor actually owns it.
    static CardStatus statusForView(const QStyleOptionViewItem &option, const QModelIndex &index) {
        const bool opensEditor = !option.widget || !option.widget->property("selectionIsInspection").toBool();
        return statusOf(index, opensEditor && option.state.testFlag(QStyle::State_Selected));
    }
    struct TitleLayout { QRect title, status, unread, question, provider; QString text, agent; };
    static TitleLayout titleLayout(const QStyleOptionViewItem &option, const QModelIndex &index, const CardStatus &status) {
        return titleLayout(option.rect, option.font, option.widget && option.widget->property("compact").toBool(), index, status);
    }
    static TitleLayout titleLayout(const QRect &row, const QFont &base, bool compact, const QModelIndex &index, const CardStatus &status) {
        const bool child = !index.data(SessionRoles::ChildId).toString().isEmpty();
        const QRect r = sessionCardRect(row).adjusted(child ? 24 : 0, 0, 0, 0);
        const int x = r.x() + 12 + (status.muted ? 18 : 0), right = r.right() - 11;
        const int y = r.y() + (child ? (compact ? 5 : 6) : (compact ? 3 : 5));
        int statusWidth = SessionStatusBadge::width(status.caption, status.kind, base);
        const int questions = optionalQuestionCount(index);
        const int questionWidth = questions ? OptionalQuestionBadge::width(questions, base) : 0;
        // Room the badges after the status take: a new reply, then questions.
        const int trailing = (status.additionalUnread ? 28 : 0) + (questionWidth ? questionWidth + 6 : 0);
        QFont font = base; font.setPixelSize(14); font.setWeight(QFont::DemiBold);
        const QString agent = index.data(SessionRoles::Agent).toString();
        const int reservedTitle = qMin(72, CachedText::width(font, index.data(SessionRoles::Title).toString()));
        const int providerRoom = right - x - statusWidth - 16 - trailing - reservedTitle;
        const int naturalWidth = agent.isEmpty() ? 0 : IdentityBadges::width(IdentityBadges::Provider, agent, base, child ? 85 : 95);
        const int providerWidth = naturalWidth && !child && providerRoom < naturalWidth ? 26 : naturalWidth;
        // Long elapsed/status captions elide within their badge at narrow widths.
        statusWidth = qMin(statusWidth, qMax(26, right - x - 8 - trailing - (providerWidth ? providerWidth + 8 : 0) - reservedTitle));
        const int titleWidth = qMax(0, right - x - statusWidth - 8 - trailing - (providerWidth ? providerWidth + 8 : 0));
        const QString text = CachedText::elided(font, index.data(SessionRoles::Title).toString(), Qt::ElideRight, titleWidth);
        const QRect badge(x + CachedText::width(font, text) + 8, y, statusWidth, 20);
        const QRect unread = status.additionalUnread ? QRect(badge.right() + 6, y, 22, 20) : QRect();
        const QRect question = questionWidth ? QRect((unread.isEmpty() ? badge : unread).right() + 6, y, questionWidth, 20) : QRect();
        return {QRect(x, y, titleWidth, 21), badge, unread, question,
            providerWidth ? QRect(right - providerWidth, y + 1, providerWidth, 18) : QRect(), text, agent};
    }
    struct StripLayout { QRect status, provider, elapsed; QStringList lines; int top, textWidth; };
    static StripLayout stripLayout(const QStyleOptionViewItem &option, const QModelIndex &index) {
        const QRect r = sessionCardRect(option.rect);
        const int side = r.height(), inset = 9, textWidth = side - 2 * inset;
        QFont font = option.font; font.setPixelSize(12); font.setWeight(QFont::DemiBold);
        const auto lines = SessionStrip::twoLines(index.data(SessionRoles::Title).toString(), QFontMetrics(font), textWidth);
        const int block = 20 + 7 + int(lines.size()) * 16 + 5 + 13;
        const int top = r.y() + qMax(6, (side - block) / 2);
        return {QRect(r.x() + inset, top, 26, 20), QRect(r.x() + side - inset - 26, top + 1, 26, 18),
            QRect(r.x() + inset, top + 20 + 7 + int(lines.size()) * 16 + 5, textWidth, 13), lines, top, textWidth};
    }
    static QRect counterRect(int &right, const QRect &r, const QString &text, const QFont &font) {
        const int width = CachedText::width(font, text) + 12;
        const QRect badge(right - width, r.center().y() - 9, width, 18);
        right -= width + 6; return badge;
    }
    static QRect stripCounterRect(const QRect &band, const QString &text, const QFont &font) {
        const int width = CachedText::width(font, text) + 12;
        return {band.right() - 5 - width, band.center().y() - 9, width, 18};
    }
    static QList<QRect> headerAnimationBadges(const QStyleOptionViewItem &option, const QModelIndex &index) {
        const int working = index.data(SessionRoles::Working).toInt();
        if (!working) return {};
        const qreal expansion = SessionStrip::expansion(option.widget);
        const int attention = index.data(SessionRoles::Attention).toInt(), unread = index.data(SessionRoles::Unread).toInt();
        const QRect band = option.rect.adjusted(2, 5, -2, -3), r = band.adjusted(9, 0, -9, 0);
        QFont font = option.font; font.setPixelSize(11); font.setWeight(QFont::Medium);
        int right = r.right();
        counterRect(right, r, QString::number(index.data(SessionRoles::Total).toInt()), font);
        if (attention) counterRect(right, r, QString("! %1").arg(attention), font);
        if (unread) counterRect(right, r, QString("● %1").arg(unread), font);
        QList<QRect> result;
        if (SessionStrip::fullOpacity(expansion) > 0) result.append(counterRect(right, r, QString("● %1").arg(working), font));
        if (SessionStrip::stripOpacity(expansion) > 0 && !attention && !unread)
            result.append(stripCounterRect(band, QString("● %1").arg(working), font));
        return result;
    }
    // Shared with the renderer: the timer never guesses badge positions.
    static QRegion animationRegion(const QStyleOptionViewItem &option, const QModelIndex &index) {
        const qreal expansion = SessionStrip::expansion(option.widget);
        QRegion result;
        if (index.data(SessionRoles::Header).toBool()) {
            for (const QRect &badge : headerAnimationBadges(option, index)) result += badge;
        } else {
            const auto status = statusForView(option, index);
            if (status.kind != SessionStatusBadge::Working) return {};
            if (expansion >= 1) result += SessionStatusBadge::workingIndicatorRect(rowCollapsed(index) ? compactLayout(option, index).status : titleLayout(option, index, status).status);
            else {
                QStyleOptionViewItem wide(option);
                wide.rect.setWidth(qMax(option.rect.width(), option.widget ? option.widget->property("expandedRowWidth").toInt() : 0));
                auto strip = stripLayout(option, index);
                if (!index.data(SessionRoles::ChildId).toString().isEmpty()) {
                    strip.status.moveTop(option.rect.center().y() - 10);
                    if (SessionStrip::fullOpacity(expansion) > 0) result += SessionStatusBadge::workingIndicatorRect(titleLayout(wide, index, status).status);
                    if (SessionStrip::stripOpacity(expansion) > 0) result += SessionStatusBadge::workingIndicatorRect(strip.status);
                } else result += SessionStatusBadge::workingIndicatorRect(SessionStrip::mix(strip.status, titleLayout(wide, index, status).status, expansion));
            }
        }
        return result.intersected(option.rect);
    }
    static QRegion elapsedRegion(const QStyleOptionViewItem &option, const QModelIndex &index) {
        if (index.data(SessionRoles::Header).toBool()) return {};
        // Caption width changes can move the title, status, unread and provider.
        QStyleOptionViewItem wide(option);
        const qreal expansion = SessionStrip::expansion(option.widget);
        if (rowCollapsed(index) && expansion >= 1) return QRegion(option.rect);
        if (expansion < 1) wide.rect.setWidth(qMax(option.rect.width(), option.widget ? option.widget->property("expandedRowWidth").toInt() : 0));
        const auto title = titleLayout(wide, index, statusForView(option, index));
        QRegion result(QRect(option.rect.x(), title.title.y(), option.rect.width(), 21));
        if (expansion < 1) {
            auto strip = stripLayout(option, index);
            result += strip.elapsed;
            if (!index.data(SessionRoles::ChildId).toString().isEmpty()) strip.status.moveTop(option.rect.center().y() - 10);
            // Cover the travelling status/unread badges and both title layouts.
            const int top = qMin(title.title.top(), strip.status.top() - 6);
            const int bottom = qMax(title.title.bottom(), strip.status.bottom());
            // Unclipped travelling unread backgrounds have a rounded AA edge.
            result += QRect(option.rect.x(), top - 1, option.rect.width(), bottom - top + 3);
        }
        return result.intersected(option.rect);
    }
private:
    bool paintIndicatorClip(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index, qreal expansion) const {
        // Alpha layers during expansion need their original painter order.
        if ((expansion != 0 && expansion != 1) || p->opacity() != 1) return false;
        QRegion clip;
        if (const auto *list = qobject_cast<const SessionList *>(option.widget)) clip = list->currentPaintRegion();
        if (p->hasClipping()) clip = clip.isEmpty() ? p->clipRegion() : clip.intersected(p->clipRegion());
        if (clip.isEmpty()) return false;
        clip &= option.rect;
        if (clip.isEmpty()) return true;
        const QRegion animated = animationRegion(option, index);
        if (animated.isEmpty() || !clip.subtracted(animated).isEmpty()) return false;
        const bool dark = option.widget && option.widget->property("hgsDark").toBool();
        const qreal pulse = option.widget ? option.widget->property("workingPulse").toReal() : 0.0;
        if (index.data(SessionRoles::Header).toBool()) {
            // Restore the band's pixels below the counter's antialiased corners.
            const QRect band = option.rect.adjusted(2, 5, -2, -3);
            const QColor fill = ProjectAppearance::fill(index.data(SessionRoles::ProjectColor).value<QColor>(), dark, index.data(SessionRoles::ProjectVivid).toBool());
            p->save(); p->setClipRegion(clip, Qt::IntersectClip); p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen); p->setBrush(fill); p->drawRoundedRect(band, 6, 6);
            QColor ink(dark ? "#55e39a" : "#12834e"); ink.setAlphaF(.72 + .28 * pulse);
            QFont font = option.font; font.setPixelSize(11); font.setWeight(QFont::Medium); p->setFont(font);
            for (const QRect &badge : headerAnimationBadges(option, index)) {
                p->setPen(Qt::NoPen); p->setBrush(QColor(dark ? "#123624" : "#e0f5e9")); p->drawRoundedRect(badge, 5, 5);
                p->setPen(ink); CachedText::draw(p, badge, Qt::AlignCenter, QString("● %1").arg(index.data(SessionRoles::Working).toInt()));
            }
            p->restore();
        } else {
            const auto status = statusForView(option, index);
            QRect badge = expansion == 1 ? (rowCollapsed(index) ? compactLayout(option, index).status : titleLayout(option, index, status).status) : stripLayout(option, index).status;
            if (expansion == 0 && !index.data(SessionRoles::ChildId).toString().isEmpty()) badge.moveTop(option.rect.center().y() - 10);
            p->save(); p->setClipRegion(clip, Qt::IntersectClip); p->setFont(option.font);
            SessionStatusBadge::paint(p, badge, {}, SessionStatusBadge::Working, dark, pulse, 0, true); p->restore();
        }
        return true;
    }
    struct SharedBadges { QRect status, unread, provider; QString agent; };
    void paintCompactRow(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {
        p->save(); p->setClipRect(option.rect, Qt::IntersectClip); p->setRenderHint(QPainter::Antialiasing);
        const bool dark = option.widget && option.widget->property("hgsDark").toBool();
        const auto status = statusForView(option, index);
        QStyleOptionViewItem cardOption(option); cardOption.rect.adjust(0, 2, 0, -4);
        IdentityBadges::paintRow(p, cardOption, dark, status.emphasis);
        const auto layout = compactLayout(option, index);
        QFont font = option.font; font.setPixelSize(14); font.setWeight(QFont::DemiBold);
        p->setFont(font); p->setPen(QColor(dark ? "#e8edf4" : "#1a2733"));
        CachedText::draw(p, layout.title, Qt::AlignVCenter, layout.text);
        SessionStatusBadge::paint(p, layout.status, status.caption, status.kind, dark,
            option.widget ? option.widget->property("workingPulse").toReal() : 0.0);
        IdentityBadges::paint(p, layout.provider, IdentityBadges::Provider, layout.agent, dark);
        const QColor muted(dark ? "#a1adbb" : "#647386");
        if (!layout.mute.isEmpty()) workspaceIcon("mute", muted).paint(p, layout.mute);
        // The live list overlays this glyph with a real accessible button.
        workspaceIcon("expand-row", muted).paint(p, layout.expand.adjusted(4, 4, -4, -4));
        p->restore();
    }
    void paintHeader(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index, qreal expansion) const {
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        const bool dark = option.widget && option.widget->property("hgsDark").toBool();
        const int attention = index.data(SessionRoles::Attention).toInt(), working = index.data(SessionRoles::Working).toInt(), unread = index.data(SessionRoles::Unread).toInt();
        const QRect band = option.rect.adjusted(2, 5, -2, -3), r = band.adjusted(9, 0, -9, 0);
        const QColor projectColor = index.data(SessionRoles::ProjectColor).value<QColor>();
        const bool vivid = index.data(SessionRoles::ProjectVivid).toBool();
        const auto fg = ProjectAppearance::ink(projectColor,dark,vivid);
        const auto background = ProjectAppearance::fill(projectColor,dark,vivid);
        p->setPen(Qt::NoPen); p->setBrush(background); p->drawRoundedRect(band,6,6);
        // The band keeps its color at any width; its contents fade into the strip summary.
        const qreal full = SessionStrip::fullOpacity(expansion), strip = SessionStrip::stripOpacity(expansion);
        p->setOpacity(full);
        p->setPen(QPen(fg, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPoint c(r.x() + 3, r.center().y()); QPolygon arrow;
        if (index.data(SessionRoles::Collapsed).toBool()) arrow << c + QPoint(-2,-4) << c + QPoint(2,0) << c + QPoint(-2,4);
        else arrow << c + QPoint(-4,-2) << c + QPoint(0,2) << c + QPoint(4,-2);
        p->drawPolyline(arrow);
        workspaceIcon("projects", fg).paint(p, QRect(r.x() + 15, r.center().y() - 8, 16, 16));
        QFont font = option.font; font.setPixelSize(11); font.setWeight(QFont::Medium); p->setFont(font);
        int right = r.right();
        const auto counter = [&](const QString &text, const QColor &color, const QColor &background) {
            const QRect badge = counterRect(right, r, text, font); p->setPen(Qt::NoPen); p->setBrush(background);
            p->drawRoundedRect(badge, 5, 5); p->setPen(color); CachedText::draw(p, badge, Qt::AlignCenter, text);
        };
        counter(QString::number(index.data(SessionRoles::Total).toInt()), fg, MachineAppearance::blend(background,fg,.13));
        if (attention) counter(QString("! %1").arg(attention), QColor("#392900"), QColor(dark ? "#ffda76" : "#f4ce65"));
        if (unread) counter(QString("● %1").arg(unread), QColor("#392900"), QColor(dark ? "#ffda76" : "#f4ce65"));
        QColor progress(dark ? "#55e39a" : "#12834e");
        progress.setAlphaF(.72 + .28 * (option.widget ? option.widget->property("workingPulse").toReal() : 0.0));
        if (working) counter(QString("● %1").arg(working), progress, QColor(dark ? "#123624" : "#e0f5e9"));
        font.setPixelSize(12); font.setWeight(QFont::DemiBold); p->setFont(font); p->setPen(fg);
        CachedText::draw(p, QRect(r.x() + 39, r.y(), qMax(0, right - r.x() - 39), r.height()), Qt::AlignVCenter,
            CachedText::elided(font, index.data(SessionRoles::Title).toString(), Qt::ElideRight, qMax(0, right - r.x() - 39)));
        if (strip > 0) {
            // One counter per band in the strip: what needs a response, then
            // unread replies, then work in progress, then the session count.
            QString text = QString::number(index.data(SessionRoles::Total).toInt());
            QColor ink = fg, fill = MachineAppearance::blend(background, fg, .13);
            if (attention || unread) {
                text = attention ? QString("! %1").arg(attention) : QString("● %1").arg(unread);
                ink = QColor("#392900"); fill = QColor(dark ? "#ffda76" : "#f4ce65");
            } else if (working) {
                text = QString("● %1").arg(working); ink = progress; fill = QColor(dark ? "#123624" : "#e0f5e9");
            }
            font.setPixelSize(11); font.setWeight(QFont::Medium); p->setFont(font); p->setOpacity(strip);
            const QRect badge = stripCounterRect(band, text, font);
            p->setPen(Qt::NoPen); p->setBrush(fill); p->drawRoundedRect(badge, 5, 5); p->setPen(ink); CachedText::draw(p, badge, Qt::AlignCenter, text);
            // The project name takes what the counter leaves.
            const QRect name(band.x() + 8, band.y(), badge.left() - 5 - band.x() - 8, band.height());
            font.setWeight(QFont::DemiBold); p->setFont(font); p->setPen(fg);
            if (name.width() >= 14) CachedText::draw(p, name, Qt::AlignVCenter | Qt::AlignLeft,
                CachedText::elided(font, index.data(SessionRoles::Title).toString(), Qt::ElideRight, name.width()));
        }
        p->restore();
    }
    // The full card. With shared set, its surface and the badges that travel
    // between strip and card are left to the caller, which records their places.
    void paintCard(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index, SharedBadges *shared = nullptr) const {
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        const bool dark = option.widget && option.widget->property("hgsDark").toBool();
        const bool childRow = !index.data(SessionRoles::ChildId).toString().isEmpty();
        const bool expandable = index.data(SessionRoles::HasChildren).toBool();
        const QRect r = sessionCardRect(option.rect).adjusted(childRow ? 24 : 0, 0, 0, 0);
        const bool compact = option.widget && option.widget->property("compact").toBool();
        const auto status = statusForView(option, index);
        const bool attention = status.attention, unread = status.unread, working = status.working, paused = status.paused;
        const QString state = status.state;
        p->setClipRect(option.rect, Qt::IntersectClip);
        QStyleOptionViewItem cardOption(option); cardOption.rect.adjust(childRow ? 24 : 0, 2, 0, -4);
        if (!shared) IdentityBadges::paintRow(p, cardOption, dark, status.emphasis);
        const int x = r.x() + 12, textX = x + 8, right = r.right() - 11, width = right - textX;
        const QColor foreground(dark ? "#e8edf4" : "#1a2733"), muted(dark ? "#a1adbb" : "#647386");
        if (childRow) {
            p->setPen(QPen(QColor(dark ? "#38434e" : "#c5cfd9"), 1));
            p->drawLine(option.rect.left() + 17, option.rect.top(), option.rect.left() + 17, option.rect.bottom());
            p->drawLine(option.rect.left() + 17, r.center().y(), r.left() - 3, r.center().y());
        }
        const int titleY = r.y() + (childRow ? (compact ? 5 : 6) : (compact ? 3 : 5));
        const int metadataY = titleY + (compact ? 22 : 24);
        const int detailY = metadataY + (compact ? 20 : 22);
        const int modelY = option.rect.bottom() - 26;

        // Provider goes with the session title; machine goes with its folder.
        // At narrow widths the provider keeps its icon, leaving room for the
        // title and elapsed status. The footer is independent of those badges.
        const auto kind = status.kind;
        const QString caption = status.caption;
        const bool additionalUnread = status.additionalUnread;
        const auto title = titleLayout(option, index, status);
        QFont font = option.font; font.setPixelSize(14); font.setWeight(QFont::DemiBold);
        const QString agent = title.agent, host = index.data(SessionRoles::Host).toString();
        const int providerWidth = title.provider.width();
        p->setFont(font); p->setPen(foreground);
        CachedText::draw(p, title.title, Qt::AlignVCenter, title.text);
        if (status.muted) workspaceIcon("mute", SessionStatusBadge::mutedTone(dark)).paint(p, QRect(title.title.x() - 18, title.title.y() + 3, 14, 14));
        const QRect statusBadge = title.status, unreadBadge = title.unread, providerBadge = title.provider;
        if (shared) *shared = {statusBadge, unreadBadge, providerBadge, agent};
        else {
            SessionStatusBadge::paint(p, statusBadge, caption, kind, dark,
                option.widget ? option.widget->property("workingPulse").toReal() : 0.0);
            if (additionalUnread) SessionStatusBadge::paint(p, unreadBadge, {}, status.muted ? SessionStatusBadge::MutedUnread : SessionStatusBadge::Unread, dark);
            if (providerWidth) IdentityBadges::paint(p, providerBadge, IdentityBadges::Provider, agent, dark);
        }
        // While a strip tile grows, the chip fades in at its place with the card.
        if (!title.question.isEmpty())
            OptionalQuestionBadge::paint(p, title.question, optionalQuestionCount(index), option.font, dark, status.muted);

        if (childRow) {
            font.setPixelSize(11); font.setWeight(QFont::Normal); p->setFont(font); p->setPen(muted);
            CachedText::draw(p, QRect(x, titleY + 25, right - x, 18), Qt::AlignVCenter,
                CachedText::elided(font, index.data(SessionRoles::Detail).toString().simplified(), Qt::ElideRight, right - x));
            p->restore(); return;
        }
        font.setPixelSize(11); font.setWeight(QFont::Normal); p->setFont(font);
        const int hostWidth = IdentityBadges::width(IdentityBadges::Machine, host, font, width / 3 + 10);
        const QRect machineBadge(right - hostWidth, metadataY, hostWidth, 18);
        IdentityBadges::paint(p, machineBadge, IdentityBadges::Machine, host, dark, index.data(SessionRoles::MachineColor).value<QColor>(), index.data(SessionRoles::MachineName).toString());
        p->setPen(muted);
        const QRect gitBadge = gitStatusRect(option.rect, index, option.font, compact);
        if (!gitBadge.isEmpty()) GitStatusBadge::paint(p, gitBadge,
            GitStatusBadge::fitted(GitStatusBadge::marks(index.data(SessionRoles::GitStatus).toJsonObject()), option.font, gitBadge.width()), option.font, dark);
        const int metadataWidth = qMax(0, (!gitBadge.isEmpty() ? gitBadge.left() : machineBadge.left()) - textX - 8);
        const QString metaIcon = index.data(SessionRoles::MetaIcon).toString();
        const int iconInset = !metaIcon.isEmpty() && metadataWidth >= 32 ? 18 : 0;
        if (iconInset) workspaceIcon(metaIcon, muted).paint(p, QRect(textX, metadataY + 2, 14, 14));
        CachedText::draw(p, QRect(textX + iconInset, metadataY, qMax(0, metadataWidth - iconInset), 18), Qt::AlignVCenter,
                    CachedText::elided(font, index.data(SessionRoles::Meta).toString(), Qt::ElideMiddle, qMax(0, metadataWidth - iconInset)));

        QString detail = index.data(SessionRoles::Detail).toString();
        // Status is already beside the title. Keep the activity excerpt useful
        // without repeating "Working" or treating an old tool as a new reply.
        if (detail == state) detail.clear();
        else if (detail.startsWith(state + ": ")) detail.remove(0, state.size() + 2);
        if (detail.isEmpty() && unread && !working && !attention) detail = tr("Response ready to read");
        else if (detail.isEmpty() && state == "Ready") detail = tr("Waiting for your next message");
        else if (detail.isEmpty() && state == "Not tracked") detail = tr("Open terminal to view activity");
        else if (detail.isEmpty() && paused) detail = tr("Resume to continue");
        const bool reviewLater = index.data(SessionRoles::ReviewLater).toBool();
        const int detailX = textX + (reviewLater ? 18 : 0);
        if (reviewLater) workspaceIcon("attention", status.muted ? SessionStatusBadge::mutedTone(dark) : QColor(dark ? "#f0c77b" : "#885400")).paint(p, QRect(textX, detailY + 2, 14, 14));
        const int detailWidth = width - (detailX - textX);
        font.setPixelSize(12); p->setFont(font); p->setPen(state == "Offline" ? muted : foreground);
        CachedText::draw(p, QRect(detailX, detailY, detailWidth, 18), Qt::AlignVCenter, CachedText::elided(font, detail, Qt::ElideRight, detailWidth));

        font.setPixelSize(11); p->setFont(font); p->setPen(muted);
        const QString children = SessionList::childrenLabel(index);
        const QRect childrenControl = SessionList::childrenControlRect(option.rect, index, option.font);
        const int childWidth = expandable ? childrenControl.width() : children.isEmpty() ? 0 : CachedText::width(font, children) + 32;
        if (!children.isEmpty()) {
            const QRect counter = expandable ? childrenControl : QRect(right - childWidth, modelY, childWidth, 20);
            const bool hovered = expandable && option.widget && option.widget->property("hoveredChildren").toString() == index.data(SessionRoles::Key).toString();
            if (expandable) {
                p->setPen(Qt::NoPen); p->setBrush(QColor(dark ? (hovered ? "#344650" : "#28323c") : (hovered ? "#d4e4e9" : "#e7edf2")));
                p->drawRoundedRect(counter, 4, 4);
            }
            const QColor ink = hovered ? foreground : muted;
            const qreal cx = counter.x() + 10, cy = counter.y() + counter.height() / 2.0;
            p->setPen(QPen(ink, 1.2)); p->setBrush(Qt::NoBrush);
            p->drawEllipse(QPointF(cx, cy - 3), 2, 2); p->drawEllipse(QPointF(cx + 6, cy - 2), 2, 2);
            p->drawArc(QRectF(cx - 4, cy, 8, 6), 0, 180 * 16); p->drawArc(QRectF(cx + 2, cy + 1, 7, 5), 0, 180 * 16);
            CachedText::draw(p, counter.adjusted(26, 0, expandable ? -20 : -6, 0), Qt::AlignCenter, children);
            if (expandable) {
                p->setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                const QPointF c(counter.right() - 9, cy); QPolygonF arrow;
                const int direction = index.data(SessionRoles::Expanded).toBool() ? -1 : 1;
                arrow << c + QPoint(-3, -direction) << c + QPoint(0, 2 * direction) << c + QPoint(3, -direction);
                p->drawPolyline(arrow);
            }
        }
        font.setPixelSize(10); p->setFont(font);
        const QString modified = index.data(SessionRoles::ModifiedAt).toString();
        const int modifiedWidth = modified.isEmpty() ? 0 : qMin(width / 2, qCeil(CachedText::advance(font, modified)));
        const int footerWidth = width - (modifiedWidth ? modifiedWidth + 10 : 0) - (childWidth ? childWidth + 10 : 0);
        const QString effort = index.data(SessionRoles::Effort).toString();
        const bool hasEffort = !effort.isEmpty() && effort != "—"
            && (modified.isEmpty() || CachedText::width(font, effort) + 10 <= footerWidth);
        const int effortWidth = hasEffort ? CachedText::width(font, effort) + 10 : 0;
        const int modelWidth = qMax(0, footerWidth - (hasEffort ? effortWidth + 7 : 0));
        const QString model = CachedText::elided(font, index.data(SessionRoles::Model).toString(), Qt::ElideMiddle, modelWidth);
        p->setPen(muted); CachedText::draw(p, QRect(textX, modelY, modelWidth, 20), Qt::AlignVCenter, model);
        if (hasEffort) {
            const QRect effortBadge(textX + CachedText::width(font, model) + 7, modelY + 2, effortWidth, 16);
            p->setPen(Qt::NoPen); p->setBrush(QColor(dark ? "#2b343e" : "#e7edf2")); p->drawRoundedRect(effortBadge, 3, 3);
            p->setPen(muted); CachedText::draw(p, effortBadge, Qt::AlignCenter, effort);
        }
        if (modifiedWidth) {
            p->setPen(muted);
            CachedText::draw(p, QRect(right - modifiedWidth, modelY, modifiedWidth, 20), Qt::AlignVCenter | Qt::AlignRight,
                CachedText::elided(font, modified, Qt::ElideMiddle, modifiedWidth));
        }
        p->restore();
    }
    // A strip tile on its way to a full card: the surface follows the row width,
    // status and provider badges travel to their card places, the card text
    // fades in at full width and the initials fade out.
    void paintStripCard(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index, qreal expansion) const {
        p->save(); p->setRenderHint(QPainter::Antialiasing); p->setClipRect(option.rect, Qt::IntersectClip);
        const bool dark = option.widget && option.widget->property("hgsDark").toBool();
        const qreal full = SessionStrip::fullOpacity(expansion), strip = SessionStrip::stripOpacity(expansion);
        const qreal pulse = option.widget ? option.widget->property("workingPulse").toReal() : 0.0;
        QStyleOptionViewItem wide(option);
        wide.rect.setWidth(qMax(option.rect.width(), option.widget ? option.widget->property("expandedRowWidth").toInt() : 0));
        const auto status = statusForView(option, index);
        const QRect r = sessionCardRect(option.rect);
        // A square tile with one centred block: status and provider icons, the
        // name on up to two lines, then the state. Insets keep it off the edge mark.
        const auto layout = stripLayout(option, index);
        const int inset = 9, textWidth = layout.textWidth, top = layout.top;
        QFont nameFont = option.font; nameFont.setPixelSize(12); nameFont.setWeight(QFont::DemiBold);
        const auto lines = layout.lines;
        const QRect stripStatus = layout.status, stripProvider = layout.provider;
        const QColor muted(dark ? "#a1adbb" : "#647386"), foreground(dark ? "#e8edf4" : "#1a2733");
        const auto neutralDot = [&](const QRect &badge) {
            if (status.kind != SessionStatusBadge::Neutral || strip <= 0) return;
            p->save(); p->setOpacity(strip * .6); p->setPen(Qt::NoPen); p->setBrush(muted);
            p->drawEllipse(QPointF(badge.center()) + QPointF(.5, .5), 2.5, 2.5); p->restore();
        };
        if (!index.data(SessionRoles::ChildId).toString().isEmpty()) {
            // Subagent rows keep their status and name in the strip.
            p->save(); p->setOpacity(full); paintCard(p, wide, index); p->restore();
            const QRect badge(stripStatus.x(), option.rect.center().y() - 10, 26, 20);
            p->setOpacity(strip); SessionStatusBadge::paint(p, badge, status.caption, status.kind, dark, pulse, 0); neutralDot(badge);
            QFont font = option.font; font.setPixelSize(11); p->setFont(font); p->setPen(muted);
            const QRect name(badge.right() + 6, badge.y(), qMax(0, r.right() - 5 - badge.right() - 6), badge.height());
            CachedText::draw(p, name, Qt::AlignVCenter | Qt::AlignLeft, CachedText::elided(font, index.data(SessionRoles::Title).toString(), Qt::ElideRight, name.width()));
            p->restore(); return;
        }
        // Every tile has its own surface in the strip; cards in the list do not.
        const bool selected = option.state & QStyle::State_Selected, hovered = option.state & QStyle::State_MouseOver;
        const QColor surface = SessionStrip::surface(dark);
        if (strip > 0) {
            p->save(); p->setOpacity(strip); p->setPen(Qt::NoPen); p->setBrush(surface);
            p->drawRoundedRect(QRectF(r), 8, 8); p->restore();
        }
        QStyleOptionViewItem cardOption(option); cardOption.rect.adjust(0, 2, 0, -4);
        IdentityBadges::paintRow(p, cardOption, dark);
        // A tile shows its state in the color of its frame, with a faint glow
        // inward; the card's attention edge appears only as it grows.
        const QColor edge = SessionStatusBadge::edge(status.emphasis == IdentityBadges::Draft ? SessionStatusBadge::Draft : status.kind, dark);
        if (strip > 0) {
            p->save(); p->setOpacity(strip); p->setBrush(Qt::NoBrush);
            for (int i = 1; edge.isValid() && i <= 7; ++i) {
                QColor glow(edge); glow.setAlphaF(.26 * qPow(1 - (i - 1) / 7.0, 1.6));
                const qreal radius = qMax(1.0, 7.5 - i);
                p->setPen(QPen(glow, 1)); p->drawRoundedRect(QRectF(r).adjusted(i + .5, i + .5, -i - .5, -i - .5), radius, radius);
            }
            // A selected tile without a state keeps the selection frame.
            if (edge.isValid() || !selected) {
                p->setPen(QPen(edge.isValid() ? edge : QColor(dark ? "#29313a" : "#dfe5eb"), edge.isValid() ? 1.5 : 1));
                const qreal half = edge.isValid() ? .75 : .5;
                p->drawRoundedRect(QRectF(r).adjusted(half, half, -half, -half), 8, 8);
            }
            p->restore();
        }
        if (status.emphasis != IdentityBadges::Normal && full > 0) {
            p->save(); p->setOpacity(full);
            IdentityBadges::paintEdge(p, cardOption.rect, IdentityBadges::emphasisColor(dark, status.emphasis));
            p->restore();
        }
        SharedBadges shared;
        p->save(); p->setOpacity(full); paintCard(p, wide, index, &shared); p->restore();
        const QRect statusBadge = SessionStrip::mix(stripStatus, shared.status, expansion);
        SessionStatusBadge::paint(p, statusBadge, status.caption, status.kind, dark, pulse, full);
        neutralDot(statusBadge);
        if (status.additionalUnread) {
            // A dot on the strip badge grows into the separate unread badge.
            const QRect badge = SessionStrip::mix(QRect(stripStatus.right() - 6, stripStatus.y() - 4, 10, 10), shared.unread, expansion);
            p->setPen(Qt::NoPen);
            if (strip > 0) {
                p->setOpacity(strip);
                p->setBrush(selected ? QColor(dark ? "#303b47" : "#dde7ef") : hovered ? QColor(dark ? "#242c34" : "#edf1f4") : surface);
                p->drawRoundedRect(badge.adjusted(-2, -2, 2, 2), 7, 7);
            }
            p->setOpacity(1); p->setBrush(status.muted ? QColor(dark ? "#2b343e" : "#e7edf2") : QColor(dark ? "#ffda76" : "#f4ce65")); p->drawRoundedRect(badge, 5, 5);
            if (full > 0) {
                p->setOpacity(full); p->setPen(QPen(status.muted ? SessionStatusBadge::mutedTone(dark) : QColor("#392900"), 1)); p->setBrush(Qt::NoBrush);
                const QPointF c(badge.x() + 11, badge.center().y() + .5);
                p->drawRoundedRect(QRectF(c.x() - 4, c.y() - 3, 8, 6), 1, 1);
                p->drawLine(c + QPointF(-4, -3), c); p->drawLine(c, c + QPointF(4, -3));
            }
            p->setOpacity(1);
        }
        if (!shared.agent.isEmpty())
            IdentityBadges::paint(p, SessionStrip::mix(stripProvider, shared.provider, expansion),
                IdentityBadges::Provider, shared.agent, dark, {}, {}, full);
        if (strip > 0 && index.data(SessionRoles::OptionalQuestions).toInt() > 0) {
            // The card's question chip replaces this mark as the tile grows.
            p->setOpacity(strip);
            OptionalQuestionBadge::paintStripMark(p, OptionalQuestionBadge::stripMarkRect(stripProvider), option.font, dark,
                selected ? QColor(dark ? "#303b47" : "#dde7ef") : hovered ? QColor(dark ? "#242c34" : "#edf1f4") : surface, status.muted);
            p->setOpacity(1);
        }
        if (strip > 0) {
            p->setOpacity(strip);
            if (status.muted) workspaceIcon("mute", SessionStatusBadge::mutedTone(dark)).paint(p, QRect(r.center().x() - 7, top + 3, 14, 14));
            p->setFont(nameFont); p->setPen(foreground);
            int y = top + 20 + 7;
            for (const auto &line : lines) { CachedText::draw(p, QRect(r.x() + inset, y, textWidth, 16), Qt::AlignCenter, line); y += 16; }
            // The state in words: how long it has worked, or what it waits for.
            const double since = index.data(SessionRoles::WorkingSince).toDouble();
            const QString state = status.working && since > 0 ? SessionElapsed::text(since) : status.caption;
            const QColor tone = status.kind == SessionStatusBadge::Working ? QColor(dark ? "#55e39a" : "#12834e")
                : status.kind == SessionStatusBadge::Background ? QColor(dark ? "#aac0df" : "#466b99")
                : status.kind == SessionStatusBadge::Error ? QColor(dark ? "#ffabb6" : "#a32238")
                : status.kind == SessionStatusBadge::Attention || status.kind == SessionStatusBadge::Unread ? QColor(dark ? "#f0c77b" : "#885400")
                : status.kind == SessionStatusBadge::MutedAttention || status.kind == SessionStatusBadge::MutedUnread ? SessionStatusBadge::mutedTone(dark)
                : status.kind == SessionStatusBadge::Scheduled ? QColor(dark ? "#8fd3e6" : "#1f6b7d") : muted;
            QFont font = option.font; font.setPixelSize(10); font.setWeight(QFont::Medium); p->setFont(font); p->setPen(tone);
            CachedText::draw(p, layout.elapsed, Qt::AlignCenter, CachedText::elided(font, state, Qt::ElideRight, textWidth));
        }
        p->restore();
    }
};
