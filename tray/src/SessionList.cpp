#include "SessionList.h"
#include "SessionCardDelegate.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QUuid>
#include <QScrollBar>
#include <QHelpEvent>
#include <QToolTip>
#include <QToolButton>
#include <QDynamicPropertyChangeEvent>
#include <cmath>

namespace { constexpr auto Mime = "application/x-hgs-session-placement"; }

SessionList::SessionList(QWidget *parent) : WorkspaceList(parent), m_token(QUuid::createUuid().toString())
{
    // Ordinary navigation selects one row. Explicit modified clicks operate
    // on the selection model without changing the open conversation.
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true); setAcceptDrops(true); setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction); setDropIndicatorShown(false); setAutoScroll(true);
    setAutoScrollMargin(28);
    m_activityTimer.setInterval(80);
    connect(&m_activityTimer, &QTimer::timeout, this, [this] {
        syncActivityAnimation();
        if (!m_activityTimer.isActive()) return;
        const qreal pulse = (1.0 - std::cos((m_activityClock.elapsed() % 1600) * 6.28318530718 / 1600.0)) / 2.0;
        setProperty("workingPulse", pulse);
        QHash<QPersistentModelIndex, QRegion> nextRegions;
        for (const auto &index : m_visibleWorkingRows) {
            const auto option = animationOption(index);
            const auto region = SessionDelegate::animationRegion(option, index);
            const auto previous = m_animationRegions.constFind(index);
            // Elapsed caption widths can move the badge at a second boundary.
            // Restore the entire title band before using the new indicator clip.
            if (previous != m_animationRegions.cend() && *previous != region)
                viewport()->update(SessionDelegate::elapsedRegion(option, index));
            else viewport()->update(region);
            nextRegions.insert(index, region);
        }
        m_animationRegions = std::move(nextRegions);
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &SessionList::syncActivityAnimation);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &SessionList::scheduleRowExpandButtons);
    connect(model(), &QAbstractItemModel::dataChanged, this, &SessionList::scheduleRowExpandButtons);
    connect(model(), &QAbstractItemModel::rowsInserted, this, &SessionList::scheduleRowExpandButtons);
    connect(model(), &QAbstractItemModel::rowsRemoved, this, &SessionList::scheduleRowExpandButtons);
    connect(model(), &QAbstractItemModel::modelReset, this, &SessionList::scheduleRowExpandButtons);
    connect(model(), &QAbstractItemModel::layoutChanged, this, &SessionList::scheduleRowExpandButtons);
    m_elapsedTimer.setInterval(1000);
    connect(&m_elapsedTimer, &QTimer::timeout, this, [this] {
        syncActivityAnimation();
        if (!m_elapsedTimer.isActive()) return;
        for (const auto &index : m_visibleWorkingRows) {
            if (index.data(SessionRoles::WorkingSince).toDouble() > 0)
                viewport()->update(SessionDelegate::elapsedRegion(animationOption(index), index));
        }
    });
}

SessionList::~SessionList()
{
    // QWidget deletes the owned model/scrollbar after our members are gone;
    // QObject's automatic disconnection happens later than that teardown.
    disconnect(model(), nullptr, this, nullptr);
    disconnect(verticalScrollBar(), nullptr, this, nullptr);
    m_activityTimer.stop(); m_elapsedTimer.stop();
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
}

