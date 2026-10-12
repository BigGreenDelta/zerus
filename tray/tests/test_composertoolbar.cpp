#include "ComposerToolbar.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

namespace {
ToolbarChip *makeChip(const char *name, const QString &full, const QString &shortText, const QString &icon, ChipTone tone)
{
    auto *chip = new ToolbarChip; chip->setObjectName(name); chip->setLabels(full, shortText);
    chip->setIconName(icon); chip->setTone(tone); chip->setActive(true); return chip;
}

struct Row {
    QWidget window;
    QLineEdit *field = new QLineEdit;
    ComposerToolbar *toolbar = new ComposerToolbar;
    ToolbarChip *attachments = makeChip("attachments", "3 attached", "3", "attachment", ChipTone::Neutral);
    ToolbarChip *read = makeChip("read", "Mark as read", {}, "read-all", ChipTone::Quiet);
    ToolbarChip *recovery = makeChip("recovery", "Retry in 42 s", "42 s", "refresh", ChipTone::Warning);
    ToolbarChip *cache = makeChip("cache", "Cold cache", "Cold", "context-warning", ChipTone::Danger);
    QPushButton *context = new QPushButton("54% (139k/258k)");
    Row() {
        auto *layout = new QVBoxLayout(&window); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
        layout->addWidget(toolbar); layout->addWidget(field);
        context->setObjectName("context"); context->setFixedHeight(24); context->setFocusPolicy(Qt::TabFocus);
        // Added out of order on purpose: slots, not calls, decide the visual order.
        toolbar->add(ComposerToolbar::Slot::Context, context);
        toolbar->add(ComposerToolbar::Slot::Cache, cache);
        toolbar->add(ComposerToolbar::Slot::MarkRead, read);
        toolbar->add(ComposerToolbar::Slot::Recovery, recovery);
        toolbar->add(ComposerToolbar::Slot::Attachments, attachments);
    }
    void show(int width) { window.resize(width, 90); window.show(); }
};

QRect bounds(QWidget *widget, QWidget *root) { return QRect(widget->mapTo(root, QPoint()), widget->size()); }

// Shortening follows priority, notices never disappear and nothing overlaps.
void verifyFits(Row &row)
{
    QCOMPARE(row.toolbar->height(), ComposerToolbar::RowHeight);
    const QList<ToolbarChip *> lowestFirst{row.read, row.attachments, row.cache, row.recovery};
    bool higherShortened = false;
    for (qsizetype i = lowestFirst.size() - 1; i >= 0; --i) {
        if (higherShortened) QVERIFY2(lowestFirst[i]->isCompact() || lowestFirst[i]->isHidden(), qPrintable(lowestFirst[i]->objectName()));
        higherShortened = higherShortened || lowestFirst[i]->isCompact();
    }
    for (QWidget *always : QList<QWidget *>{row.attachments, row.recovery, row.cache, row.context})
        QVERIFY2(always->isVisible(), qPrintable(always->objectName()));
    if (row.read->isHidden()) for (auto *chip : {row.attachments, row.cache, row.recovery}) QVERIFY(chip->isCompact());
    int right = -1;
    for (QWidget *item : QList<QWidget *>{row.attachments, row.read, row.recovery, row.cache, row.context}) {
        if (item->isHidden()) continue;
        const QRect rect = bounds(item, row.toolbar);
        QVERIFY2(rect.left() > right, qPrintable(item->objectName()));
        QVERIFY2(rect.right() < row.toolbar->width(), qPrintable(item->objectName()));
        QVERIFY(qAbs(rect.center().y() - row.toolbar->height() / 2) <= 1);
        right = rect.right();
    }
}
}

class TestComposerToolbar : public QObject {
    Q_OBJECT
private slots:
    void slotsFixOrderAndRowHeight();
    void narrowRowsShortenInPriorityOrder_data();
    void narrowRowsShortenInPriorityOrder();
    void compactChipsKeepFullAccessibleNames();
    void popoverOpensFromMouseAndKeyboardAndRestoresFocus();
    void themeReachesChipsAddedBeforeAndAfter();
    void flashHighlightsBriefly();
    void roomyRowsShowWholeLabels();
    void chipStyleStaysOutOfPopover();
    void popoverFitsItsContent();
    void tasksChipLeadsTheTrailingZone();
    void hiddenChipHandsFocusToTheField();
    void openPopoverFollowsItsContent();
    void exactWidthNeedsNoShortening();
    void accentChipIsAGreenPill();
};

