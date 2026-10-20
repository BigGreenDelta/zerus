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
#include "OptionalQuestionBadge.h"
#include "SessionPresentation.h"
#include "WorkspaceIcons.h"
#include "MessageBoxIcons.h"
#include "SessionRowPreferences.h"
#include <QAccessible>
#include <QToolButton>
#include <QHelpEvent>
#include <QToolTip>
#include <QTextDocument>
#include <QPointer>
#include <QDir>
#include <algorithm>

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
    void localWindowsFoldersFollowTheirMachine();
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
    void confirmationCancelIsRightmost_data();
    void confirmationCancelIsRightmost();
    void dialogDismissalStaysRightmostAcrossChanges_data();
    void dialogDismissalStaysRightmostAcrossChanges();
    void draftStatusSitsBetweenWorkAndReplies();
    void draftCardEdgesKeepTheirColor_data();
    void draftCardEdgesKeepTheirColor();
    void gitStatusSeparatesEvidenceAndExpires();
    void gitStatusCardsKeepMetadataAndMachineReadable();
    void optionalQuestionChipFollowsTheStatusBadge();
    void subagentInactivityKeepsConflictingAndUnknownEvidence();
    void explicitIndependentWaitKeepsNativePriorityAndRejectsStaleInspection();
    void compactRowsKeepTheirGeometryAndHideDetails_data();
    void compactRowsKeepTheirGeometryAndHideDetails();
    void compactRowsHaveAccessibleExpandControls();
    void compactRowsExposeElidedFields();
    void compactRowPreferencesKeepExactBoundedIdentities();
    void mutedAttentionUsesNeutralPresentation_data();
    void mutedAttentionUsesNeutralPresentation();
    void sessionListDestructionStopsOwnedCallbacks_data();
    void sessionListDestructionStopsOwnedCallbacks();
};

void TestWorkspace::sessionListDestructionStopsOwnedCallbacks_data()
{
    QTest::addColumn<bool>("populated");
    QTest::newRow("empty") << false;
    QTest::newRow("populated") << true;
}

void TestWorkspace::sessionListDestructionStopsOwnedCallbacks()
{
    QFETCH(bool, populated);
    QWidget owner; owner.resize(340, 200);
    auto *list = new SessionList(&owner); list->resize(320, 180);
    list->setItemDelegate(new SessionDelegate(list));
    if (populated) {
        for (int i = 0; i < 4; ++i) {
            auto *row = new QListWidgetItem(list); row->setData(SessionRoles::Key, QString::number(i));
            row->setData(SessionRoles::Title, "Working session " + QString::number(i)); row->setData(SessionRoles::Agent, "codex");
            row->setData(SessionRoles::Status, "Working"); row->setData(SessionRoles::Working, 1);
            row->setData(SessionRoles::WorkingSince, QDateTime::currentSecsSinceEpoch() - 10);
            row->setData(SessionRoles::RowCollapsed, true);
        }
        owner.show(); QTRY_VERIFY(list->isVisible()); list->syncRowExpandButtons(); list->syncActivityAnimation();
        QVERIFY(!list->findChildren<QToolButton *>("expandSessionRow").isEmpty());
        QVERIFY(list->activityAnimationRunning()); QVERIFY(list->elapsedTimerRunning());
        list->item(0)->setData(SessionRoles::Title, "A pending compact-row update");
    }
    QPointer<QObject> remaining(list); QSignalSpy destroyed(list, &QObject::destroyed);
    QObject observer; bool modelDestroyed = false; int lateCalls = 0;
    // Pending work must expire with the derived widget, even if another child
    // destructor drains queued calls before QObject's automatic cleanup runs.
    QTimer::singleShot(0, list, [&lateCalls] { ++lateCalls; });
    QObject::connect(list->model(), &QObject::destroyed, &observer, [receiver = static_cast<QObject *>(list), &modelDestroyed] {
        modelDestroyed = true; QCoreApplication::sendPostedEvents(receiver, QEvent::MetaCall);
    });
    delete list;
    QVERIFY(remaining.isNull()); QCOMPARE(destroyed.size(), 1); QVERIFY(modelDestroyed); QCOMPARE(lateCalls, 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); QCOMPARE(lateCalls, 0);
}

void TestWorkspace::compactRowsKeepTheirGeometryAndHideDetails_data()
{
    QTest::addColumn<bool>("dark"); QTest::addColumn<bool>("dense"); QTest::addColumn<int>("width"); QTest::addColumn<int>("scale"); QTest::addColumn<QString>("agent");
    for (bool dark : {false, true}) for (bool dense : {false, true}) for (int width : {250, 320, 500}) for (int scale : {1, 2})
        for (const auto &agent : {"codex", "sh", "claude", "kimi", "opencode", "dsh"})
            QTest::newRow(qPrintable(QString("%1-%2-%3-%4-%5").arg(dark).arg(dense).arg(width).arg(scale).arg(agent))) << dark << dense << width << scale << QString(agent);
}

