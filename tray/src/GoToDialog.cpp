#include "GoToDialog.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int kMaxRows = 12;
constexpr int kPageStep = 10;
constexpr int kRowHeight = 30;

bool wordStart(const QString &text, int index)
{
    return index == 0 || !text.at(index - 1).isLetterOrNumber();
}

class GoToDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(0, kRowHeight);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const GoToItem it = index.data(Qt::UserRole).value<GoToItem>();
        const bool selected = option.state & QStyle::State_Selected;
        const QPalette &pal = option.palette;
        painter->save();
        if (selected) painter->fillRect(option.rect, pal.highlight());
        const QColor main = selected ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text);
        QColor muted = main;
        muted.setAlphaF(0.65);
        QRect r = option.rect.adjusted(10, 0, -10, 0);
        if (!it.chord.isEmpty()) {
            painter->setPen(muted);
            const int w = painter->fontMetrics().horizontalAdvance(it.chord);
            painter->drawText(r, Qt::AlignRight | Qt::AlignVCenter, it.chord);
            r.setRight(r.right() - w - 12);
        }
        QFont bold = option.font;
        bold.setBold(true);
        painter->setFont(bold);
        painter->setPen(main);
        const QFontMetrics bm(bold);
        const QString title = bm.elidedText(it.title, Qt::ElideRight, r.width());
        painter->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, title);
        const int used = bm.horizontalAdvance(title) + 10;
        if (!it.detail.isEmpty() && used < r.width()) {
            painter->setFont(option.font);
            painter->setPen(muted);
            const QString detail = QFontMetrics(option.font).elidedText(it.detail, Qt::ElideRight, r.width() - used);
            painter->drawText(r.adjusted(used, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, detail);
        }
        painter->restore();
    }
};
}

namespace GoTo {
int score(const QString &query, const QString &text)
{
    if (query.isEmpty()) return 0;
    const QString q = query.toLower();
    const QString t = text.toLower();
    int total = 0;
    int pos = 0;
    int previous = -1;
    for (int i = 0; i < q.size(); ++i) {
        const int found = t.indexOf(q.at(i), pos);
        if (found < 0) return -1;
        int points = 10;
        if (i == 0 && found == 0) points += 100;
        if (wordStart(t, found)) points += 20;
        if (previous >= 0) {
            const int gap = found - previous - 1;
            if (gap == 0) points += 10;
            else points -= std::min(gap, 8);
        }
        total += points;
        previous = found;
        pos = found + 1;
    }
    return total;
}

QList<GoToItem> rank(const QList<GoToItem> &items, const QString &query)
{
    QString q = query.trimmed();
    const bool commandsOnly = q.startsWith('>');
    if (commandsOnly) q = q.mid(1).trimmed();

    struct Entry { GoToItem item; int score; int order; };
    QList<Entry> entries;
    for (int i = 0; i < items.size(); ++i) {
        const GoToItem &it = items.at(i);
        if (commandsOnly && it.kind != GoToItem::Command) continue;
        const int s = score(q, it.title + QLatin1Char(' ') + it.detail);
        if (s < 0) continue;
        entries.append({it, s, i});
    }
    if (q.isEmpty()) {
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            const bool ar = a.item.kind == GoToItem::Session && a.item.recency >= 0;
            const bool br = b.item.kind == GoToItem::Session && b.item.recency >= 0;
            if (ar != br) return ar;
            if (ar) return a.item.recency < b.item.recency;
            return a.order < b.order;
        });
    } else {
        std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.item.kind != b.item.kind) return a.item.kind < b.item.kind;
            return a.order < b.order;
        });
    }
    QList<GoToItem> out;
    out.reserve(entries.size());
    for (const Entry &e : entries) out.append(e.item);
    return out;
}
}

GoToDialog::GoToDialog(std::function<QList<GoToItem>()> provider, QWidget *parent)
    : QFrame(parent, Qt::Popup), m_provider(std::move(provider))
{
    qRegisterMetaType<GoToItem>();
    setObjectName("goToDialog");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    m_query = new QLineEdit(this);
    m_query->setObjectName("goToQuery");
    m_query->setPlaceholderText(tr("Go to… (type > for commands)"));
    m_query->setClearButtonEnabled(true);
    m_query->installEventFilter(this);
    layout->addWidget(m_query);
    m_results = new QListWidget(this);
    m_results->setObjectName("goToResults");
    m_results->setItemDelegate(new GoToDelegate(m_results));
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setFocusPolicy(Qt::NoFocus);
    m_results->setUniformItemSizes(true);
    m_results->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_results);
    connect(m_query, &QLineEdit::textChanged, this, [this] { refresh(); });
    connect(m_results, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        m_results->setCurrentItem(item);
        activateCurrent();
    });
}

void GoToDialog::popup()
{
    m_items = m_provider ? m_provider() : QList<GoToItem>();
    m_query->blockSignals(true);
    m_query->clear();
    m_query->blockSignals(false);
    refresh();
    QWidget *host = parentWidget() ? parentWidget()->window() : nullptr;
    const int hostWidth = host ? host->width() : 640 + 32;
    setFixedWidth(std::max(200, std::min(640, hostWidth - 32)));
    fitToResults();
    if (host) move(host->mapToGlobal(QPoint((host->width() - width()) / 2, 12)));
    show();
    raise();
    m_query->setFocus();
}

void GoToDialog::refresh()
{
    const QList<GoToItem> ranked = GoTo::rank(m_items, m_query->text());
    m_results->clear();
    for (const GoToItem &it : ranked) {
        auto *row = new QListWidgetItem(m_results);
        row->setData(Qt::UserRole, QVariant::fromValue(it));
        row->setSizeHint(QSize(0, kRowHeight));
    }
    if (m_results->count() > 0) m_results->setCurrentRow(0);
    fitToResults();
}

void GoToDialog::fitToResults()
{
    const int rows = std::min(m_results->count(), kMaxRows);
    m_results->setFixedHeight(rows * kRowHeight + 2 * m_results->frameWidth());
    m_results->setVisible(rows > 0);
    layout()->activate();
    setFixedHeight(sizeHint().height());
}

void GoToDialog::moveCurrent(int delta)
{
    const int count = m_results->count();
    if (count == 0) return;
    const int row = std::clamp(m_results->currentRow() + delta, 0, count - 1);
    m_results->setCurrentRow(row);
}

void GoToDialog::activateCurrent()
{
    QListWidgetItem *row = m_results->currentItem();
    if (!row) return;
    const GoToItem it = row->data(Qt::UserRole).value<GoToItem>();
    hide();
    emit activated(it);
}

bool GoToDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_query && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Up: moveCurrent(-1); return true;
        case Qt::Key_Down: moveCurrent(1); return true;
        case Qt::Key_PageUp: moveCurrent(-kPageStep); return true;
        case Qt::Key_PageDown: moveCurrent(kPageStep); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: activateCurrent(); return true;
        case Qt::Key_Escape: hide(); return true;
        case Qt::Key_K:
            if (key->modifiers() & Qt::ControlModifier) { hide(); return true; }
            break;
        default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}