void TestComposerToolbar::slotsFixOrderAndRowHeight()
{
    Row row; row.show(900); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    QTRY_VERIFY(row.context->isVisible());
    verifyFits(row); if (QTest::currentTestFailed()) return;
    const int fieldTop = row.field->y();
    for (auto *chip : {row.attachments, row.read, row.recovery, row.cache}) chip->setActive(false);
    row.context->hide(); QCoreApplication::processEvents();
    QCOMPARE(row.toolbar->height(), ComposerToolbar::RowHeight); QCOMPARE(row.field->y(), fieldTop);
    row.cache->setActive(true); QTRY_VERIFY(row.cache->isVisible()); QCOMPARE(row.field->y(), fieldTop);
}

void TestComposerToolbar::narrowRowsShortenInPriorityOrder_data()
{
    QTest::addColumn<int>("width");
    for (int width : {900, 640, 420, 320}) QTest::newRow(qPrintable(QString::number(width))) << width;
}

void TestComposerToolbar::narrowRowsShortenInPriorityOrder()
{
    QFETCH(int, width);
    Row row; row.show(900); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    QTRY_VERIFY(row.context->isVisible());
    for (auto *chip : {row.attachments, row.read, row.recovery, row.cache}) QVERIFY(!chip->isCompact());
    row.window.resize(width, 90); QTRY_COMPARE(row.toolbar->width(), width);
    QCoreApplication::processEvents();
    verifyFits(row); if (QTest::currentTestFailed()) return;
    if (width <= 320) QVERIFY(row.read->isCompact() || row.read->isHidden());
    // Widening restores every full label and Mark as read.
    row.window.resize(900, 90);
    QTRY_VERIFY(!row.recovery->isCompact() && !row.read->isCompact() && row.read->isVisible());
}

void TestComposerToolbar::compactChipsKeepFullAccessibleNames()
{
    Row row; row.recovery->setDetail("Provider overloaded. The next attempt starts in 42 s.");
    row.show(320); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    QTRY_VERIFY(row.attachments->isCompact());
    for (auto *chip : {row.attachments, row.recovery, row.cache}) QCOMPARE(chip->accessibleName(), chip->fullLabel());
    QCOMPARE(row.recovery->toolTip(), QString("Provider overloaded. The next attempt starts in 42 s."));
    QCOMPARE(row.recovery->accessibleDescription(), row.recovery->toolTip());
    QCOMPARE(row.cache->toolTip(), QString("Cold cache"));
}

void TestComposerToolbar::popoverOpensFromMouseAndKeyboardAndRestoresFocus()
{
    Row row;
    auto *content = new QWidget; auto *action = new QPushButton("Clear context", content); action->setObjectName("popoverAction");
    (new QVBoxLayout(content))->addWidget(action);
    row.cache->setPopoverContent(content);
    // Leave room above the row: a popover never leaves the screen.
    row.window.move(40, 300); row.show(600); row.window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&row.window));
    row.field->setFocus(); QTRY_VERIFY(row.field->hasFocus());
    const auto escape = [&] {
        QWidget *target = QApplication::focusWidget() ? QApplication::focusWidget() : row.cache->popover();
        QTest::keyClick(target, Qt::Key_Escape);
    };
    // Mouse: opens above the chip and gives focus back to the field.
    QTest::mouseClick(row.cache, Qt::LeftButton);
    QTRY_VERIFY(row.cache->popover()->isVisible());
    QVERIFY2(row.cache->popover()->geometry().bottom() < row.cache->mapToGlobal(QPoint(0, 0)).y(),
        qPrintable(QString("popover %1,%2 chip top %3").arg(row.cache->popover()->geometry().top()).arg(row.cache->popover()->geometry().bottom()).arg(row.cache->mapToGlobal(QPoint(0, 0)).y())));
    escape(); QTRY_VERIFY(!row.cache->popover()->isVisible()); QTRY_VERIFY(row.field->hasFocus());
    // Keyboard: Enter and Space open it; Escape returns to the chip.
    for (const auto key : {Qt::Key_Return, Qt::Key_Space}) {
        row.cache->setFocus(Qt::TabFocusReason); QTRY_VERIFY(row.cache->hasFocus());
        QTest::keyClick(row.cache, key); QTRY_VERIFY(row.cache->popover()->isVisible());
        escape(); QTRY_VERIFY(!row.cache->popover()->isVisible()); QTRY_VERIFY(row.cache->hasFocus());
    }
    // A chip without a popover only reports the click, and a click never takes focus.
    row.field->setFocus(); QTRY_VERIFY(row.field->hasFocus());
    QSignalSpy clicked(row.attachments, &QPushButton::clicked);
    QTest::mouseClick(row.attachments, Qt::LeftButton); QCOMPARE(clicked.size(), 1); QVERIFY(row.field->hasFocus());
    // Deactivating a chip closes its popover.
    QTest::mouseClick(row.cache, Qt::LeftButton); QTRY_VERIFY(row.cache->popover()->isVisible());
    row.cache->setActive(false); QVERIFY(!row.cache->popover()->isVisible());
}

