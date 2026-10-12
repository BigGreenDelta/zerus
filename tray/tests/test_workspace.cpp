#include <QtTest>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QLineEdit>
#include <QVBoxLayout>
#include "SessionOrganization.h"
#include "SessionList.h"
#include "SessionCardDelegate.h"
#include "SessionElapsed.h"
#include "IdentityBadge.h"
#include "GitStatusBadge.h"
#include "SessionPresentation.h"
#include "WorkspaceIcons.h"
#include "MessageBoxIcons.h"
#include <QDir>

class ListProbe : public SessionList { public: using SessionList::mimeData; using SessionList::initViewItemOption; };
class TestWorkspace : public QObject {
    Q_OBJECT
private slots:
    void durableIdentityAndMissingSessions();
    void observedRowsKeepLocalOrderAndRenameWithoutClaimingMembership();
    void orderingAndRemoval();
    void invalidDataAndMoves();
    void nativeDropPlacement();
    void pinningRestoresPreviousGroup();
    void projectsShareFoldersButOwnSessions();
    void folderTransfersPreserveCatalogIdentity_data();
    void folderTransfersPreserveCatalogIdentity();
    void folderTransfersReuseExistingLinks_data();
    void folderTransfersReuseExistingLinks();
    void folderTransfersRejectInvalidTargets();
    void defaultProjectCanBeRenamedAndReplaced();
    void keyboardFocusFollowsInput();
    void activityAnimationOnlyRunsForVisibleWorkingRows();
    void elapsedWorkTimeKeepsUpdatingWithoutAnimation();
    void shortIdentityLabelsFitWithoutElision();
    void workspaceIconsAreReused();
    void darkQuestionBoxesShowLightGlyph();
    void draftStatusSitsBetweenWorkAndReplies();
    void gitStatusSeparatesEvidenceAndExpires();
    void gitStatusCardsKeepMetadataAndMachineReadable();
    void subagentInactivityKeepsConflictingAndUnknownEvidence();
};

void TestWorkspace::subagentInactivityKeepsConflictingAndUnknownEvidence()
{
    using SessionPresentation::inactiveChild;
    for (const auto &state : {"finished", "ready", "idle"}) {
        QVERIFY(inactiveChild({{"state", state}}));
        QVERIFY(inactiveChild({{"state", state}, {"display_state", ""}}));
        QVERIFY(inactiveChild({{"state", state}, {"display_state", QJsonValue::Null}}));
        for (const auto &display : {"working", "input", "approval", "attention", "error", "paused", "unknown", "future-state"})
            QVERIFY(!inactiveChild({{"state", state}, {"display_state", display}}));
    }
    QVERIFY(inactiveChild({{"state", "working"}, {"display_state", "idle"}}));
    QVERIFY(!inactiveChild({})); QVERIFY(!inactiveChild({{"state", "unknown"}}));
    const QJsonObject group{{"group", true}, {"active_count", 0}, {"total_count", 9}};
    QVERIFY(inactiveChild(group, true)); QVERIFY(!inactiveChild(group, false));
    auto uncertain = group; uncertain["active_count"] = "0"; QVERIFY(!inactiveChild(uncertain, true));
    uncertain["active_count"] = -1; QVERIFY(!inactiveChild(uncertain, true));
    uncertain = group; uncertain["display_state"] = "approval"; QVERIFY(!inactiveChild(uncertain, true));
    QCOMPARE(SessionPresentation::childStatus(uncertain), QString("Needs input"));
    uncertain = group; uncertain["state"] = "working"; QVERIFY(!inactiveChild(uncertain, true));
}

static QJsonObject verifiedGit() {
    return {{"state", "ok"}, {"root", "/workspace/zerus"}, {"branch", "main"}, {"upstream", "origin/main"},
        {"changed_files", 0}, {"ahead", 0}, {"behind", 0}, {"remote_state", "verified"}, {"remote_age_seconds", 0},
        {"remote_checked_at", QDateTime::currentSecsSinceEpoch()}, {"received_at_ms", QDateTime::currentMSecsSinceEpoch()}};
}

