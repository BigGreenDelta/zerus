#include "TasksView.h"

#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <utility>

namespace {

constexpr int ColumnWidth = 244, CollapsedWidth = 44;
constexpr int CardPadding = 10, CardGap = 3, RefRow = 16, ChipHeight = 18, MaxTitleLines = 3;
const QString ProjectSetting = QStringLiteral("workspace/tasksProject");

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QFont sized(QFont font, int pixels, QFont::Weight weight)
{
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

QString owner(const OvermindModel::Session &session, const QString &name = {})
{
    return session.isEmpty() ? QString() : session.machine + QLatin1Char('\n') + (name.isEmpty() ? session.name : name);
}

QString markerText(const QString &marker)
{
    if (marker == QLatin1String("working")) return QObject::tr("working");
    if (marker == QLatin1String("reviewed")) return QObject::tr("reviewed");
    if (marker == QLatin1String("needs_work")) return QObject::tr("needs work");
    if (marker == QLatin1String("conflict")) return QObject::tr("conflict");
    if (marker == QLatin1String("uncertain")) return QObject::tr("unconfirmed delivery");
    if (marker == QLatin1String("orphaned")) return QObject::tr("orphaned");
    return marker;
}

// Up to maxLines lines of `text` in `width`; the last one is elided.
QStringList wrapped(const QString &text, const QFont &font, int width, int maxLines)
{
    QStringList lines;
    const QString simple = text.simplified();
    if (width <= 0 || simple.isEmpty()) return lines;
    QTextLayout layout(simple, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();
    for (;;) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(width);
        if (lines.size() == maxLines - 1) {
            lines << QFontMetrics(font).elidedText(simple.mid(line.textStart()), Qt::ElideRight, width);
            break;
        }
        lines << simple.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return lines;
}

} // namespace

TasksView::Palette TasksView::Palette::of(bool dark)
{
    Palette p;
    p.fg = QColor(dark ? "#e8edf4" : "#1a2733");
    p.muted = QColor(dark ? "#a2adbc" : "#627082");
    p.surface = QColor(dark ? "#1f262e" : "#ffffff");
    p.border = QColor(dark ? "#333c47" : "#dce2e8");
    p.accent = QColor(dark ? "#8bdfc0" : "#167357");
    p.hover = QColor(dark ? "#2b3540" : "#edf2f5");
    p.selected = QColor(dark ? "#233d35" : "#e0f0e9");
    p.column = QColor(dark ? "#151b21" : "#f3f6f8");
    p.chip = QColor(dark ? "#2b3540" : "#e9eef2");
    p.warning = QColor(dark ? "#f0c77b" : "#885400");
    p.warningFill = QColor(dark ? "#2e2717" : "#fff6e1");
    p.warningBorder = QColor(dark ? "#6a5428" : "#efd59a");
    p.danger = QColor(dark ? "#ff9298" : "#b52b3d");
    p.working = QColor(dark ? "#55e39a" : "#12834e");
    return p;
}

// One card: ref, priority and markers; the title in up to three lines; the
// session tag and labels as chips. The open card has an accent border, a card
// of the selected session an accent edge on a tinted surface.
class TaskCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setPalette(const TasksView::Palette &palette) { m_palette = palette; }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int width = itemWidth(option);
        const auto title = wrapped(index.data(TaskRoles::Title).toString(), titleFont(option.font), width - 2 * CardPadding, MaxTitleLines);
        const int lines = qMax(1, int(title.size()));
        const bool chips = !index.data(TaskRoles::Session).toString().isEmpty() || !index.data(TaskRoles::Labels).toStringList().isEmpty();
        const int height = CardGap + 8 + RefRow + 4 + lines * QFontMetrics(titleFont(option.font)).lineSpacing()
            + (chips ? 6 + ChipHeight : 0) + 9 + CardGap;
        return {width, height};
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect card = option.rect.adjusted(1, CardGap, -1, -CardGap);
        const bool open = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const bool highlighted = index.data(TaskRoles::Highlighted).toBool();
        QColor fill = highlighted ? m_palette.selected : m_palette.surface;
        if (hovered && !highlighted) fill = m_palette.hover;
        p->setPen(QPen(open ? m_palette.accent : m_palette.border, open ? 1.6 : 1));
        p->setBrush(fill);
        p->drawRoundedRect(QRectF(card).adjusted(.5, .5, -.5, -.5), 8, 8);
        if (highlighted) {
            QPainterPath shape;
            shape.addRoundedRect(QRectF(card), 8, 8);
            p->setClipPath(shape);
            p->fillRect(QRect(card.left(), card.top(), 4, card.height()), m_palette.accent);
            p->setClipping(false);
        }
        const int left = card.left() + CardPadding, right = card.right() - CardPadding, width = right - left;
        int y = card.top() + 8;

        // Ref, priority and markers.
        const QFont refFont = sized(option.font, 11, QFont::DemiBold);
        p->setFont(refFont);
        p->setPen(m_palette.muted);
        const QString ref = index.data(TaskRoles::Ref).toString();
        const QFontMetrics refMetrics(refFont);
        p->drawText(QRect(left, y, width, RefRow), Qt::AlignLeft | Qt::AlignVCenter, ref);
        int x = left + refMetrics.horizontalAdvance(ref) + 6;
        const int priority = index.data(TaskRoles::Priority).toInt();
        if (priority <= 1) {
            const QString text = QStringLiteral("P%1").arg(priority);
            const QRect badge(x, y + 1, refMetrics.horizontalAdvance(text) + 8, RefRow - 2);
            const QColor tone = priority == 0 ? m_palette.danger : m_palette.warning;
            QColor background = tone; background.setAlpha(36);
            p->setPen(Qt::NoPen); p->setBrush(background); p->drawRoundedRect(badge, 4, 4);
            p->setPen(tone); p->drawText(badge, Qt::AlignCenter, text);
        }
        int markerRight = right;
        const auto markers = index.data(TaskRoles::Markers).toStringList();
        QList<QPair<QString, QColor>> glyphs;
        if (markers.contains(QLatin1String("working"))) glyphs.append({QStringLiteral("▶"), m_palette.working});
        if (markers.contains(QLatin1String("reviewed"))) glyphs.append({QStringLiteral("✓"), m_palette.accent});
        if (markers.contains(QLatin1String("needs_work"))) glyphs.append({QStringLiteral("✗"), m_palette.danger});
        if (markers.contains(QLatin1String("conflict")) || markers.contains(QLatin1String("uncertain")))
            glyphs.append({QStringLiteral("⚠"), m_palette.warning});
        if (markers.contains(QLatin1String("orphaned"))) glyphs.append({QStringLiteral("⊘"), m_palette.danger});
        const QFont glyphFont = sized(option.font, 12, QFont::Bold);
        p->setFont(glyphFont);
        for (int i = glyphs.size(); i-- > 0;) {
            const int w = QFontMetrics(glyphFont).horizontalAdvance(glyphs[i].first) + 2;
            p->setPen(glyphs[i].second);
            p->drawText(QRect(markerRight - w, y, w, RefRow), Qt::AlignCenter, glyphs[i].first);
            markerRight -= w + 3;
        }
        y += RefRow + 4;

        // Title.
        const QFont font = titleFont(option.font);
        const QFontMetrics metrics(font);
        p->setFont(font);
        p->setPen(m_palette.fg);
        for (const auto &line : wrapped(index.data(TaskRoles::Title).toString(), font, width, MaxTitleLines)) {
            p->drawText(QRect(left, y, width, metrics.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, line);
            y += metrics.lineSpacing();
        }

        // Session tag and labels.
        const QString session = index.data(TaskRoles::Session).toString();
        const auto labels = index.data(TaskRoles::Labels).toStringList();
        if (!session.isEmpty() || !labels.isEmpty()) {
            y += 6;
            const QFont chipFont = sized(option.font, 11, QFont::Medium);
            const QFontMetrics chipMetrics(chipFont);
            p->setFont(chipFont);
            int cx = left;
            const auto chip = [&](const QString &text, const QColor &ink, const QColor &background, bool dot) {
                const int available = right - cx;
                if (available < 28) return false;
                const int inset = dot ? 16 : 7;
                const QString shown = chipMetrics.elidedText(text, Qt::ElideMiddle, available - inset - 7);
                const QRect rect(cx, y, inset + chipMetrics.horizontalAdvance(shown) + 7, ChipHeight);
                p->setPen(Qt::NoPen); p->setBrush(background); p->drawRoundedRect(rect, 5, 5);
                if (dot) { p->setBrush(ink); p->drawEllipse(QPointF(rect.left() + 8.5, rect.center().y() + .5), 3, 3); }
                p->setPen(ink);
                p->drawText(rect.adjusted(inset, 0, -7, 0), Qt::AlignLeft | Qt::AlignVCenter, shown);
                cx = rect.right() + 5;
                return true;
            };
            if (!session.isEmpty()) {
                QColor background = m_palette.accent; background.setAlpha(highlighted ? 44 : 30);
                chip(session, m_palette.accent, background, true);
            }
            for (const auto &label : labels) {
                QColor ink = m_palette.muted, background = m_palette.chip;
                if (label == QLatin1String("blocked")) { ink = m_palette.warning; background = m_palette.warningFill; }
                else if (label == QLatin1String("bug")) { ink = m_palette.danger; background = m_palette.danger; background.setAlpha(30); }
                if (!chip(label, ink, background, false)) break;
            }
        }
        if ((option.state & QStyle::State_HasFocus) && (option.state & QStyle::State_KeyboardFocusChange)) {
            p->setPen(QPen(m_palette.accent, 1, Qt::DotLine));
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(QRectF(card).adjusted(3, 3, -3, -3), 6, 6);
        }
        p->restore();
    }

private:
    static QFont titleFont(const QFont &base) { return sized(base, 13, QFont::Medium); }
    static int itemWidth(const QStyleOptionViewItem &option)
    {
        if (const auto *view = qobject_cast<const QAbstractScrollArea *>(option.widget)) return view->viewport()->width();
        return option.rect.width() > 0 ? option.rect.width() : ColumnWidth;
    }
    TasksView::Palette m_palette = TasksView::Palette::of(false);
};

// A column header: the stage, its card count and a chevron. Clicking it
// collapses or opens the column; a collapsed column shows it vertically.
// Cards dropped on it go to the end of the column.
class TaskColumnHeader : public QAbstractButton {
public:
    explicit TaskColumnHeader(const TasksView::Palette *palette) : m_palette(palette)
    {
        setObjectName(QStringLiteral("taskColumnHeader"));
        setAcceptDrops(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }
    void setState(const QString &name, int shown, int total, bool collapsed)
    {
        m_name = name; m_shown = shown; m_total = total; m_collapsed = collapsed;
        setAccessibleName(tr("%1: %n task(s)", nullptr, total).arg(name));
        setAccessibleDescription(collapsed ? tr("Collapsed. Activate to show the cards.") : tr("Activate to collapse the column."));
        setToolTip(collapsed ? tr("Show %1").arg(name) : tr("Collapse %1").arg(name));
        setSizePolicy(QSizePolicy::Preferred, collapsed ? QSizePolicy::Expanding : QSizePolicy::Fixed);
        updateGeometry();
        update();
    }
    QSize sizeHint() const override { return {m_collapsed ? CollapsedWidth - 8 : ColumnWidth - 12, 30}; }
    QSize minimumSizeHint() const override { return {24, 30}; }
    std::function<void(const QString &uuid)> dropped;
    std::function<void(bool)> hovering;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const auto &c = *m_palette;
        if (underMouse() || hasFocus()) {
            p.setPen(hasFocus() ? QPen(c.accent, 1) : Qt::NoPen);
            p.setBrush(c.hover);
            p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 6, 6);
        }
        const QFont nameFont = sized(font(), 12, QFont::DemiBold), countFont = sized(font(), 11, QFont::Medium);
        const QString count = m_shown == m_total ? QString::number(m_total) : QStringLiteral("%1/%2").arg(m_shown).arg(m_total);
        const int countWidth = QFontMetrics(countFont).horizontalAdvance(count) + 12;
        QColor countFill = c.muted; countFill.setAlpha(36);
        p.setPen(QPen(c.muted, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (m_collapsed) {
            // Chevron (pointing right: opens), count, then the name downwards.
            const QPointF tip(width() / 2.0 + 2, 15);
            p.drawPolyline(QPolygonF{tip + QPointF(-4, -4), tip, tip + QPointF(-4, 4)});
            const QRect badge((width() - countWidth) / 2, 28, countWidth, 18);
            p.setPen(Qt::NoPen); p.setBrush(countFill); p.drawRoundedRect(badge, 5, 5);
            p.setFont(countFont); p.setPen(c.muted); p.drawText(badge, Qt::AlignCenter, count);
            p.save();
            p.translate(width() / 2.0 + QFontMetrics(nameFont).ascent() / 2.0 - 1, 56);
            p.rotate(90);
            p.setFont(nameFont); p.setPen(c.fg);
            const int length = height() - 64;
            p.drawText(QRect(0, -QFontMetrics(nameFont).ascent(), length, QFontMetrics(nameFont).height()), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(nameFont).elidedText(m_name, Qt::ElideRight, length));
            p.restore();
            return;
        }
        // Name, count, and a chevron pointing down (collapses).
        const QPointF tip(width() - 14, height() / 2.0 + 2);
        p.drawPolyline(QPolygonF{tip + QPointF(-4, -4), tip, tip + QPointF(4, -4)});
        p.setFont(nameFont); p.setPen(c.fg);
        const int nameWidth = qMax(0, width() - 8 - countWidth - 6 - 30);
        const QString name = QFontMetrics(nameFont).elidedText(m_name, Qt::ElideRight, nameWidth);
        p.drawText(QRect(8, 0, nameWidth, height()), Qt::AlignLeft | Qt::AlignVCenter, name);
        const QRect badge(8 + QFontMetrics(nameFont).horizontalAdvance(name) + 6, (height() - 18) / 2, countWidth, 18);
        p.setPen(Qt::NoPen); p.setBrush(countFill); p.drawRoundedRect(badge, 5, 5);
        p.setFont(countFont); p.setPen(c.muted); p.drawText(badge, Qt::AlignCenter, count);
    }
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!event->mimeData()->hasFormat(QLatin1String(TasksView::MimeType))) { event->ignore(); return; }
        event->setDropAction(Qt::MoveAction); event->accept();
        if (hovering) hovering(true);
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!event->mimeData()->hasFormat(QLatin1String(TasksView::MimeType))) { event->ignore(); return; }
        event->setDropAction(Qt::MoveAction); event->accept();
    }
    void dragLeaveEvent(QDragLeaveEvent *) override { if (hovering) hovering(false); }
    void dropEvent(QDropEvent *event) override
    {
        if (hovering) hovering(false);
        if (!event->mimeData()->hasFormat(QLatin1String(TasksView::MimeType))) { event->ignore(); return; }
        event->setDropAction(Qt::MoveAction); event->accept();
        if (dropped) dropped(QString::fromUtf8(event->mimeData()->data(QLatin1String(TasksView::MimeType))));
    }

