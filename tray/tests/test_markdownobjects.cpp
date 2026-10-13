#include "ContentScale.h"
#include "MarkdownHtml.h"
#include "MarkdownObjects.h"

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <ctime>
#include <limits>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
// CPU time of the calling thread; unlike wall time it excludes preemption by other processes.
qint64 threadCpuMs()
{
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ticks = [](const FILETIME &time) { return (qint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10000;   // 100 ns units
#else
    timespec now{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#endif
}
const MarkdownTheme &light()
{
    static const MarkdownTheme theme = MarkdownTheme::github(false, QColor("#ffffff"));
    return theme;
}
std::unique_ptr<QTextDocument> document(const QString &markdown, const MarkdownLinkPolicy &links = {})
{
    auto result = std::make_unique<QTextDocument>();
    result->setHtml(MarkdownHtml::render(markdown, light(), links));
    return result;
}
QString all(QTextDocument *doc)
{
    QTextCursor cursor(doc); cursor.select(QTextCursor::Document);
    return MarkdownObjects::plainText(cursor);
}
// Each inline code run: its text and the format of its first character.
struct Code { QString text; QTextCharFormat format; };
QList<Code> code(QTextDocument *doc)
{
    QList<Code> result; int end = -1;
    for (auto block = doc->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!MarkdownObjects::isInlineCode(fragment.charFormat())) continue;
            if (fragment.position() == end) result.last().text += fragment.text();
            else result.append({fragment.text(), fragment.charFormat()});
            end = fragment.position() + fragment.length();
        }
    return result;
}
// The Activity view's drawing: the document on white, then the inline code chips.
QImage rendered(QTextDocument *doc, const QList<QAbstractTextDocumentLayout::Selection> &selections = {})
{
    auto *layout = doc->documentLayout();
    QImage image(layout->documentSize().toSize(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
    QPalette palette; palette.setColor(QPalette::Base, Qt::white);
    QAbstractTextDocumentLayout::PaintContext context; context.palette = palette; context.selections = selections;
    QPainter painter(&image);
    layout->draw(&painter, context);
    MarkdownObjects::paintChips(&painter, doc, image.rect(), palette, selections);
    return image;
}
// Equal up to the step or two that antialiasing and compositing round away.
bool close(const QColor &a, const QColor &b)
{
    return qAbs(a.red() - b.red()) <= 3 && qAbs(a.green() - b.green()) <= 3 && qAbs(a.blue() - b.blue()) <= 3;
}
QColor over(const QColor &top, const QColor &bottom)
{
    const auto mix = [&](int a, int b) { return qRound(a * top.alphaF() + b * (1 - top.alphaF())); };
    return QColor(mix(top.red(), bottom.red()), mix(top.green(), bottom.green()), mix(top.blue(), bottom.blue()));
}
// Where a 24 px code run of a scale-2 paragraph sits, in document pixels.
struct CodeGeometry {
    QTextBlock block; QPointF origin; QFontMetricsF metrics{QFont()}; int start = 0, end = 0;
    CodeGeometry(QTextDocument *doc, const QString &code, const QString &family) : block(doc->begin())
    {
        QFont mono(family); mono.setPixelSize(24); metrics = QFontMetricsF(mono);
        origin = doc->documentLayout()->blockBoundingRect(block).topLeft();
        start = block.text().indexOf(code); end = start + code.size();
    }
    QTextLine line(int position) const { return block.layout()->lineForTextPosition(position); }
    qreal x(int position) const { return origin.x() + line(position).cursorToX(position); }
    qreal baseline(const QTextLine &l) const { return origin.y() + l.y() + l.ascent(); }
    qreal top(const QTextLine &l) const { return baseline(l) - metrics.ascent() - 0.2 * 24; }
    qreal bottom(const QTextLine &l) const { return baseline(l) + metrics.descent() + 0.2 * 24; }
};
}

class TestMarkdownObjects : public QObject {
    Q_OBJECT
private slots:
    void resourcesAreDrawnAtScaleAndPixelRatio();
    void inlineCodeStaysSelectableText();
    void codeIsPaddedLikeGitHub();
    void codeInsideLinkStaysClickable();
    void plainTextSpellsOutMarkersAndSkipsDecoration();
    void plainTextKeepsBlankLinesInsideMessages();
    void codeFollowsItsTextSizeAndColour();
    void longJournalsConvertAndPaintQuickly();
    void richRepliesPaintWithinFrameBudget_data();
    void richRepliesPaintWithinFrameBudget();
    void chipIndexFollowsLayoutAndContentChanges();
    void denseSelectedCodeKeepsEveryGlyph_data();
    void denseSelectedCodeKeepsEveryGlyph();
    void codeLinesKeepTheParagraphPitch();
    void wrappedCodeKeepsRoundedEnds();
    void selectedCodeKeepsTheSelectionLook();
};

void TestMarkdownObjects::resourcesAreDrawnAtScaleAndPixelRatio()
{
    const auto disc = MarkdownObjects::resource(QUrl("hgs-md:disc/light/150"), 2.0);
    QCOMPARE(disc.size(), QSize(84, 36)); QCOMPARE(disc.devicePixelRatio(), 2.0);
    QVERIFY(qAlpha(disc.pixel(40, 19)) > 200);   // centre of the 5×5 disc at x 11..16, y 4..9
    QVERIFY(qAlpha(disc.pixel(7, 2)) == 0);
    const auto corner = MarkdownObjects::resource(QUrl("hgs-md:corner-br/dark/100"), 1.0);
    QCOMPARE(corner.size(), QSize(6, 6));
    QCOMPARE(QColor::fromRgba(corner.pixel(0, 0)).name(), QString("#151b23"));
    QVERIFY(qAlpha(corner.pixel(5, 5)) < 255);
    const auto filled = MarkdownObjects::resource(QUrl("hgs-md:corner-tl/light/100?fill=e8eef2"), 1.0);
    QCOMPARE(QColor::fromRgba(filled.pixel(5, 5)).name(), QString("#e8eef2"));
    for (const char *url : {"hgs-md:corner-tl/light/100?fill=zz0000", "hgs-md:disc/blue/100", "hgs-md:unknown/light/100", "hgs-md:disc/light/abc", "https://example.com/x.png", "file:///etc/passwd"})
        QVERIFY2(MarkdownObjects::resource(QUrl(url), 1.0).isNull(), url);
}

void TestMarkdownObjects::inlineCodeStaysSelectableText()
{
    // Inline code is text in chip colours: find() matches inside it and a
    // selection may start in prose and end within the code.
    auto doc = document("a `b c` d `e`");
    QCOMPARE(MarkdownObjects::convertChips(doc.get(), light()), 2);
    const auto found = code(doc.get());
    QCOMPARE(found.size(), 2);
    QCOMPARE(found[0].text, QString("b c")); QCOMPARE(found[1].text, QString("e"));
    QCOMPARE(doc->toPlainText(), QString("a b c d e"));
    QVERIFY(!doc->find("b c").isNull());
    QCOMPARE(found[0].format.colorProperty(MarkdownObjects::ChipFill), light().chip);
    QCOMPARE(found[0].format.fontFamilies().toStringList().value(0), light().monoFamily);
    QCOMPARE(found[0].format.background().style(), Qt::NoBrush);
    QTextCursor part(doc.get()); part.setPosition(0); part.setPosition(3, QTextCursor::KeepAnchor);
    QCOMPARE(MarkdownObjects::plainText(part), QString("a b"));
    QCOMPARE(all(doc.get()), QString("a b c d e"));
}

void TestMarkdownObjects::codeIsPaddedLikeGitHub()
{
    // paintChips() draws the rounded chip: a character background would be square.
    // GitHub pads code .4em on both sides: the character before it and its last one advance further.
    auto doc = document("See `ctest -R x`.");
    MarkdownObjects::convertChips(doc.get(), light());
    const int start = doc->toPlainText().indexOf("ctest");
    QTextCursor at(doc.get());
    for (const int after : {start, start + 10}) {
        at.setPosition(after);
        QCOMPARE(at.charFormat().fontLetterSpacingType(), QFont::AbsoluteSpacing);
        QCOMPARE(at.charFormat().fontLetterSpacing(), 0.4 * 12);
    }
    at.setPosition(start + 1);
    QCOMPARE(at.charFormat().fontLetterSpacing(), 0.0);
}

void TestMarkdownObjects::codeInsideLinkStaysClickable()
{
    auto doc = document("[`code`](https://example.com)", [](const QString &href) { return MarkdownLink{href, {}, {}}; });
    MarkdownObjects::convertChips(doc.get(), light());
    QCOMPARE(code(doc.get()).size(), 1);
    QVERIFY(code(doc.get()).first().format.isAnchor());
    QCOMPARE(code(doc.get()).first().format.anchorHref(), QString("https://example.com"));
}

void TestMarkdownObjects::plainTextSpellsOutMarkersAndSkipsDecoration()
{
    auto doc = document("Intro with `x`\n\n- a\n- b\n\n3. c\n\n- [x] done\n\n```\nline 1\n  line 2\n```\n\n---\n\nEnd");
    MarkdownObjects::convertChips(doc.get(), light());
    QCOMPARE(all(doc.get()), QString::fromUtf8("Intro with x\n• a\n• b\n3. c\n[x] done\nline 1\n  line 2\nEnd"));
    QVERIFY(!all(doc.get()).contains(QChar::ObjectReplacementCharacter));
}

void TestMarkdownObjects::plainTextKeepsBlankLinesInsideMessages()
{
    // Activity cards are two-column tables; a user's blank line must survive copying.
    QTextDocument doc;
    doc.setHtml("<table><tr><td width='3'></td><td><p style='white-space:pre-wrap;'>first\n\nsecond</p></td></tr></table>");
    QCOMPARE(all(&doc), QString("first\n\nsecond"));
}

void TestMarkdownObjects::codeFollowsItsTextSizeAndColour()
{
    auto doc = document("## Title `main`\n\nBody `main`\n\n###### Note `main`");
    QCOMPARE(MarkdownObjects::convertChips(doc.get(), light()), 3);
    const auto found = code(doc.get());
    QCOMPARE(found.size(), 3);
    QCOMPARE(found[0].format.font().pixelSize(), 21);                              // heading size, not 85 %
    QVERIFY(found[0].format.boolProperty(MarkdownObjects::ChipHeading));         // heading padding 0 .2em
    QCOMPARE(found[0].format.fontWeight(), 600);                                    // heading weight
    QCOMPARE(found[1].format.font().pixelSize(), 12);
    QVERIFY(!found[1].format.boolProperty(MarkdownObjects::ChipHeading));
    QCOMPARE(found[2].format.foreground().color().name(), QString("#59636e"));
}

void TestMarkdownObjects::longJournalsConvertAndPaintQuickly()
{
    // Activity re-renders on every poll and repaints while scrolling: 4000 chips in
    // 800 card tables must not stall the GUI thread.
    QString html;
    for (int i = 0; i < 800; ++i)
        html += "<table width='100%'><tr><td width='3'></td><td>"
            + MarkdownHtml::render(QString("Reply %1 uses `a` `b` `c` `d` `e`.").arg(i), light(), {}) + "</td></tr></table>";
    auto doc = std::make_unique<QTextDocument>(); doc->setHtml(html); doc->setTextWidth(600);
    doc->documentLayout()->documentSize();
    QElapsedTimer timer; timer.start();
    QCOMPARE(MarkdownObjects::convertChips(doc.get(), light()), 4000);
    QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed()) + " ms"));
    const qreal middle = doc->documentLayout()->documentSize().height() / 2;
    QImage image(600, 800, QImage::Format_ARGB32_Premultiplied); QPalette palette;
    QPainter painter(&image); painter.translate(0, -middle);
    MarkdownObjects::paintChips(&painter, doc.get(), QRectF(0, middle, 600, 800), palette, {});   // finds the runs once
    // Thread CPU time, best of three: parallel builds and test shards preempt this thread
    // and can halve its speed. Culled frames cost about 50 ms; painting every chip of the
    // journal costs about 800 ms, so the bound still catches that regression.
    qint64 best = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3; ++run) {
        const auto start = threadCpuMs();
        for (int frame = 0; frame < 10; ++frame) MarkdownObjects::paintChips(&painter, doc.get(), QRectF(0, middle, 600, 800), palette, {});
        best = std::min(best, threadCpuMs() - start);
    }
    QVERIFY2(best < 200, qPrintable(QString::number(best) + " ms for 10 frames"));
}