void TestWorkspace::compactRowsKeepTheirGeometryAndHideDetails()
{
    QFETCH(bool, dark); QFETCH(bool, dense); QFETCH(int, width); QFETCH(int, scale); QFETCH(QString, agent);
    ListProbe list; list.setProperty("hgsDark", dark); list.setProperty("compact", dense);
    auto *row = new QListWidgetItem(&list); row->setData(SessionRoles::Title, "A long session name to retain");
    row->setData(SessionRoles::Agent, agent); row->setData(SessionRoles::Status, "Needs approval"); row->setData(SessionRoles::Attention, true);
    row->setData(SessionRoles::RowCollapsed, true);
    const auto index = list.model()->index(0, 0); SessionDelegate delegate;
    QStyleOptionViewItem option; option.widget = &list; option.font = list.font(); option.font.setPixelSize(11 * scale);
    option.rect = {0, 0, width, 40}; option.state = QStyle::State_Enabled | QStyle::State_Selected;
    QCOMPARE(delegate.sizeHint(option, index).height(), 40);
    const auto layout = SessionDelegate::compactLayout(option, index);
    for (const auto &rect : {layout.title, layout.status, layout.provider, layout.expand}) {
        QVERIFY(!rect.isEmpty()); QVERIFY(option.rect.contains(rect));
    }
    QVERIFY(layout.title.width() >= 60); QVERIFY(!layout.title.intersects(layout.status));
    QVERIFY(!layout.status.intersects(layout.provider)); QVERIFY(!layout.provider.intersects(layout.expand));
    const auto providerFont = IdentityBadges::font(option.font, IdentityBadges::Provider);
    const int providerLabelWidth = CachedText::width(providerFont, IdentityBadges::providerName(agent));
    QVERIFY(layout.provider.adjusted(22, 0, -6, 0).width() >= providerLabelWidth);
    QCOMPARE(SessionDelegate::expandRowRect(option, index), layout.expand);
    const auto render = [&] {
        QImage image(QSize(width, 40) * scale, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(scale);
        image.fill(dark ? QColor("#171c22") : QColor("#f3f6f8")); { QPainter painter(&image); delegate.paint(&painter, option, index); } return image;
    };
    const QImage compact = render();
    // Check painted label pixels too: a nonempty badge alone can still contain
    // only the shared Codex/Shell glyph when its text rectangle has no room.
    const QRect providerText = layout.provider.adjusted(22, 2, -6, -2);
    const QColor providerBackground(dark ? "#303942" : "#e5ebef"); int labelInk = 0;
    for (int y = providerText.top() * scale; y <= providerText.bottom() * scale; ++y)
        for (int x = providerText.left() * scale; x <= providerText.right() * scale; ++x)
            if (compact.pixelColor(x, y) != providerBackground) ++labelInk;
    QVERIFY(labelInk > 4);
    row->setData(SessionRoles::Meta, "hidden folder / branch"); row->setData(SessionRoles::MetaIcon, "git-branch");
    row->setData(SessionRoles::Detail, "hidden current action"); row->setData(SessionRoles::Model, "hidden model"); row->setData(SessionRoles::Effort, "high");
    row->setData(SessionRoles::Host, "hidden machine"); row->setData(SessionRoles::Children, "12"); row->setData(SessionRoles::HasChildren, true);
    row->setData(SessionRoles::OptionalQuestions, 8); row->setData(SessionRoles::GitStatus, QJsonObject{{"changed_files", 5}});
    QCOMPARE(render(), compact); // The compact painter uses only name, status and provider.
    QVERIFY(SessionList::childrenControlRect(option.rect, index, option.font).isEmpty());
    QVERIFY(SessionDelegate::gitStatusRect(option.rect, index, option.font, dense).isEmpty());
    QVERIFY(SessionDelegate::optionalQuestionRect(option.rect, index, option.font, dense).isEmpty());
    row->setData(SessionRoles::Muted, true);
    const auto muted = SessionDelegate::compactLayout(option, index); QVERIFY(!muted.mute.isEmpty());
    QVERIFY(!muted.provider.intersects(muted.mute)); QVERIFY(!muted.mute.intersects(muted.expand));
    QVERIFY(muted.provider.adjusted(22, 0, -6, 0).width() >= providerLabelWidth);
    QVERIFY(!muted.title.isEmpty()); QVERIFY(!muted.title.intersects(muted.status));
    row->setData(SessionRoles::RowCollapsed, false); QVERIFY(delegate.sizeHint(option, index).height() > 40);
    QVERIFY(SessionDelegate::expandRowRect(option, index).isEmpty());
    row->setData(SessionRoles::RowCollapsed, true); list.setProperty("expansion", 0.0);
    QVERIFY(delegate.sizeHint(option, index).height() > 40); QVERIFY(SessionDelegate::expandRowRect(option, index).isEmpty());
}

void TestWorkspace::compactRowsExposeElidedFields()
{
    ListProbe list; list.setItemDelegate(new SessionDelegate(&list)); list.resize(250, 120);
    auto *row = new QListWidgetItem(&list); row->setData(SessionRoles::Key, "tooltip-session");
    const QString name = "The complete long session title that cannot fit in the compact row";
    const QString status = "Waiting for acceptance from another session on a remote computer";
    row->setData(SessionRoles::Title, name); row->setData(SessionRoles::Status, status);
    row->setData(SessionRoles::Agent, "codex"); row->setData(SessionRoles::RowCollapsed, true);
    list.show(); QTRY_VERIFY(list.isVisible()); list.syncRowExpandButtons();
    QStyleOptionViewItem option; list.initViewItemOption(&option); option.widget = &list; option.rect = list.visualItemRect(row);
    const auto index = list.model()->index(0, 0);
    const auto complete = SessionDelegate::compactTooltip(option, index);
    QVERIFY(complete.contains(name)); QVERIFY(complete.contains(status)); QVERIFY(complete.contains("Codex"));
    const auto buttons = list.findChildren<QToolButton *>("expandSessionRow"); QCOMPARE(buttons.size(), 1);
    auto *accessible = QAccessible::queryAccessibleInterface(buttons.first()); QVERIFY(accessible);
    QCOMPARE(accessible->text(QAccessible::Description), complete);
    const auto point = SessionDelegate::compactLayout(option, index).title.center();
    QHelpEvent help(QEvent::ToolTip, point, list.viewport()->mapToGlobal(point)); QApplication::sendEvent(list.viewport(), &help);
    QCOMPARE(QToolTip::text(), Qt::convertFromPlainText(complete));
    QTextDocument tooltip; tooltip.setHtml(QToolTip::text());
    QCOMPARE(tooltip.toPlainText(), complete);
    QToolTip::hideText();
    row->setData(SessionRoles::Title, "One"); row->setData(SessionRoles::Status, "Ready");
    option.rect.setWidth(640); QVERIFY(SessionDelegate::compactTooltip(option, index).isEmpty());
    row->setData(SessionRoles::RowCollapsed, false); QVERIFY(SessionDelegate::compactTooltip(option, index).isEmpty());
}

void TestWorkspace::compactRowsHaveAccessibleExpandControls()
{
    ListProbe list; list.setItemDelegate(new SessionDelegate(&list)); list.resize(320, 180);
    for (int i = 0; i < 300; ++i) {
        auto *row = new QListWidgetItem(&list); row->setData(SessionRoles::Key, QString::number(i));
        row->setData(SessionRoles::Title, "Session " + QString::number(i)); row->setData(SessionRoles::Agent, "codex");
        row->setData(SessionRoles::Status, "Ready"); row->setData(SessionRoles::RowCollapsed, true);
    }
    list.setCurrentRow(0); list.show(); QTRY_VERIFY(list.isVisible()); list.syncRowExpandButtons();
    const auto buttons = list.findChildren<QToolButton *>("expandSessionRow"); QVERIFY(!buttons.isEmpty()); QVERIFY(buttons.size() < 10);
    const auto found = std::find_if(buttons.cbegin(), buttons.cend(), [](QToolButton *b) { return b->property("sessionKey") == "1"; });
    QVERIFY(found != buttons.cend()); auto *button = *found;
    QVERIFY(button->isVisible()); QVERIFY(button->accessibleName().contains("Session 1"));
    auto *accessible = QAccessible::queryAccessibleInterface(button); QVERIFY(accessible); QCOMPARE(accessible->role(), QAccessible::Button);
    QVERIFY(accessible->actionInterface()); QVERIFY(accessible->actionInterface()->actionNames().contains(QAccessibleActionInterface::pressAction()));
    QSignalSpy expand(&list, &SessionList::rowExpanded);
    QTest::mouseClick(button, Qt::LeftButton); QCOMPARE(expand.size(), 1); QCOMPARE(expand.last()[0].toString(), QString("1"));
    QCOMPARE(list.currentRow(), 0); // The expand icon does not open another conversation.
    list.setFocus(); QTest::keyClick(&list, Qt::Key_Right); QCOMPARE(expand.size(), 2); QCOMPARE(expand.last()[0].toString(), QString("0"));
    list.scrollToItem(list.item(250), QAbstractItemView::PositionAtTop); list.syncRowExpandButtons();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto moved = list.findChildren<QToolButton *>("expandSessionRow"); QVERIFY(!moved.isEmpty()); QVERIFY(moved.size() < 10);
    for (auto *control : moved) QVERIFY(control->property("sessionKey").toString().toInt() >= 250);
    list.setProperty("expansion", 0.0); list.syncRowExpandButtons();
    for (auto *control : moved) QVERIFY(!control->isVisible());
}

void TestWorkspace::compactRowPreferencesKeepExactBoundedIdentities()
{
    SessionInfo session; session.name = "codex/project/one"; session.cmd = "codex"; session.created = 10;
    SessionRowPreferences prefs; QVERIFY(prefs.setCollapsed({}, session, true));
    QVERIFY(prefs.collapsed({}, session)); QVERIFY(!prefs.collapsed("peer", session));
    session.conversationId = "conversation-one"; QVERIFY(prefs.observe({}, session)); QVERIFY(prefs.collapsed({}, session));
    session.name = "codex/project/renamed"; session.runId = "resumed-run"; QVERIFY(prefs.collapsed({}, session));
    SessionRowPreferences restored(prefs.save()); QVERIFY(restored.collapsed({}, session));
    auto other = session; other.conversationId = "conversation-two"; QVERIFY(!restored.collapsed({}, other));
    other = session; other.archiveId = "archive-one"; QVERIFY(!restored.collapsed({}, other));
    other = session; other.cmd = "claude"; QVERIFY(!restored.collapsed({}, other));
    QVERIFY(restored.setCollapsed("peer", session, true)); restored.expandAll(); QVERIFY(restored.empty()); QVERIFY(restored.save().isEmpty());
    QVERIFY(SessionRowPreferences(QByteArray(1, 'x')).empty());
    QVERIFY(SessionRowPreferences(QByteArray(SessionRowPreferences::MaxRows * SessionRowPreferences::KeyBytes + 32, 'x')).empty());
}

void TestWorkspace::mutedAttentionUsesNeutralPresentation_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void TestWorkspace::mutedAttentionUsesNeutralPresentation()
{
    QFETCH(bool,dark);
    SessionList list; list.setProperty("hgsDark",dark);
    auto *row=new QListWidgetItem(&list); row->setData(SessionRoles::Title,"Muted session");
    row->setData(SessionRoles::Agent,"codex"); row->setData(SessionRoles::Status,"Needs approval");
    row->setData(SessionRoles::Attention,true); row->setData(SessionRoles::Unread,true);
    row->setData(SessionRoles::ReviewLater,true);
    row->setData(SessionRoles::OptionalQuestions,1);
    const auto index=list.model()->index(0,0);
    const auto normal=SessionDelegate::statusOf(index,false);
    QCOMPARE(normal.kind,SessionStatusBadge::Attention); QCOMPARE(normal.emphasis,IdentityBadges::Attention);
    row->setData(SessionRoles::Muted,true);
    const auto muted=SessionDelegate::statusOf(index,false);
    QCOMPARE(muted.kind,SessionStatusBadge::MutedAttention); QCOMPARE(muted.emphasis,IdentityBadges::Muted);
    QCOMPARE(muted.caption,normal.caption); QVERIFY(muted.attention); QVERIFY(muted.unread);
    QCOMPARE(IdentityBadges::emphasisColor(dark,muted.emphasis),SessionStatusBadge::mutedTone(dark));
    QVERIFY(SessionStatusBadge::edge(muted.kind,dark)!=SessionStatusBadge::edge(normal.kind,dark));
    const QRect bounds(0,0,480,104);
    const auto layout=SessionDelegate::titleLayout(bounds,list.font(),false,index,muted);
    QVERIFY(layout.title.left()>=sessionCardRect(bounds).left()+30); // The mute icon has reserved space.
    SessionDelegate delegate;
    const auto render=[&](qreal expansion) {
        list.setProperty("expansion",expansion); list.setProperty("expandedRowWidth",480);
        QImage image(480,104,QImage::Format_ARGB32_Premultiplied); image.fill(dark?QColor("#151b21"):QColor("#f3f6f8"));
        QStyleOptionViewItem option; option.rect=expansion<1?QRect(0,0,SessionStrip::width(false),104):bounds;
        option.font=list.font(); option.widget=&list;
        QPainter painter(&image); delegate.paint(&painter,option,index); painter.end(); return image;
    };
    const auto hasColor=[](const QImage &image,const QColor &color) {
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==color)return true;
        return false;
    };
    for(qreal expansion:{0.0,1.0}) {
        row->setData(SessionRoles::Muted,false);
        QVERIFY(hasColor(render(expansion),OptionalQuestionBadge::fill(dark)));
        row->setData(SessionRoles::Muted,true);
        const auto image=render(expansion);
        QVERIFY(!hasColor(image,OptionalQuestionBadge::fill(dark)));
        QVERIFY(hasColor(image,OptionalQuestionBadge::fill(dark,true)));
        QVERIFY(hasColor(image,SessionStatusBadge::mutedTone(dark)));
        QCOMPARE(index.data(SessionRoles::OptionalQuestions).toInt(),1);
    }
    row->setData(SessionRoles::Attention,false); row->setData(SessionRoles::ReviewLater,false);
    QCOMPARE(SessionDelegate::statusOf(index,false).kind,SessionStatusBadge::MutedUnread);
    row->setData(SessionRoles::Working,true); row->setData(SessionRoles::Status,"Working");
    QCOMPARE(SessionDelegate::statusOf(index,false).kind,SessionStatusBadge::Working);
    row->setData(SessionRoles::Attention,true); row->setData(SessionRoles::Failure,true);
    QCOMPARE(SessionDelegate::statusOf(index,false).kind,SessionStatusBadge::Error);
    row->setData(SessionRoles::Working,false); row->setData(SessionRoles::Failure,false);
    row->setData(SessionRoles::Muted,false); row->setData(SessionRoles::Status,"Needs approval");
    QCOMPARE(SessionDelegate::statusOf(index,false).kind,SessionStatusBadge::Attention);
}