private:
    const TasksView::Palette *m_palette;
    QString m_name;
    int m_shown = 0, m_total = 0;
    bool m_collapsed = false;
};

// The cards of one column. Drags carry the task's UUID; a drop reports the
// card it lands before (empty: the end). A line shows where it will land: at
// the cursor inside the card's own column, at the end of another one.
class TaskList : public QListWidget {
public:
    TaskList(const QString &stage, const TasksView::Palette *palette) : m_stage(stage), m_palette(palette)
    {
        setObjectName(QStringLiteral("taskList"));
        setProperty("stage", stage);
        setAccessibleName(tr("%1 cards").arg(stage));
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setUniformItemSizes(false);
        setResizeMode(QListView::Adjust);
        setMouseTracking(true);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setDropIndicatorShown(false);
    }
    std::function<void(const QString &uuid, const QString &before)> dropped;
    std::function<void(bool)> hovering;

protected:
    void startDrag(Qt::DropActions) override
    {
        auto *item = currentItem();
        if (!item || !(item->flags() & Qt::ItemIsDragEnabled)) return;
        auto *mime = new QMimeData;
        mime->setData(QLatin1String(TasksView::MimeType), item->data(TaskRoles::Uuid).toString().toUtf8());
        mime->setData(QLatin1String(TasksView::StageMimeType), m_stage.toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        const QRect rect = visualItemRect(item);
        if (rect.isValid()) {
            QStyleOptionViewItem option;
            initViewItemOption(&option);
            option.rect = QRect(QPoint(), rect.size());
            option.state |= QStyle::State_Selected;
            const qreal ratio = devicePixelRatioF();
            QPixmap pixmap(rect.size() * ratio);
            pixmap.setDevicePixelRatio(ratio);
            pixmap.fill(Qt::transparent);
            {
                QPainter painter(&pixmap);
                itemDelegate()->paint(&painter, option, indexFromItem(item));
            }
            drag->setPixmap(pixmap);
            drag->setHotSpot(viewport()->mapFromGlobal(QCursor::pos()) - rect.topLeft());
        }
        drag->exec(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!accepts(event)) return;
        if (hovering) hovering(true);
        track(event->position().toPoint(), event->mimeData());
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!accepts(event)) return;
        const int y = int(event->position().y());
        if (y < 24) verticalScrollBar()->setValue(verticalScrollBar()->value() - 12);
        else if (y > viewport()->height() - 24) verticalScrollBar()->setValue(verticalScrollBar()->value() + 12);
        track(event->position().toPoint(), event->mimeData());
    }
    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_line = -1; viewport()->update();
        if (hovering) hovering(false);
    }
    void dropEvent(QDropEvent *event) override
    {
        m_line = -1; viewport()->update();
        if (hovering) hovering(false);
        if (!accepts(event)) return;
        int line = 0;
        const QString before = beforeAt(event->position().toPoint(), &line);
        if (dropped) dropped(QString::fromUtf8(event->mimeData()->data(QLatin1String(TasksView::MimeType))), before);
    }
    void paintEvent(QPaintEvent *event) override
    {
        QListWidget::paintEvent(event);
        if (m_line < 0) return;
        QPainter p(viewport());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(m_palette->accent, 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(6, m_line), QPointF(viewport()->width() - 6, m_line));
    }

