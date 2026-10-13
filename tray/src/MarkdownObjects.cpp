#include "MarkdownObjects.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
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
    // Keep the clip local to a text block. A document-wide union of rounded
    // pills makes the raster engine clip every glyph against all other pills.
    QTextCursor everything(document); everything.select(QTextCursor::Document);
    QTextCharFormat hidden; hidden.setForeground(QColor(Qt::transparent));
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = palette;
    context.selections.append({everything, hidden});
    qsizetype pieceStart = 0;
    for (const QTextBlock &each : std::as_const(blocks)) {
        QPainterPath area; area.setFillRule(Qt::WindingFill);
        const qsizetype from = pieceStart;
        while (pieceStart < pieces.size() && pieces[pieceStart].block == each)
            area.addPath(pieces[pieceStart++].shape);
        const QRectF reach = area.boundingRect();
        painter->save();
        painter->setClipPath(area, Qt::IntersectClip);
        painter->fillRect(reach, palette.brush(QPalette::Base));
        context.clip = reach;
        layout->draw(painter, context);
        painter->restore();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        for (qsizetype i = from; i < pieceStart; ++i) painter->fillPath(pieces[i].shape, pieces[i].fill);
        painter->restore();
        QList<QTextLayout::FormatRange> ranges;
        for (const auto &selection : selections) {
            const int start = selection.cursor.selectionStart() - each.position(), end = selection.cursor.selectionEnd() - each.position();
            if (start < each.length() && end > 0 && end > start) ranges.append({start, end - start, selection.format});
        }
        painter->save();
        painter->setClipPath(area, Qt::IntersectClip);
        painter->setPen(palette.color(QPalette::Text));
        each.layout()->draw(painter, layout->blockBoundingRect(each).topLeft() - each.layout()->position(), ranges, reach);
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