void TestWorkspace::explicitIndependentWaitKeepsNativePriorityAndRejectsStaleInspection()
{
    SessionInfo s; s.state="running"; s.activity="idle"; s.phase="idle"; s.tracked=true;
    s.runId="run"; s.conversationId="conversation"; s.unreadReply=true;
    QJsonObject wait{{"schema",1},{"status","ok"},{"revision",2},{"run_id",s.runId},{"conversation_id",s.conversationId},{"count",1},
        {"items",QJsonArray{QJsonObject{{"state","awaiting_ack"},{"created_at",QDateTime::currentSecsSinceEpoch()-120},
            {"producer",QJsonObject{{"name","claude/project/worker"},{"host","peer"}}}}}}};
    s.coordinationWait=wait;
    QVERIFY(SessionPresentation::active(s)); QVERIFY(!s.visibleUnreadReply()); QVERIFY(!s.needsAction());
    QVERIFY(SessionPresentation::status(s).contains("Waiting for acceptance from worker on peer"));
    QVERIFY(SessionPresentation::status(s).contains("2m"));
    SessionList list;auto *row=new QListWidgetItem(&list);row->setData(SessionRoles::Title,"Coordinator");
    row->setData(SessionRoles::Agent,"codex");row->setData(SessionRoles::Status,SessionPresentation::status(s));
    row->setData(SessionRoles::Waiting,true);row->setData(SessionRoles::CoordinationWait,wait);
    const auto index=list.model()->index(0,0);const auto badge=SessionDelegate::statusOf(index,false);
    QCOMPARE(badge.kind,SessionStatusBadge::Waiting);QVERIFY(badge.caption.startsWith("Waiting ("));
    const auto layout=SessionDelegate::titleLayout(QRect(0,0,280,104),list.font(),false,index,badge);
    QVERIFY(layout.title.width()>=72);QVERIFY(!layout.status.intersects(layout.provider));
    s.backgroundProcesses=1; QCOMPARE(SessionPresentation::status(s),QString("Background"));
    s.backgroundProcesses=0; s.phase="approval"; QCOMPARE(SessionPresentation::status(s),QString("Needs approval"));
    s.phase="tool"; s.activity="busy"; QCOMPARE(SessionPresentation::status(s),QString("Using a tool"));
    s.phase="idle"; s.activity="idle";
    QJsonObject details{{"run_id",s.runId},{"conversation_id",s.conversationId}};
    auto stale=wait; stale["revision"]=1; stale["count"]=0; details["coordination_wait"]=stale;
    QVERIFY(SessionPresentation::inspected(s,details).hasCoordinationWait());
    auto settled=wait; settled["revision"]=3; settled["count"]=0; settled["items"]=QJsonArray{}; details["coordination_wait"]=settled;
    auto done=SessionPresentation::inspected(s,details); QVERIFY(!done.hasCoordinationWait()); QVERIFY(done.visibleUnreadReply());
    QVERIFY(!SessionPresentation::active(s,false));
    s.runId="reused"; QVERIFY(!s.hasCoordinationWait()); QCOMPARE(SessionPresentation::status(s),QString("Ready"));
}