private:
    bool accepts(QDropEvent *event)
    {
        if (!event->mimeData()->hasFormat(QLatin1String(TasksView::MimeType))) { event->ignore(); return false; }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return true;
    }
    void track(const QPoint &position, const QMimeData *mime)
    {
        int line = 0;
        const bool here = QString::fromUtf8(mime->data(QLatin1String(TasksView::StageMimeType))) == m_stage;
        beforeAt(here ? position : QPoint(0, viewport()->height() + 1), &line);
        if (line != m_line) { m_line = line; viewport()->update(); }
    }
    // The first shown card whose middle is below the point, and where the line goes.
    QString beforeAt(const QPoint &position, int *line) const
    {
        int bottom = CardGap;
        for (int row = 0; row < count(); ++row) {
            if (isRowHidden(row)) continue;
            const QRect rect = visualItemRect(item(row));
            if (position.y() < rect.center().y()) { *line = qMax(1, rect.top() + 1); return item(row)->data(TaskRoles::Uuid).toString(); }
            bottom = rect.bottom();
        }
        *line = qMin(bottom + 1, viewport()->height() - 2);
        return {};
    }
    QString m_stage;
    const TasksView::Palette *m_palette;
    int m_line = -1;
};

// A column: its header above its cards. A collapsed column is a narrow strip.
class TaskColumn : public QFrame {
public:
    TaskColumn(const OvermindModel::Stage &source, const TasksView::Palette *palette, QStyledItemDelegate *delegate)
        : stage(source.name), role(source.role), m_palette(palette)
    {
        setObjectName(QStringLiteral("taskColumn"));
        setProperty("stage", stage);
        setProperty("role", role);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(4);
        header = new TaskColumnHeader(palette);
        list = new TaskList(stage, palette);
        list->setItemDelegate(delegate);
        layout->addWidget(header);
        layout->addWidget(list, 1);
        list->hovering = header->hovering = [this](bool on) { m_dropTarget = on; update(); };
    }
    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed)
    {
        m_collapsed = collapsed;
        setProperty("collapsed", collapsed);
        list->setVisible(!collapsed);
        setFixedWidth(collapsed ? CollapsedWidth : ColumnWidth);
        updateHeader();
    }
    void setCounts(int shown, int total) { m_shown = shown; m_total = total; updateHeader(); }
    // Takes the column out of the board until it is deleted.
    void retire()
    {
        setObjectName({}); header->setObjectName({}); list->setObjectName({});
        hide();
        deleteLater();
    }

    const QString stage, role;
    TaskColumnHeader *header;
    TaskList *list;
    QList<QJsonObject> shown;   // the task views in the list, to skip unchanged columns

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(m_dropTarget ? QPen(m_palette->accent, 1.6) : QPen(m_palette->border, 1));
        p.setBrush(m_palette->column);
        p.drawRoundedRect(QRectF(rect()).adjusted(.8, .8, -.8, -.8), 10, 10);
    }