void TestWorkspace::gitStatusSeparatesEvidenceAndExpires()
{
    auto data = verifiedGit();
    QCOMPARE(GitStatusBadge::marks(data).first().icon, QString("git-synced"));
    data["changed_files"] = 4; data["ahead"] = 2; data["behind"] = 3;
    auto marks = GitStatusBadge::marks(data);
    QCOMPARE(marks.size(), 3); QCOMPARE(marks[0].icon, QString("git-diff"));
    QCOMPARE(marks[1].icon, QString("git-push")); QCOMPARE(marks[2].icon, QString("git-pull"));
    QVERIFY(GitStatusBadge::tooltip(data).contains("4 changed or new files"));
    data["remote_state"] = "changed";
    marks = GitStatusBadge::marks(data); QCOMPARE(marks.size(), 4);
    QCOMPARE(marks[0].tone, GitStatusBadge::Attention); QCOMPARE(marks[1].tone, GitStatusBadge::Muted);
    QVERIFY(GitStatusBadge::tooltip(data).contains("Fetch to update"));
    data = verifiedGit(); data["remote_age_seconds"] = 61;
    QCOMPARE(GitStatusBadge::marks(data).first().icon, QString("git-unknown"));
    data = verifiedGit(); data["received_at_ms"] = QDateTime::currentMSecsSinceEpoch() - 31000;
    QCOMPARE(GitStatusBadge::marks(data).first().icon, QString("git-unknown"));
    data = verifiedGit(); data["offline"] = true;
    QCOMPARE(GitStatusBadge::marks(data).first().icon, QString("git-unknown"));
    data = verifiedGit(); data.remove("changed_files");
    QCOMPARE(GitStatusBadge::marks(data).first().icon, QString("git-unknown"));
    SessionInfo session; session.cwd = "/workspace/zerus"; session.gitRoot = session.cwd; session.gitBranch = "feature/new";
    QVERIFY(!GitStatusBadge::verified(SessionPresentation::gitStatus(session, verifiedGit(), true)));
    session.state = "archived"; QVERIFY(SessionPresentation::gitStatusPath(session).isEmpty());
    data["state"] = "not_repo"; QVERIFY(GitStatusBadge::marks(data).isEmpty());
}