void TestMarkdownObjects::richRepliesPaintWithinFrameBudget_data()
{
    QTest::addColumn<int>("replies");
    QTest::addColumn<bool>("paragraph");
    QTest::newRow("one-rich-reply") << 1 << false;
    QTest::newRow("long-rich-history") << 200 << false;
    QTest::newRow("dense-paragraph") << 1 << true;
    QTest::newRow("dense-paragraph-history") << 200 << true;
}

void TestMarkdownObjects::richRepliesPaintWithinFrameBudget()
{
    QFETCH(int, replies); QFETCH(bool, paragraph);
    const auto theme = MarkdownTheme::github(true, QColor("#1d252e"));
    QString body = "## Configuration example\n\nA readable response uses `tool settings` and `current_session`.\n\n";
    for (int i = 0; i < 18; ++i)
        body += QString("- This paragraph keeps `option_%1` and `example/cache/model-catalog/long-catalog-file.json` readable. "
                        "Longer prose spans multiple lines; selection and links must work without slowing scrolling.\n").arg(i);
    if (paragraph) { body.replace("\n- ", " "); body.replace("\n", " "); }
    const QString card = "<table width='100%' cellpadding='12'><tr><td width='3' bgcolor='#82b4d6'></td><td bgcolor='#23303a'>"
        + MarkdownHtml::render(body, theme, {}) + "</td></tr></table>";
    QTextDocument doc; doc.setHtml(card.repeated(replies));
    MarkdownObjects::convertChips(&doc, theme); doc.setTextWidth(850);
    auto *layout = doc.documentLayout();
    const qreal top = qMax(qreal(0), layout->documentSize().height() / 2 - 350);
    MarkdownObjects::prepareChips(&doc);
    QImage image(1700, 1400, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(2);
    QPalette palette; palette.setColor(QPalette::Base, theme.canvas);
    QPainter painter(&image); painter.translate(0, -top);
    QAbstractTextDocumentLayout::PaintContext context; context.palette = palette; context.clip = QRectF(0, top, 850, 700);
    const auto frame = [&] {
        layout->draw(&painter, context);
        MarkdownObjects::paintChips(&painter, &doc, context.clip, palette, {});
    };
    frame();
    qint64 best = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3; ++run) {
        const auto start = threadCpuMs();
        for (int i = 0; i < 20; ++i) frame();
        best = std::min(best, threadCpuMs() - start);
    }
    // The former document-wide rounded clip costs 150-225 ms per frame on
    // this rich response. Allow ample platform margin above the ~15 ms fix.
    QVERIFY2(best < 1000, qPrintable(QString("%1 ms CPU for 20 rich frames").arg(best)));
}

