#include "MarkdownObjects.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEngine>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOption>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTextFragment>
#include <QTextTable>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {
using MarkdownObjects::ChipFill, MarkdownObjects::ChipScale, MarkdownObjects::ChipHeading;

// Draw the document's backgrounds and images without painting glyphs. A
// transparent foreground selection still shapes/draws every glyph through a
// complicated selection clip; QTextLayout's public paint-engine path lets us
// omit those text items while keeping the original layout and decoration.
class BackgroundEngine final : public QPaintEngine {
public:
    explicit BackgroundEngine(QPainter &target) : QPaintEngine(AllFeatures), m_target(target) {}
    bool begin(QPaintDevice *) override { m_target.save(); return true; }
    bool end() override { m_target.restore(); return true; }
    Type type() const override { return User; }
    void updateState(const QPaintEngineState &state) override
    {
        const auto dirty = state.state();
        if (dirty & DirtyPen) m_target.setPen(state.pen());
        if (dirty & DirtyBrush) m_target.setBrush(state.brush());
        if (dirty & DirtyBrushOrigin) m_target.setBrushOrigin(state.brushOrigin());
        if (dirty & DirtyFont) m_target.setFont(state.font());
        if (dirty & DirtyBackground) m_target.setBackground(state.backgroundBrush());
        if (dirty & DirtyBackgroundMode) m_target.setBackgroundMode(state.backgroundMode());
        if (dirty & DirtyTransform) m_target.setWorldTransform(state.transform());
        if (dirty & DirtyHints) {
            m_target.setRenderHints(m_target.renderHints(), false); m_target.setRenderHints(state.renderHints());
        }
        if (dirty & DirtyCompositionMode) m_target.setCompositionMode(state.compositionMode());
        if (dirty & DirtyOpacity) m_target.setOpacity(state.opacity());
        if (dirty & DirtyClipRegion) m_target.setClipRegion(state.clipRegion(), state.clipOperation());
        if (dirty & DirtyClipPath) m_target.setClipPath(state.clipPath(), state.clipOperation());
        if (dirty & DirtyClipEnabled) m_target.setClipping(state.isClipEnabled());
    }
    void drawTextItem(const QPointF &, const QTextItem &) override {}
    void drawPixmap(const QRectF &r, const QPixmap &p, const QRectF &s) override { m_target.drawPixmap(r, p, s); }
    void drawImage(const QRectF &r, const QImage &i, const QRectF &s, Qt::ImageConversionFlags f) override { m_target.drawImage(r, i, s, f); }
    void drawPath(const QPainterPath &path) override { m_target.drawPath(path); }
    void drawRects(const QRectF *r, int n) override { m_target.drawRects(r, n); }
    void drawRects(const QRect *r, int n) override { m_target.drawRects(r, n); }
    void drawLines(const QLineF *l, int n) override { m_target.drawLines(l, n); }
    void drawLines(const QLine *l, int n) override { m_target.drawLines(l, n); }
    void drawEllipse(const QRectF &r) override { m_target.drawEllipse(r); }
    void drawEllipse(const QRect &r) override { m_target.drawEllipse(r); }
    void drawPoints(const QPointF *p, int n) override { m_target.drawPoints(p, n); }
    void drawPoints(const QPoint *p, int n) override { m_target.drawPoints(p, n); }
    void drawPolygon(const QPointF *p, int n, PolygonDrawMode mode) override
    {
        if (mode == PolylineMode) m_target.drawPolyline(p, n);
        else if (mode == ConvexMode) m_target.drawConvexPolygon(p, n);
        else m_target.drawPolygon(p, n, mode == WindingMode ? Qt::WindingFill : Qt::OddEvenFill);
    }
private:
    QPainter &m_target;
};