private:
    void updateHeader() { header->setState(stage, m_shown, m_total, m_collapsed); }
    const TasksView::Palette *m_palette;
    int m_shown = 0, m_total = 0;
    bool m_collapsed = false, m_dropTarget = false;
};

TasksView::TasksView(OvermindModel *model, QWidget *parent)
    : QWidget(parent), m_model(model), m_palette(Palette::of(false))
{
    setObjectName(QStringLiteral("tasksView"));
    m_delegate = new TaskCardDelegate(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);

    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    m_projects = new QComboBox;
    m_projects->setObjectName(QStringLiteral("tasksProject"));
    m_projects->setAccessibleName(tr("Project"));
    m_projects->setToolTip(tr("The project whose tasks this board shows"));
    m_projects->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_projects->setMinimumContentsLength(16);
    top->addWidget(m_projects);
    m_filter = new QLineEdit;
    m_filter->setObjectName(QStringLiteral("tasksFilter"));
    m_filter->setPlaceholderText(tr("Filter tasks"));
    m_filter->setAccessibleName(tr("Filter tasks"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(260);
    top->addWidget(m_filter, 1);
    top->addStretch();
    layout->addLayout(top);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("tasksStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->hide();
    layout->addWidget(m_status);

    m_messageBar = new QFrame;
    m_messageBar->setObjectName(QStringLiteral("tasksMessageBar"));
    auto *messageLayout = new QHBoxLayout(m_messageBar);
    messageLayout->setContentsMargins(10, 4, 4, 4);
    messageLayout->setSpacing(8);
    m_message = new QLabel;
    m_message->setObjectName(QStringLiteral("tasksMessage"));
    m_message->setWordWrap(true);
    m_message->setTextFormat(Qt::PlainText);
    m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    messageLayout->addWidget(m_message, 1);
    m_sendAnyway = new QPushButton(tr("Send anyway"));
    m_sendAnyway->setObjectName(QStringLiteral("tasksSendAnyway"));
    m_sendAnyway->setAutoDefault(false);
    messageLayout->addWidget(m_sendAnyway);
    m_dismiss = new QPushButton;
    m_dismiss->setObjectName(QStringLiteral("tasksDismissMessage"));
    m_dismiss->setToolTip(tr("Dismiss"));
    m_dismiss->setAccessibleName(tr("Dismiss the message"));
    m_dismiss->setFixedSize(26, 26);
    m_dismiss->setIconSize(QSize(14, 14));
    m_dismiss->setAutoDefault(false);
    messageLayout->addWidget(m_dismiss, 0, Qt::AlignTop);
    m_messageBar->hide();
    layout->addWidget(m_messageBar);

    m_yourCall = new QFrame;
    m_yourCall->setObjectName(QStringLiteral("tasksYourCall"));
    m_yourCall->setAccessibleName(tr("Your Call"));
    auto *yourCall = new QHBoxLayout(m_yourCall);
    yourCall->setContentsMargins(10, 4, 6, 4);
    yourCall->setSpacing(6);
    m_yourCallIcon = new QLabel;
    m_yourCallIcon->setFixedSize(16, 16);
    yourCall->addWidget(m_yourCallIcon);
    m_yourCallTitle = new QLabel;
    m_yourCallTitle->setObjectName(QStringLiteral("tasksYourCallTitle"));
    yourCall->addWidget(m_yourCallTitle);
    m_yourCallItems = new QHBoxLayout;
    m_yourCallItems->setSpacing(4);
    yourCall->addLayout(m_yourCallItems);
    m_yourCallMore = new QToolButton;
    m_yourCallMore->setObjectName(QStringLiteral("tasksYourCallMore"));
    m_yourCallMore->setPopupMode(QToolButton::InstantPopup);
    m_yourCallMore->setMenu(new QMenu(m_yourCallMore));
    yourCall->addWidget(m_yourCallMore);
    yourCall->addStretch();
    m_yourCall->hide();
    layout->addWidget(m_yourCall);

    m_sessionStrip = new QFrame;
    m_sessionStrip->setObjectName(QStringLiteral("tasksSessionStrip"));
    auto *strip = new QHBoxLayout(m_sessionStrip);
    strip->setContentsMargins(10, 3, 6, 3);
    strip->setSpacing(6);
    m_sessionQueue = new QLabel;
    m_sessionQueue->setObjectName(QStringLiteral("tasksSessionQueue"));
    strip->addWidget(m_sessionQueue);
    auto *separator = new QLabel(QStringLiteral("·"));
    separator->setObjectName(QStringLiteral("tasksSessionSeparator"));
    strip->addWidget(separator);
    m_sendNext = new QPushButton(tr("Send next"));
    m_sendNext->setObjectName(QStringLiteral("tasksSendNext"));
    m_sendNext->setAutoDefault(false);
    m_sendNext->setCursor(Qt::PointingHandCursor);
    strip->addWidget(m_sendNext);
    strip->addStretch();
    m_sessionStrip->hide();
    layout->addWidget(m_sessionStrip);

    m_board = new QScrollArea;
    m_board->setObjectName(QStringLiteral("tasksBoard"));
    m_board->setFrameShape(QFrame::NoFrame);
    m_board->setWidgetResizable(true);
    m_board->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_board->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("tasksBoardContent"));
    m_boardLayout = new QHBoxLayout(content);
    m_boardLayout->setContentsMargins(0, 0, 0, 2);
    m_boardLayout->setSpacing(10);
    m_boardLayout->addStretch(1);
    m_board->setWidget(content);
    layout->addWidget(m_board, 1);

    connect(m_projects, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0) setProject(m_projects->itemData(index).toString(), true);
    });
    connect(m_filter, &QLineEdit::textChanged, this, &TasksView::applyFilter);
    connect(m_sendNext, &QPushButton::clicked, this, [this] { sendNext(); });
    connect(m_sendAnyway, &QPushButton::clicked, this, [this] {
        const auto retry = m_retry;
        clearMessage();
        sendNext(retry);
    });
    connect(m_dismiss, &QPushButton::clicked, this, &TasksView::clearMessage);
    if (m_model) {
        connect(m_model, &OvermindModel::boardChanged, this, [this](const QString &project) {
            if (project == m_project) render();
        });
        connect(m_model, &OvermindModel::errorChanged, this, [this] {
            if (m_model) renderStatus(m_model->board(m_project));
        });
    }
    applyStyle();
    render();
}