void TestWorkspace::gitStatusCardsKeepMetadataAndMachineReadable()
{
    const QString destination = qEnvironmentVariable("HGS_GIT_STATUS_PREVIEW");
    const QStringList titles{"Local changes", "Waiting for push", "All changes published", "Incoming commits", "Remote unavailable", "Merge conflicts"};
    for (const bool dark : {true, false}) {
        ListProbe list; list.setItemDelegate(new SessionDelegate(&list));
        list.setProperty("hgsDark", dark); list.setProperty("compact", false);
        list.setStyleSheet(dark ? "QListWidget { background:#171c22; border:0; }" : "QListWidget { background:#f3f6f8; border:0; }");
        list.setFrameShape(QFrame::NoFrame); list.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        for (int i = 0; i < titles.size(); ++i) {
            auto *item = new QListWidgetItem(&list);
            item->setData(SessionRoles::Title, titles[i]); item->setData(SessionRoles::Meta, "zerus / main");
            item->setData(SessionRoles::MetaIcon, "git-branch");
            item->setData(SessionRoles::Host, "arch"); item->setData(SessionRoles::MachineName, "arch");
            item->setData(SessionRoles::Agent, "codex"); item->setData(SessionRoles::Status, "Ready");
            item->setData(SessionRoles::Detail, "Git status follows this checkout and branch");
            item->setData(SessionRoles::Model, "gpt-6.1-sol"); item->setData(SessionRoles::Effort, "high");
            auto data = verifiedGit();
            if (i == 0) { data["changed_files"] = 4; data["ahead"] = 2; }
            if (i == 1) data["ahead"] = 2;
            if (i == 3) data["behind"] = 3;
            if (i == 4) { data["remote_state"] = "unavailable"; data["changed_files"] = 4; data["ahead"] = 2; }
            if (i == 5) { data["changed_files"] = 4; data["conflicts"] = 1; data["ahead"] = 2; data["behind"] = 3; }
            item->setData(SessionRoles::GitStatus, data);
        }
        for (const int width : {280, 360, 500}) {
            list.resize(width, titles.size() * 104); list.show(); QTest::qWait(10);
            for (int i = 0; i < list.count(); ++i) {
                const auto index = list.model()->index(i, 0);
                const auto rect = SessionDelegate::gitStatusRect(list.visualItemRect(list.item(i)), index, list.font(), false);
                QVERIFY(!rect.isEmpty()); QVERIFY(rect.left() > 55);
                QVERIFY(rect.right() < width - 50);
            }
            if (!destination.isEmpty()) {
                QDir().mkpath(destination);
                QVERIFY(list.grab().save(destination + QString("/cards-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
            }
        }
    }
}

void TestWorkspace::workspaceIconsAreReused()
{
    // Polls and repaints request the same few icons; each must be painted once.
    const auto first = workspaceIcon("refresh", QColor("#506278"));
    QCOMPARE(workspaceIcon("refresh", QColor("#506278")).cacheKey(), first.cacheKey());
    QVERIFY(workspaceIcon("refresh", QColor("#8ce3c9")).cacheKey() != first.cacheKey());
    QVERIFY(workspaceIcon("add", QColor("#506278")).cacheKey() != first.cacheKey());
    QVERIFY(!first.pixmap(24, 24).isNull());
}

void TestWorkspace::darkQuestionBoxesShowLightGlyph()
{
    // macOS's question glyph is translucent and dark; a dark palette inverts it
    // and makes the body opaque enough to read.
    QImage translucent(1, 1, QImage::Format_ARGB32); translucent.setPixel(0, 0, qRgba(40, 40, 40, 100));
    const QRgb face = darkPaletteQuestionGlyph(QPixmap::fromImage(translucent)).toImage().pixel(0, 0);
    QVERIFY(qAlpha(face) >= 245); QVERIFY(qGray(face) >= 205);

    MessageBoxIcons::install();
    const auto image = [](const QMessageBox &box) { return box.iconPixmap().toImage().convertToFormat(QImage::Format_ARGB32); };
    QMessageBox question(QMessageBox::Question, "Question", "Continue?"), warning(QMessageBox::Warning, "Warning", "Continue?");
    const QImage nativeQuestion = image(question), nativeWarning = image(warning);
    QVERIFY(!nativeQuestion.isNull());
    const QImage lightGlyph = darkPaletteQuestionGlyph(QPixmap::fromImage(nativeQuestion)).toImage().convertToFormat(QImage::Format_ARGB32);
    QPalette dark = question.palette(), light = dark;
    dark.setColor(QPalette::Window, QColor(30, 30, 30)); light.setColor(QPalette::Window, QColor(236, 236, 236));
    question.setPalette(dark); warning.setPalette(dark);
    question.show(); warning.show();
    QCOMPARE(image(question), lightGlyph);
    QCOMPARE(image(warning), nativeWarning);
    // Appearance can change while the box is open.
    question.setPalette(light);
    QTRY_COMPARE(image(question), nativeQuestion);
    QCOMPARE(question.icon(), QMessageBox::Question);
    question.setPalette(dark);
    QTRY_COMPARE(image(question), lightGlyph);
}

void TestWorkspace::shortIdentityLabelsFitWithoutElision()
{
    // Exercise native metrics and fractional advances even on platforms whose
    // default font happens to give these aliases integral widths.
    for (const qreal spacing : {0.0, 0.1, 0.2, 0.3}) {
        QFont base = QApplication::font(); base.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
        for (const QString &name : {QString("mac"), QString("arch"), QString("studio-mac")}) {
            IdentityBadge badge(IdentityBadges::Machine); badge.setFont(base); badge.setValue(name);
            const QFontMetrics metrics(IdentityBadges::font(base, IdentityBadges::Machine));
            QCOMPARE(metrics.elidedText(name, Qt::ElideMiddle, badge.sizeHint().width() - 12), name);
        }
        IdentityBadge longName(IdentityBadges::Machine); longName.setFont(base);
        longName.setValue("a-very-long-machine-name-that-must-still-fit-in-the-layout");
        QVERIFY(longName.sizeHint().width() <= 140);
    }
}

void TestWorkspace::elapsedWorkTimeKeepsUpdatingWithoutAnimation()
{
    QCOMPARE(SessionElapsed::working(100, 1176), QString("Working (17m 56s)"));
    QCOMPARE(SessionElapsed::text(100, 118), QString("18s"));
    QCOMPARE(SessionElapsed::text(100, 3760), QString("1h 01m"));
    QCOMPARE(SessionElapsed::text(100, 176500), QString("2d 1h"));
    QCOMPARE(SessionElapsed::text(100, 99), QString("0s"));
    QCOMPARE(SessionElapsed::working(0, 100), QString("Working"));
    QCOMPARE(SessionElapsed::status("Compacting", 100, 183), QString("Compacting (1m 23s)"));
    SessionList list; list.resize(320, 240); list.setActivityAnimationEnabled(false);
    auto *busy = new QListWidgetItem("Working", &list);
    busy->setData(SessionRoles::Working, true); busy->setData(SessionRoles::WorkingSince, 100.0);
    list.show(); QTRY_VERIFY(list.elapsedTimerRunning()); QVERIFY(!list.activityAnimationRunning());
    busy->setData(SessionRoles::Working, false); list.syncActivityAnimation(); QVERIFY(!list.elapsedTimerRunning());
    busy->setData(SessionRoles::Working, true); list.syncActivityAnimation(); QVERIFY(list.elapsedTimerRunning());
    busy->setHidden(true); list.syncActivityAnimation(); QVERIFY(!list.elapsedTimerRunning());
    busy->setHidden(false); list.syncActivityAnimation(); QVERIFY(list.elapsedTimerRunning());
    list.hide(); QVERIFY(!list.elapsedTimerRunning());
}

void TestWorkspace::activityAnimationOnlyRunsForVisibleWorkingRows()
{
    SessionList list; list.resize(320, 240);
    auto *busy = new QListWidgetItem("Working", &list); busy->setData(SessionRoles::Working, 1);
    list.syncActivityAnimation(); QVERIFY(!list.activityAnimationRunning());
    list.show(); QTRY_VERIFY(list.activityAnimationRunning());
    QTRY_VERIFY(list.property("workingPulse").toReal() > .05);
    list.setActivityAnimationEnabled(false); QVERIFY(!list.activityAnimationRunning());
    list.setActivityAnimationEnabled(true); QVERIFY(list.activityAnimationRunning());
    busy->setHidden(true); list.syncActivityAnimation(); QVERIFY(!list.activityAnimationRunning());
    auto *group = new QListWidgetItem("Collapsed group", &list); group->setData(SessionRoles::Header, true); group->setData(SessionRoles::Working, 1);
    list.syncActivityAnimation(); QVERIFY(list.activityAnimationRunning());
    group->setData(SessionRoles::Working, 0); list.syncActivityAnimation(); QVERIFY(!list.activityAnimationRunning());
    busy->setHidden(false); list.syncActivityAnimation(); QVERIFY(list.activityAnimationRunning());
    list.hide(); QVERIFY(!list.activityAnimationRunning());
}

void TestWorkspace::durableIdentityAndMissingSessions()
{
    SessionOrganization model;
    const auto group = model.createGroup("Release");
    const auto arch = model.observe("arch", "codex/project/review", "run-1");
    const auto mac = model.observe("mac", "codex/project/review", "run-1");
    model.moveSession(arch, group); model.moveSession(mac, group); model.setCollapsed(group, true);
    // Reload with one machine offline: its membership and position are retained.
    SessionOrganization reloaded(model.toJson());
    QCOMPARE(reloaded.observe("arch", "codex/project/review", "run-2"), arch);
    QCOMPARE(reloaded.group(group)->sessions, QStringList({arch, mac}));
    QVERIFY(reloaded.group(group)->collapsed);
    const auto renamed = reloaded.observe("arch", "codex/project/release", "run-2");
    QCOMPARE(reloaded.groupFor(renamed), group);
    QCOMPARE(reloaded.group(group)->sessions, QStringList({renamed, mac}));
    // Archive occurrences remain distinct even with identical names and run IDs.
    const auto archive = reloaded.observe("arch", "codex/project/release", "run-2", "archive-1");
    QVERIFY(archive != renamed); QCOMPARE(reloaded.groupFor(archive), QString("ungrouped"));
    QCOMPARE(reloaded.observe("arch", "codex/project/renamed-archive", "run-2", "archive-1"), archive);
    QCOMPARE(SessionOrganization(reloaded.toJson()).toJson(), reloaded.toJson());
}

void TestWorkspace::observedRowsKeepLocalOrderAndRenameWithoutClaimingMembership()
{
    SessionOrganization model;
    const auto one = model.observe("arch", "codex/example/one", "run-one");
    const auto two = model.observe("arch", "codex/example/two", "run-two");
    const auto archive = model.observe("arch", "codex/example/one", "run-one", "archive-one");
    model = SessionOrganization(model.toJson());
    QCOMPARE(model.group("ungrouped")->sessions, QStringList({one, two, archive}));
    QCOMPARE(model.toJson()["unassigned_sessions"].toArray().size(), 3);
    model.moveSession(one, "missing"); // Rejected moves do not create intent.
    QVERIFY(model.toJson()["unassigned_sessions"].toArray().contains(one));
    model.moveSession(two, "ungrouped", one); // Explicit same-project ordering.
    QCOMPARE(model.group("ungrouped")->sessions, QStringList({two, one, archive}));
    QVERIFY(!model.toJson()["unassigned_sessions"].toArray().contains(two));
    const auto renamed = model.observe("arch", "codex/example/renamed", "run-one");
    QVERIFY(model.toJson()["unassigned_sessions"].toArray().contains(renamed));
    QVERIFY(!model.toJson()["unassigned_sessions"].toArray().contains(one));
    // A catalog assignment may arrive for the new name before a fleet refresh.
    const auto work = model.createGroup("Work");
    model.moveSession("arch\ncodex/example/final", work);
    const auto finalName = model.observe("arch", "codex/example/final", "run-one");
    QCOMPARE(model.groupFor(finalName), work);
    QVERIFY(!model.toJson()["unassigned_sessions"].toArray().contains(finalName));
    QVERIFY(!model.group("ungrouped")->sessions.contains(renamed));
    QCOMPARE(model.observe("arch", "codex/example/final", "run-one", "archive-one"), archive);
    QVERIFY(model.toJson()["unassigned_sessions"].toArray().contains(archive));
    QCOMPARE(SessionOrganization(model.toJson()).toJson(), model.toJson());
}

void TestWorkspace::orderingAndRemoval()
{
    SessionOrganization model;
    const auto first = model.createGroup("First"), second = model.createGroup("Second");
    for (const auto &id : {"a", "hidden", "b", "c"}) model.moveSession(id, first);
    model.moveSession("c", first, "b"); // Moving a visible item doesn't drop filtered members.
    QCOMPARE(model.group(first)->sessions, QStringList({"a", "hidden", "c", "b"}));
    model.moveSession("a", second); model.moveSession("c", second, "a");
    QCOMPARE(model.group(second)->sessions, QStringList({"c", "a"}));
    model.moveGroup(second, first); QCOMPARE(model.groups().at(0).id, second);
    model.moveGroup("ungrouped", second); QCOMPARE(model.groups().at(0).id, QString("ungrouped"));
    model.removeGroup(second); QVERIFY(!model.group(second));
    QCOMPARE(model.group("ungrouped")->sessions, QStringList({"c", "a"}));
    model.renameGroup(first, "  New name  "); QCOMPARE(model.group(first)->name, QString("New name"));
    model.removeGroup("ungrouped"); QVERIFY(model.group("ungrouped"));
}

void TestWorkspace::invalidDataAndMoves()
{
    SessionOrganization model(QJsonObject{{"groups", QJsonArray{
        QJsonObject{{"id", "one"}, {"name", "One"}, {"sessions", QJsonArray{"a", "a", ""}}},
        QJsonObject{{"id", "one"}, {"name", "Duplicate"}},
        QJsonObject{{"id", "two"}, {"name", "Two"}, {"sessions", QJsonArray{"a", "b"}}}}}});
    QCOMPARE(model.groups().size(), 3); QCOMPARE(model.group("one")->sessions, QStringList{"a"});
    QCOMPARE(model.group("two")->sessions, QStringList{"b"});
    const auto before = model.toJson();
    model.moveSession("a", "missing"); model.moveSession("a", "two", "missing"); model.moveSession("a", "one", "a");
    model.moveGroup("one", "missing"); model.moveGroup("one", "one"); model.createGroup(" ");
    QCOMPARE(model.toJson(), before);
}

void TestWorkspace::nativeDropPlacement()
{
    ListProbe list; list.resize(310, 450);
    const auto add = [&](const QString &group, const QString &id, bool header = false) {
        auto *item = new QListWidgetItem(header ? group : id, &list); item->setSizeHint(QSize(300, header ? 34 : 68));
        item->setData(SessionRoles::Header, header); item->setData(SessionRoles::Group, group); item->setData(SessionRoles::Identity, id);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled); return item;
    };
    auto *first = add("first", {}, true); auto *a = add("first", "a"); auto *b = add("first", "b");
    auto *second = add("second", {}, true); auto *c = add("second", "c"); list.show(); QTest::qWait(20);
    QSignalSpy moved(&list, &SessionList::sessionMoved), reordered(&list, &SessionList::groupMoved);
    const auto drop = [&](QListWidgetItem *source, QPoint point) {
        std::unique_ptr<QMimeData> mime(list.mimeData({source}));
        QDragEnterEvent enter(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &enter); QVERIFY(enter.isAccepted());
        QDragMoveEvent motion(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &motion); QVERIFY(motion.isAccepted());
        QDropEvent event(point, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list.viewport(), &event); QVERIFY(event.isAccepted());
    };
    drop(a, list.visualItemRect(second).center()); // Can drop into an empty or collapsed group.
    QCOMPARE(moved.takeFirst(), QList<QVariant>({"a", "second", ""}));
    drop(c, list.visualItemRect(b).topLeft() + QPoint(50, 3));
    QCOMPARE(moved.takeFirst(), QList<QVariant>({"c", "first", "b"}));
    drop(second, list.visualItemRect(first).topLeft() + QPoint(50, 3));
    QCOMPARE(reordered.takeFirst(), QList<QVariant>({"second", "first"}));
    drop(first, QPoint(100, 430)); QCOMPARE(reordered.takeFirst(), QList<QVariant>({"first", ""}));
    auto *child = new QListWidgetItem("Child");
    child->setData(SessionRoles::ChildId, "child"); child->setData(SessionRoles::ParentKey, a->data(SessionRoles::Key));
    child->setData(SessionRoles::Group, "first"); child->setSizeHint(QSize(280, 40));
    list.insertItem(list.row(a) + 1, child);
    std::unique_ptr<QMimeData> childMime(list.mimeData({child}));
    QVERIFY(!childMime->hasFormat("application/x-hgs-session-placement"));
    drop(c, list.visualItemRect(child).bottomLeft() + QPoint(50, -2));
    QCOMPARE(moved.takeFirst(), QList<QVariant>({"c", "first", "b"}));
    delete list.takeItem(list.row(child));
    // Payloads from another window/application are rejected.
    ListProbe foreign; auto *other = new QListWidgetItem("foreign", &foreign); other->setData(SessionRoles::Identity, "a");
    std::unique_ptr<QMimeData> mime(foreign.mimeData({other}));
    QDragEnterEvent invalid(QPoint(20, 20), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list.viewport(), &invalid); QVERIFY(!invalid.isAccepted());
    QSignalSpy toggled(&list, &SessionList::groupToggled);
    list.setCurrentItem(a); QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, list.visualItemRect(first).center());
    QCOMPARE(toggled.size(), 1); QCOMPARE(list.currentItem(), a);
}

void TestWorkspace::pinningRestoresPreviousGroup()
{
    const QJsonObject legacy{{"version",1},{"groups",QJsonArray{
        QJsonObject{{"id","pinned"},{"name","Pinned"},{"sessions",QJsonArray{"one","orphan"}}},
        QJsonObject{{"id","work"},{"name","Work"},{"collapsed",true},{"sessions",QJsonArray{"two"}}}}},
        {"before_pin",QJsonObject{{"one","work"},{"orphan","removed"}}}};
    SessionOrganization model(legacy);
    QVERIFY(!model.group("pinned")); QCOMPARE(model.groupFor("one"),QString("work"));
    QCOMPARE(model.groupFor("orphan"),QString("ungrouped")); QVERIFY(model.group("work")->collapsed);
    QVERIFY(model.toJson().contains("projects")); QVERIFY(!model.toJson().contains("groups"));
    QCOMPARE(SessionOrganization(model.toJson()).toJson(), model.toJson());
}

void TestWorkspace::defaultProjectCanBeRenamedAndReplaced()
{
    SessionOrganization model;const auto initial=model.defaultProject();
    model.renameGroup(initial,"My workspace");QCOMPARE(model.group(initial)->name,QString("My workspace"));
    model.observe("arch","session","run",{});const auto target=model.createGroup("Product");
    QVERIFY(!model.removeGroup(initial));QVERIFY(!model.removeGroup(initial,"missing"));
    QVERIFY(model.removeGroup(initial,target));QCOMPARE(model.defaultProject(),target);
    QCOMPARE(model.groupFor("arch\nsession"),target);QVERIFY(!model.group(initial));
    SessionOrganization restored(model.toJson());QCOMPARE(restored.groups().size(),1);
    QCOMPARE(restored.defaultProject(),target);restored.observe("mac","new","another",{});
    QCOMPARE(restored.groupFor("mac\nnew"),target);QVERIFY(!restored.removeGroup(target));
}

void TestWorkspace::projectsShareFoldersButOwnSessions()
{
    SessionOrganization model; const auto a=model.createGroup("Product / alpha"),b=model.createGroup("Customer beta");
    const auto one=model.addFolder(a,"arch","/workspace/shared","Backend");
    QVERIFY(!one.isEmpty()); QCOMPARE(model.addFolder(a,"arch","/workspace/shared/"),one);
    QVERIFY(!model.addFolder(b,"arch","/workspace/shared").isEmpty());
    QVERIFY(!model.addFolder(a,"mac","/Users/test/shared").isEmpty());
    model.setColor(a,"#ffcc00"); model.moveSession("arch\nsession",a); model.moveSession("arch\nsession",b);
    QVERIFY(!model.group(a)->sessions.contains("arch\nsession")); QCOMPARE(model.groupFor("arch\nsession"),b);
    QCOMPARE(model.group(a)->folders.size(),2); QCOMPARE(model.group(b)->folders.size(),1);
    QVERIFY(model.editFolder(a,one,"arch","/workspace/renamed","New folder"));
    QCOMPARE(model.group(a)->folders.first().id,one);
    QCOMPARE(model.group(b)->folders.first().path,QString("/workspace/shared"));
    model.renameGroup(a,"Renamed"); QCOMPARE(model.group(a)->color,QString("#ffcc00"));
    SessionOrganization restored(model.toJson()); QCOMPARE(restored.group(a)->folders[0].id,one);
    restored.removeGroup(b); QCOMPARE(restored.groupFor("arch\nsession"),QString("ungrouped"));
    QCOMPARE(restored.group(a)->folders.size(),2);
}

void TestWorkspace::folderTransfersPreserveCatalogIdentity_data()
{
    QTest::addColumn<bool>("copy");
    QTest::newRow("move") << false;
    QTest::newRow("copy") << true;
}

void TestWorkspace::folderTransfersPreserveCatalogIdentity()
{
    QFETCH(bool, copy);
    SessionOrganization model;
    const auto source = model.createGroup("Sources"), target = model.createGroup("Destination");
    const auto original = model.addFolder(source, "remote", "/workspace/plain folder", "Shared sources");
    model.addFolder(source, "local", "/workspace/keep");
    model.moveSession(model.observe("remote", "agent/session", "run-1"), source);
    model.setColor(source, "#123456"); model.setCollapsed(source, true);
    auto data = model.toJson(); auto projects = data["projects"].toArray();
    for (int i = 0; i < projects.size(); ++i) {
        auto project = projects[i].toObject();
        if (project["id"].toString() != source) continue;
        auto folders = project["folders"].toArray(); auto folder = folders[0].toObject();
        folder["machine_id"] = "machine-remote"; folder["machine_name"] = "Offline workstation";
        folders[0] = folder; project["folders"] = folders; projects[i] = project;
    }
    data["projects"] = projects; model = SessionOrganization(data);
    const auto result = copy ? model.copyFolder(source, original, target) : model.moveFolder(source, original, target);
    QVERIFY(!result.isEmpty());
    if (copy) QVERIFY(result != original); else QCOMPARE(result, original);
    QCOMPARE(model.group(source)->folders.size(), copy ? 2 : 1);
    QCOMPARE(model.group(target)->folders.size(), 1);
    const auto folder = model.group(target)->folders.first();
    QCOMPARE(folder.id, result); QCOMPARE(folder.machine, QString("remote"));
    QCOMPARE(folder.machineId, QString("machine-remote")); QCOMPARE(folder.machineName, QString("Offline workstation"));
    QCOMPARE(folder.path, QString("/workspace/plain folder")); QCOMPARE(folder.name, QString("Shared sources"));
    QCOMPARE(model.groupFor("remote\nagent/session"), source);
    QCOMPARE(model.group(source)->color, QString("#123456")); QVERIFY(model.group(source)->collapsed);
    QCOMPARE(model.toJson()["runs"], data["runs"]); QCOMPARE(model.toJson()["default_project"], data["default_project"]);
    QCOMPARE(SessionOrganization(model.toJson()).toJson(), model.toJson());
    // Separate IDs allow either copied link to be edited/removed independently.
    QVERIFY(model.editFolder(target, result, "remote", "/workspace/renamed", "Renamed"));
    if (copy) QCOMPARE(model.group(source)->folders.first().path, QString("/workspace/plain folder"));
    model.removeFolder(target, result); QVERIFY(model.group(target)->folders.isEmpty());
    QCOMPARE(model.group(source)->folders.size(), copy ? 2 : 1);
}

void TestWorkspace::folderTransfersReuseExistingLinks_data()
{
    QTest::addColumn<bool>("copy"); QTest::addColumn<bool>("stableIdentity");
    QTest::newRow("move-alias") << false << false;
    QTest::newRow("copy-alias") << true << false;
    QTest::newRow("move-stable-machine-id") << false << true;
    QTest::newRow("copy-stable-machine-id") << true << true;
}

void TestWorkspace::folderTransfersReuseExistingLinks()
{
    QFETCH(bool, copy); QFETCH(bool, stableIdentity);
    SessionOrganization model; const auto source = model.createGroup("Source"), target = model.createGroup("Target");
    const auto original = model.addFolder(source, "remote", "/workspace/shared", "Source label");
    const auto existing = model.addFolder(target, stableIdentity ? "other-alias" : "remote", "/workspace/shared", "Target label");
    model.addFolder(target, "local", "/workspace/shared", "Same path, other computer");
    if (stableIdentity) {
        auto data = model.toJson(); auto projects = data["projects"].toArray();
        for (int i = 0; i < projects.size(); ++i) {
            auto project = projects[i].toObject(); auto folders = project["folders"].toArray();
            for (int j = 0; j < folders.size(); ++j) {
                auto folder = folders[j].toObject();
                if (folder["id"] != original && folder["id"] != existing) continue;
                folder["machine_id"] = "machine-remote"; folders[j] = folder;
            }
            project["folders"] = folders; projects[i] = project;
        }
        data["projects"] = projects; model = SessionOrganization(data);
    }
    const auto before = model.toJson();
    QCOMPARE(copy ? model.copyFolder(source, original, target) : model.moveFolder(source, original, target), existing);
    QCOMPARE(model.group(target)->folders.size(), 2);
    QCOMPARE(model.group(target)->folders.first().id, existing);
    QCOMPARE(model.group(target)->folders.first().name, QString("Target label"));
    QCOMPARE(model.group(target)->folders.first().machine, stableIdentity ? QString("other-alias") : QString("remote"));
    QCOMPARE(model.group(source)->folders.size(), copy ? 1 : 0);
    if (copy) QCOMPARE(model.toJson(), before);
}

void TestWorkspace::folderTransfersRejectInvalidTargets()
{
    SessionOrganization model; const auto source = model.createGroup("Source"), target = model.createGroup("Target");
    const auto folder = model.addFolder(source, "local", "/workspace/plain");
    const auto before = model.toJson();
    for (const bool copy : {false, true}) {
        const auto transfer = [&](const QString &from, const QString &id, const QString &to) {
            return copy ? model.copyFolder(from, id, to) : model.moveFolder(from, id, to);
        };
        QVERIFY(transfer(source, folder, source).isEmpty());
        QVERIFY(transfer("missing", folder, target).isEmpty());
        QVERIFY(transfer(source, folder, "missing").isEmpty());
        QVERIFY(transfer(source, "missing", target).isEmpty());
        QVERIFY(transfer(source, {}, target).isEmpty());
        QCOMPARE(model.toJson(), before);
    }
}

void TestWorkspace::keyboardFocusFollowsInput()
{
    QWidget window; QVBoxLayout layout(&window); QLineEdit input; ListProbe list;
    layout.addWidget(&input); layout.addWidget(&list); list.addItems({"First", "Second"}); window.show(); window.activateWindow(); QTest::qWait(20);
    const auto keyboard = [&] { QStyleOptionViewItem option; list.initViewItemOption(&option); return bool(option.state & QStyle::State_KeyboardFocusChange); };
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, list.visualItemRect(list.item(0)).center());
    QCOMPARE(list.currentRow(), 0); QVERIFY(list.hasFocus()); QVERIFY(!keyboard());
    QTest::keyClick(&list, Qt::Key_Down); QCOMPARE(list.currentRow(), 1); QVERIFY(keyboard());
    QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, list.visualItemRect(list.item(1)).center());
    QCOMPARE(list.currentRow(), 1); QVERIFY(!keyboard());
    input.setFocus(); QTest::keyClick(&input, Qt::Key_Tab); QVERIFY(list.hasFocus()); QVERIFY(keyboard());
    QTest::mouseClick(list.viewport(), Qt::RightButton, Qt::NoModifier, list.visualItemRect(list.item(1)).center());
    QVERIFY(!keyboard()); QCOMPARE(list.currentRow(), 1);
}

