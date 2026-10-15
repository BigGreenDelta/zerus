#include "GoToDialog.h"
#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QtTest>

namespace {
GoToItem item(GoToItem::Kind kind, const QString &id, const QString &title, const QString &detail = {},
              int recency = -1)
{
    GoToItem it;
    it.kind = kind;
    it.id = id;
    it.title = title;
    it.detail = detail;
    it.recency = recency;
    return it;
}

QStringList ids(const QList<GoToItem> &items)
{
    QStringList out;
    for (const GoToItem &it : items) out << it.id;
    return out;
}

GoToDialog *makeDialog(QWidget &parent, const QList<GoToItem> &items)
{
    parent.resize(800, 600);
    parent.show();
    return new GoToDialog([items] { return items; }, &parent);
}
}

class TestGoToDialog : public QObject {
    Q_OBJECT
private slots:
    void scoreRanksTitleStartFirst();
    void scoreMatchesSubsequences();
    void emptyQueryScoresZero();
    void scorePrefersWordStarts();
    void commandFilter();
    void emptyQueryUsesRecency();
    void tiesFollowKindThenInputOrder();
    void enterEmitsHighlightedItem();
    void arrowKeysMoveHighlight();
    void escapeHidesWithoutEmitting();
    void closeKeysHideThePopup();
    void clickActivates();
};

void TestGoToDialog::scoreRanksTitleStartFirst()
{
    QVERIFY(GoTo::score("ses", "Sessions") > GoTo::score("ses", "Close session"));
}

void TestGoToDialog::scoreMatchesSubsequences()
{
    QVERIFY(GoTo::score("zrs", "Zerus") >= 0);
    QVERIFY(GoTo::score("ZRS", "zerus") >= 0);
    QVERIFY(GoTo::score("xyz", "Zerus") < 0);
    QVERIFY(GoTo::score("zz", "Zerus") < 0);
}

void TestGoToDialog::scorePrefersWordStarts()
{
    // "ns" must align to the word starts of "New session", not the "s" inside "New".
    QVERIFY(GoTo::score("ns", "New session") > GoTo::score("ns", "Newsletter session x"));
    QCOMPARE(GoTo::score("ns", "New session"), 2 * 10 + 2 * 20 + 100 - 3);
}

void TestGoToDialog::emptyQueryScoresZero()
{
    QCOMPARE(GoTo::score(QString(), "anything"), 0);
}

void TestGoToDialog::commandFilter()
{
    // Sections are commands too.
    const QList<GoToItem> items{item(GoToItem::Session, "s", "Refresh logs"),
                                item(GoToItem::Project, "p", "Settings rework"),
                                item(GoToItem::Command, "view.refresh", "Refresh"),
                                item(GoToItem::Section, "go.settings", "Settings"),
                                item(GoToItem::Command, "session.new", "New session")};
    QCOMPARE(ids(GoTo::rank(items, ">ref")), QStringList{"view.refresh"});
    QCOMPARE(ids(GoTo::rank(items, ">")), (QStringList{"view.refresh", "go.settings", "session.new"}));
    QCOMPARE(ids(GoTo::rank(items, ">sett")), QStringList{"go.settings"});
    QCOMPARE(ids(GoTo::rank(items, "ref")), (QStringList{"s", "view.refresh"}));
}

void TestGoToDialog::emptyQueryUsesRecency()
{
    const QList<GoToItem> items{item(GoToItem::Project, "p", "Project"),
                                item(GoToItem::Session, "a", "A", {}, 1),
                                item(GoToItem::Session, "b", "B"),
                                item(GoToItem::Session, "c", "C", {}, 0)};
    QCOMPARE(ids(GoTo::rank(items, QString())), (QStringList{"c", "a", "p", "b"}));
}

void TestGoToDialog::tiesFollowKindThenInputOrder()
{
    const QList<GoToItem> items{item(GoToItem::Command, "c", "Same"), item(GoToItem::Project, "p", "Same"),
                                item(GoToItem::Session, "s2", "Same"), item(GoToItem::Session, "s1", "Same")};
    QCOMPARE(ids(GoTo::rank(items, "same")), (QStringList{"s2", "s1", "p", "c"}));
}