void TestWorkspace::subagentInactivityKeepsConflictingAndUnknownEvidence()
{
    using SessionPresentation::inactiveChild;
    for (const auto &state : {"finished", "ready", "idle"}) {
        QVERIFY(inactiveChild({{"state", state}}));
        QVERIFY(inactiveChild({{"state", state}, {"display_state", ""}}));
        QVERIFY(inactiveChild({{"state", state}, {"display_state", QJsonValue::Null}}));
        for (const auto &display : {"working", "input", "approval", "attention", "paused", "unknown", "future-state"})
            QVERIFY(!inactiveChild({{"state", state}, {"display_state", display}}));
    }
    QVERIFY(inactiveChild({{"state", "working"}, {"display_state", "idle"}}));
    // A failed child turn has ended and hides with completed turns.
    QVERIFY(inactiveChild({{"state", "finished"}, {"display_state", "error"}}));
    QVERIFY(inactiveChild({{"state", "error"}}));
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

void TestWorkspace::optionalQuestionChipFollowsTheStatusBadge()
{
    // The chip trails the status badge, or a separate new reply badge, in the
    // title row and has the attention amber; the Git row keeps its room.
    const QString destination = qEnvironmentVariable("HGS_GIT_STATUS_PREVIEW");
    for (const bool dark : {true, false}) {
        ListProbe list; list.setItemDelegate(new SessionDelegate(&list));
        list.setProperty("hgsDark", dark); list.setProperty("compact", false);
        list.setStyleSheet(dark ? "QListWidget { background:#171c22; border:0; }" : "QListWidget { background:#f3f6f8; border:0; }");
        list.setFrameShape(QFrame::NoFrame); list.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        struct Row { QString title; int questions; bool attention, child; };
        const QList<Row> rows{{"nebula", 1, false, false}, {"Long session title that has to elide", 120, false, false},
                              {"needs input", 2, true, false}, {"no questions", 0, false, false}, {"child", 3, false, true}};
        for (const auto &row : rows) {
            auto *item = new QListWidgetItem(&list);
            item->setData(SessionRoles::Title, row.title); item->setData(SessionRoles::Meta, "zerus / main");
            item->setData(SessionRoles::Host, "arch"); item->setData(SessionRoles::MachineName, "arch");
            item->setData(SessionRoles::Agent, "codex"); item->setData(SessionRoles::Detail, "Checking the release");
            item->setData(SessionRoles::Status, row.attention ? "Needs input" : "Working");
            item->setData(SessionRoles::Working, row.attention ? 0 : 1);
            item->setData(SessionRoles::WorkingSince, QDateTime::currentSecsSinceEpoch() - 124.0);
            item->setData(SessionRoles::Attention, row.attention); item->setData(SessionRoles::Unread, row.attention);
            item->setData(SessionRoles::OptionalQuestions, row.questions);
            auto git = verifiedGit(); git["changed_files"] = 6; git["behind"] = 1; item->setData(SessionRoles::GitStatus, git);
            if (row.child) item->setData(SessionRoles::ChildId, "child-1");
        }
        for (const int width : {280, 360, 500}) {
            list.resize(width, rows.size() * 104); list.show(); QTest::qWait(10);
            const QImage image = list.grab().toImage();
            for (int i = 0; i < list.count(); ++i) {
                const auto index = list.model()->index(i, 0);
                const QRect rowRect = list.visualItemRect(list.item(i));
                QStyleOptionViewItem option; option.rect = rowRect; option.font = list.font(); option.widget = &list;
                const auto status = SessionDelegate::statusOf(index, false);
                const auto title = SessionDelegate::titleLayout(option, index, status);
                const QRect question = SessionDelegate::optionalQuestionRect(rowRect, index, list.font(), false);
                QCOMPARE(question, title.question);
                if (!rows[i].questions || rows[i].child) { QVERIFY(question.isEmpty()); continue; }
                QVERIFY(!question.isEmpty());
                QCOMPARE(question.top(), title.status.top()); QCOMPARE(question.height(), title.status.height());
                QCOMPARE(status.additionalUnread, rows[i].attention);
                QCOMPARE(question.left(), (status.additionalUnread ? title.unread : title.status).right() + 6);
                if (!title.provider.isEmpty()) QVERIFY2(question.right() < title.provider.left(), qPrintable(rows[i].title));
                QVERIFY(question.right() < width);
                QVERIFY(!question.intersects(SessionDelegate::gitStatusRect(rowRect, index, list.font(), false)));
                // The fill left of the glyph is the attention amber.
                const QColor fill = image.pixelColor(question.x() + 2, question.center().y());
                const QColor amber = OptionalQuestionBadge::fill(dark);
                QVERIFY2(qAbs(fill.red() - amber.red()) < 12 && qAbs(fill.green() - amber.green()) < 12
                    && qAbs(fill.blue() - amber.blue()) < 12, qPrintable(fill.name()));
            }
            if (!destination.isEmpty()) {
                QDir().mkpath(destination);
                QVERIFY(image.save(destination + QString("/questions-%1-%2.png").arg(dark ? "dark" : "light").arg(width)));
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

void TestWorkspace::confirmationCancelIsRightmost_data()
{
    QTest::addColumn<bool>("extraActions");
    QTest::newRow("submit") << false;
    QTest::newRow("send-compact-clear") << true;
}

void TestWorkspace::confirmationCancelIsRightmost()
{
    QFETCH(bool, extraActions);
    MessageBoxIcons::install();
    QMessageBox box(QMessageBox::Warning, "Cold cache", "Choose how to continue.");
    // Exercise the Mac order even on Linux. The per-box policy must override it.
    box.setStyleSheet("QDialogButtonBox { button-layout: 1; }");
    auto *proceed = box.addButton("Continue", QMessageBox::AcceptRole);
    if (extraActions) {
        box.addButton("Compact and continue", QMessageBox::ActionRole);
        box.addButton("Clear context", QMessageBox::ActionRole);
    }
    auto *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel); box.setEscapeButton(cancel);
    box.show(); QVERIFY(QTest::qWaitForWindowExposed(&box));
    QVERIFY(!box.testAttribute(Qt::WA_DontShowOnScreen));
    for (auto *button : box.buttons()) if (button != cancel)
        QVERIFY2(button->geometry().right() < cancel->geometry().left(), qPrintable(button->text()));
    QCOMPARE(box.defaultButton(), cancel); QCOMPARE(box.escapeButton(), cancel);
    QCOMPARE(box.buttonRole(proceed), QMessageBox::AcceptRole);
    QTest::keyClick(&box, Qt::Key_Escape);
    QTRY_COMPARE(box.clickedButton(), cancel);
}

void TestWorkspace::dialogDismissalStaysRightmostAcrossChanges_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("save-cancel") << 0;
    QTest::newRow("apply-cancel") << 1;
    QTest::newRow("review-remove-close") << 2;
}

void TestWorkspace::dialogDismissalStaysRightmostAcrossChanges()
{
    QFETCH(int, kind); MessageBoxIcons::install();
    QDialog dialog; auto *layout = new QVBoxLayout(&dialog);
    auto *buttons = new QDialogButtonBox(kind == 0 ? QDialogButtonBox::Save | QDialogButtonBox::Cancel
        : kind == 1 ? QDialogButtonBox::Apply | QDialogButtonBox::Cancel : QDialogButtonBox::Close);
    if (kind == 2) {
        buttons->addButton("Review again", QDialogButtonBox::ResetRole);
        buttons->addButton("Remove selected…", QDialogButtonBox::ActionRole);
    }
    layout->addWidget(buttons);
    auto *dismiss = buttons->button(kind == 2 ? QDialogButtonBox::Close : QDialogButtonBox::Cancel);
    dismiss->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    const auto ordered = [&] {
        for (auto *button : buttons->buttons()) if (button != dismiss && button->isVisible())
            if (button->geometry().right() >= dismiss->geometry().left()) return false;
        return true;
    };
    QTRY_VERIFY(ordered());
    // Adding actions and changing appearance recreates Qt's internal layout.
    auto *help = buttons->addButton(QDialogButtonBox::Help);
    buttons->setStyleSheet("QDialogButtonBox { button-layout: 1; }");
    QTRY_VERIFY(ordered());
    help->setText("Help for this operation"); QTRY_VERIFY(ordered());
    auto visual = buttons->buttons();
    std::sort(visual.begin(), visual.end(), [](const QAbstractButton *a, const QAbstractButton *b) { return a->geometry().left() < b->geometry().left(); });
    for (int index = 0; index + 1 < visual.size(); ++index) {
        auto *next = visual[index]->nextInFocusChain();
        for (int skipped = 0; !visual.contains(qobject_cast<QAbstractButton *>(next)) && skipped < 100; ++skipped) next = next->nextInFocusChain();
        QCOMPARE(next, visual[index+1]);
    }
    for (auto *button : buttons->buttons()) if (button != dismiss) button->setEnabled(false);
    QTRY_VERIFY(ordered()); QVERIFY(dismiss->isEnabled()); QVERIFY(dismiss->isDefault());
    QTest::keyClick(&dialog, Qt::Key_Return); QTRY_VERIFY(!dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
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
    QVERIFY(archive != renamed); QCOMPARE(reloaded.groupFor(archive), group);
    // A later run reuses the same label and moves projects before its old
    // archive is fetched. The old run keeps the group captured at observation.
    const auto other=reloaded.createGroup("Other");
    const auto reused=reloaded.observe("arch","codex/project/release","run-3");
    reloaded.moveSession(reused,other);
    const auto late=reloaded.observe("arch","codex/project/review","run-1","archive-old");
    QCOMPARE(reloaded.groupFor(late),group);
    const auto newest=reloaded.observe("arch","codex/project/release","run-3","archive-new");
    QCOMPARE(reloaded.groupFor(newest),other);
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

void TestWorkspace::localWindowsFoldersFollowTheirMachine()
{
    using MachinePlatform::Style;
    SessionOrganization model; const auto a=model.createGroup("Windows"),b=model.createGroup("Other");
    // The local Windows machine stores the Qt form and finds a folder with either
    // separator and in any case (D15, D16).
    const auto drive=model.addFolder(a,"desktop","C:\\Work\\Project\\","Drive",Style::Windows);
    QVERIFY(!drive.isEmpty()); QCOMPARE(model.group(a)->folders.last().path,QString("C:/Work/Project"));
    QCOMPARE(model.addFolder(a,"desktop","c:/work/project",{},Style::Windows),drive);
    QCOMPARE(model.addFolder(a,"desktop","C:\\work/PROJECT\\.",{},Style::Windows),drive);
    const auto mixed=model.addFolder(a,"desktop","D:\\work/sub\\dir",{},Style::Windows);
    QVERIFY(!mixed.isEmpty()); QCOMPARE(model.group(a)->folders.last().path,QString("D:/work/sub/dir"));
    const auto share=model.addFolder(a,"desktop","\\\\server\\share\\dir",{},Style::Windows);
    QVERIFY(!share.isEmpty()); QCOMPARE(model.group(a)->folders.last().path,QString("//server/share/dir"));
    QCOMPARE(model.addFolder(a,"desktop","//SERVER/Share/dir/",{},Style::Windows),share);
    const auto root=model.addFolder(a,"desktop","c:\\",{},Style::Windows);
    QVERIFY(!root.isEmpty()); QCOMPARE(model.group(a)->folders.last().path,QString("c:/"));
    // Not folders of a Windows machine: rooted without a drive, drive-relative, verbatim and POSIX paths.
    for(const char *path:{"\\work","C:work","\\\\?\\C:\\work","/workspace/project","work",""})
        QVERIFY2(model.addFolder(a,"desktop",path,{},Style::Windows).isEmpty(),path);
    // Peers are POSIX: no drive or backslash UNC folder, whatever their own platform.
    for(const char *path:{"C:\\work","C:/work","\\\\server\\share"})
        QVERIFY2(model.addFolder(a,"mac",path).isEmpty(),path);
    QCOMPARE(model.group(a)->folders.size(),4);
    // POSIX folders keep exact names: another case is another folder.
    const auto posix=model.addFolder(b,"mac","/Users/test/Work");
    QCOMPARE(model.addFolder(b,"mac","/Users/test/Work/"),posix);
    QVERIFY(!model.addFolder(b,"mac","/Users/test/work").isEmpty());
    QVERIFY(model.addFolder(b,"mac","/Users/test/work")!=posix);
    QVERIFY(!model.editFolder(b,posix,"mac","C:\\work","Drive on a peer"));
    // Edits compare the same way and store the Qt form.
    QVERIFY(!model.editFolder(a,share,"desktop","c:\\WORK\\project","Duplicate",Style::Windows));
    QVERIFY(!model.editFolder(a,share,"desktop","E:\\Other","Posix rules",Style::Posix));
    QVERIFY(model.editFolder(a,share,"desktop","E:\\Other\\","Other",Style::Windows));
    QVERIFY(model.editFolder(a,share,"desktop","e:/other","Renamed",Style::Windows));
    QCOMPARE(model.group(a)->folders[2].path,QString("e:/other"));
    // Saved or merged back from the swarm, drive and UNC folders survive as stored.
    auto data=model.toJson(); auto projects=data["projects"].toArray(); auto project=projects[0].toObject();
    auto folders=project["folders"].toArray();
    folders.append(QJsonObject{{"id","unc"},{"machine","desktop"},{"path","\\\\server\\share"}});
    for(const char *path:{"C:work","\\work","relative"})folders.append(QJsonObject{{"id",path},{"machine","desktop"},{"path",path}});
    project["folders"]=folders; projects[0]=project; data["projects"]=projects;
    const SessionOrganization restored(data); QCOMPARE(restored.group(a)->folders.size(),5);
    QCOMPARE(restored.group(a)->folders[0].path,QString("C:/Work/Project"));
    QCOMPARE(restored.group(a)->folders[4].path,QString("\\\\server\\share"));
    // A transfer finds the same Windows folder in another case; POSIX ones stay exact.
    const auto existing=model.addFolder(b,"desktop","C:\\WORK\\project",{},Style::Windows);
    QCOMPARE(model.copyFolder(a,drive,b),existing); QCOMPARE(model.group(b)->folders.size(),3);
    const auto lower=model.addFolder(a,"mac","/users/test/WORK");
    QVERIFY(model.copyFolder(a,lower,b)!=posix); QCOMPARE(model.group(b)->folders.size(),4);
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
    QCOMPARE(status().emphasis, IdentityBadges::Draft);QCOMPARE(status(true).emphasis, IdentityBadges::Draft);
    QCOMPARE(status(true).kind, SessionStatusBadge::Neutral);   // the open session shows its draft in place
    item->setData(SessionRoles::Unread, true);
    QCOMPARE(status().kind, SessionStatusBadge::Draft); QVERIFY(status().additionalUnread);
    item->setData(SessionRoles::Working, 1);
    QCOMPARE(status().kind, SessionStatusBadge::Working); QVERIFY(!status().additionalUnread);
    QVERIFY(!status().unread); QCOMPARE(status().emphasis, IdentityBadges::Draft);
    QVERIFY(item->data(SessionRoles::Unread).toBool()); // Presentation never consumes the read identity.
    item->setData(SessionRoles::Working, 0); item->setData(SessionRoles::Attention, true);
    QCOMPARE(status().kind, SessionStatusBadge::Attention);
    QCOMPARE(status().emphasis, IdentityBadges::Attention);
    item->setData(SessionRoles::Attention, false); item->setData(SessionRoles::Unread, false); item->setData(SessionRoles::Status, "Paused");
    QCOMPARE(status().kind, SessionStatusBadge::Draft);
    item->setData(SessionRoles::Draft, false);
    QCOMPARE(status().kind, SessionStatusBadge::Paused);
    item->setData(SessionRoles::Draft, true); item->setData(SessionRoles::Status, "Offline");
    QCOMPARE(status().kind, SessionStatusBadge::Neutral); QCOMPARE(status().caption, QString("Offline"));
    QCOMPARE(status().emphasis, IdentityBadges::Draft);
}

void TestWorkspace::draftCardEdgesKeepTheirColor_data()
{
    QTest::addColumn<bool>("dark");QTest::addColumn<bool>("selected");QTest::addColumn<QString>("state");
    for(bool dark:{false,true})for(bool selected:{false,true})for(const auto &state:{"Ready","Working","Offline","Needs input","Error"})
        QTest::newRow(qPrintable(QString("%1-%2-%3").arg(dark?"dark":"light",selected?"selected":"closed",state)))<<dark<<selected<<QString(state);
}
void TestWorkspace::draftCardEdgesKeepTheirColor()
{
    QFETCH(bool,dark);QFETCH(bool,selected);QFETCH(QString,state);
    QListWidget list;list.setProperty("hgsDark",dark);list.setProperty("compact",true);
    auto *item=new QListWidgetItem(&list);item->setData(SessionRoles::Title,"Pending draft");item->setData(SessionRoles::Agent,"codex");
    item->setData(SessionRoles::Status,state);item->setData(SessionRoles::Draft,true);item->setData(SessionRoles::Unread,true);
    item->setData(SessionRoles::Working,state=="Working"?1:0);
    const bool attention=state=="Needs input"||state=="Error";item->setData(SessionRoles::Attention,attention);
    item->setData(SessionRoles::Failure,state=="Error");
    const auto index=list.model()->index(0,0);
    SessionDelegate delegate;
    const QColor expected=attention?IdentityBadges::attentionColor(dark,state=="Error"):SessionStatusBadge::edge(SessionStatusBadge::Draft,dark);
    const auto near=[](QColor a,QColor b){return qAbs(a.red()-b.red())+qAbs(a.green()-b.green())+qAbs(a.blue()-b.blue())<8;};
    for(bool inspection:{false,true})for(qreal expansion:{1.,.6,0.}){
        list.setProperty("selectionIsInspection",inspection);list.setProperty("expansion",expansion);list.setProperty("expandedRowWidth",360);
        QImage image(expansion==1?360:84,94,QImage::Format_ARGB32_Premultiplied);image.fill(dark?QColor("#1c2229"):QColor("#ffffff"));
        QStyleOptionViewItem option;option.widget=&list;option.font=list.font();option.rect=image.rect();option.state=QStyle::State_Enabled|(selected?QStyle::State_Selected:QStyle::State_None);
        {QPainter painter(&image);delegate.paint(&painter,option,index);}
        const auto previews=qEnvironmentVariable("HGS_DRAFT_CARD_PREVIEW");
        if(!previews.isEmpty()&&!inspection){QDir().mkpath(previews);QVERIFY(image.save(previews+QString("/%1-%2-%3-%4.png").arg(dark?"dark":"light",selected?"selected":"closed",state).arg(expansion)));}
        if(expansion==1)QVERIFY(near(image.pixelColor(4,47),expected));
        else if(expansion>0){
            const QColor surface=selected?QColor(dark?"#303b47":"#dde7ef"):QColor(dark?"#1c2229":"#ffffff");
            const auto alpha=SessionStrip::fullOpacity(expansion);
            const QColor blended(qRound(expected.red()*alpha+surface.red()*(1-alpha)),qRound(expected.green()*alpha+surface.green()*(1-alpha)),qRound(expected.blue()*alpha+surface.blue()*(1-alpha)));
            QVERIFY(near(image.pixelColor(4,47),blended));
        }
        else if(expansion==0){
            const auto kind=attention?(state=="Error"?SessionStatusBadge::Error:SessionStatusBadge::Attention):SessionStatusBadge::Draft;
            QVERIFY(near(image.pixelColor(2,47),SessionStatusBadge::edge(kind,dark)));
        }
        QVERIFY(item->data(SessionRoles::Draft).toBool());QVERIFY(item->data(SessionRoles::Unread).toBool());
    }
}

QTEST_MAIN(TestWorkspace)
#include "test_workspace.moc"