void TasksView::setProjects(const QList<SessionOrganization::Group> &groups)
{
    QList<SessionOrganization::Group> accessible;
    for (const auto &group : groups) if (group.accessible) accessible.append(group);
    bool same = accessible.size() == m_groups.size();
    for (int i = 0; same && i < accessible.size(); ++i)
        same = accessible[i].id == m_groups[i].id && accessible[i].name == m_groups[i].name && accessible[i].color == m_groups[i].color;
    if (same && m_projects->count() == accessible.size()) return;
    m_groups = accessible;
    fillProjects();
    ensureProject();
    if (m_project.isEmpty()) renderStatus({});
}

void TasksView::fillProjects()
{
    const QSignalBlocker block(m_projects);
    m_projects->clear();
    for (const auto &group : std::as_const(m_groups))
        m_projects->addItem(workspaceIcon(QStringLiteral("projects"), QColor(group.color)), group.name, group.id);
    m_projects->setCurrentIndex(m_projects->findData(m_project));
}

void TasksView::setSession(const Selection &selection)
{
    if (selection.key == m_selection.key && selection.project == m_selection.project && selection.host == m_selection.host
        && selection.session.machine == m_selection.session.machine && selection.session.name == m_selection.session.name)
        return;
    m_selection = selection;
    ensureProject();
    applyHighlights();
}

// The project shown: the current one while it is offered; else the stored
// choice; else, once the tab is first opened, the selected session's project.
void TasksView::ensureProject()
{
    const auto offered = [this](const QString &id) {
        if (id.isEmpty()) return false;
        for (const auto &group : std::as_const(m_groups)) if (group.id == id) return true;
        return false;
    };
    if (offered(m_project)) return;
    const QString stored = QSettings().value(ProjectSetting).toString();
    if (offered(stored)) { setProject(stored, false); return; }
    const bool firstUse = stored.isEmpty();
    if (firstUse && !m_opened) {
        if (!m_project.isEmpty()) setProject({}, false);
        return;
    }
    // A chosen project that is no longer offered falls back without
    // forgetting the choice, which returns when it is offered again.
    const QString fallback = offered(m_selection.project) ? m_selection.project : m_groups.isEmpty() ? QString() : m_groups.first().id;
    setProject(fallback, firstUse && !fallback.isEmpty());
}

void TasksView::setProject(const QString &project, bool persist)
{
    if (persist && !project.isEmpty()) QSettings().setValue(ProjectSetting, project);
    if (project == m_project) {
        const QSignalBlocker block(m_projects);
        m_projects->setCurrentIndex(m_projects->findData(project));
        return;
    }
    if (!m_project.isEmpty()) m_openTasks.insert(m_project, m_openTask);
    m_project = project;
    m_openTask = m_openTasks.value(project);
    {
        const QSignalBlocker block(m_projects);
        m_projects->setCurrentIndex(m_projects->findData(project));
    }
    clearMessage();
    m_stages.clear();
    rebuildColumns();
    m_board->horizontalScrollBar()->setValue(0);
    if (m_model && !project.isEmpty()) m_model->watchProject(project);
    render();
}

void TasksView::showProject(const QString &project, const QString &sessionKey)
{
    bool offered = false;
    for (const auto &group : std::as_const(m_groups)) offered |= group.id == project;
    if (offered) setProject(project, true);
    emit showRequested();
    if (sessionKey.isEmpty() || sessionKey != m_selection.key) return;
    for (auto *column : std::as_const(m_columns))
        for (int row = 0; row < column->list->count(); ++row)
            if (column->list->item(row)->data(TaskRoles::Highlighted).toBool()) {
                m_board->ensureWidgetVisible(column);
                column->list->scrollToItem(column->list->item(row));
                return;
            }
}

void TasksView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_opened = true;
    ensureProject();
    if (m_model) m_model->setActive(true);
}

void TasksView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_model) m_model->setActive(false);
}

void TasksView::setTheme(bool dark)
{
    m_dark = dark;
    m_palette = Palette::of(dark);
    m_delegate->setPalette(m_palette);
    applyStyle();
    for (auto *column : std::as_const(m_columns)) {
        column->update();
        column->header->update();
        column->list->viewport()->update();
    }
}