void SessionList::setActivityAnimationEnabled(bool enabled)
{
    m_animateActivity = enabled; syncActivityAnimation();
}
void SessionList::syncActivityAnimation()
{
    bool active = false, elapsed = false;
    m_visibleWorkingRows.clear();
    if (count() && isVisible() && !window()->isMinimized()) {
        // Walk viewport geometry instead of scanning every hidden/offscreen row.
        const QRect viewportRect = viewport()->rect();
        for (int y = viewportRect.top(); y <= viewportRect.bottom();) {
            const auto index = indexAt(QPoint(viewportRect.center().x(), y));
            if (!index.isValid()) { ++y; continue; }
            const QRect row = visualRect(index);
            if (index.data(SessionRoles::Working).toInt() > 0) {
                m_visibleWorkingRows.append(index);
                active = m_animateActivity;
                elapsed |= !index.data(SessionRoles::Header).toBool() && index.data(SessionRoles::WorkingSince).toDouble() > 0;
            }
            y = qMax(y + 1, row.bottom() + 1);
        }
    }
    if (active && !m_activityTimer.isActive()) { m_activityClock.start(); m_activityTimer.start(); }
    else if (!active && m_activityTimer.isActive()) { m_activityTimer.stop(); setProperty("workingPulse", 0.0); viewport()->update(); m_animationRegions.clear(); }
    if (elapsed && !m_elapsedTimer.isActive()) m_elapsedTimer.start();
    else if (!elapsed) m_elapsedTimer.stop();
}
QStyleOptionViewItem SessionList::animationOption(const QModelIndex &index) const
{
    QStyleOptionViewItem option; initViewItemOption(&option);
    option.rect = visualRect(index);
    option.state.setFlag(QStyle::State_Selected, selectionModel()->isSelected(index));
    return option;
}
void SessionList::showEvent(QShowEvent *event) { WorkspaceList::showEvent(event); syncActivityAnimation(); scheduleRowExpandButtons(); }
void SessionList::hideEvent(QHideEvent *event) { m_activityTimer.stop(); m_elapsedTimer.stop(); setProperty("workingPulse", 0.0); WorkspaceList::hideEvent(event); }

void SessionList::clearBulkSelection()
{
    m_bulkSelecting = false; m_rangeAnchor.clear(); m_rangeCursor.clear();
    clearSelection(); emit bulkSelectionChanged();
}
void SessionList::setSelectionBar(QWidget *bar, bool visible)
{
    m_selectionBar = bar;
    if (bar->parentWidget() != this) bar->setParent(this);
    setViewportMargins(0, 0, 0, visible ? bar->height() + 10 : 0);
    bar->setGeometry(6, height() - bar->height() - 6, qMax(0, width() - 12), bar->height());
    bar->setVisible(visible); if (visible) bar->raise();
}
void SessionList::resizeEvent(QResizeEvent *event)
{
    WorkspaceList::resizeEvent(event);
    scheduleRowExpandButtons();
    if (m_selectionBar) m_selectionBar->setGeometry(6, height() - m_selectionBar->height() - 6, qMax(0, width() - 12), m_selectionBar->height());
}

void SessionList::scheduleRowExpandButtons()
{
    if (m_rowExpandButtonsPending) return;
    m_rowExpandButtonsPending = true;
    QTimer::singleShot(0, this, [this] { m_rowExpandButtonsPending = false; syncRowExpandButtons(); });
}

bool SessionList::event(QEvent *event)
{
    if (event->type() == QEvent::DynamicPropertyChange) {
        const auto name = static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName();
        if (name == "expansion" || name == "hgsDark") scheduleRowExpandButtons();
    }
    return WorkspaceList::event(event);
}

