#include "IconFactory.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QSettings>
#include <QTransform>

namespace {

// Размеры, которые панель реально запрашивает у StatusNotifier-иконки, плюс пара
// промежуточных — набор размеров для разных масштабов экрана.
// Каждый размер рендерится отдельно (а не масштабируется из соседнего), потому что текст
// не переживает масштабирование: его надо укладывать в сетку на том же кегле, в котором
// он будет показан.
constexpr int kIconSizes[] = {16, 22, 24, 32, 48};

// The selected monochrome Swarm: three separated blades, with the same curves
// and transforms as resources/icons/hgs-zerus-symbolic.svg and the v6 preview.
QPainterPath zerusMark()
{
    QPainterPath blade;
    blade.moveTo(39, 9);
    blade.cubicTo(54, 3, 70, 13, 77, 29);
    blade.cubicTo(72, 25, 69, 24, 72, 32);
    blade.cubicTo(81, 44, 77, 58, 65, 65);
    blade.cubicTo(70, 51, 65, 36, 52, 28);
    blade.cubicTo(40, 21, 30, 25, 22, 34);
    blade.cubicTo(25, 21, 38, 15, 51, 19);
    blade.cubicTo(57, 21, 56, 16, 51, 13);
    blade.cubicTo(47, 11, 43, 10, 39, 9);
    blade.closeSubpath();

    QTransform spacing;
    spacing.translate(56, 30);
    spacing.scale(0.88, 0.88);
    spacing.translate(-56, -30);
    blade = spacing.map(blade);

    QPainterPath path;
    for (int angle : {0, 120, 240}) {
        QTransform orbit;
        orbit.translate(50, 50);
        orbit.scale(1.06, 1.06);
        orbit.rotate(angle);
        orbit.translate(-50, -50);
        path.addPath(orbit.map(blade));
    }
    return path;
}

} // namespace

QString IconFactory::countText(int count)
{
    if (count < 0)
        count = 0; // защитно: отрицательной суммы сессий не бывает, но не рисовать же "-5"
    if (count > 99)
        return QStringLiteral("99+");
    return QString::number(count);
}

void IconFactory::setForegroundOverride(const QString &colorName)
{
    m_foregroundOverride = colorName;
}

QColor IconFactory::foreground(const QString &override)
{
    if (!override.isEmpty()) {
        const QColor c(override);
        if (c.isValid())
            return c;
    }
    // Палитра приложения — не «лучшее из ничего вообще», а лучшее из доступного ЗДЕСЬ, и
    // причина у каждой платформы своя.
    //
    // Linux/Plasma: панель не сообщает элементу трея свой фон по протоколу
    // StatusNotifier — item отдаёт готовые пиксели и не получает обратно ничего о
    // поверхности, на которую они лягут. Палитра приложения — приближение: под Plasma это
    // та же цветовая схема, которой панель рисует собственные часы, но совпадение не
    // гарантировано, и override — аварийный люк на случай рассинхрона (светлое приложение
    // поверх тёмной панели и наоборот).
    //
    // Each counter gets a contrasting surface, including on macOS where the
    // menu-bar wallpaper can differ from the application palette.
#ifdef Q_OS_WIN
    // Windows: the taskbar has its own theme, dark by default even when apps are
    // light. The registry value is what Explorer itself follows.
    const QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalize.value(QStringLiteral("SystemUsesLightTheme"), 0).toInt() == 1 ? QColor(Qt::black)
                                                                                     : QColor(Qt::white);
#else
    return QApplication::palette().windowText().color();
#endif
}

QPixmap IconFactory::brandPixmap(int size, const QColor &color)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(size / 100.0, size / 100.0);
    p.fillPath(zerusMark(), color);
    return pm;
}

QPixmap IconFactory::renderTile(int size, const QString &total, const QString &attention,
                                 bool highlighted, bool dot, const QColor &fg)
{
    QPixmap pm(size, size); pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const int gap = qMax(1, qRound(size / 22.0));
    const int upperHeight = (size - gap) / 2;
    const QRectF upper(0, 0, size, upperHeight);
    const QRectF lower(0, upperHeight + gap, size, size - upperHeight - gap);
    const QColor surface = fg.lightnessF() > 0.5 ? QColor("#29323c") : QColor("#e3e9ef");
    const qreal radius = qMax(1.5, size / 9.0);
    p.setPen(Qt::NoPen); p.setBrush(surface); p.drawRoundedRect(upper, radius, radius);
    p.setBrush(highlighted ? QColor("#f3bd64") : surface); p.drawRoundedRect(lower, radius, radius);

    auto drawCount = [&](const QRectF &rect, const QString &text, const QColor &color) {
        QFont font = QApplication::font(); font.setBold(true);
        int pixels = qMax(8, qRound(size * 0.55));
        QPainterPath glyphs;
        do {
            font.setPixelSize(pixels);
            glyphs = QPainterPath(); glyphs.addText(0, 0, font, text);
            const auto bounds = glyphs.boundingRect();
            if ((bounds.width() <= rect.width() && bounds.height() <= rect.height()) || pixels <= 5) break;
            --pixels;
        } while (true);
        const auto bounds = glyphs.boundingRect();
        p.save(); p.translate(rect.center() - bounds.center()); p.fillPath(glyphs, color); p.restore();
    };
    // Keep the Swarm beside everyday totals. Large capped counts use the entire
    // top row so neither "99+" nor the lower attention count has to become tiny.
    const int markSize = total.size() <= 2 ? upperHeight : 0;
    if (markSize) p.drawPixmap(QRect(0, 0, markSize, markSize), brandPixmap(markSize, fg));
    const int inset = qMax(1, qRound(size / 24.0));
    drawCount(upper.adjusted(markSize ? markSize : inset, 1, -inset, -1), total, fg);
    QColor quiet = fg; quiet.setAlpha(160);
    drawCount(lower.adjusted(inset, 1, -inset, -1), attention,
              highlighted ? QColor("#20252c") : quiet);
    if (dot) {
        p.setPen(Qt::NoPen); p.setBrush(fg);
        const qreal r = qMax(1.0, size / 16.0);
        p.drawEllipse(QPointF(size - r, r), r, r);
    }
    return pm;
}

QIcon IconFactory::cached(const QString &total, const QString &attention, bool highlighted, bool dot)
{
    const QColor fg = foreground(m_foregroundOverride);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
        .arg(total, attention).arg(highlighted).arg(dot).arg(fg.name(QColor::HexArgb));
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return *it;
    QIcon icon;
    for (int size : kIconSizes) icon.addPixmap(renderTile(size, total, attention, highlighted, dot, fg));
    // A template mask would turn the amber attention background monochrome.
    // Both text colors have their own surface, independent of the panel wallpaper.
    icon.setIsMask(false);
    if (m_cache.size() >= 128) m_cache.clear();
    m_cache.insert(key, icon);
    return icon;
}

QIcon IconFactory::forCounts(int total, int attention, bool stale)
{
    return cached(countText(total), countText(attention), attention > 0, stale);
}

QIcon IconFactory::forError()
{
    return cached(QStringLiteral("?"), QStringLiteral("?"), false, true);
}