void TasksView::applyStyle()
{
    const auto &c = m_palette;
    const auto rgba = [](QColor color, int alpha) { color.setAlpha(alpha); return color.name(QColor::HexArgb); };
    QString style = QStringLiteral(R"(
        QScrollArea#tasksBoard, QWidget#tasksBoardContent { background:transparent; border:0; }
        QListWidget#taskList { background:transparent; border:0; outline:0; }
        QLabel#tasksStatus { color:{muted}; font-size:12px; }
        QLabel#tasksStatus[error="true"] { color:{danger}; }
        QFrame#tasksMessageBar { background:{column}; border:1px solid {border}; border-radius:8px; }
        QLabel#tasksMessage { color:{fg}; }
        QLabel#tasksMessage[error="true"] { color:{danger}; }
        QPushButton#tasksSendAnyway { padding:4px 10px; min-height:16px; }
        QPushButton#tasksDismissMessage { background:transparent; border:1px solid transparent; border-radius:6px; padding:0; min-height:0; }
        QPushButton#tasksDismissMessage:hover { background:{hover}; }
        QFrame#tasksYourCall { background:{warningFill}; border:1px solid {warningBorder}; border-radius:8px; }
        QLabel#tasksYourCallTitle { color:{warning}; font-weight:600; }
        QPushButton#tasksDecision { background:transparent; color:{fg}; border:1px solid transparent; border-radius:6px; padding:3px 8px; min-height:16px; text-align:left; }
        QPushButton#tasksDecision:hover { background:{warningTint}; border-color:{warningBorder}; }
        QPushButton#tasksDecision[highlighted="true"] { border-color:{accent}; }
        QPushButton#tasksDecision[open="true"] { background:{warningTint}; border-color:{accent}; }
        QToolButton#tasksYourCallMore { color:{warning}; background:transparent; border:1px solid transparent; border-radius:6px; padding:3px 8px; }
        QToolButton#tasksYourCallMore:hover { border-color:{warningBorder}; }
        QToolButton#tasksYourCallMore::menu-indicator { image:none; width:0; }
        QFrame#tasksSessionStrip { background:{selected}; border:1px solid {accentTint}; border-radius:8px; }
        QLabel#tasksSessionQueue { color:{fg}; font-weight:600; }
        QLabel#tasksSessionSeparator { color:{muted}; }
        QPushButton#tasksSendNext { background:transparent; color:{accent}; border:1px solid transparent; border-radius:6px; padding:2px 6px; min-height:16px; font-weight:600; }
        QPushButton#tasksSendNext:hover { background:{hover}; }
        QPushButton#tasksSendNext:disabled { color:{muted}; background:transparent; }
        QComboBox#tasksProject, QLineEdit#tasksFilter { padding:6px 10px; }
    )");
    const QList<QPair<QString, QString>> tokens{
        {QStringLiteral("{fg}"), c.fg.name()}, {QStringLiteral("{muted}"), c.muted.name()}, {QStringLiteral("{border}"), c.border.name()},
        {QStringLiteral("{accent}"), c.accent.name()}, {QStringLiteral("{column}"), c.column.name()}, {QStringLiteral("{hover}"), c.hover.name()},
        {QStringLiteral("{warning}"), c.warning.name()}, {QStringLiteral("{danger}"), c.danger.name()},
        {QStringLiteral("{warningFill}"), c.warningFill.name()}, {QStringLiteral("{warningBorder}"), c.warningBorder.name()},
        {QStringLiteral("{warningTint}"), rgba(c.warning, 34)}, {QStringLiteral("{selected}"), c.selected.name()},
        {QStringLiteral("{accentTint}"), rgba(c.accent, 70)}};
    for (const auto &token : tokens) style.replace(token.first, token.second);
    setWorkspaceStyle(this, style + workspaceScrollbars(m_dark));
    m_dismiss->setIcon(workspaceIcon(QStringLiteral("close"), c.muted));
    m_yourCallIcon->setPixmap(workspaceIcon(QStringLiteral("attention"), c.warning).pixmap(16, 16));
}

void TasksView::render()
{
    if (!m_model) return;
    const OvermindModel::Board board = m_model->board(m_project);
    QList<OvermindModel::Stage> stages;
    for (const auto &stage : board.stages) if (stage.role != QLatin1String("needs_human")) stages.append(stage);
    bool same = stages.size() == m_stages.size();
    for (int i = 0; same && i < stages.size(); ++i) same = stages[i].name == m_stages[i].name && stages[i].role == m_stages[i].role;
    if (!same) {
        m_stages = stages;
        rebuildColumns();
    }
    fillColumns(board);
    renderDecisions(board);
    renderSession(board);
    renderStatus(board);
    applyFilter();
}

void TasksView::rebuildColumns()
{
    for (auto *column : std::as_const(m_columns)) {
        m_boardLayout->removeWidget(column);
        column->retire();
    }
    m_columns.clear();
    for (const auto &stage : std::as_const(m_stages)) {
        auto *column = new TaskColumn(stage, &m_palette, m_delegate);
        column->setCollapsed(collapsed(stage.name, stage.role));
        const QString name = stage.name;
        connect(column->header, &QAbstractButton::clicked, this, [this, name] { toggleColumn(name); });
        column->header->dropped = [this, name](const QString &uuid) { dropTask(uuid, name, {}); };
        column->list->dropped = [this, name](const QString &uuid, const QString &before) { dropTask(uuid, name, before); };
        auto *list = column->list;
        connect(list, &QListWidget::itemSelectionChanged, this, [this, list] {
            const auto items = list->selectedItems();
            if (items.isEmpty()) return;
            const QString uuid = items.first()->data(TaskRoles::Uuid).toString();
            if (uuid != m_openTask) openTask(uuid);
        });
        m_boardLayout->insertWidget(m_columns.size(), column);
        m_columns.append(column);
    }
}

void TasksView::fillColumns(const OvermindModel::Board &board)
{
    QHash<QString, QList<int>> byStage;
    for (int i = 0; i < board.tasks.size(); ++i) byStage[board.tasks[i].stage].append(i);
    for (auto *column : std::as_const(m_columns)) {
        const auto indexes = byStage.value(column->stage);
        QList<QJsonObject> shown;
        shown.reserve(indexes.size());
        for (int i : indexes) shown.append(board.tasks[i].json);
        if (shown == column->shown) continue;
        column->shown = shown;
        auto *list = column->list;
        const int scroll = list->verticalScrollBar()->value();
        {
            const QSignalBlocker block(list);
            list->clear();
            for (int i : indexes) {
                const auto &task = board.tasks[i];
                QStringList labels;
                for (const auto &label : task.labels)
                    if (label != QLatin1String("reviewed") && label != QLatin1String("review-failed")) labels.append(label);
                const QString pinned = task.pinnedCurrent.isEmpty() ? task.pinned.name : task.pinnedCurrent;
                const QString session = !task.assignee.isEmpty() ? task.assignee.name : !task.queueOf.isEmpty() ? task.queueOf.name
                    : !pinned.isEmpty() ? pinned : task.reviewer.name;
                QStringList owners{owner(task.assignee), owner(task.queueOf), owner(task.pinned, task.pinnedCurrent), owner(task.reviewer)};
                owners.removeAll(QString());
                QStringList markers;
                for (const auto &marker : task.markers) markers.append(markerText(marker));
                auto *item = new QListWidgetItem;
                item->setData(TaskRoles::Uuid, task.uuid);
                item->setData(TaskRoles::Ref, task.ref);
                item->setData(TaskRoles::Title, task.title);
                item->setData(TaskRoles::Labels, labels);
                item->setData(TaskRoles::Markers, task.markers);
                item->setData(TaskRoles::Session, session);
                item->setData(TaskRoles::Priority, task.priority);
                item->setData(TaskRoles::Stage, task.stage);
                item->setData(TaskRoles::Owners, owners);
                item->setData(TaskRoles::Highlighted, owned(owners));
                item->setData(Qt::DisplayRole, task.title);
                QStringList description{task.ref, task.title};
                if (!markers.isEmpty()) description << markers.join(QStringLiteral(", "));
                if (!labels.isEmpty()) description << tr("labels %1").arg(labels.join(QStringLiteral(", ")));
                if (!session.isEmpty()) description << tr("session %1").arg(session);
                item->setData(Qt::AccessibleTextRole, description.join(QStringLiteral(", ")));
                QStringList tip{task.ref + QStringLiteral(" · P%1").arg(task.priority), task.title};
                if (!session.isEmpty()) tip << tr("Session: %1").arg(session);
                if (!labels.isEmpty()) tip << tr("Labels: %1").arg(labels.join(QStringLiteral(", ")));
                if (!markers.isEmpty()) tip << tr("Markers: %1").arg(markers.join(QStringLiteral(", ")));
                item->setToolTip(tip.join(QLatin1Char('\n')));
                Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
                if (!task.json.value(QStringLiteral("read_only")).toBool()) flags |= Qt::ItemIsDragEnabled;
                item->setFlags(flags);
                list->addItem(item);
                if (task.uuid == m_openTask) item->setSelected(true);
            }
        }
        list->doItemsLayout();
        list->verticalScrollBar()->setValue(scroll);
    }
}