void SessionList::syncRowExpandButtons()
{
    const bool full = SessionStrip::expansion(this) >= 1;
    if (full != m_rowFullLayout) { m_rowFullLayout = full; doItemsLayout(); }
    auto unused = m_rowExpandButtons;
    const bool dark = property("hgsDark").toBool();
    // Overlay real accessible controls only for visible rows, never for the
    // complete session inventory. A very tall viewport still has a hard cap.
    if (isVisible() && full) for (int y = 0, controls = 0; y < viewport()->height() && controls < 128;) {
        const auto index = indexAt(QPoint(viewport()->width() / 2, y));
        if (!index.isValid()) { ++y; continue; }
        const auto option = animationOption(index);
        const QRect rect = SessionDelegate::expandRowRect(option, index);
        y = qMax(y + 1, option.rect.bottom() + 1);
        if (rect.isEmpty()) continue;
        ++controls;
        QPersistentModelIndex pin(index);
        auto *button = m_rowExpandButtons.value(pin);
        if (!button) {
            button = new QToolButton(viewport());
            button->setAutoRaise(true); button->setIconSize({16, 16}); button->setFocusPolicy(Qt::StrongFocus);
            connect(button, &QToolButton::clicked, this, [this, pin, button] {
                if (!pin.isValid() || !SessionDelegate::rowCollapsed(pin)) return;
                const auto key = pin.data(SessionRoles::Key).toString();
                m_expandClickKey = key;
                m_expandClickRect = button->geometry();
                m_expandClickClock.start();
                emit rowExpanded(key);
            });
            m_rowExpandButtons.insert(pin, button);
        }
        unused.remove(pin);
        const auto caption = tr("Expand session row");
        button->setObjectName("expandSessionRow");
        button->setProperty("sessionKey", index.data(SessionRoles::Key));
        button->setAccessibleName(tr("Expand session row: %1")
            .arg(index.data(SessionRoles::Title).toString()));
        button->setAccessibleDescription(SessionDelegate::compactDescription(option, index));
        const auto tooltip = SessionDelegate::compactTooltip(option, index);
        button->setToolTip(tooltip.isEmpty() ? caption : caption + '\n' + tooltip);
        if (!button->property("rowTheme").isValid() || button->property("rowTheme").toBool() != dark) {
            button->setProperty("rowTheme", dark);
            button->setIcon(workspaceIcon("expand-row", QColor(dark ? "#a1adbb" : "#647386")));
            button->setStyleSheet(QString("QToolButton { background:transparent; border:1px solid transparent; border-radius:4px; padding:0; } QToolButton:hover { background:%1; } QToolButton:focus { border-color:%2; }")
                .arg(dark ? "#344650" : "#d4e4e9", dark ? "#8bdfc0" : "#22795b"));
        }
        button->setGeometry(rect); button->show(); button->raise();
    }
    for (auto it = unused.cbegin(); it != unused.cend(); ++it) {
        if (it.value()->hasFocus()) setFocus(Qt::OtherFocusReason);
        it.value()->hide(); it.value()->deleteLater(); m_rowExpandButtons.remove(it.key());
    }
}

QString SessionList::childrenLabel(const QModelIndex &index)
{
    const auto count = index.data(SessionRoles::Children).toString();
    return count.isEmpty() && index.data(SessionRoles::HasChildren).toBool() ? tr("Agents") : count;
}

QRect SessionList::childrenControlRect(const QRect &row, const QModelIndex &index, QFont font)
{
    if (SessionDelegate::rowCollapsed(index) || !index.data(SessionRoles::HasChildren).toBool() || index.data(SessionRoles::Header).toBool()
        || !index.data(SessionRoles::ChildId).toString().isEmpty()) return {};
    font.setPixelSize(11); font.setWeight(QFont::Normal);
    const int width = CachedText::width(font, childrenLabel(index)) + 46;
    return QRect(row.right() - 13 - width, row.bottom() - 26, width, 20);
}

// The collapsed strip, or the list on its way there, shows tiles instead of cards.
static bool narrow(const QWidget *list)
{
    const auto expansion = list->property("expansion");
    return expansion.isValid() && expansion.toReal() < 1;
}

// Tiles keep a session's subagents closed: they have no counter to open them.
QRect SessionList::childrenControlRect(const QListWidgetItem *item) const
{
    return item && !narrow(this) ? childrenControlRect(visualItemRect(item), indexFromItem(item), font()) : QRect();
}

