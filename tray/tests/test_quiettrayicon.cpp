#include <QtTest>

#include <QIcon>
#include <QMenu>
#include <QPixmap>

#include <memory>

#include "QuietTrayIcon.h"

// Декоратор над TrayIcon для мака: не дёргать NSStatusItem без нужды (см. QuietTrayIcon.h,
// там же -- зачем: дедлок AppKit на macOS 26). Проверяется РЕЗУЛЬТАТ на подставной
// внутренней иконке: что и сколько раз до неё долетело. Ни системного трея, ни живого
// меню -- QMenu здесь только источник сигналов aboutToShow/aboutToHide, которые тест
// испускает сам.

namespace {

class RecordingTrayIcon : public TrayIcon {
public:
    QList<qint64> iconKeys;
    QStringList toolTips;
    QMenu *menu = nullptr;
    int shows = 0;
    int messages = 0;
    QList<qint64> pictures;

    void setIcon(const QIcon &icon) override { iconKeys << icon.cacheKey(); }
    void setToolTip(const QString &text) override { toolTips << text; }
    void setContextMenu(QMenu *m) override { menu = m; }
    void showMessage(const QString &, const QString &, QSystemTrayIcon::MessageIcon,
                     int) override
    {
        ++messages;
    }
    void showClickableIconMessage(const QString &, const QString &, const QIcon &icon, int,
                                  std::function<void()>) override
    {
        pictures << icon.cacheKey();
    }
    void show() override { ++shows; }
    void primaryClick() { activate(); }
};

// Каждый вызов -- новый QIcon с новым cacheKey, как и у IconFactory до мемоизации.
QIcon makeIcon()
{
    QPixmap pm(4, 4);
    pm.fill(Qt::red);
    return QIcon(pm);
}

struct Rig {
    RecordingTrayIcon *inner;
    QuietTrayIcon quiet;
    Rig()
        : inner(nullptr)
        , quiet([this]() {
            auto owned = std::make_unique<RecordingTrayIcon>();
            inner = owned.get();
            return owned;
        }())
    {
    }
};

} // namespace

class TestQuietTrayIcon : public QObject {
    Q_OBJECT
private slots:
    void forwardsFirstIconAndToolTip();
    void skipsRepeatOfTheSameIcon();
    void passesADifferentIcon();
    void skipsRepeatOfTheSameToolTip();
    void defersIconWhileMenuIsOpen();
    void coalescesUpdatesWhileMenuIsOpen();
    void dropsUpdateRevertedWhileMenuIsOpen();
    void defersToolTipWhileMenuIsOpen();
    void showAndMessageAreNeverDeferred();
    void followsTheMenuGivenLast();
    void primaryClickIsForwarded();
};

void TestQuietTrayIcon::forwardsFirstIconAndToolTip()
{
    Rig r;
    const QIcon a = makeIcon();
    r.quiet.setIcon(a);
    r.quiet.setToolTip(QStringLiteral("3 here"));
    QCOMPARE(r.inner->iconKeys, QList<qint64>{a.cacheKey()});
    QCOMPARE(r.inner->toolTips, QStringList{QStringLiteral("3 here")});
}

void TestQuietTrayIcon::skipsRepeatOfTheSameIcon()
{
    // Тот же QIcon (или его копия -- cacheKey у копии тот же) -- нечего обновлять.
    Rig r;
    const QIcon a = makeIcon();
    r.quiet.setIcon(a);
    r.quiet.setIcon(a);
    const QIcon copy = a;
    r.quiet.setIcon(copy);
    QCOMPARE(r.inner->iconKeys.size(), 1);
}

void TestQuietTrayIcon::passesADifferentIcon()
{
    Rig r;
    const QIcon a = makeIcon(), b = makeIcon();
    r.quiet.setIcon(a);
    r.quiet.setIcon(b);
    QCOMPARE(r.inner->iconKeys, (QList<qint64>{a.cacheKey(), b.cacheKey()}));
}

void TestQuietTrayIcon::skipsRepeatOfTheSameToolTip()
{
    Rig r;
    r.quiet.setToolTip(QStringLiteral("t"));
    r.quiet.setToolTip(QStringLiteral("t"));
    r.quiet.setToolTip(QStringLiteral("u"));
    QCOMPARE(r.inner->toolTips, (QStringList{QStringLiteral("t"), QStringLiteral("u")}));
}

void TestQuietTrayIcon::defersIconWhileMenuIsOpen()
{
    Rig r;
    QMenu menu;
    r.quiet.setContextMenu(&menu);
    QCOMPARE(r.inner->menu, &menu);

    const QIcon a = makeIcon(), b = makeIcon();
    r.quiet.setIcon(a);
    emit menu.aboutToShow();
    r.quiet.setIcon(b);
    QCOMPARE(r.inner->iconKeys.size(), 1);   // меню открыто -- иконку не трогаем
    emit menu.aboutToHide();
    QCOMPARE(r.inner->iconKeys, (QList<qint64>{a.cacheKey(), b.cacheKey()}));
}