void TestMarkdownObjects::chipIndexFollowsLayoutAndContentChanges()
{
    // A tall left table cell overlaps many later blocks in the right cell.
    // Document order and vertical order differ, so culling must retain both.
    const QString html = "<table width='100%'><tr><td width='50%'>"
        + MarkdownHtml::render("`long left code repeats words across many lines and keeps its final rounded edge`", light(), {})
        + "</td><td>" + MarkdownHtml::render("First `right`\n\nSecond `right`\n\nThird `right`", light(), {}) + "</td></tr></table>";
    const auto fresh = [&](int width, bool prefix = false, bool largerFont = false) {
        auto doc = std::make_unique<QTextDocument>(); doc->setHtml(html);
        MarkdownObjects::convertChips(doc.get(), light()); doc->setTextWidth(width);
        if (prefix) {
            QTextCursor cursor(doc.get()); cursor.setPosition(0);
            cursor.insertText("New paragraph before the original table.\n");
        }
        if (largerFont) {
            QFont font = doc->defaultFont(); font.setPixelSize(21); doc->setDefaultFont(font);
        }
        return doc;
    };
    auto cached = fresh(250);
    const auto full = rendered(cached.get()); // Populate the index before subsequent mutations.
    for (int top = 0; top < full.height(); top += 40) {
        const QRect exposed(0, top, full.width(), qMin(60, full.height() - top));
        QImage tile(exposed.size(), QImage::Format_ARGB32_Premultiplied); tile.fill(Qt::white);
        QPalette palette; palette.setColor(QPalette::Base, Qt::white);
        QAbstractTextDocumentLayout::PaintContext context; context.palette = palette; context.clip = exposed;
        QPainter painter(&tile); painter.translate(0, -top);
        cached->documentLayout()->draw(&painter, context);
        MarkdownObjects::paintChips(&painter, cached.get(), exposed, palette, {});
        painter.end();
        QCOMPARE(tile, full.copy(exposed));
    }
    for (const int width : {170, 650, 230}) {
        cached->setTextWidth(width);
        QCOMPARE(rendered(cached.get()), rendered(fresh(width).get()));
    }
    QTextCursor cursor(cached.get()); cursor.setPosition(0);
    cursor.insertText("New paragraph before the original table.\n");
    QCOMPARE(rendered(cached.get()), rendered(fresh(230, true).get()));
    QFont font = cached->defaultFont(); font.setPixelSize(21); cached->setDefaultFont(font);
    QCOMPARE(rendered(cached.get()), rendered(fresh(230, true, true).get()));
    // A fresh document has never indexed this content or width.
    const QString replacement = MarkdownHtml::render("Changed `code`\n\nMore `new_code`.", light(), {});
    cached->setHtml(replacement); MarkdownObjects::convertChips(cached.get(), light()); cached->setTextWidth(180);
    QTextDocument replacementDoc; replacementDoc.setHtml(replacement);
    MarkdownObjects::convertChips(&replacementDoc, light()); replacementDoc.setTextWidth(180);
    QCOMPARE(rendered(cached.get()), rendered(&replacementDoc));
}