bool SessionList::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip && !narrow(this)) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const auto *row = itemAt(help->pos());
        if (row) {
            const auto index = indexFromItem(row);
            if (SessionDelegate::rowCollapsed(index)) {
                const auto text = SessionDelegate::compactTooltip(animationOption(index), index);
                if (!text.isEmpty()) QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(text), viewport(), visualItemRect(row));
                else QToolTip::hideText();
                return true;
            }
            const auto question = SessionDelegate::optionalQuestionRect(visualItemRect(row), indexFromItem(row), font(),
                property("compact").toBool(), row->isSelected());
            if (question.contains(help->pos())) {
                const auto text = OptionalQuestionBadge::tooltip(row->data(SessionRoles::OptionalQuestions).toInt());
                QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(text), viewport(), question);
                return true;
            }
            const auto rect = SessionDelegate::gitStatusRect(visualItemRect(row), indexFromItem(row), font(), property("compact").toBool());
            if (rect.contains(help->pos())) {
                const auto text = GitStatusBadge::tooltip(row->data(SessionRoles::GitStatus).toJsonObject());
                QToolTip::showText(help->globalPos(), Qt::convertFromPlainText(text), viewport(), rect);
                return true;
            }
        }
    }
    if (event->type() == QEvent::ToolTip && narrow(this)) {
        // The collapsed strip hides card text; its tooltip names the session and its state.
        const auto *help = static_cast<QHelpEvent *>(event);
        const auto *row = itemAt(help->pos());
        if (row && !row->data(SessionRoles::Header).toBool()) {
            QString state = row->data(SessionRoles::Status).toString();
            if (row->data(SessionRoles::Working).toInt() > 0) state = tr("Working");
            else if (row->data(SessionRoles::Unread).toBool() && !row->data(SessionRoles::Attention).toBool()) state = tr("New reply");
            QStringList lines{QStringLiteral("<b>%1</b> · %2").arg(row->data(SessionRoles::Title).toString().toHtmlEscaped(), state.toHtmlEscaped())};
            const QString detail = row->data(SessionRoles::Detail).toString().simplified();
            if (!detail.isEmpty()) lines << detail.toHtmlEscaped();
            QStringList place;
            for (const int role : {int(SessionRoles::Meta), int(SessionRoles::Host)})
                if (!row->data(role).toString().isEmpty()) place << row->data(role).toString().toHtmlEscaped();
            if (!place.isEmpty()) lines << place.join(QStringLiteral(" · "));
            const auto git = GitStatusBadge::tooltip(row->data(SessionRoles::GitStatus).toJsonObject());
            if (!git.isEmpty()) lines << Qt::convertFromPlainText(git);
            const int optional = row->data(SessionRoles::OptionalQuestions).toInt();
            if (optional > 0) lines << SessionPresentation::optionalQuestionsLabel(optional).toHtmlEscaped();
            QToolTip::showText(help->globalPos(), lines.join(QStringLiteral("<br>")), viewport(), visualItemRect(row));
            return true;
        }
    }
    if (event->type() == QEvent::Leave) {
        setProperty("hoveredChildren", QString()); viewport()->unsetCursor(); viewport()->update();
    }
    return WorkspaceList::viewportEvent(event);
}

void SessionList::mousePressEvent(QMouseEvent *e)
{
    setKeyboardFocusVisible(false);
    m_press = e->position().toPoint(); m_pressedGroup.clear(); m_pressedParent.clear(); m_togglePress = false;
    if (e->button() == Qt::RightButton) { e->accept(); return; }
    auto *item = itemAt(m_press);
    const bool additive = e->modifiers().testFlag(Qt::ControlModifier) || e->modifiers().testFlag(Qt::MetaModifier);
    if (e->button() == Qt::LeftButton && (additive || e->modifiers().testFlag(Qt::ShiftModifier))) {
        m_togglePress = true;
        if (rangeSelectable(item)) {
            if (e->modifiers().testFlag(Qt::ShiftModifier)) selectRange(row(item), additive);
            else {
                m_rangeAnchor = m_rangeCursor = item->data(SessionRoles::Key).toString();
                m_bulkSelecting = true; selectionModel()->select(indexFromItem(item), QItemSelectionModel::Toggle);
            }
            setFocus(); emit bulkSelectionChanged();
        }
        e->accept(); return;
    }
    if (e->button() == Qt::LeftButton) { m_bulkSelecting = false; emit bulkSelectionChanged(); }
    if (item && !item->data(SessionRoles::LaunchId).toString().isEmpty()) { e->accept(); return; }
    if (e->button() == Qt::LeftButton && item && item->data(SessionRoles::Header).toBool()) {
        m_pressedGroup = item->data(SessionRoles::Group).toString(); setFocus(); e->accept(); return;
    }
    if (e->button() == Qt::LeftButton && childrenControlRect(item).contains(m_press)) {
        m_pressedParent = item->data(SessionRoles::Key).toString(); setFocus(); e->accept(); return;
    }
    QListWidget::mousePressEvent(e);
    if (e->button() == Qt::LeftButton && rangeSelectable(currentItem()))
        m_rangeAnchor = m_rangeCursor = currentItem()->data(SessionRoles::Key).toString();
}

