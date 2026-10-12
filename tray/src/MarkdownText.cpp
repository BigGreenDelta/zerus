#include "MarkdownText.h"
#include "ContentScale.h"
#include "MarkdownObjects.h"

#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QTextCursor>
#include <QTextDocument>
#include <QWheelEvent>
#include <QtMath>

namespace {
// QTextDocument otherwise falls back to reading local files for a resource it
// cannot resolve. Markdown text is display data, never a source of file reads.
class MarkdownDocument final : public QTextDocument {
public:
    using QTextDocument::QTextDocument;
    qreal pixelRatio = 1.0;
protected:
    QVariant loadResource(int, const QUrl &url) override { return MarkdownObjects::resource(url, pixelRatio); }
};
}

MarkdownText::MarkdownText(QWidget *parent) : QTextBrowser(parent)
{
    setDocument(new MarkdownDocument(this));
    document()->setDocumentMargin(0);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setOpenLinks(false); setTabChangesFocus(true); setFocusPolicy(Qt::ClickFocus);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred); policy.setHeightForWidth(true); setSizePolicy(policy);
    setClickThrough(false);
}

MarkdownText::~MarkdownText() = default;

void MarkdownText::setMarkdownSource(const QString &markdown, const MarkdownTheme &theme, const MarkdownLinkPolicy &links)
{
    m_source = markdown;
    static_cast<MarkdownDocument *>(document())->pixelRatio = devicePixelRatioF();
    // One edit block for the markup and its chips, as in Activity.
    QTextCursor edit(document()); edit.beginEditBlock();
    setHtml(ContentScale::html(MarkdownHtml::render(markdown, theme, links), theme.scalePercent / 100.0));
    MarkdownObjects::convertChips(document(), theme);
    edit.endEditBlock();
    invalidateHeight();
}

void MarkdownText::setClickThrough(bool clickThrough)
{
    m_clickThrough = clickThrough;
    setTextInteractionFlags(clickThrough ? Qt::LinksAccessibleByMouse : Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
}

int MarkdownText::heightForWidth(int width) const
{
    const QMargins margins = contentsMargins();
    const int textWidth = qMax(1, width - margins.left() - margins.right());
    if (!m_measure) { m_measure.reset(document()->clone()); m_measuredWidth = -1; }
    if (textWidth != m_measuredWidth) {
        m_measure->setTextWidth(textWidth);
        m_measuredWidth = textWidth; m_measuredHeight = qCeil(m_measure->size().height());
    }
    return m_measuredHeight + margins.top() + margins.bottom();
}

QSize MarkdownText::sizeHint() const
{
    const int width = this->width() > 0 ? this->width() : QTextBrowser::sizeHint().width();
    return {QTextBrowser::sizeHint().width(), heightForWidth(width)};
}

QSize MarkdownText::minimumSizeHint() const { return {0, 0}; }

void MarkdownText::invalidateHeight()
{
    m_measure.reset(); m_measuredWidth = -1;
    updateGeometry();
}

QMimeData *MarkdownText::createMimeDataFromSelection() const
{
    auto *data = new QMimeData;
    data->setText(MarkdownObjects::plainText(textCursor()));
    return data;
}

void MarkdownText::paintEvent(QPaintEvent *event)
{
    QTextBrowser::paintEvent(event);
    MarkdownObjects::paintChips(this, event->rect());
}

void MarkdownText::changeEvent(QEvent *event)
{
    QTextBrowser::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange) invalidateHeight();
}

bool MarkdownText::passesThrough(QMouseEvent *event) const
{
    return m_clickThrough && anchorAt(event->position().toPoint()).isEmpty();
}

// Without text selection QTextBrowser ignores even a press on a link, which
// would then reach the parent too. A link keeps its clicks.
void MarkdownText::mousePressEvent(QMouseEvent *event)
{
    if (passesThrough(event)) { event->ignore(); return; }
    QTextBrowser::mousePressEvent(event);
    if (m_clickThrough) event->accept();
}

void MarkdownText::mouseReleaseEvent(QMouseEvent *event)
{
    if (passesThrough(event)) { event->ignore(); return; }
    QTextBrowser::mouseReleaseEvent(event);
    if (m_clickThrough) event->accept();
}

void MarkdownText::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (passesThrough(event)) { event->ignore(); return; }
    QTextBrowser::mouseDoubleClickEvent(event);
    if (m_clickThrough) event->accept();
}

// The text is laid out at full height, so the wheel always belongs to the
// surrounding scroll area. QTextEdit would also zoom on Ctrl+wheel.
void MarkdownText::wheelEvent(QWheelEvent *event) { event->ignore(); }