void TestMarkdownObjects::denseSelectedCodeKeepsEveryGlyph_data()
{
    QTest::addColumn<qreal>("ratio"); QTest::addColumn<qreal>("top");
    for (const qreal ratio : {1.0, 1.25, 1.5, 2.0})
        for (const qreal top : {0.0, 17.25})
            QTest::newRow(qPrintable(QString("%1x-offset-%2").arg(ratio).arg(top))) << ratio << top;
}

void TestMarkdownObjects::denseSelectedCodeKeepsEveryGlyph()
{
    QFETCH(qreal, ratio); QFETCH(qreal, top);
    const auto theme = MarkdownTheme::github(true, QColor("#1d252e"));
    QString prose;
    for (int i = 0; i < 40; ++i)
        prose += QString("Item `option_%1` beside `example/cache/model-catalog/long-catalog-file.json` and prose. ").arg(i);
    QTextDocument doc; doc.setHtml(MarkdownHtml::render(prose, theme, {}));
    MarkdownObjects::convertChips(&doc, theme); doc.setTextWidth(260);
    const QRectF exposed(0, top, 260, 700);
    QTextCursor cursor(&doc); cursor.select(QTextCursor::Document);
    QTextCharFormat selected; selected.setBackground(QColor("#315873")); selected.setForeground(Qt::white);
    QPalette palette; palette.setColor(QPalette::Base, theme.canvas);
    QAbstractTextDocumentLayout::PaintContext context; context.palette = palette; context.clip = exposed;
    context.selections.append({cursor, selected});
    QImage image(qCeil(260 * ratio), qCeil(700 * ratio), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio); image.fill(theme.canvas);
    QPainter painter(&image); painter.translate(0, -top); doc.documentLayout()->draw(&painter, context);
    const auto original = image.copy();
    MarkdownObjects::paintChips(&painter, &doc, exposed, palette, context.selections); painter.end();
    int ink = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto pixel = original.pixelColor(x, y);
            if (pixel.red() < 250 || pixel.green() < 250 || pixel.blue() < 250) continue;
            ++ink;
            QVERIFY2(close(image.pixelColor(x, y), pixel), qPrintable(QString("Selected glyph changed at %1,%2").arg(x).arg(y)));
        }
    QVERIFY(ink > 1000);
}