bool SessionList::rangeSelectable(const QListWidgetItem *item) const
{
    return item && !item->isHidden() && !item->data(SessionRoles::Header).toBool()
        && item->flags().testFlag(Qt::ItemIsEnabled) && item->flags().testFlag(Qt::ItemIsSelectable)
        && item->data(SessionRoles::ChildId).toString().isEmpty()
        && item->data(SessionRoles::LaunchId).toString().isEmpty();
}

int SessionList::rangeRow(const QString &key) const
{
    if (key.isEmpty()) return -1;
    for (int i = 0; i < count(); ++i)
        if (rangeSelectable(item(i)) && item(i)->data(SessionRoles::Key).toString() == key) return i;
    return -1;
}

void SessionList::selectRange(int targetRow, bool additive)
{
    auto *target = item(targetRow);
    if (!rangeSelectable(target)) return;
    int anchor = rangeRow(m_rangeAnchor);
    if (anchor < 0) {
        anchor = rangeSelectable(currentItem()) ? row(currentItem()) : targetRow;
        m_rangeAnchor = item(anchor)->data(SessionRoles::Key).toString();
    }
    QItemSelection selection;
    int first = -1;
    for (int i = 0; i <= count(); ++i) {
        const bool included = i < count() && rangeSelectable(item(i))
            && ((i >= qMin(anchor, targetRow) && i <= qMax(anchor, targetRow))
                || (additive && selectionModel()->isSelected(model()->index(i, 0))));
        if (included && first < 0) first = i;
        else if (!included && first >= 0) {
            selection.select(model()->index(first, 0), model()->index(i - 1, 0)); first = -1;
        }
    }
    m_bulkSelecting = true;
    m_rangeCursor = target->data(SessionRoles::Key).toString();
    // Keep the current/open conversation intact, independently of the range tip.
    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    scrollToItem(target);
}

void SessionList::mouseDoubleClickEvent(QMouseEvent *e)
{
    const auto point = e->position().toPoint();
    const auto *item = itemAt(point);
    if (e->button() != Qt::LeftButton || narrow(this) || !rangeSelectable(item)
        || item->data(SessionRoles::Key).toString().isEmpty() || childrenControlRect(item).contains(point)) {
        WorkspaceList::mouseDoubleClickEvent(e);
        return;
    }
    // The first press already handled selection. Do not turn the second
    // release into another navigation or range-selection action.
    m_togglePress = true;
    m_pressedGroup.clear(); m_pressedParent.clear();
    e->accept();
    if (e->modifiers() != Qt::NoModifier) return;
    const auto key = item->data(SessionRoles::Key).toString();
    // Expanding by the icon removes that button under the pointer. Its second
    // click can reach the row, but must not immediately compact it again.
    if (m_expandClickKey == key && m_expandClickRect.contains(point) && m_expandClickClock.isValid()
        && m_expandClickClock.elapsed() <= QApplication::doubleClickInterval()) return;
    if (SessionDelegate::rowCollapsed(indexFromItem(item))) emit rowExpanded(key);
    else emit rowCompacted(key);
}

