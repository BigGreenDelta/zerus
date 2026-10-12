#pragma once

#include "MarkdownHtml.h"

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QList>
#include <QRectF>
#include <QTextFormat>
#include <QUrl>

class QPainter;
class QPalette;
class QRect;
class QTextCursor;
class QTextDocument;
class QTextEdit;

// Pieces of GitHub Markdown that HTML cannot express in Qt rich text.
namespace MarkdownObjects {
// Inline code formats carry their chip colour, the content scale and whether
// they sit in a heading (GitHub pads those 0 .2em).
enum : int { ChipFill = QTextFormat::UserProperty + 41, ChipScale, ChipHeading };

// Draws hgs-md: resources named by MarkdownTheme::resource(); null for anything else.
QImage resource(const QUrl &url, qreal devicePixelRatio);
// Turns inline-code sentinel runs into text in the code font, padded like GitHub's
// chips. It stays text: find() matches it, a selection may start or end anywhere
// in it, and long runs wrap. paintChips() draws the chips.
int convertChips(QTextDocument *document, const MarkdownTheme &theme);
bool isInlineCode(const QTextFormat &format);
// Draws the rounded chips beneath the inline code once the document itself is drawn:
// painter in document coordinates, palette's Base under the document, selections as
// the view painted them (they stay on top of the chips).
void paintChips(QPainter *painter, QTextDocument *document, const QRectF &exposed, const QPalette &palette,
                const QList<QAbstractTextDocumentLayout::Selection> &selections);
// The same for a view, after QTextEdit::paintEvent(): its found text, then the
// reader's selection. exposed is in viewport coordinates.
void paintChips(QTextEdit *view, const QRect &exposed);
// The selection as a reader sees it: list markers "• ", "1. ", "[x] ";
// code-block corners and rule fillers give nothing.
QString plainText(const QTextCursor &selection);
}