class BackgroundDevice final : public QPaintDevice {
public:
    BackgroundDevice(QImage &image, QPainter &target) : m_image(image), m_engine(target) {}
    QPaintEngine *paintEngine() const override { return &m_engine; }
protected:
    int metric(PaintDeviceMetric m) const override
    {
        switch (m) {
        case PdmWidth: return m_image.width();
        case PdmHeight: return m_image.height();
        case PdmWidthMM: return qRound(m_image.width() * 25.4 / m_image.logicalDpiX());
        case PdmHeightMM: return qRound(m_image.height() * 25.4 / m_image.logicalDpiY());
        case PdmDepth: return m_image.depth();
        case PdmDpiX: case PdmPhysicalDpiX: return m_image.logicalDpiX();
        case PdmDpiY: case PdmPhysicalDpiY: return m_image.logicalDpiY();
        case PdmDevicePixelRatio: return 1;
        case PdmDevicePixelRatioScaled: return devicePixelRatioFScale();
        default: return 0;
        }
    }
private:
    QImage &m_image;
    mutable BackgroundEngine m_engine;
};

// GitHub pads code .2em .4em, and 0 .2em inside headings.
QPointF chipPadding(const QTextCharFormat &format)
{
    const qreal em = format.font().pixelSize();
    return format.boolProperty(ChipHeading) ? QPointF(0.2 * em, 0) : QPointF(0.4 * em, 0.2 * em);
}

// Content and layout indexes are independent: resizing keeps the code runs but
// rebuilds their block bounds. Paint only the blocks intersecting the viewport.
class Chips final : public QObject {
    Q_OBJECT
public:
    struct Run { int start = 0, end = 0; QTextCharFormat format; };
    struct BlockRuns {
        QTextBlock block;
        QRectF bounds, padded;
        qsizetype begin = 0, end = 0;
    };
    explicit Chips(QTextDocument *document) : QObject(document), m_document(document)
    {
        connect(document, &QTextDocument::contentsChange, this, [this] { m_stale = m_boundsStale = true; });
        connect(document, &QTextDocument::documentLayoutChanged, this, [this] { watchLayout(); });
        watchLayout();
    }
    static Chips *of(QTextDocument *document) { return document->findChild<Chips *>(QString(), Qt::FindDirectChildrenOnly); }
    const QList<Run> &runs()
    {
        if (!m_stale) return m_runs;
        m_runs.clear(); m_stale = false;
        for (auto block = m_document->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment(); const auto format = fragment.charFormat();
                if (!MarkdownObjects::isInlineCode(format)) continue;
                if (!m_runs.isEmpty() && m_runs.last().end == fragment.position()) m_runs.last().end += fragment.length();
                else m_runs.append({fragment.position(), fragment.position() + fragment.length(), format});
            }
        return m_runs;
    }
    void prepare()
    {
        auto *layout = m_document->documentLayout();
        const QSizeF size = layout->documentSize(); // Finish any pending layout before indexing.
        if (!m_boundsStale && m_size == size && m_width == m_document->textWidth()
            && m_font == m_document->defaultFont()) return;
        const auto &code = runs();
        m_blocks.clear();
        for (qsizetype i = 0; i < code.size();) {
            const auto block = m_document->findBlock(code[i].start);
            const qsizetype begin = i;
            qreal padding = 0;
            do { padding = qMax(padding, qreal(code[i++].format.font().pixelSize())); }
            while (i < code.size() && block.contains(code[i].start));
            const QRectF bounds = layout->blockBoundingRect(block);
            m_blocks.append({block, bounds, bounds.adjusted(-padding, -padding, padding, padding), begin, i});
        }
        std::sort(m_blocks.begin(), m_blocks.end(), [](const auto &a, const auto &b) { return a.padded.top() < b.padded.top(); });
        // Blocks in table columns need not be ordered vertically, or have equal
        // heights. Prefix maxima retain a tall overlapping block at any scroll.
        m_bottoms.clear();
        qreal bottom = -std::numeric_limits<qreal>::infinity();
        for (const auto &block : std::as_const(m_blocks)) {
            bottom = qMax(bottom, block.padded.bottom()); m_bottoms.append(bottom);
        }
        m_size = size; m_width = m_document->textWidth(); m_font = m_document->defaultFont();
        m_boundsStale = false;
    }
    QList<const BlockRuns *> visible(const QRectF &exposed)
    {
        prepare();
        QList<const BlockRuns *> result;
        auto i = std::lower_bound(m_bottoms.cbegin(), m_bottoms.cend(), exposed.top()) - m_bottoms.cbegin();
        for (; i < m_blocks.size() && m_blocks[i].padded.top() <= exposed.bottom(); ++i)
            if (m_blocks[i].padded.intersects(exposed)) result.append(&m_blocks[i]);
        std::sort(result.begin(), result.end(), [](const auto *a, const auto *b) { return a->begin < b->begin; });
        return result;
    }
private:
    void watchLayout()
    {
        for (const auto &connection : std::as_const(m_layoutConnections)) disconnect(connection);
        m_layoutConnections.clear(); m_boundsStale = true;
        auto *layout = m_document->documentLayout();
        m_layoutConnections.append(connect(layout, &QAbstractTextDocumentLayout::update, this, [this] { m_boundsStale = true; }));
        m_layoutConnections.append(connect(layout, &QAbstractTextDocumentLayout::updateBlock, this, [this] { m_boundsStale = true; }));
        m_layoutConnections.append(connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { m_boundsStale = true; }));
    }
    QTextDocument *m_document;
    QList<Run> m_runs;
    QList<BlockRuns> m_blocks;
    QList<qreal> m_bottoms;
    QList<QMetaObject::Connection> m_layoutConnections;
    QSizeF m_size;
    QFont m_font;
    qreal m_width = 0;
    bool m_stale = true;
    bool m_boundsStale = true;
};