void SessionList::mouseMoveEvent(QMouseEvent *e)
{
    if (m_togglePress) { e->accept(); return; }
    const auto point = e->position().toPoint();
    const auto *item = itemAt(point);
    const QString hovered = childrenControlRect(item).contains(point) ? item->data(SessionRoles::Key).toString() : QString();
    if (property("hoveredChildren").toString() != hovered) {
        setProperty("hoveredChildren", hovered); viewport()->update();
        if (hovered.isEmpty()) viewport()->unsetCursor(); else viewport()->setCursor(Qt::PointingHandCursor);
    }
    if (!m_pressedParent.isEmpty()) { e->accept(); return; }
    if (!m_pressedGroup.isEmpty() && e->buttons().testFlag(Qt::LeftButton)) {
        if ((e->position().toPoint() - m_press).manhattanLength() >= QApplication::startDragDistance()) startDrag(Qt::MoveAction);
        e->accept(); return;
    }
    QListWidget::mouseMoveEvent(e);
}

void SessionList::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_togglePress) { m_togglePress = false; e->accept(); return; }
    if (e->button() == Qt::RightButton) { e->accept(); return; }
    if (!m_pressedParent.isEmpty()) {
        const auto key = m_pressedParent; m_pressedParent.clear();
        const auto *item = itemAt(e->position().toPoint());
        if (e->button() == Qt::LeftButton && item && item->data(SessionRoles::Key).toString() == key
            && childrenControlRect(item).contains(e->position().toPoint())
            && (e->position().toPoint() - m_press).manhattanLength() < QApplication::startDragDistance()) emit childrenToggled(key);
        e->accept(); return;
    }
    if (!m_pressedGroup.isEmpty()) {
        const QString id = m_pressedGroup; m_pressedGroup.clear();
        const auto *item = itemAt(e->position().toPoint());
        if (e->button() == Qt::LeftButton && item && item->data(SessionRoles::Header).toBool() && item->data(SessionRoles::Group).toString() == id)
            emit groupToggled(id);
        e->accept(); return;
    }
    QListWidget::mouseReleaseEvent(e);
}

void SessionList::startDrag(Qt::DropActions)
{
    if (m_bulkSelecting && selectedItems().size() > 1) return;
    const auto *item = m_pressedGroup.isEmpty() ? currentItem() : itemAt(m_press);
    if (!item || !item->flags().testFlag(Qt::ItemIsDragEnabled)) return;
    QDrag drag(this); drag.setMimeData(mimeData({const_cast<QListWidgetItem *>(item)}));
    drag.setPixmap(viewport()->grab(visualItemRect(item))); drag.setHotSpot(QPoint(20, 15));
    m_dragging = true; m_pressedGroup.clear(); drag.exec(Qt::MoveAction);
    m_dragging = false; m_indicator = {}; viewport()->update(); emit dragFinished();
}

QMimeData *SessionList::mimeData(const QList<QListWidgetItem *> &items) const
{
    auto *mime = new QMimeData;
    if (items.size() != 1) return mime;
    const auto *item = items.first(); const bool group = item->data(SessionRoles::Header).toBool();
    if (!item->data(SessionRoles::ChildId).toString().isEmpty()) return mime;
    const QJsonObject data{{"token", m_token}, {"group", group},
        {"id", item->data(group ? SessionRoles::Group : SessionRoles::Identity).toString()}};
    mime->setData(Mime, QJsonDocument(data).toJson(QJsonDocument::Compact));
    return mime;
}

bool SessionList::validDrop(const QMimeData *mime) const
{
    if (!mime->hasFormat(Mime)) return false;
    const auto data = QJsonDocument::fromJson(mime->data(Mime)).object();
    if (data.value("token").toString() != m_token || data.value("id").toString().isEmpty()) return false;
    const int role = data.value("group").toBool() ? SessionRoles::Group : SessionRoles::Identity;
    for (int row = 0; row < count(); ++row) if (item(row)->data(role).toString() == data.value("id").toString()) return true;
    return false;
}

