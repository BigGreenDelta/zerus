#include <QtTest>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include "CachedText.h"
#include "DashboardPage.h"
#include "MachineAppearance.h"
#include "SessionCardDelegate.h"

// Paint-path caches must be invisible: the same pixels, strings and settings.
class TestPaintCache : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    static QList<QFont> fonts() {
        QList<QFont> result{QApplication::font()};
        for (int size : {10, 11, 12, 14}) for (auto weight : {QFont::Normal, QFont::Medium, QFont::DemiBold}) {
            QFont font = QApplication::font(); font.setPixelSize(size); font.setWeight(weight); result << font;
        }
        return result;
    }
    static QStringList texts() {
        return {"Codex", "Response ready to read", "gpt-6-astra-preview", "99+", "!", "Сессия готова к работе", "a", "W",
            "  leading", "trailing ", "tab\tinside", "line\nbreak", "amp & ersand", QString(), "feature-branch-with-a-long-name-17 / main"};
    }
    static FleetState fleet(int perHost) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        FleetState result;
        for (const QString host : {"arch", "mac", "studio"}) {
            BoxState box; box.host = host; box.ok = true;
            for (int i = 0; i < perHost; ++i) {
                SessionInfo s; s.cmd = i % 2 ? "claude" : "codex"; s.project = "project"; s.tag = host + "-" + QString::number(i);
                s.name = s.cmd + "/" + s.project + "/" + s.tag; s.model = "gpt-6-astra"; s.lastEventAt = now / 1000. - i;
                s.activity = "idle"; s.phase = "input"; s.activityDetail = "Choose the deployment target";
                box.sessions.append(s);
            }
            if (host == "arch") result.setLocal(box, now); else result.setPeer(box, now);
        }
        return result;
    }