void TestComposerToolbar::themeReachesChipsAddedBeforeAndAfter()
{
    Row row; row.toolbar->setTheme(false);
    QVERIFY(row.cache->styleSheet().contains("#b52d48")); QVERIFY(row.recovery->styleSheet().contains("#91621a"));
    auto *late = makeChip("late", "Limit reached", "Limit", "attention", ChipTone::Danger);
    row.toolbar->add(ComposerToolbar::Slot::UsageLimit, late); QVERIFY(late->styleSheet().contains("#b52d48"));
    late->setPopoverContent(new QWidget); QVERIFY(late->popover()->styleSheet().contains("#f8fafb"));
    row.toolbar->setTheme(true);
    QVERIFY(late->styleSheet().contains("#ff9ca8")); QVERIFY(late->popover()->styleSheet().contains("#171d24"));
}

void TestComposerToolbar::flashHighlightsBriefly()
{
    Row row; row.show(900); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    const auto before = row.attachments->styleSheet();
    row.attachments->flash();
    QVERIFY(row.attachments->property("flashing").toBool()); QVERIFY(row.attachments->styleSheet() != before);
    QTRY_VERIFY_WITH_TIMEOUT(!row.attachments->property("flashing").toBool(), 2000);
    QCOMPARE(row.attachments->styleSheet(), before);
}

void TestComposerToolbar::roomyRowsShowWholeLabels()
{
    Row row; row.show(900); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    QTRY_VERIFY(row.context->isVisible()); QCoreApplication::processEvents();
    for (auto *chip : {row.attachments, row.read, row.recovery, row.cache}) {
        QVERIFY(!chip->isCompact()); QCOMPARE(chip->visibleText(), chip->fullLabel());
    }
    row.window.resize(420, 90); QTRY_COMPARE(row.toolbar->width(), 420); QCoreApplication::processEvents();
    // Compact labels are whole too while the row has room for them.
    for (auto *chip : {row.attachments, row.recovery, row.cache})
        if (chip->isCompact()) QCOMPARE(chip->visibleText(), chip->shortLabel());
}

void TestComposerToolbar::chipStyleStaysOutOfPopover()
{
    Row row; auto *content = new QWidget; auto *inside = new QPushButton("Retry now", content);
    (new QVBoxLayout(content))->addWidget(inside); row.recovery->setPopoverContent(content);
    QPushButton outside("Retry now"); outside.ensurePolished(); inside->ensurePolished();
    // The chip's 11 px font, padding and border belong to the chip alone.
    QCOMPARE(inside->font(), outside.font()); QCOMPARE(inside->sizeHint(), outside.sizeHint());
}

void TestComposerToolbar::popoverFitsItsContent()
{
    // The recovery panel's row of five actions must not be squeezed into a narrow popover.
    Row row; auto *content = new QWidget; content->setMinimumWidth(500);
    row.recovery->setPopoverContent(content);
    row.window.move(10, 300); row.show(600); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    row.recovery->openPopover(); QTRY_VERIFY(row.recovery->popover()->isVisible());
    QVERIFY2(row.recovery->popover()->width() >= 500, qPrintable(QString::number(row.recovery->popover()->width())));
    QVERIFY(row.recovery->popover()->geometry().right() <= row.window.screen()->availableGeometry().right());
}

void TestComposerToolbar::hiddenChipHandsFocusToTheField()
{
    // A chip that ends (recovery succeeded) must not strand keyboard focus on hidden widgets.
    Row row; row.toolbar->setFocusFallback(row.field);
    auto *content = new QWidget; auto *action = new QPushButton("Retry now", content); (new QVBoxLayout(content))->addWidget(action);
    row.recovery->setPopoverContent(content);
    row.window.move(40, 300); row.show(600); row.window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&row.window));
    row.recovery->setFocus(Qt::TabFocusReason); QTRY_VERIFY(row.recovery->hasFocus());
    QTest::keyClick(row.recovery, Qt::Key_Return); QTRY_VERIFY(row.recovery->popover()->isVisible());
    action->setFocus(); QTRY_VERIFY(action->hasFocus());
    row.recovery->setActive(false);
    QVERIFY(!row.recovery->popover()->isVisible()); QTRY_VERIFY(row.field->hasFocus());
    // A focused chip without an open popover hands focus over too.
    row.cache->setFocus(Qt::TabFocusReason); QTRY_VERIFY(row.cache->hasFocus());
    row.cache->setActive(false); QTRY_VERIFY(row.field->hasFocus());
}