void TestQuietTrayIcon::coalescesUpdatesWhileMenuIsOpen()
{
    // За время открытого меню состояние могло смениться несколько раз; после закрытия
    // показывается ПОСЛЕДНЕЕ, одним обновлением.
    Rig r;
    QMenu menu;
    r.quiet.setContextMenu(&menu);
    const QIcon a = makeIcon(), b = makeIcon(), c = makeIcon();
    r.quiet.setIcon(a);
    emit menu.aboutToShow();
    r.quiet.setIcon(b);
    r.quiet.setIcon(c);
    emit menu.aboutToHide();
    QCOMPARE(r.inner->iconKeys, (QList<qint64>{a.cacheKey(), c.cacheKey()}));
}

void TestQuietTrayIcon::dropsUpdateRevertedWhileMenuIsOpen()
{
    // Ушло и вернулось, пока меню было открыто, -- показанное и так верное.
    Rig r;
    QMenu menu;
    r.quiet.setContextMenu(&menu);
    const QIcon a = makeIcon(), b = makeIcon();
    r.quiet.setIcon(a);
    emit menu.aboutToShow();
    r.quiet.setIcon(b);
    r.quiet.setIcon(a);
    emit menu.aboutToHide();
    QCOMPARE(r.inner->iconKeys.size(), 1);
}

void TestQuietTrayIcon::defersToolTipWhileMenuIsOpen()
{
    Rig r;
    QMenu menu;
    r.quiet.setContextMenu(&menu);
    r.quiet.setToolTip(QStringLiteral("t"));
    emit menu.aboutToShow();
    r.quiet.setToolTip(QStringLiteral("u"));
    QCOMPARE(r.inner->toolTips.size(), 1);
    emit menu.aboutToHide();
    QCOMPARE(r.inner->toolTips, (QStringList{QStringLiteral("t"), QStringLiteral("u")}));
}

void TestQuietTrayIcon::showAndMessageAreNeverDeferred()
{
    // show() -- один раз при старте, балун -- ответ на действие пользователя; ни то ни
    // другое не трогает картинку статус-айтема, и задерживать их незачем.
    Rig r;
    QMenu menu;
    r.quiet.setContextMenu(&menu);
    emit menu.aboutToShow();
    r.quiet.show();
    r.quiet.showMessage(QStringLiteral("t"), QStringLiteral("b"), QSystemTrayIcon::Warning, 1);
    QCOMPARE(r.inner->shows, 1);
    QCOMPARE(r.inner->messages, 1);
    // A message with a picture reaches the inner icon with that picture.
    const QIcon picture = makeIcon();
    r.quiet.showClickableIconMessage(QStringLiteral("t"), QStringLiteral("b"), picture, 1, {});
    QCOMPARE(r.inner->pictures, QList<qint64>{picture.cacheKey()});
    QCOMPARE(r.inner->messages, 1);
}

void TestQuietTrayIcon::followsTheMenuGivenLast()
{
    // Меню подменили -- слушаем новое, старое больше ничего не гейтит.
    Rig r;
    QMenu first, second;
    r.quiet.setContextMenu(&first);
    r.quiet.setContextMenu(&second);
    QCOMPARE(r.inner->menu, &second);

    const QIcon a = makeIcon(), b = makeIcon(), c = makeIcon();
    r.quiet.setIcon(a);
    emit first.aboutToShow();
    r.quiet.setIcon(b);
    QCOMPARE(r.inner->iconKeys.size(), 2);   // first не в счёт
    emit second.aboutToShow();
    r.quiet.setIcon(c);
    QCOMPARE(r.inner->iconKeys.size(), 2);   // second гейтит
    emit second.aboutToHide();
    QCOMPARE(r.inner->iconKeys.last(), c.cacheKey());
}

void TestQuietTrayIcon::primaryClickIsForwarded()
{
    Rig r; QMenu menu; r.quiet.setContextMenu(&menu);
    int activations = 0; r.quiet.setActivationHandler([&]() { ++activations; });
    r.inner->primaryClick(); QCOMPARE(activations, 1);
    QMetaObject::invokeMethod(&menu, "aboutToShow");
    r.inner->primaryClick(); QCOMPARE(activations, 2);
    r.quiet.setActivationHandler({}); r.inner->primaryClick(); QCOMPARE(activations, 2);
}

QTEST_MAIN(TestQuietTrayIcon)
#include "test_quiettrayicon.moc"