private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir.path());
        QCoreApplication::setOrganizationName("hgs-paint-cache-test"); QCoreApplication::setApplicationName("appearance");
    }
    void machineAppearanceFollowsItsWriters()
    {
        QSettings().clear(); MachineAppearance::invalidate();
        const QColor automatic = MachineAppearance::color("arch");
        // Painting reads the cache; only the appearance writers change it.
        QSettings().setValue("machines/colors/arch", "#123456");
        QCOMPARE(MachineAppearance::color("arch"), automatic);
        MachineAppearance::setColor("arch", QColor(255, 136, 0, 90));
        QCOMPARE(MachineAppearance::color("arch"), QColor("#ff8800"));
        QCOMPARE(QSettings().value("machines/colors/arch").toString(), QString("#ff8800"));
        MachineAppearance::setColor("arch", QColor());
        QCOMPARE(MachineAppearance::color("arch"), QColor("#ff8800"));
        MachineAppearance::resetColor("arch");
        QCOMPARE(MachineAppearance::color("arch"), automatic); QVERIFY(!QSettings().contains("machines/colors/arch"));
        QVERIFY(!MachineAppearance::vivid("arch"));
        MachineAppearance::setVivid("arch", true);
        QVERIFY(MachineAppearance::vivid("arch")); QVERIFY(QSettings().value("machines/vivid/arch").toBool());
        QSettings().setValue("machines/vivid/arch", false);
        QVERIFY(MachineAppearance::vivid("arch"));
        MachineAppearance::invalidate();
        QVERIFY(!MachineAppearance::vivid("arch"));
        QSettings().setValue("machines/colors/arch", "#123456"); MachineAppearance::invalidate();
        QCOMPARE(MachineAppearance::color("arch"), QColor("#123456"));
        // Another settings scope never sees this one's cached values.
        QCoreApplication::setApplicationName("appearance-other");
        QCOMPARE(MachineAppearance::color("arch"), automatic);
        QCoreApplication::setApplicationName("appearance");
        QCOMPARE(MachineAppearance::color("arch"), QColor("#123456"));
        QSettings().clear(); MachineAppearance::invalidate();
    }
    void textMetricsMatchQFontMetrics()
    {
        const QStringList samples = texts() << QString("x").repeated(90);
        for (const auto &font : fonts()) for (const QString &text : samples) {
            QCOMPARE(CachedText::width(font, text), QFontMetrics(font).horizontalAdvance(text));
            QCOMPARE(CachedText::advance(font, text), QFontMetricsF(font).horizontalAdvance(text));
            for (auto mode : {Qt::ElideRight, Qt::ElideMiddle, Qt::ElideLeft, Qt::ElideNone})
                for (int width = -1; width < 240; width += 3)
                    QCOMPARE(CachedText::elided(font, text, mode, width), QFontMetrics(font).elidedText(text, mode, width));
        }
    }
    void textDrawsLikeQPainter_data()
    {
        QTest::addColumn<qreal>("dpr"); QTest::addColumn<bool>("opaque");
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) for (bool opaque : {false, true})
            QTest::addRow("dpr %.2f %s", dpr, opaque ? "opaque" : "transparent") << dpr << opaque;
    }
    void textDrawsLikeQPainter()
    {
        QFETCH(qreal, dpr); QFETCH(bool, opaque);
        const QList<int> flags{Qt::AlignVCenter, Qt::AlignCenter, Qt::AlignVCenter | Qt::AlignLeft, Qt::AlignTop | Qt::AlignLeft,
            Qt::AlignBottom | Qt::AlignHCenter, Qt::AlignRight | Qt::AlignVCenter, Qt::AlignVCenter | Qt::TextSingleLine, Qt::AlignLeft};
        const QList<QRectF> rects{QRectF(3, 2, 150, 20), QRectF(10.5, 4.25, 40, 13), QRectF(2, 1, 0, 18), QRectF(1, 0.5, 22, 24.5)};
        const auto render = [&](bool cached, const QFont &font, const QString &text, int flag, const QRectF &rect, int variant) {
            QImage image(QSize(170, 30) * dpr, opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr); image.fill(opaque ? QColor("#1d252c") : QColor(Qt::transparent));
            QPainter p(&image); p.setRenderHint(QPainter::Antialiasing, variant & 1); p.setFont(font);
            p.setPen(QColor(variant & 2 ? "#e8edf4" : "#1a2733"));
            if (variant & 4) { p.translate(0.5, 0.75); p.setOpacity(0.7); }
            if (cached) CachedText::draw(&p, rect, flag, text); else p.drawText(rect, flag, text);
            return image;
        };
        int compared = 0;
        for (const auto &font : fonts()) for (const auto &text : texts()) for (int flag : flags) for (const auto &rect : rects)
            for (int variant = 0; variant < 8; variant += 3) {
                const QImage expected = render(false, font, text, flag, rect, variant);
                for (int pass = 0; pass < 2; ++pass) {
                    if (render(true, font, text, flag, rect, variant) != expected)
                        QFAIL(qPrintable(QString("%1 px, flags %2, rect %3,%4 %5x%6, variant %7, pass %8: \"%9\"").arg(font.pixelSize()).arg(flag)
                            .arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()).arg(variant).arg(pass).arg(text)));
                    ++compared;
                }
            }
        QVERIFY(compared > 1000);
    }
    void repeatedTextReusesShaping()
    {
        QFont font = QApplication::font(); font.setPixelSize(13);
        const QString text = "unique shaping probe " + QString::number(QRandomGenerator::global()->generate());
        QImage image(200, 30, QImage::Format_ARGB32_Premultiplied); QPainter p(&image); p.setFont(font);
        const auto run = [&] {
            CachedText::draw(&p, QRectF(0, 0, 200, 30), Qt::AlignVCenter, text); CachedText::width(font, text);
            CachedText::elided(font, text, Qt::ElideRight, 60); CachedText::elided(font, text, Qt::ElideRight, 1000);
        };
        const auto before = CachedText::misses(); run();
        const auto first = CachedText::misses(); QVERIFY(first > before);
        run(); QCOMPARE(CachedText::misses(), first);
    }
    void unchangedCardRepaintsWithoutShaping()
    {
        QStandardItemModel model; auto *item = new QStandardItem; model.appendRow(item);
        item->setData("codex/hgs/dashboard", SessionRoles::Key); item->setData("dashboard", SessionRoles::Title);
        item->setData("hgs / main", SessionRoles::Meta); item->setData("git-branch", SessionRoles::MetaIcon); item->setData("Waiting", SessionRoles::Status);
        item->setData("Choose the deployment target", SessionRoles::Detail); item->setData("codex", SessionRoles::Agent);
        item->setData("arch", SessionRoles::Host); item->setData("arch", SessionRoles::MachineName);
        item->setData(MachineAppearance::color("arch"), SessionRoles::MachineColor); item->setData(true, SessionRoles::Attention);
        item->setData("gpt-6-astra", SessionRoles::Model); item->setData("high", SessionRoles::Effort); item->setData("2", SessionRoles::Children);
        QWidget owner; owner.setProperty("hgsDark", true);
        SessionDelegate delegate;
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 360, 104); option.font = QApplication::font();
        option.widget = &owner; option.state = QStyle::State_Enabled;
        const auto paint = [&] {
            QImage image(QSize(360, 104) * 2, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(2); image.fill(Qt::transparent);
            QPainter p(&image); delegate.paint(&p, option, model.index(0, 0)); return image;
        };
        const QImage first = paint(); const auto misses = CachedText::misses();
        QCOMPARE(paint(), first);
        QCOMPARE(CachedText::misses(), misses);
    }
    void overviewScrollsWithoutRepaintingVisibleCards()
    {
        DashboardPage page; page.setTheme(true); page.resize(1200, 700); page.setFleet(fleet(6)); page.show();
        QVERIFY(QTest::qWaitForWindowExposed(&page)); QCoreApplication::processEvents();
        auto *scroll = page.findChild<QScrollArea *>("dashboardScroll"); QVERIFY(scroll);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 40);
        struct Counter : QObject {
            QHash<QObject *, int> paints;
            bool eventFilter(QObject *o, QEvent *e) override { if (e->type() == QEvent::Paint) ++paints[o]; return false; }
        } counter;
        const auto rows = page.findChildren<QPushButton *>("dashboardSessionRow"); QVERIFY(!rows.isEmpty());
        const auto visible = [&](QWidget *row) { return QRect(row->mapTo(scroll->viewport(), QPoint()), row->size()); };
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->value() + visible(rows.first()).y() - 20);
        QCoreApplication::processEvents(); QCoreApplication::processEvents();
        QVERIFY(scroll->verticalScrollBar()->value() + 8 <= scroll->verticalScrollBar()->maximum());
        for (auto *row : rows) row->installEventFilter(&counter);
        QSet<QObject *> stable;
        const QRect viewport = scroll->viewport()->rect().adjusted(0, 8, 0, -8);
        for (auto *row : rows) if (viewport.contains(visible(row))) stable.insert(row);
        QVERIFY(!stable.isEmpty());
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->value() + 8);
        QCoreApplication::processEvents(); QCoreApplication::processEvents();
        for (auto *row : stable) QCOMPARE(counter.paints.value(row), 0);
    }
    void overviewBackgroundFollowsTheme()
    {
        DashboardPage page; page.resize(900, 600); page.setFleet(fleet(1));
        for (bool dark : {true, false}) {
            page.setTheme(dark); page.show(); QVERIFY(QTest::qWaitForWindowExposed(&page));
            auto *content = page.findChild<QWidget *>("dashboardContent"); QVERIFY(content);
            const QImage image = content->grab().toImage();
            QCOMPARE(QColor(image.pixel(image.width() - 8, image.height() - 8)), QColor(dark ? "#161c21" : "#f3f5f7"));
        }
    }
};

QTEST_MAIN(TestPaintCache)
#include "test_paintcache.moc"