void TestComposerToolbar::openPopoverFollowsItsContent()
{
    // Content that grows or shrinks while open keeps the popover above its chip.
    Row row; auto *content = new QWidget; auto *text = new QLabel("One line", content); (new QVBoxLayout(content))->addWidget(text);
    row.recovery->setPopoverContent(content);
    row.window.move(40, 300); row.show(600); QVERIFY(QTest::qWaitForWindowExposed(&row.window));
    row.recovery->openPopover(); QTRY_VERIFY(row.recovery->popover()->isVisible());
    auto *popover = row.recovery->popover(); const int chipTop = row.recovery->mapToGlobal(QPoint(0, 0)).y(); const int small = popover->height();
    text->setText(QStringList(8, "Another line").join('\n'));
    QTRY_VERIFY(popover->height() > small + 40); QTRY_VERIFY(popover->geometry().bottom() < chipTop);
    text->setText("One line");
    QTRY_COMPARE(popover->height(), small); QTRY_VERIFY(popover->geometry().bottom() < chipTop);
}

void TestComposerToolbar::exactWidthNeedsNoShortening()
{
    // Five items have four gaps: a row exactly that wide keeps every full label.
    Row row; row.show(900); QVERIFY(QTest::qWaitForWindowExposed(&row.window)); QTRY_VERIFY(row.context->isVisible());
    int needed = row.context->sizeHint().width() + 4 * 6;
    for (auto *chip : {row.attachments, row.read, row.recovery, row.cache}) needed += chip->labelSizeHint(false).width();
    row.window.resize(needed, 90); QTRY_COMPARE(row.toolbar->width(), needed); QCoreApplication::processEvents();
    for (auto *chip : {row.attachments, row.read, row.recovery, row.cache}) QVERIFY2(!chip->isCompact(), qPrintable(chip->objectName()));
    verifyFits(row);
}

// The session's Tasks chip opens the trailing zone, before the usage, recovery
// and cache notices; a narrow row shortens it after Attachments, before Cache.
void TestComposerToolbar::tasksChipLeadsTheTrailingZone()
{
    Row row;
    auto *tasks = makeChip("tasks", "Tasks 12", "12", "tasks", ChipTone::Neutral);
    auto *limit = makeChip("limit", "Limit reached · 2h 14m", "Limit", "attention", ChipTone::Danger);
    row.toolbar->add(ComposerToolbar::Slot::UsageLimit, limit);
    row.toolbar->add(ComposerToolbar::Slot::Tasks, tasks);
    row.show(1100); QVERIFY(QTest::qWaitForWindowExposed(&row.window)); QTRY_VERIFY(tasks->isVisible());
    QCoreApplication::processEvents();
    QVERIFY(!tasks->isCompact());
    int right = -1;
    for (QWidget *item : QList<QWidget *>{row.attachments, row.read, tasks, limit, row.recovery, row.cache, row.context}) {
        const QRect rect = bounds(item, row.toolbar);
        QVERIFY2(rect.left() > right, qPrintable(item->objectName()));
        right = rect.right();
    }
    // Tasks belongs to the trailing zone: it stays right of the gap.
    QVERIFY(bounds(tasks, row.toolbar).left() - bounds(row.read, row.toolbar).right() > 20);
    bool compacted = false;
    for (int width = 1100; width >= 360; width -= 20) {
        row.window.resize(width, 90); QTRY_COMPARE(row.toolbar->width(), width); QCoreApplication::processEvents(); row.toolbar->fit();
        QVERIFY2(tasks->isVisible(), qPrintable(QString::number(width)));
        if (tasks->isCompact()) QVERIFY2(row.attachments->isCompact(), qPrintable(QString::number(width)));
        if (row.cache->isCompact()) QVERIFY2(tasks->isCompact(), qPrintable(QString::number(width)));
        compacted = compacted || tasks->isCompact();
    }
    QVERIFY(compacted);
    QCOMPARE(tasks->accessibleName(), QString("Tasks 12"));
}

void TestComposerToolbar::accentChipIsAGreenPill()
{
    // Mark as read keeps its green pill inside the toolbar.
    Row row; row.read->setTone(ChipTone::Accent);
    for (const auto &[dark, text, fill, border] : {std::tuple{true, "#8bdfc0", "#233a35", "#456e61"}, std::tuple{false, "#167357", "#e7f3ed", "#a5c8b8"}}) {
        row.toolbar->setTheme(dark); const auto style = row.read->styleSheet();
        QVERIFY2(style.contains(QString("color:%1;").arg(text)) && style.contains(QString("background:%1;").arg(fill))
            && style.contains(QString("border:1px solid %1;").arg(border)) && style.contains("border-radius:11px;"), qPrintable(style));
    }
}

QTEST_MAIN(TestComposerToolbar)
#include "test_composertoolbar.moc"