QString markerText(const QString &imageName)
{
    if (!imageName.startsWith(QLatin1String("hgs-md:"))) return QString();
    const QString name = QUrl(imageName).path().section('/', 0, 0);
    if (name == "disc") return QString::fromUtf8("• ");
    if (name == "circle") return QString::fromUtf8("◦ ");
    if (name == "square") return QString::fromUtf8("▪ ");
    if (name == "check-on") return QStringLiteral("[x] ");
    if (name == "check-off") return QStringLiteral("[ ] ");
    if (name == "gap") return QStringLiteral(" ");
    return QString();
}

QTextTable *tableOf(const QTextBlock &block)
{
    return block.isValid() ? QTextCursor(block).currentTable() : nullptr;
}

}

QImage MarkdownObjects::resource(const QUrl &url, qreal devicePixelRatio)
{
    if (url.scheme() != QLatin1String("hgs-md")) return {};
    const QStringList parts = url.path().split('/');
    if (parts.size() != 3 || (parts[1] != "light" && parts[1] != "dark")) return {};
    bool ok = false; const double scale = parts[2].toInt(&ok) / 100.0;
    if (!ok || scale < 0.5 || scale > 4.0) return {};
    const bool dark = parts[1] == "dark";
    auto theme = MarkdownTheme::github(dark, QColor(dark ? "#0d1117" : "#ffffff"), scale);
    if (url.hasQuery()) {
        static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{6}$"));
        const QString fill = QUrlQuery(url).queryItemValue(QStringLiteral("fill"));
        if (!hex.match(fill).hasMatch()) return {};
        theme.subtle = QColor('#' + fill);
    }
    const qreal ratio = qMax<qreal>(1.0, devicePixelRatio);
    const auto draw = [&](qreal width, qreal height, const std::function<void(QPainter &)> &paint) {
        QImage image(qCeil(width * scale * ratio), qCeil(height * scale * ratio), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio); image.fill(Qt::transparent);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing); painter.scale(scale, scale);
        paint(painter);
        return image;
    };
    const QString &name = parts[0];
    // Markers fill the 28 px list gutter; the dot sits 5.5 px above the image bottom (the baseline).
    if (name == "disc") return draw(28, 12, [&](QPainter &p) { p.setPen(Qt::NoPen); p.setBrush(theme.fg); p.drawEllipse(QRectF(11, 4, 5, 5)); });
    if (name == "circle") return draw(28, 12, [&](QPainter &p) { p.setPen(QPen(theme.fg, 1)); p.setBrush(Qt::NoBrush); p.drawEllipse(QRectF(11.5, 4.5, 4, 4)); });
    if (name == "square") return draw(28, 12, [&](QPainter &p) { p.fillRect(QRectF(11, 4, 5, 5), theme.fg); });
    // Centred on the line (vertical-align: middle), like GitHub's task checkbox.
    if (name == "check-on" || name == "check-off") return draw(28, 18, [&](QPainter &p) {
        const bool on = name == "check-on";
        p.setPen(QPen(theme.muted, 1)); p.setBrush(on ? theme.muted : theme.canvas);
        p.drawRoundedRect(QRectF(8.5, 1.5, 12, 12), 2, 2);
        if (on) {
            p.setPen(QPen(theme.canvas, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawPolyline(QPolygonF{QPointF(11.5, 7.5), QPointF(13.5, 9.5), QPointF(17.5, 5.0)});
        }
    });
    // Ordered markers are text right-aligned in the gutter between two transparent spacers.
    if (name == "blank" || name == "gap") return draw(1, 1, [](QPainter &) {});
    static const QHash<QString, QPointF> corners{{"corner-tl", {0, 0}}, {"corner-tr", {-6, 0}}, {"corner-bl", {0, -6}}, {"corner-br", {-6, -6}}};
    if (corners.contains(name)) return draw(6, 6, [&](QPainter &p) {
        p.setPen(Qt::NoPen); p.setBrush(theme.subtle);
        p.drawRoundedRect(QRectF(corners.value(name), QSizeF(12, 12)), 6, 6);
    });
    return {};
}

int MarkdownObjects::convertChips(QTextDocument *document, const MarkdownTheme &theme)
{
    struct Run { int position = 0, length = 0; QString text; QTextCharFormat format; };
    QList<Run> runs;
    for (auto block = document->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment(); const auto format = fragment.charFormat();
            if (format.background().style() == Qt::NoBrush) continue;
            const QColor background = format.background().color();
            if (background != MarkdownHtml::chipSentinel() && background != MarkdownHtml::headingChipSentinel()) continue;
            if (!runs.isEmpty() && runs.last().position + runs.last().length == fragment.position()) {
                runs.last().length += fragment.length(); runs.last().text += fragment.text();
            } else runs.append({fragment.position(), fragment.length(), fragment.text(), format});
        }
    if (runs.isEmpty()) return 0;
    const double scale = theme.scalePercent / 100.0;
    if (!Chips::of(document)) new Chips(document);
    // One edit block: per-run edits would each relayout the document (seconds on long journals).
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (auto it = runs.crbegin(); it != runs.crend(); ++it) {
        cursor.setPosition(it->position); cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        // Plain spaces again: long code wraps like GitHub's (white-space: break-spaces).
        QString text = it->text; text.replace(QChar::Nbsp, ' ');
        // The renderer sized and coloured the run after its surroundings (already content-scaled).
        // A character background would be a square box a line high: paintChips() draws the chip.
        QTextCharFormat format = it->format;
        format.clearBackground();
        if (!format.hasProperty(QTextFormat::ForegroundBrush)) format.setForeground(theme.fg);
        format.setFontFamilies(QStringList{theme.monoFamily});
        format.setProperty(QTextFormat::FontPixelSize, it->format.font().pixelSize() > 0 ? it->format.font().pixelSize() : qRound(12 * scale));
        format.setProperty(ChipFill, theme.chip); format.setProperty(ChipScale, scale);
        format.setProperty(ChipHeading, it->format.background().color() == MarkdownHtml::headingChipSentinel());
        // GitHub's horizontal padding: the character before the code and its last one advance further.
        QTextCharFormat padded;
        padded.setFontLetterSpacingType(QFont::AbsoluteSpacing); padded.setFontLetterSpacing(chipPadding(format).x());
        cursor.insertText(text.chopped(1), format);
        format.merge(padded); cursor.insertText(text.right(1), format);
        if (it->position > cursor.block().position()) {
            QTextCursor before(document);
            before.setPosition(it->position - 1); before.setPosition(it->position, QTextCursor::KeepAnchor);
            if (before.charFormat().objectType() == QTextFormat::NoObject) before.mergeCharFormat(padded);
        }
    }
    cursor.endEditBlock();
    return runs.size();
}

bool MarkdownObjects::isInlineCode(const QTextFormat &format)
{
    return format.hasProperty(ChipFill);
}

void MarkdownObjects::prepareChips(QTextDocument *document)
{
    if (auto *chips = Chips::of(document)) chips->prepare();
}

void MarkdownObjects::paintChips(QPainter *painter, QTextDocument *document, const QRectF &exposed, const QPalette &palette,
                                 const QList<QAbstractTextDocumentLayout::Selection> &selections)
{
    auto *chips = Chips::of(document);
    if (!chips) return;
    auto *layout = document->documentLayout();
    struct Piece { QPainterPath shape; QColor fill; QTextBlock block; };
    QList<Piece> pieces; QList<QTextBlock> blocks;
    const auto &runs = chips->runs();
    for (const auto *entry : chips->visible(exposed)) {
        const auto block = entry->block; const auto bounds = entry->bounds;
        const QTextLayout *text = block.layout(); const QString content = block.text();
        for (qsizetype r = entry->begin; r < entry->end; ++r) {
            const auto &run = runs[r];
            const QFont font = run.format.font(); const qreal em = font.pixelSize();
            if (!bounds.adjusted(-em, -em, em, em).intersects(exposed)) continue;
            const QFontMetricsF metrics(font); const QPointF padding = chipPadding(run.format);
            const qreal radius = 6 * run.format.doubleProperty(ChipScale);
            const int from = run.start - block.position(), to = run.end - block.position();
            bool drawn = false;
            for (int i = 0; i < text->lineCount(); ++i) {
                const QTextLine line = text->lineAt(i);
                const int start = qMax(from, line.textStart());
                int end = qMin(to, line.textStart() + line.textLength());
                // Like GitHub's (box-decoration-break: slice): padded and rounded where the code begins
                // and ends, cut square where a line breaks it, and without the spaces hanging there.
                const bool first = start == from, last = end == to;
                while (!last && end > start && content.at(end - 1).isSpace()) --end;
                if (start >= end) continue;
                const qreal baseline = bounds.top() + line.y() + line.ascent();
                const QRectF rect(QPointF(bounds.left() + line.cursorToX(start) - (first ? padding.x() : 0), baseline - metrics.ascent() - padding.y()),
                                  QPointF(bounds.left() + line.cursorToX(end), baseline + metrics.descent() + padding.y()));
                if (!rect.intersects(exposed)) continue;
                QPainterPath shape; shape.setFillRule(Qt::WindingFill);
                shape.addRoundedRect(rect, radius, radius);
                const qreal square = qMin(radius, rect.width() / 2);
                if (!first) shape.addRect(QRectF(rect.left(), rect.top(), square, rect.height()));
                if (!last) shape.addRect(QRectF(rect.right() - square, rect.top(), square, rect.height()));
                pieces.append({shape, run.format.colorProperty(ChipFill), block}); drawn = true;
            }
            if (drawn && (blocks.isEmpty() || blocks.last() != block)) blocks.append(block);
        }
    }
    if (pieces.isEmpty()) return;
    // Bound the clip complexity even for a single dense paragraph. Large
    // rounded-path unions make the raster engine clip every glyph against all
    // the other pills. Disjoint horizontal bands preserve every pixel once.
    struct Batch { QTextBlock block; QPainterPath area; QRectF clip; bool sliced = false; };
    QList<Batch> batches;
    qsizetype pieceStart = 0;
    for (const QTextBlock &each : std::as_const(blocks)) {
        const qsizetype from = pieceStart;
        QPainterPath area; area.setFillRule(Qt::WindingFill);
        while (pieceStart < pieces.size() && pieces[pieceStart].block == each)
            area.addPath(pieces[pieceStart++].shape);
        const QRectF reach = area.boundingRect();
        if (pieceStart - from <= 8 || painter->combinedTransform().type() > QTransform::TxTranslate) {
            batches.append({each, area, reach}); continue;
        }
        const auto device = painter->deviceTransform();
        const auto inverse = device.inverted();
        const QRect pixels = device.mapRect(reach).toAlignedRect();
        const int step = qMax(1, qRound(32 * painter->device()->devicePixelRatioF()));
        for (int top = pixels.top(); top < pixels.top() + pixels.height(); top += step) {
            const QRectF band = inverse.mapRect(QRectF(pixels.left(), top, pixels.width(),
                qMin(step, pixels.top() + pixels.height() - top)));
            QPainterPath local; local.setFillRule(Qt::WindingFill);
            for (qsizetype i = from; i < pieceStart; ++i)
                if (pieces[i].shape.boundingRect().intersects(band)) local.addPath(pieces[i].shape);
            if (!local.isEmpty()) batches.append({each, local, band, true});
        }
    }
    // Restore backgrounds everywhere first, then paint every pill once in its
    // original order. This also preserves overlapping decorations at line ends.
    QTextCursor everything(document); everything.select(QTextCursor::Document);
    QTextCharFormat hidden; hidden.setForeground(QColor(Qt::transparent));
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = palette; context.selections.append({everything, hidden});
    // Render the underlying document once with a rectangular clip. Repeating
    // transparent text through each rounded clip is costly for long paragraphs.
    // Align the temporary tile to the destination's physical pixel grid.
    QImage background;
    QPointF backgroundOrigin;
    const auto transform = painter->combinedTransform();
    const bool translated = transform.type() <= QTransform::TxTranslate;
    if (translated && painter->opacity() == 1 && painter->compositionMode() == QPainter::CompositionMode_SourceOver
        && palette.brush(QPalette::Base).isOpaque()) {
        QRectF reach;
        for (const auto &batch : std::as_const(batches)) reach = reach.united(batch.clip);
        const qreal ratio = painter->device()->devicePixelRatioF();
        const QRect pixels = painter->deviceTransform().mapRect(reach).toAlignedRect();
        backgroundOrigin = painter->deviceTransform().inverted().map(QPointF(pixels.topLeft()));
        background = QImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
        background.setDotsPerMeterX(qRound(painter->device()->logicalDpiX() / .0254));
        background.setDotsPerMeterY(qRound(painter->device()->logicalDpiY() / .0254));
        {
            QPainter target(&background); BackgroundDevice device(background, target); QPainter behind(&device);
            behind.setRenderHints(painter->renderHints());
            behind.setWorldTransform(QTransform(ratio, 0, 0, ratio, -backgroundOrigin.x() * ratio, -backgroundOrigin.y() * ratio));
            behind.fillRect(QRectF(backgroundOrigin, QSizeF(pixels.size()) / ratio), palette.brush(QPalette::Base));
            auto backdrop = context; backdrop.clip = reach; backdrop.selections.clear();
            layout->draw(&behind, backdrop);
        }
        background.setDevicePixelRatio(ratio);
    }
    if (background.isNull()) {
        // Preserve the caller's compositing for transparent bases or unusual
        // transforms. Restore once, rather than repainting all text per band.
        QPainterPath area; area.setFillRule(Qt::WindingFill);
        for (const auto &piece : std::as_const(pieces)) area.addPath(piece.shape);
        painter->save(); painter->setClipPath(area, Qt::IntersectClip);
        context.clip = area.boundingRect();
        painter->fillRect(context.clip, palette.brush(QPalette::Base)); layout->draw(painter, context);
        painter->restore();
    } else for (const auto &batch : std::as_const(batches)) {
        painter->save();
        if (batch.sliced) painter->setClipRect(batch.clip, Qt::IntersectClip);
        painter->setClipPath(batch.area, Qt::IntersectClip);
        const qreal ratio = background.devicePixelRatio();
        const QRectF pixels((batch.clip.topLeft() - backgroundOrigin) * ratio, batch.clip.size() * ratio);
        painter->drawImage(batch.clip, background, pixels);
        painter->restore();
    }
    painter->save(); painter->setRenderHint(QPainter::Antialiasing);
    for (const auto &piece : std::as_const(pieces)) painter->fillPath(piece.shape, piece.fill);
    painter->restore();
    for (const auto &batch : std::as_const(batches)) {
        const auto each = batch.block;
        QList<QTextLayout::FormatRange> ranges;
        for (const auto &selection : selections) {
            const int start = selection.cursor.selectionStart() - each.position(), end = selection.cursor.selectionEnd() - each.position();
            if (start < each.length() && end > 0 && end > start) ranges.append({start, end - start, selection.format});
        }
        painter->save();
        if (batch.sliced) painter->setClipRect(batch.clip, Qt::IntersectClip);
        painter->setClipPath(batch.area, Qt::IntersectClip);
        painter->setPen(palette.color(QPalette::Text));
        QRectF textClip = batch.clip;
        if (batch.sliced) {
            qreal lineHeight = 0;
            for (int i = 0; i < each.layout()->lineCount(); ++i) lineHeight = qMax(lineHeight, each.layout()->lineAt(i).height());
            // Retain neighboring lines: CoreText glyph ink can overhang line
            // metrics, and slicing Qt's selection regions changes glyph edges.
            textClip.adjust(0, -lineHeight, 0, lineHeight);
        }
        each.layout()->draw(painter, layout->blockBoundingRect(each).topLeft() - each.layout()->position(), ranges, textClip);
        painter->restore();
    }

}

void MarkdownObjects::paintChips(QTextEdit *view, const QRect &exposed)
{
    // The selections as QTextEdit paints them: found text, then the reader's.
    QList<QAbstractTextDocumentLayout::Selection> selections;
    for (const auto &extra : view->extraSelections()) selections.append({extra.cursor, extra.format});
    if (view->textCursor().hasSelection()) {
        const auto group = view->hasFocus() ? QPalette::Active : QPalette::Inactive;
        QTextCharFormat format;
        format.setBackground(view->palette().brush(group, QPalette::Highlight));
        format.setForeground(view->palette().brush(group, QPalette::HighlightedText));
        QStyleOption option; option.initFrom(view);
        if (view->style()->styleHint(QStyle::SH_RichText_FullWidthSelection, &option, view)) format.setProperty(QTextFormat::FullWidthSelection, true);
        selections.append({view->textCursor(), format});
    }
    QPainter painter(view->viewport());
    const QPoint scroll(view->horizontalScrollBar()->value(), view->verticalScrollBar()->value());
    painter.translate(-scroll);
    paintChips(&painter, view->document(), QRectF(exposed.translated(scroll)), view->viewport()->palette(), selections);
}

QString MarkdownObjects::plainText(const QTextCursor &selection)
{
    if (!selection.hasSelection()) return QString();
    QTextDocument copy;
    QTextCursor(&copy).insertFragment(selection.selection());
    QString result;
    for (auto block = copy.begin(); block.isValid(); block = block.next()) {
        QString line;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment(); const auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                line += markerText(format.toImageFormat().name());
            } else {
                QString text = fragment.text();
                text.remove(QChar(0x200B)).remove(QChar::ObjectReplacementCharacter);
                line += text.replace(QChar::Nbsp, ' ').replace(QChar::LineSeparator, '\n');
            }
        }
        const QTextCursor at(block);
        QTextTable *table = at.currentTable();
        if (line.isEmpty()) {
            // Corners, edges and rule fillers are cells holding one decorative block;
            // Qt also separates adjacent tables with an empty block. Blank lines inside
            // a message (same cell, same table on both sides) are kept.
            bool lone = false;
            if (table) {
                const auto cell = table->cellAt(at);
                lone = cell.firstCursorPosition().block() == block && cell.lastCursorPosition().block() == block;
            }
            if (lone || tableOf(block.previous()) != table || tableOf(block.next()) != table) continue;
        }
        result += line + '\n';
    }
    if (result.endsWith('\n')) result.chop(1);
    return result;
}

#include "MarkdownObjects.moc"