void TestGoToDialog::enterEmitsHighlightedItem()
{
    QWidget parent;
    auto *d = makeDialog(parent, {item(GoToItem::Session, "s1", "Alpha"), item(GoToItem::Project, "p1", "Beta")});
    qRegisterMetaType<GoToItem>();
    QSignalSpy spy(d, &GoToDialog::activated);
    d->popup();
    auto *query = d->findChild<QLineEdit *>("goToQuery");
    auto *results = d->findChild<QListWidget *>("goToResults");
    QVERIFY(query && results);
    QCOMPARE(results->count(), 2);
    QCOMPARE(results->currentRow(), 0);
    QTest::keyClicks(query, "bet");
    QCOMPARE(results->count(), 1);
    QTest::keyClick(query, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<GoToItem>().id, QString("p1"));
    QVERIFY(!d->isVisible());
}

void TestGoToDialog::arrowKeysMoveHighlight()
{
    QWidget parent;
    auto *d = makeDialog(parent, {item(GoToItem::Session, "a", "A"), item(GoToItem::Session, "b", "B"),
                                  item(GoToItem::Session, "c", "C")});
    d->popup();
    auto *query = d->findChild<QLineEdit *>("goToQuery");
    auto *results = d->findChild<QListWidget *>("goToResults");
    QTest::keyClick(query, Qt::Key_Down);
    QCOMPARE(results->currentRow(), 1);
    QTest::keyClick(query, Qt::Key_Down);
    QTest::keyClick(query, Qt::Key_Down);
    QCOMPARE(results->currentRow(), 2);
    QTest::keyClick(query, Qt::Key_Up);
    QCOMPARE(results->currentRow(), 1);
    QTest::keyClick(query, Qt::Key_PageUp);
    QCOMPARE(results->currentRow(), 0);
    QTest::keyClick(query, Qt::Key_PageDown);
    QCOMPARE(results->currentRow(), 2);
}

void TestGoToDialog::escapeHidesWithoutEmitting()
{
    QWidget parent;
    auto *d = makeDialog(parent, {item(GoToItem::Session, "a", "A")});
    qRegisterMetaType<GoToItem>();
    QSignalSpy spy(d, &GoToDialog::activated);
    d->popup();
    QVERIFY(d->isVisible());
    QTest::keyClick(d->findChild<QLineEdit *>("goToQuery"), Qt::Key_Escape);
    QVERIFY(!d->isVisible());
    QCOMPARE(spy.count(), 0);
}

void TestGoToDialog::closeKeysHideThePopup()
{
    QWidget parent;
    auto *d = makeDialog(parent, {item(GoToItem::Session, "a", "A")});
    qRegisterMetaType<GoToItem>();
    QSignalSpy spy(d, &GoToDialog::activated);
    auto *query = d->findChild<QLineEdit *>("goToQuery");
    // Without close keys, Ctrl+K is no command of the popup.
    d->popup();
    QTest::keyClick(query, Qt::Key_K, Qt::ControlModifier);
    QVERIFY(d->isVisible());
    // The chords of Go to close it, a two-key sequence on its second key.
    d->setCloseKeys({QKeySequence::fromString("Ctrl+J", QKeySequence::PortableText),
                     QKeySequence::fromString("Ctrl+G, Ctrl+K", QKeySequence::PortableText)});
    d->popup();
    QTest::keyClick(query, Qt::Key_K, Qt::ControlModifier);
    QVERIFY(d->isVisible());
    QTest::keyClick(query, Qt::Key_J, Qt::ControlModifier);
    QVERIFY(!d->isVisible());
    d->popup();
    QTest::keyClick(query, Qt::Key_G, Qt::ControlModifier);
    QVERIFY(d->isVisible());
    QTest::keyClick(query, Qt::Key_K, Qt::ControlModifier);
    QVERIFY(!d->isVisible());
    // A key that breaks the sequence is typed as usual.
    d->popup();
    QTest::keyClick(query, Qt::Key_G, Qt::ControlModifier);
    QTest::keyClick(query, Qt::Key_A);
    QVERIFY(d->isVisible());
    QCOMPARE(query->text(), QString("a"));
    QCOMPARE(spy.count(), 0);
}

void TestGoToDialog::clickActivates()
{
    QWidget parent;
    auto *d = makeDialog(parent, {item(GoToItem::Session, "a", "A"), item(GoToItem::Session, "b", "B")});
    qRegisterMetaType<GoToItem>();
    QSignalSpy spy(d, &GoToDialog::activated);
    d->popup();
    auto *results = d->findChild<QListWidget *>("goToResults");
    emit results->itemClicked(results->item(1));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<GoToItem>().id, QString("b"));
    QVERIFY(!d->isVisible());
}

QTEST_MAIN(TestGoToDialog)
#include "test_gotodialog.moc"