void TestMarkdownObjects::codeLinesKeepTheParagraphPitch()
{
    // Code reaching further below the baseline than text must neither grow its
    // line nor lift its baseline: every line of a paragraph keeps the same pitch.
    const QString markdown = "Only the round hash decides. `setWinner` / `#updateWinner` and the old bot are gone. "
        "Every round on the stand matched its hash, which plain lines like this one and the next one show without "
        "any code at all, until `roll` comes back at the very end.";
    for (const double scale : {1.0, 1.25}) {
        const auto theme = MarkdownTheme::github(false, QColor("#ffffff"), scale);
        QTextDocument doc; doc.setTextWidth(qRound(260 * scale));
        doc.setHtml(ContentScale::html(MarkdownHtml::render(markdown, theme, {}), scale));
        QCOMPARE(MarkdownObjects::convertChips(&doc, theme), 3);
        doc.documentLayout()->documentSize();
        const QTextBlock block = doc.begin();
        const QTextLayout *layout = block.layout();
        int withCode = 0;
        for (int i = 0; i < layout->lineCount(); ++i) {
            const QTextLine line = layout->lineAt(i);
            bool code = false;
            for (int position = line.textStart(); position < line.textStart() + line.textLength(); ++position) {
                QTextCursor at(&doc); at.setPosition(block.position() + position + 1);
                code = code || MarkdownObjects::isInlineCode(at.charFormat());
            }
            withCode += code;
            if (i == 0) continue;
            const QTextLine previous = layout->lineAt(i - 1);
            QCOMPARE(line.y() + line.ascent() - previous.y() - previous.ascent(), block.blockFormat().lineHeight());
        }
        QVERIFY2(withCode > 0 && withCode < layout->lineCount(), "the paragraph needs lines with and without code");
    }
}

