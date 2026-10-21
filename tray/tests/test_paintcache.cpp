#include <QtTest>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QPaintEvent>
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
    void animatedClipsMatchFullPaints()
    {
        QStandardItemModel model; auto *item = new QStandardItem; model.appendRow(item);
        item->setData("A sufficiently long session title", SessionRoles::Title);
        item->setData("feature/example", SessionRoles::Meta); item->setData("git-branch", SessionRoles::MetaIcon);
        item->setData("codex", SessionRoles::Agent); item->setData("example", SessionRoles::Host);
        item->setData("Rendering a changing pulse", SessionRoles::Detail); item->setData("example-model", SessionRoles::Model);
        item->setData("Working", SessionRoles::Status); item->setData(12, SessionRoles::Working);
        item->setData(25, SessionRoles::Total); item->setData(QColor("#60ae91"), SessionRoles::ProjectColor);
        QWidget owner; SessionDelegate delegate;
        QStyleOptionViewItem option; option.font = QApplication::font(); option.widget = &owner;
        const auto index = model.index(0, 0);
        for (qreal dpr : {1.0, 1.25, 1.5, 2.0}) for (bool dark : {false, true}) for (bool compact : {false, true})
            for (qreal expansion : {0.0, .15, .5, 1.0}) for (int kind : {0, 1, 2, 3}) for (bool selected : {false, true}) {
                owner.setProperty("hgsDark", dark); owner.setProperty("compact", compact); owner.setProperty("expansion", expansion);
                owner.setProperty("expandedRowWidth", 360);
                item->setData(kind >= 2, SessionRoles::Header); item->setData(kind == 1 ? "child" : "", SessionRoles::ChildId);
                item->setData(kind == 3 ? 2 : 0, SessionRoles::Attention); item->setData(kind == 3 ? 3 : 0, SessionRoles::Unread);
                option.state = QStyle::State_Enabled | (selected ? QStyle::State_Selected : QStyle::State_MouseOver);
                option.rect = QRect(0, 0, expansion == 1 ? 360 : expansion == 0 ? 98 : 180, kind >= 2 ? 40 : kind == 1 ? 64 : compact ? 94 : 104);
                const QColor background(dark ? "#161c21" : "#f3f5f7");
                const auto render = [&](qreal pulse, QImage previous = {}, QRegion clip = {}) {
                    owner.setProperty("workingPulse", pulse);
                    QImage image = previous.isNull() ? QImage(QSize(360, 104) * dpr, QImage::Format_ARGB32_Premultiplied) : previous;
                    image.setDevicePixelRatio(dpr);
                    if (previous.isNull()) image.fill(background);
                    QPainter p(&image); p.setFont(option.font);
                    if (!clip.isEmpty()) { p.setClipRegion(clip); p.fillRect(option.rect, background); }
                    delegate.paint(&p, option, index); return image;
                };
                auto previous = render(0);
                const QRegion region = SessionDelegate::animationRegion(option, index);
                for (qreal pulse : {.25, 1.0, .75, 0.0}) {
                    const QImage expected = render(pulse);
                    const QImage actual = region.isEmpty() ? previous : render(pulse, previous, region);
                    if (actual != expected)
                        QFAIL(qPrintable(QString("DPR %1 dark %2 compact %3 expansion %4 kind %5 selected %6 pulse %7")
                            .arg(dpr).arg(dark).arg(compact).arg(expansion).arg(kind).arg(selected).arg(pulse)));
                    previous = actual;
                }
            }
    }
    void indicatorClipsSkipStaticCardWork()
    {
        struct Model : QStandardItemModel {
            mutable int staticReads = 0;
            QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override {
                if (role == SessionRoles::Meta || role == SessionRoles::Detail || role == SessionRoles::Model || role == SessionRoles::GitStatus) ++staticReads;
                return QStandardItemModel::data(index, role);
            }
        } model;
        auto *item = new QStandardItem; model.appendRow(item);
        item->setData("Probe", SessionRoles::Title); item->setData("Working", SessionRoles::Status); item->setData(1, SessionRoles::Working);
        QWidget owner; SessionDelegate delegate; QStyleOptionViewItem option;
        option.font = QApplication::font(); option.widget = &owner; option.state = QStyle::State_Enabled;
        for (bool compact : {false, true}) for (qreal expansion : {0.0, 1.0}) {
            owner.setProperty("compact", compact); owner.setProperty("expansion", expansion); owner.setProperty("expandedRowWidth", 360);
            option.rect = QRect(0, 0, expansion == 1 ? 360 : 98, compact ? 94 : 104);
            QImage image(360, 104, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white); QPainter p(&image); p.setFont(option.font);
            p.setClipRegion(SessionDelegate::animationRegion(option, model.index(0, 0)));
            model.staticReads = 0; delegate.paint(&p, option, model.index(0, 0)); QCOMPARE(model.staticReads, 0);
            p.setClipping(false); delegate.paint(&p, option, model.index(0, 0)); QVERIFY(model.staticReads > 0);
        }
    }
    void elapsedClipsRestoreMovingBadges()
    {
        QStandardItemModel model; auto *item = new QStandardItem; model.appendRow(item);
        item->setData("A long title that elides as elapsed text grows", SessionRoles::Title); item->setData("codex", SessionRoles::Agent);
        item->setData("Working", SessionRoles::Status); item->setData(1, SessionRoles::Working); item->setData(true, SessionRoles::Unread);
        QWidget owner; SessionDelegate delegate; QStyleOptionViewItem option;
        option.font = QApplication::font(); option.widget = &owner; option.state = QStyle::State_Selected | QStyle::State_Enabled;
        owner.setProperty("expandedRowWidth", 360);
        for (qreal dpr : {1.0, 1.25, 2.0}) for (bool compact : {false, true}) for (qreal expansion : {0.0, .15, .5, 1.0}) {
            owner.setProperty("compact", compact); owner.setProperty("expansion", expansion); owner.setProperty("workingPulse", .8);
            option.rect = QRect(0, 0, expansion == 1 ? 360 : expansion == 0 ? 98 : 180, compact ? 94 : 104);
            QImage previous;
            for (int seconds : {9, 10, 59, 60, 3599, 3600, 86400}) {
                item->setData(double(QDateTime::currentSecsSinceEpoch() - seconds), SessionRoles::WorkingSince);
                const auto render = [&](QImage image, bool clipped) {
                    if (image.isNull()) { image = QImage(QSize(360, 104) * dpr, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(dpr); image.fill(Qt::white); }
                    QPainter p(&image); p.setFont(option.font);
                    if (clipped) { p.setClipRegion(SessionDelegate::elapsedRegion(option, model.index(0, 0))); p.fillRect(option.rect, Qt::white); }
                    delegate.paint(&p, option, model.index(0, 0)); return image;
                };
                QImage expected, actual;
                qint64 sampled;
                do {
                    sampled = QDateTime::currentSecsSinceEpoch();
                    expected = render({}, false);
                    if (!previous.isNull()) actual = render(previous, true);
                } while (sampled != QDateTime::currentSecsSinceEpoch());
                if (!previous.isNull()) {
                    if (actual != expected) {
                        QRect differences;
                        for (int y = 0; y < actual.height(); ++y) for (int x = 0; x < actual.width(); ++x)
                            if (actual.pixel(x, y) != expected.pixel(x, y)) differences |= QRect(x, y, 1, 1);
                        QFAIL(qPrintable(QString("elapsed DPR %1 compact %2 expansion %3 seconds %4 difference %5,%6 %7x%8").arg(dpr).arg(compact).arg(expansion).arg(seconds)
                            .arg(differences.x()).arg(differences.y()).arg(differences.width()).arg(differences.height())));
                    }
                }
                previous = expected;
            }
        }
    }
    void workingListPaintsOnlyIndicators()
    {
        SessionList list; list.resize(360, 800); list.setItemDelegate(new SessionDelegate(&list));
        for (int i = 0; i < 1000; ++i) {
            auto *item = new QListWidgetItem(&list); item->setData(SessionRoles::Title, QString("session %1").arg(i));
            item->setData(SessionRoles::Working, 1); item->setData(SessionRoles::Status, "Working");
        }
        struct Counter : QObject {
            qint64 area = 0; int paints = 0;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::Paint) {
                    ++paints; for (const auto &rect : static_cast<QPaintEvent *>(event)->region()) area += qint64(rect.width()) * rect.height();
                }
                return false;
            }
        } counter;
        list.show(); QVERIFY(QTest::qWaitForWindowExposed(&list)); QTest::qWait(100);
        // Wait for the frames themselves: a loaded runner can deliver fewer of
        // the 80 ms activity frames within any fixed interval.
        list.viewport()->installEventFilter(&counter);
        QTRY_VERIFY(counter.paints >= 2); QVERIFY(counter.area < qint64(counter.paints) * 8 * 200);
        list.hide(); QVERIFY(!list.activityAnimationRunning());
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