SessionList::Placement SessionList::placement(const QPoint &point, bool movingGroup) const
{
    Placement target;
    const auto *over = itemAt(point);
    if (!over) {
        // Blank area after the list is an explicit move to Ungrouped/end.
        target.group = "ungrouped";
        int bottom = 2;
        for (int i = 0; i < count(); ++i) if (!item(i)->isHidden()) bottom = visualItemRect(item(i)).bottom();
        target.indicator = QRect(8, bottom, viewport()->width() - 16, 2);
        return target;
    }
    target.group = over->data(SessionRoles::Group).toString();
    // A child cannot be reordered independently. Drops treat the entire
    // expanded family as one root session, including its lower boundary.
    while (!over->data(SessionRoles::ChildId).toString().isEmpty() && row(over) > 0) over = item(row(over) - 1);
    auto rect = visualItemRect(over);
    if (!movingGroup) for (int n = row(over) + 1; n < count() && !item(n)->data(SessionRoles::ChildId).toString().isEmpty(); ++n)
        if (!item(n)->isHidden()) rect.setBottom(visualItemRect(item(n)).bottom());
    const bool header = over->data(SessionRoles::Header).toBool();
    if (movingGroup) {
        // The upper/lower half of any group's header places before/after it.
        target.before = target.group;
        int line = rect.top();
        int headerRow = row(over);
        while (headerRow > 0 && !item(headerRow)->data(SessionRoles::Header).toBool()) --headerRow;
        if (!header || point.y() > rect.center().y()) {
            int next = row(over) + 1;
            while (next < count() && !item(next)->data(SessionRoles::Header).toBool()) ++next;
            target.before = next < count() ? item(next)->data(SessionRoles::Group).toString() : QString();
            line = next < count() ? visualItemRect(item(next)).top() : rect.bottom();
        } else line = visualItemRect(item(headerRow)).top();
        target.indicator = QRect(8, line, viewport()->width() - 16, 2);
    } else if (header) {
        target.onGroup = true; target.indicator = rect.adjusted(3, 1, -3, -1);
    } else {
        target.before = over->data(SessionRoles::Identity).toString();
        int line = rect.top();
        if (point.y() > rect.center().y()) {
            int next = row(over) + 1;
            while (next < count() && (item(next)->isHidden() || !item(next)->data(SessionRoles::ChildId).toString().isEmpty())) ++next;
            target.before = next < count() && item(next)->data(SessionRoles::Group).toString() == target.group && !item(next)->data(SessionRoles::Header).toBool()
                ? item(next)->data(SessionRoles::Identity).toString() : QString();
            line = rect.bottom();
        }
        target.indicator = QRect(12, line, viewport()->width() - 24, 2);
    }
    return target;
}