void TestWorkspace::draftStatusSitsBetweenWorkAndReplies()
{
    // An unsent draft is a status of its own: below anything asking for the user or
    // still working, above a new reply (kept as the extra badge), a pause and Ready.
    QListWidget list; auto *item = new QListWidgetItem("session", &list);
    const auto status = [&](bool selected = false) { return SessionDelegate::statusOf(list.model()->index(0, 0), selected); };
    item->setData(SessionRoles::Status, "Ready"); item->setData(SessionRoles::Draft, true);
    QCOMPARE(status().kind, SessionStatusBadge::Draft); QCOMPARE(status().caption, QString("Draft"));
    QCOMPARE(status(true).kind, SessionStatusBadge::Neutral);   // the open session shows its draft in place
    item->setData(SessionRoles::Unread, true);
    QCOMPARE(status().kind, SessionStatusBadge::Draft); QVERIFY(status().additionalUnread);
    item->setData(SessionRoles::Working, 1);
    QCOMPARE(status().kind, SessionStatusBadge::Working);
    item->setData(SessionRoles::Working, 0); item->setData(SessionRoles::Attention, true);
    QCOMPARE(status().kind, SessionStatusBadge::Attention);
    item->setData(SessionRoles::Attention, false); item->setData(SessionRoles::Unread, false); item->setData(SessionRoles::Status, "Paused");
    QCOMPARE(status().kind, SessionStatusBadge::Draft);
    item->setData(SessionRoles::Draft, false);
    QCOMPARE(status().kind, SessionStatusBadge::Paused);
    item->setData(SessionRoles::Draft, true); item->setData(SessionRoles::Status, "Offline");
    QCOMPARE(status().kind, SessionStatusBadge::Neutral); QCOMPARE(status().caption, QString("Offline"));
}

QTEST_MAIN(TestWorkspace)
#include "test_workspace.moc"