void TasksView::renderDecisions(const OvermindModel::Board &board)
{
    while (auto *item = m_yourCallItems->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    auto *menu = m_yourCallMore->menu();
    menu->clear();
    QList<const OvermindModel::Task *> waiting;
    for (const auto &task : board.tasks)
        if (board.role(task.stage) == QLatin1String("needs_human")) waiting.append(&task);
    m_yourCall->setVisible(!waiting.isEmpty());
    if (waiting.isEmpty()) return;
    m_yourCallTitle->setText(tr("Your Call (%1)").arg(waiting.size()));
    constexpr int Shown = 3;
    for (int i = 0; i < waiting.size(); ++i) {
        const auto &task = *waiting[i];
        QStringList owners{owner(task.assignee), owner(task.pinned, task.pinnedCurrent), owner(task.reviewer)};
        owners.removeAll(QString());
        const QString uuid = task.uuid;
        if (i >= Shown) {
            auto *action = menu->addAction(task.ref + QStringLiteral("  ") + task.title);
            connect(action, &QAction::triggered, this, [this, uuid] { openTask(uuid); });
            continue;
        }
        auto *button = new QPushButton;
        button->setObjectName(QStringLiteral("tasksDecision"));
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setText(task.ref + QStringLiteral("  ") + button->fontMetrics().elidedText(task.title, Qt::ElideRight, 260));
        button->setToolTip(tr("%1 waits for your answer: %2").arg(task.ref, task.title));
        button->setAccessibleName(tr("Open %1: %2").arg(task.ref, task.title));
        button->setProperty("uuid", uuid);
        button->setProperty("owners", owners);
        button->setProperty("highlighted", owned(owners));
        button->setProperty("open", uuid == m_openTask);
        connect(button, &QPushButton::clicked, this, [this, uuid] { openTask(uuid); });
        m_yourCallItems->addWidget(button);
    }
    m_yourCallMore->setVisible(waiting.size() > Shown);
    m_yourCallMore->setText(tr("+%1 more").arg(waiting.size() - Shown));
}

void TasksView::renderSession(const OvermindModel::Board &board)
{
    const bool here = !m_selection.key.isEmpty() && !m_project.isEmpty() && m_selection.project == m_project && board.loaded;
    m_sessionStrip->setVisible(here);
    if (!here) return;
    int queued = 0;
    for (const auto &task : board.tasks)
        if (board.role(task.stage) == QLatin1String("ready") && !task.queueOf.isEmpty() && belongs(task.queueOf.machine, task.queueOf.name)) ++queued;
    m_sessionQueue->setText(tr("This session's queue: %1").arg(queued));
    const QString next = nextTask(board);
    const bool reachable = !m_selection.session.machine.isEmpty() || m_selection.host.isEmpty();
    m_sendNext->setEnabled(!next.isEmpty() && reachable && !m_sending);
    QString tip;
    if (!reachable) tip = tr("Overmind does not know this session's computer yet.");
    else if (next.isEmpty()) tip = tr("No task waits in this session's queue or in the project pool.");
    else
        for (const auto &task : board.tasks)
            if (task.uuid == next) tip = tr("Send %1 to %2: %3").arg(task.ref, m_selection.session.name, task.title);
    m_sendNext->setToolTip(tip);
}

void TasksView::renderStatus(const OvermindModel::Board &board)
{
    QString text;
    bool error = false;
    if (!m_model || !m_model->isEnabled()) {
        text = tr("This computer has no task board. Select a session on a machine with Overmind.");
    } else if (m_project.isEmpty()) {
        text = m_groups.isEmpty() ? tr("No projects yet.") : tr("Choose a project to see its tasks.");
    } else if (!board.loaded) {
        if (!m_model->error().isEmpty()) { text = tr("Tasks are unavailable: %1").arg(m_model->error()); error = true; }
        else text = tr("Loading tasks…");
    } else if (!board.error.isEmpty()) {
        text = tr("Could not read the latest tasks: %1").arg(board.error);
        error = true;
    } else if (board.tasks.isEmpty()) {
        text = tr("No tasks in this project yet.");
    }
    m_status->setText(text);
    if (m_status->property("error").toBool() != error) {
        m_status->setProperty("error", error);
        repolish(m_status);
    }
    m_status->setVisible(!text.isEmpty());
}

void TasksView::applyFilter()
{
    const auto words = m_filter->text().simplified().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (auto *column : std::as_const(m_columns)) {
        auto *list = column->list;
        int shown = 0;
        for (int row = 0; row < list->count(); ++row) {
            const auto *item = list->item(row);
            const QString text = QStringList{item->data(TaskRoles::Ref).toString(), item->data(TaskRoles::Title).toString(),
                item->data(TaskRoles::Labels).toStringList().join(QLatin1Char(' ')), item->data(TaskRoles::Session).toString()}
                .join(QLatin1Char(' ')).toLower();
            bool match = true;
            for (const auto &word : words) match = match && text.contains(word);
            if (list->isRowHidden(row) == match) list->setRowHidden(row, !match);
            if (match) ++shown;
        }
        column->setCounts(shown, list->count());
    }
}

bool TasksView::belongs(const QString &machine, const QString &name) const
{
    if (m_selection.session.name.isEmpty() || name != m_selection.session.name) return false;
    return m_selection.session.machine.isEmpty() || machine == m_selection.session.machine;
}

bool TasksView::owned(const QStringList &owners) const
{
    if (m_selection.key.isEmpty() || m_project.isEmpty() || m_selection.project != m_project) return false;
    for (const auto &entry : owners) {
        const int split = entry.indexOf(QLatin1Char('\n'));
        if (split >= 0 && belongs(entry.left(split), entry.mid(split + 1))) return true;
    }
    return false;
}

void TasksView::applyHighlights()
{
    for (auto *column : std::as_const(m_columns))
        for (int row = 0; row < column->list->count(); ++row) {
            auto *item = column->list->item(row);
            const bool highlighted = owned(item->data(TaskRoles::Owners).toStringList());
            if (item->data(TaskRoles::Highlighted).toBool() != highlighted) item->setData(TaskRoles::Highlighted, highlighted);
        }
    for (int i = 0; i < m_yourCallItems->count(); ++i)
        if (auto *button = m_yourCallItems->itemAt(i)->widget()) {
            const bool highlighted = owned(button->property("owners").toStringList());
            if (button->property("highlighted").toBool() != highlighted) {
                button->setProperty("highlighted", highlighted);
                repolish(button);
            }
        }
    if (m_model) renderSession(m_model->board(m_project));
}

bool TasksView::collapsed(const QString &stage, const QString &role) const
{
    return m_collapsed.value(m_project + QLatin1Char('\n') + stage, role == QLatin1String("done") || role == QLatin1String("parked"));
}

void TasksView::toggleColumn(const QString &stage)
{
    for (auto *column : std::as_const(m_columns))
        if (column->stage == stage) {
            const bool collapse = !column->isCollapsed();
            m_collapsed.insert(m_project + QLatin1Char('\n') + stage, collapse);
            column->setCollapsed(collapse);
        }
}

// The first task of this session's queue, else the first of the project pool.
QString TasksView::nextTask(const OvermindModel::Board &board) const
{
    for (const auto &task : board.tasks)
        if (board.role(task.stage) == QLatin1String("ready") && !task.queueOf.isEmpty() && belongs(task.queueOf.machine, task.queueOf.name)
            && !task.json.value(QStringLiteral("read_only")).toBool())
            return task.uuid;
    for (const auto &task : board.tasks)
        if (board.role(task.stage) == QLatin1String("ready") && task.pinned.isEmpty() && !task.labels.contains(QLatin1String("epic"))
            && !task.json.value(QStringLiteral("read_only")).toBool())
            return task.uuid;
    return {};
}

void TasksView::sendNext(const QJsonObject &retry)
{
    if (!m_model || m_sending) return;
    QJsonObject request = retry;
    if (request.isEmpty()) {
        const QString uuid = nextTask(m_model->board(m_project));
        if (uuid.isEmpty() || m_selection.session.name.isEmpty()) return;
        QJsonObject target{{QStringLiteral("name"), m_selection.session.name}};
        if (!m_selection.session.machine.isEmpty()) target.insert(QStringLiteral("machine"), m_selection.session.machine);
        else if (!m_selection.host.isEmpty()) return;
        request = {{QStringLiteral("task"), uuid}, {QStringLiteral("session"), target}};
    }
    QString ref = request.value(QStringLiteral("task")).toString();
    for (const auto &task : m_model->board(m_project).tasks) if (task.uuid == ref) ref = task.ref;
    const QString name = request.value(QStringLiteral("session")).toObject().value(QStringLiteral("name")).toString();
    clearMessage();
    m_sending = true;
    m_sendNext->setEnabled(false);
    m_model->write(QStringLiteral("send"), request, [this, request, ref, name](const OvermindModel::Result &result) {
        m_sending = false;
        if (result.ok) {
            showMessage(tr("Sent %1 to %2.").arg(ref, name), false);
        } else {
            const auto &refusal = result.refusal;
            QString message = refusal.value(QStringLiteral("message")).toString();
            if (result.uncertain()) message = tr("%1 It may have been sent: check the task before sending it again.").arg(message);
            QJsonObject again;
            const QString code = refusal.value(QStringLiteral("code")).toString();
            if (refusal.value(QStringLiteral("field")).toString() == QLatin1String("confirm") && !code.isEmpty()) {
                again = request;
                again.insert(QStringLiteral("request_id"), result.requestId);
                auto confirm = again.value(QStringLiteral("confirm")).toArray();
                if (!confirm.contains(code)) confirm.append(code);
                again.insert(QStringLiteral("confirm"), confirm);
            }
            showMessage(message, true, again);
        }
        if (m_model) renderSession(m_model->board(m_project));
    }, this);
}

void TasksView::showMessage(const QString &text, bool error, const QJsonObject &retry)
{
    m_message->setText(text);
    if (m_message->property("error").toBool() != error) {
        m_message->setProperty("error", error);
        repolish(m_message);
    }
    m_retry = retry;
    m_sendAnyway->setVisible(!retry.isEmpty());
    m_messageBar->show();
}

void TasksView::clearMessage()
{
    m_messageBar->hide();
    m_retry = {};
}

void TasksView::dropTask(const QString &uuid, const QString &stage, const QString &before)
{
    if (!m_model || uuid.isEmpty()) return;
    const OvermindModel::Board board = m_model->board(m_project);
    const OvermindModel::Task *task = nullptr;
    QStringList column;
    for (const auto &candidate : board.tasks) {
        if (candidate.uuid == uuid) task = &candidate;
        if (candidate.stage == stage) column.append(candidate.uuid);
    }
    if (!task) return;
    QString command;
    QJsonObject request{{QStringLiteral("task"), uuid}};
    if (task->stage == stage) {
        const int at = column.indexOf(uuid);
        if (before == uuid || (before.isEmpty() && at == column.size() - 1) || (!before.isEmpty() && column.indexOf(before) == at + 1)) return;
        command = QStringLiteral("reorder");
        if (!before.isEmpty()) request.insert(QStringLiteral("before"), before);
    } else {
        command = QStringLiteral("move");
        request.insert(QStringLiteral("to"), stage);
    }
    clearMessage();
    // The card stays where it is until the board is read again: a refusal leaves it there.
    m_model->write(command, request, [this](const OvermindModel::Result &result) {
        if (result.ok) {
            clearMessage();
            return;
        }
        QString message = result.refusal.value(QStringLiteral("message")).toString();
        if (result.uncertain()) message = tr("%1 The change may have been made: the board shows it once it is read again.").arg(message);
        showMessage(message, true);
    }, this);
}

void TasksView::openTask(const QString &uuid)
{
    m_openTask = uuid;
    if (!m_project.isEmpty()) m_openTasks.insert(m_project, uuid);
    selectCard(uuid, true);
    for (int i = 0; i < m_yourCallItems->count(); ++i)
        if (auto *button = m_yourCallItems->itemAt(i)->widget()) {
            const bool open = button->property("uuid").toString() == uuid;
            if (button->property("open").toBool() != open) {
                button->setProperty("open", open);
                repolish(button);
            }
        }
    emit taskOpened(uuid);
}

// Selects the card in its column and clears the others. Revealing opens a
// collapsed column and scrolls the card into view.
void TasksView::selectCard(const QString &uuid, bool reveal)
{
    for (auto *column : std::as_const(m_columns)) {
        auto *list = column->list;
        const QSignalBlocker block(list);
        QListWidgetItem *found = nullptr;
        for (int row = 0; !found && row < list->count(); ++row)
            if (list->item(row)->data(TaskRoles::Uuid).toString() == uuid) found = list->item(row);
        if (!found) {
            list->clearSelection();
            continue;
        }
        if (!found->isSelected()) list->setCurrentItem(found);
        if (!reveal) continue;
        if (column->isCollapsed()) toggleColumn(column->stage);
        m_board->ensureWidgetVisible(column);
        list->scrollToItem(found);
    }
}