void TestMarkdownObjects::wrappedCodeKeepsRoundedEnds()
{
    // Code that stays text wraps like GitHub's (box-decoration-break: slice): one
    // chip, rounded and padded where the code begins and ends, cut square where a
    // line breaks it. The spaces hanging at a break stay outside.
    const auto theme = MarkdownTheme::github(false, QColor("#ffffff"), 2.0);
    const QString code = "export HGS_STATE_DIR HGS_CONFIG_DIR HGS_RUNTIME_DIR HGS_LOG_DIR";
    QTextDocument doc; doc.setTextWidth(560);
    doc.setHtml(ContentScale::html(MarkdownHtml::render("Then run `" + code + "` again.", theme, {}), 2.0));
    MarkdownObjects::convertChips(&doc, theme);
    const QImage image = rendered(&doc);
    const CodeGeometry g(&doc, code, theme.monoFamily);
    const QTextLine first = g.line(g.start), last = g.line(g.end - 1);
    QVERIFY2(last.lineNumber() - first.lineNumber() == 2, "the code must take three lines");
    const QTextLine middle = g.block.layout()->lineAt(first.lineNumber() + 1);
    const auto pixel = [&](qreal x, qreal y) { return image.pixelColor(qFloor(x), qFloor(y)); };
    const QColor fill = over(theme.chip, Qt::white);
    const qreal padding = 0.4 * 24, radius = 12;
    const qreal left = g.x(g.start) - padding, right = g.x(g.end);
    QVERIFY(close(pixel(left + 1, g.top(first) + 1), Qt::white));
    QVERIFY(close(pixel(left + radius, g.top(first) + 1), fill));
    QVERIFY(close(pixel(left + padding / 2, (g.top(first) + g.bottom(first)) / 2), fill));
    QVERIFY(close(pixel(right - 1, g.bottom(last) - 1), Qt::white));
    // Sample close to the end: CoreText's final glyph reaches further into
    // the letter-spacing advance than the Linux rasterizer's glyph does.
    QVERIFY(close(pixel(right - padding / 4, (g.top(last) + g.bottom(last)) / 2), fill));
    for (const QTextLine &line : {first, middle}) {
        const int broken = line.textStart() + line.textLength();
        QVERIFY(g.block.text().at(broken - 1).isSpace());
        const qreal cut = g.origin.x() + line.cursorToX(broken - 1);
        QVERIFY(close(pixel(cut - 1, g.top(line) + 1), fill));
        QVERIFY(close(pixel(cut + 2, (g.top(line) + g.bottom(line)) / 2), Qt::white));
    }
    for (const QTextLine &line : {middle, last})
        QVERIFY(close(pixel(g.origin.x() + line.cursorToX(line.textStart()) + 1, g.bottom(line) - 1), fill));
    // The glyphs lie on the chip, untinted.
    bool ink = false;
    for (int y = qCeil(g.top(first)); y < g.bottom(first); ++y)
        for (int x = qCeil(left); x < g.origin.x() + first.naturalTextWidth(); ++x) ink = ink || close(image.pixelColor(x, y), theme.fg);
    QVERIFY(ink);
}

