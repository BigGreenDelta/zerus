#pragma once
#include "CachedText.h"
#include "SessionStatusBadge.h"
#include "MachineAppearance.h"
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QLabel>
#include <QStyleOptionViewItem>
#include <QWidget>
#include <QtMath>

// The same paint path is used by list delegates and standalone header badges.
namespace IdentityBadges {
enum Kind { Provider, Machine };
inline QString providerName(const QString &value) {
    return value == "codex" ? QStringLiteral("Codex") : value == "claude" ? QStringLiteral("Claude") :
        value == "kimi" ? QStringLiteral("Kimi") : value == "opencode" ? QStringLiteral("OpenCode") : value == "dsh" ? QStringLiteral("DeepSeek") : value == "sh" ? QStringLiteral("Shell") : value;
}
inline QColor providerColor(const QString &value, bool dark) {
    return value == "claude" ? QColor(dark ? "#e8b29b" : "#956044") :
        value == "kimi" ? QColor(dark ? "#bac1ff" : "#5657a2") :
        value == "dsh" ? QColor(dark ? "#8cb9ff" : "#315bb1") : QColor(dark ? "#a6d8c5" : "#38755d");
}
inline QFont font(QFont base, Kind kind) {
    base.setPixelSize(11); base.setWeight(kind == Provider ? QFont::Medium : QFont::Normal); return base;
}
inline int width(Kind kind, const QString &value, const QFont &base, int maximum = 120) {
    const QString text = kind == Provider ? providerName(value) : value;
    // elidedText compares fractional advances: rounding down can turn even
    // "mac" into an ellipsis with the macOS system font.
    return qMin(maximum, qCeil(CachedText::advance(font(base, kind), text)) + (kind == Provider ? 34 : 12));
}
inline void paint(QPainter *p, const QRect &rect, Kind kind, const QString &value, bool dark, QColor machineColor = {}, const QString &machineIdentity = {}, qreal labelOpacity = 1) {
    if (rect.isEmpty() || value.isEmpty()) return;
    p->save(); p->setClipRect(rect, Qt::IntersectClip); p->setRenderHint(QPainter::Antialiasing);
    const auto f = font(p->font(), kind); p->setFont(f);
    const QString text = kind == Provider ? providerName(value) : value;
    if (!machineColor.isValid() && kind == Machine) machineColor = MachineAppearance::color(value);
    const bool bright = kind == Machine && MachineAppearance::vivid(machineIdentity.isEmpty() ? value : machineIdentity);
    const QColor foreground = kind == Machine ? MachineAppearance::textColor(machineColor, dark, bright) :
        providerColor(value, dark);
    p->setPen(Qt::NoPen); p->setBrush(kind == Machine ? MachineAppearance::backgroundColor(machineColor, dark, bright) : QColor(dark ? "#303942" : "#e5ebef"));
    p->drawRoundedRect(rect, 4, 4);
    if (kind == Provider) {
        p->setPen(QPen(foreground, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF c(rect.x() + 10, rect.center().y() + .5);
        if (value == "claude") {
            for (int i = 0; i < 8; ++i) { p->save(); p->translate(c); p->rotate(i * 45); p->drawLine(QPointF(0, 2), QPointF(0, 5)); p->restore(); }
        } else if (value == "kimi") {
            p->drawLine(c + QPointF(-3, -4), c + QPointF(-3, 4));
            p->drawLine(c + QPointF(-3, 0), c + QPointF(3, -4)); p->drawLine(c + QPointF(-3, 0), c + QPointF(3, 4));
        } else if (value == "opencode") {
            p->drawLine(c+QPointF(-2,-3),c+QPointF(-5,0));p->drawLine(c+QPointF(-5,0),c+QPointF(-2,3));
            p->drawLine(c+QPointF(2,-3),c+QPointF(5,0));p->drawLine(c+QPointF(5,0),c+QPointF(2,3));
        } else {
            p->drawLine(c + QPointF(-4, -3), c + QPointF(-1, 0)); p->drawLine(c + QPointF(-1, 0), c + QPointF(-4, 3));
            p->drawLine(c + QPointF(1, 3), c + QPointF(4, 3));
        }
    }
    const QRect textRect = rect.adjusted(kind == Provider ? 22 : 6, 0, -6, 0);
    p->setOpacity(p->opacity() * labelOpacity);
    p->setPen(foreground); CachedText::draw(p, textRect, Qt::AlignVCenter | Qt::AlignLeft, labelOpacity < 1 ? text :
        CachedText::elided(f, text, kind == Machine ? Qt::ElideMiddle : Qt::ElideRight, qMax(0, textRect.width())));
    p->restore();
}
enum RowEmphasis { Normal, Attention, Error, Unread, Draft };
inline QColor attentionColor(bool dark, bool error = false) {
    return QColor(error ? (dark ? "#ffabb6" : "#a32238") : (dark ? "#ffda76" : "#805000"));
}
inline QColor emphasisColor(bool dark, RowEmphasis emphasis) {
    return emphasis == Draft ? SessionStatusBadge::edge(SessionStatusBadge::Draft, dark) : attentionColor(dark, emphasis == Error);
}
// The edge mark of an actionable, unread or draft row.
inline void paintEdge(QPainter *p, const QRect &row, const QColor &color) {
    const QRect r = row.adjusted(2, 2, -2, -2);
    p->save(); p->setRenderHint(QPainter::Antialiasing); p->setPen(Qt::NoPen); p->setBrush(color);
    p->drawRoundedRect(QRect(r.x() + 1, r.y() + 10, 3, r.height() - 20), 1.5, 1.5);
    p->restore();
}
inline void paintRow(QPainter *p, const QStyleOptionViewItem &option, bool dark, RowEmphasis emphasis = Normal) {
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    const QRect r = option.rect.adjusted(2, 2, -2, -2);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    // Selection has the same neutral surface in every list and every state.
    // The edge retains draft ownership even when its composer is open.
    const QColor accent = emphasisColor(dark, emphasis);
    const QColor border = selected ? QColor(dark ? "#8295a9" : "#7a91a6") : Qt::transparent;
    const QColor background = selected ? QColor(dark ? "#303b47" : "#dde7ef")
        : hovered ? QColor(dark ? "#242c34" : "#edf1f4") : Qt::transparent;
    p->setPen(QPen(border, 1)); p->setBrush(background);
    p->drawRoundedRect(r, 8, 8);
    if (emphasis != Normal) paintEdge(p, option.rect, accent);
    if ((option.state & QStyle::State_HasFocus) && (option.state & QStyle::State_KeyboardFocusChange)) {
        p->setPen(QPen(QColor(dark ? "#8bdfc0" : "#167357"), 1, Qt::DotLine)); p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(r.adjusted(2, 2, -2, -2), 6, 6);
    }
    p->restore();
}
}

class IdentityBadge : public QLabel {
public:
    explicit IdentityBadge(IdentityBadges::Kind kind, QWidget *parent = nullptr) : QLabel(parent), m_kind(kind) {
        setAttribute(Qt::WA_TransparentForMouseEvents); setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); setFixedHeight(18);
        setTextFormat(Qt::PlainText);
    }
    void setValue(const QString &value) {
        m_value = value; setText(m_kind == IdentityBadges::Provider ? IdentityBadges::providerName(value) : value); setAccessibleName(text());
        setToolTip(accessibleName().toHtmlEscaped()); setVisible(!value.isEmpty()); updateGeometry(); update();
    }
    void setTheme(bool dark) { m_dark = dark; update(); }
    const QString &value() const { return m_value; }
    bool dark() const { return m_dark; }
    QSize sizeHint() const override { return {IdentityBadges::width(m_kind, m_value, font(), m_kind == IdentityBadges::Provider ? 95 : 140), 18}; }
    QSize minimumSizeHint() const override { return {qMin(sizeHint().width(), 44), 18}; }
protected:
    void paintEvent(QPaintEvent *) override { QPainter p(this); IdentityBadges::paint(&p, rect(), m_kind, m_value, m_dark); }
private:
    IdentityBadges::Kind m_kind;
    QString m_value;
    bool m_dark = false;
};