void SessionList::dragEnterEvent(QDragEnterEvent *e)
{
    if (validDrop(e->mimeData())) { e->setDropAction(Qt::MoveAction); e->accept(); } else e->ignore();
}
void SessionList::dragMoveEvent(QDragMoveEvent *e)
{
    if (!validDrop(e->mimeData())) { e->ignore(); return; }
    // Retain Qt's edge auto-scroll; the base drop mutation is deliberately unused.
    QListWidget::dragMoveEvent(e);
    const auto target = placement(e->position().toPoint(), QJsonDocument::fromJson(e->mimeData()->data(Mime)).object().value("group").toBool());
    m_indicator = target.indicator; m_onGroup = target.onGroup; viewport()->update();
    e->setDropAction(Qt::MoveAction); e->accept();
}
void SessionList::dragLeaveEvent(QDragLeaveEvent *e)
{
    m_indicator = {}; viewport()->update(); QListWidget::dragLeaveEvent(e);
}
void SessionList::dropEvent(QDropEvent *e)
{
    if (!validDrop(e->mimeData())) { e->ignore(); return; }
    const auto data = QJsonDocument::fromJson(e->mimeData()->data(Mime)).object();
    const auto target = placement(e->position().toPoint(), data.value("group").toBool());
    m_indicator = {}; viewport()->update();
    if (data.value("group").toBool()) emit groupMoved(data.value("id").toString(), target.before);
    else emit sessionMoved(data.value("id").toString(), target.group, target.before);
    e->setDropAction(Qt::MoveAction); e->accept();
}
void SessionList::keyPressEvent(QKeyEvent *e)
{
    setKeyboardFocusVisible(true);
    if (e->key() == Qt::Key_Escape && m_bulkSelecting) { clearBulkSelection(); e->accept(); return; }
    if (e->modifiers().testFlag(Qt::ShiftModifier)
        && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down || e->key() == Qt::Key_Home || e->key() == Qt::Key_End)) {
        const bool forward = e->key() == Qt::Key_Down || e->key() == Qt::Key_Home;
        int target = rangeRow(m_rangeCursor);
        if (target < 0 && rangeSelectable(currentItem())) target = row(currentItem());
        if (e->key() == Qt::Key_Home) target = -1;
        else if (e->key() == Qt::Key_End) target = count();
        else if (target < 0) target = forward ? -1 : count();
        for (target += forward ? 1 : -1; target >= 0 && target < count(); target += forward ? 1 : -1)
            if (rangeSelectable(item(target))) {
                selectRange(target, e->modifiers().testFlag(Qt::ControlModifier) || e->modifiers().testFlag(Qt::MetaModifier));
                emit bulkSelectionChanged(); break;
            }
        e->accept(); return;
    }
    if ((e->key() == Qt::Key_Left || e->key() == Qt::Key_Right) && currentItem()) {
        const auto *current = currentItem();
        if (e->key() == Qt::Key_Right && current->data(SessionRoles::RowCollapsed).toBool() && !narrow(this)) {
            emit rowExpanded(current->data(SessionRoles::Key).toString()); e->accept(); return;
        }
        if (!current->data(SessionRoles::ChildId).toString().isEmpty()) {
            if (e->key() == Qt::Key_Left) for (int i = row(current) - 1; i >= 0; --i)
                if (item(i)->data(SessionRoles::Key) == current->data(SessionRoles::ParentKey)) { setCurrentRow(i); break; }
            e->accept(); return;
        }
        if (current->data(SessionRoles::HasChildren).toBool()) {
            const bool expanded = current->data(SessionRoles::Expanded).toBool();
            if (expanded != (e->key() == Qt::Key_Right)) {
                // The strip may close a session's subagents but does not open them.
                if (expanded || !narrow(this)) emit childrenToggled(current->data(SessionRoles::Key).toString());
            } else if (expanded && row(current) + 1 < count()) setCurrentRow(row(current) + 1);
            e->accept(); return;
        }
        const auto id = currentItem()->data(SessionRoles::Group).toString();
        for (int i = 0; i < count(); ++i) if (item(i)->data(SessionRoles::Header).toBool() && item(i)->data(SessionRoles::Group).toString() == id) {
            if (item(i)->data(SessionRoles::Collapsed).toBool() == (e->key() == Qt::Key_Right)) emit groupToggled(id);
            e->accept(); return;
        }
    }
    QListWidget::keyPressEvent(e);
    if (rangeSelectable(currentItem()) && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down
        || e->key() == Qt::Key_Home || e->key() == Qt::Key_End))
        m_rangeAnchor = m_rangeCursor = currentItem()->data(SessionRoles::Key).toString();
}
void SessionList::paintEvent(QPaintEvent *e)
{
    m_paintRegion = e->region();
    QListWidget::paintEvent(e);
    m_paintRegion = {};
    if (m_indicator.isEmpty()) return;
    QPainter p(viewport()); p.setRenderHint(QPainter::Antialiasing);
    const QColor accent(property("hgsDark").toBool() ? "#8bdfc0" : "#167357");
    p.setPen(QPen(accent, 2));
    if (m_onGroup) { p.setBrush(Qt::NoBrush); p.drawRoundedRect(m_indicator, 5, 5); }
    else p.drawLine(m_indicator.topLeft(), m_indicator.topRight());
}
