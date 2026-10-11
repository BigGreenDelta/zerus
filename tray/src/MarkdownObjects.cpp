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

#include <cmath>
#include <functional>

namespace {
using MarkdownObjects::ChipFill, MarkdownObjects::ChipScale, MarkdownObjects::ChipHeading;

// GitHub pads code .2em .4em, and 0 .2em inside headings.
QPointF chipPadding(const QTextCharFormat &format)
{
    const qreal em = format.font().pixelSize();
    return format.boolProperty(ChipHeading) ? QPointF(0.2 * em, 0) : QPointF(0.4 * em, 0.2 * em);
}

// The inline code runs of a document, found again after each change so that
// painting never walks a whole long journal.
class Chips final : public QObject {
    Q_OBJECT
public:
    struct Run { int start = 0, end = 0; QTextCharFormat format; };
    explicit Chips(QTextDocument *document) : QObject(document), m_document(document)
    {
        connect(document, &QTextDocument::contentsChange, this, [this] { m_stale = true; });
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
private:
    QTextDocument *m_document;
    QList<Run> m_runs;
    bool m_stale = true;
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

void MarkdownObjects::paintChips(QPainter *painter, QTextDocument *document, const QRectF &exposed, const QPalette &palette,
                                 const QList<QAbstractTextDocumentLayout::Selection> &selections)
{
    auto *chips = Chips::of(document);
    if (!chips) return;
    auto *layout = document->documentLayout();
    struct Piece { QPainterPath shape; QColor fill; };
    QList<Piece> pieces; QList<QTextBlock> blocks;
    QPainterPath area; area.setFillRule(Qt::WindingFill);
    QTextBlock block; QRectF bounds;
    for (const auto &run : chips->runs()) {
        // Runs come in document order, often several to a block.
        if (!block.isValid() || !block.contains(run.start)) { block = document->findBlock(run.start); bounds = layout->blockBoundingRect(block); }
        const QFont font = run.format.font(); const qreal em = font.pixelSize();
        if (!bounds.adjusted(-em, -em, em, em).intersects(exposed)) continue;
        const QFontMetricsF metrics(font); const QPointF padding = chipPadding(run.format);
        const qreal radius = 6 * run.format.doubleProperty(ChipScale);
        const QTextLayout *text = block.layout(); const QString content = block.text();
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
            pieces.append({shape, run.format.colorProperty(ChipFill)}); area.addPath(shape); drawn = true;
        }
        if (drawn && (blocks.isEmpty() || blocks.last() != block)) blocks.append(block);
    }
    if (pieces.isEmpty()) return;
    const QRectF reach = area.boundingRect();
    // What lies beneath the chips: the view's base, then the document with transparent
    // text and no selection. The clip is aliased, so glyphs are drawn only once.
    painter->save();
    painter->setClipPath(area, Qt::IntersectClip);
    painter->fillRect(reach, palette.brush(QPalette::Base));
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = palette; context.clip = reach;
    QTextCursor everything(document); everything.select(QTextCursor::Document);
    QTextCharFormat hidden; hidden.setForeground(QColor(Qt::transparent));
    context.selections.append({everything, hidden});
    layout->draw(painter, context);
    painter->restore();
    // The chips themselves, antialiased.
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    for (const auto &piece : std::as_const(pieces)) painter->fillPath(piece.shape, piece.fill);
    painter->restore();
    // On top, as in a browser: the selections as the view painted them, then the text.
    painter->save();
    painter->setClipPath(area, Qt::IntersectClip);
    painter->setPen(palette.color(QPalette::Text));
    for (const QTextBlock &each : std::as_const(blocks)) {
        QList<QTextLayout::FormatRange> ranges;
        for (const auto &selection : selections) {
            const int start = selection.cursor.selectionStart() - each.position(), end = selection.cursor.selectionEnd() - each.position();
            if (start < each.length() && end > 0 && end > start) ranges.append({start, end - start, selection.format});
        }
        each.layout()->draw(painter, layout->blockBoundingRect(each).topLeft() - each.layout()->position(), ranges, reach);
    }
    painter->restore();
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