void TestMarkdownObjects::selectedCodeKeepsTheSelectionLook()
{
    // Selected and found characters show the view's highlight on top of the chip.
    const auto theme = MarkdownTheme::github(false, QColor("#ffffff"), 2.0);
    const QString code = "cargo test --workspace --all-features --no-fail-fast";
    QTextDocument doc; doc.setTextWidth(1600);
    doc.setHtml(ContentScale::html(MarkdownHtml::render("Then run `" + code + "` again.", theme, {}), 2.0));
    MarkdownObjects::convertChips(&doc, theme);
    const CodeGeometry g(&doc, code, theme.monoFamily);
    QTextCursor selected(&doc);
    selected.setPosition(g.block.position() + g.start + 6); selected.setPosition(g.block.position() + g.start + 13, QTextCursor::KeepAnchor);
    QTextCharFormat highlight; highlight.setBackground(QColor("#00ff00"));
    const QImage image = rendered(&doc, {{selected, highlight}});
    // The spaces after "cargo" and after "test", halfway up the code.
    const auto middleOf = [&](int offset) {
        const QTextLine line = g.line(g.start + offset);
        return image.pixelColor(qFloor((g.x(g.start + offset) + g.x(g.start + offset + 1)) / 2), qFloor(g.baseline(line) - g.metrics.ascent() / 2));
    };
    QVERIFY(close(middleOf(5), over(theme.chip, Qt::white)));
    QVERIFY(close(middleOf(10), QColor("#00ff00")));
}

QTEST_MAIN(TestMarkdownObjects)
#include "test_markdownobjects.moc"
