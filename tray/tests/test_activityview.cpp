#include "ActivityView.h"
#include "MarkdownHtml.h"
#include "ContentScale.h"
#include "MarkdownObjects.h"
#include "SessionFileReference.h"
#include "SessionUsage.h"
#include "WorkspaceFocus.h"
#include "speech/VoicePlayerImage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QThread>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QFontInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QFile>
#include <QtEndian>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QTextTable>
#include <QRegularExpression>
#include <functional>
#include <QTest>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>
#include <QKeySequence>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
// Existing presentation contracts assert the committed document. Keep their
// setup explicit while the responsiveness tests below use the asynchronous API.
qint64 threadCpuMs()
{
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) qFatal("Cannot read thread CPU time");
    const auto ticks = [](const FILETIME &time) { return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return qint64((ticks(kernel) + ticks(user)) / 10000); // 100 ns units.
#else
    timespec now{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return qint64(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#endif
}
void standardKey(QWidget *widget, QKeySequence::StandardKey key)
{
    const QKeySequence sequence(key);
    QTest::keyClick(widget, sequence[0].key(), sequence[0].keyboardModifiers());
}

void settle(ActivityView &view)
{
    if (!view.isVisible()) return;
    QElapsedTimer timeout; timeout.start();
    while (view.renderPending() && timeout.elapsed() < 15000) QTest::qWait(1);
    if (view.renderPending()) qFatal("Activity render did not complete");
}

class SettledActivityView : public ActivityView {
public:
    SettledActivityView() { show(); settle(*this); }
    void setActivity(const QJsonObject &details, const QJsonArray &events, const QString &prompt = {}, bool tracked = true) {
        ActivityView::setActivity(details, events, prompt, tracked); settle(*this);
    }
    void setTimeline(const QJsonObject &details, const QJsonArray &events, const QJsonArray &messages, const QString &prompt = {}, bool tracked = true) {
        ActivityView::setTimeline(details, events, messages, prompt, tracked); settle(*this);
    }
    void setLocalMessages(const QJsonArray &messages) { ActivityView::setLocalMessages(messages); settle(*this); }
    void setTheme(bool dark) { ActivityView::setTheme(dark); settle(*this); }
    void setContentScale(double scale) { ActivityView::setContentScale(scale); settle(*this); }
    void showSearchResult(const QJsonObject &event, const QString &query) { ActivityView::showSearchResult(event, query); settle(*this); }
    void clearSearchResult() { ActivityView::clearSearchResult(); settle(*this); }
    void jumpToLatest() { ActivityView::jumpToLatest(); settle(*this); }
};

QJsonObject journalEvent(int seq, const QString &type, const QString &detail, const QString &tool = {})
{
    return {{"seq", seq}, {"at", 1791018000 + seq}, {"type", type}, {"detail", detail}, {"tool", tool}};
}

// The format of the first character of each inline code run.
QList<QTextCharFormat> codeRuns(QTextDocument *document)
{
    QList<QTextCharFormat> result; int end = -1;
    for (auto block = document->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!MarkdownObjects::isInlineCode(fragment.charFormat())) continue;
            if (fragment.position() != end) result.append(fragment.charFormat());
            end = fragment.position() + fragment.length();
        }
    return result;
}

QJsonArray history(int count)
{
    QJsonArray events;
    for (int i = 1; i <= count; ++i)
        events.append(journalEvent(i, i % 2 ? "UserPromptSubmit" : "Stop",
            QString("Message number %1\n\nReadable **formatted prose** with `inline code`.\n\nA second paragraph keeps the timeline long enough to scroll.").arg(i)));
    return events;
}

QStringList links(QTextBrowser *browser)
{
    QStringList result;
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (!format.anchorHref().isEmpty()) result.append(format.anchorHref());
        }
    return result;
}

int fontPixels(QTextBrowser *browser, const QString &text)
{
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().text().contains(text)) return QFontInfo(it.fragment().charFormat().font()).pixelSize();
    return -1;
}

void activate(QTextBrowser *browser, const QString &url)
{
    QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(url)));
    if (auto *view = qobject_cast<ActivityView *>(browser->parentWidget())) settle(*view);
}

void mouse(QWidget *widget, QEvent::Type type, const QPoint &global, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, widget->mapFromGlobal(QPointF(global)), QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

// Left edge first.
QList<QWidget *> columnEdges(ActivityView &view)
{
    auto edges = view.browser()->findChildren<QWidget *>("activityColumnEdge");
    std::sort(edges.begin(), edges.end(), [](QWidget *a, QWidget *b) { return a->x() < b->x(); });
    return edges;
}

// Drags an edge outward (positive) or inward, as the pointer moves in steps.
void dragEdge(QWidget *edge, int distance)
{
    const QPoint start = edge->mapToGlobal(QPoint(edge->width() / 2, 100));
    mouse(edge, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step)
        mouse(edge, QEvent::MouseMove, start + QPoint(distance * step / 4, 0), Qt::NoButton, Qt::LeftButton);
    mouse(edge, QEvent::MouseButtonRelease, start + QPoint(distance, 0), Qt::LeftButton, Qt::NoButton);
}

QMenu *contextMenu(QWidget *target, const QPoint &position)
{
    QContextMenuEvent event(QContextMenuEvent::Mouse, position, target->mapToGlobal(position));
    QApplication::sendEvent(target, &event);
    return qobject_cast<QMenu *>(QApplication::activePopupWidget());
}

QAction *menuAction(QMenu *menu, const QString &text)
{
    for (auto *action : menu->actions()) if (action->text() == text) return action;
    return nullptr;
}
}

class TestActivityView : public QObject {
    Q_OBJECT
private slots:
    void hiddenSnapshotsDoNotBuildDocuments();
    void preparedWorkerLifetimeFollowsDocuments();
    void longUpdatesKeepQueuedEditorInputResponsive();
    void richReplyScrollingKeepsQueuedInputResponsive();
    void tableReplyScrollingKeepsQueuedInputResponsive_data();
    void tableReplyScrollingKeepsQueuedInputResponsive();
    void richReplyScrollingKeepsQueuedInputResponsive_data();
    void staleIdentityAndPresentationResultsAreRejected();
    void streamingSnapshotsMakeProgress();
    void destroyingViewDoesNotWaitForWorker();
    void applicationShutdownDrainsOutstandingWorkers();
    void keyboardSelectionAndFindUseCommittedLineGeometry();
    void renderedModelMatchesLaidOutSource();
    void toolDetailsUseAnInstalledMonospaceFamily();
    void caughtUpActivityAcknowledgesEarlierReplyWithoutIntermediatePaint();
    void updatesRemainPaintableBeforeTheNextEventLoop();
    void startsAtLatestAndFollows();
    void unchangedPollsSkipRendering();
    void followingPaintsAtTheFinalScrollRange();
    void jumpToLatestKeepsFocusInActivity_data();
    void jumpToLatestKeepsFocusInActivity();
    void jumpOverlayPreservesViewportAndReadingPosition();
    void preservesSelectionAndReadingPosition();
    void groupsToolsAndKeepsExpansion();
    void idleNotificationsStayOutOfTheTimeline();
    void rejectsMarkupResourcesAndUnsafeLinks();
    void preservesLiteralUserMessages_data();
    void preservesLiteralUserMessages();
    void nativeQuestionReplies_data();
    void nativeQuestionReplies();
    void unknownQuestionReplyMessages_data();
    void unknownQuestionReplyMessages();
    void nativeQuestionRepliesPreserveLiteralText();
    void preservesSessionFileReferences();
    void fileReferencesFollowTheGuiPlatform_data();
    void fileReferencesFollowTheGuiPlatform();
    void expandsMatchingSnapshotWithoutDuplicate();
    void kimiTurnStartedKeepsRequestBeforeTools();
    void taskNotificationIsAColoredNoticeNotYou();
    void subagentReportIsShortUntilExpanded();
    void crossSessionMessageIsAFullNoticeFromThatSession();
    void kimiWireAnswerReplacesEmptyStop();
    void progressCommentaryAppearsBeforeFinalResponse();
    void claudeThinkingStaysSeparateFromToolsAndReplies();
    void kimiCommentaryDoesNotReplaceEmptyStop();
    void emptyPromptHooksDoNotCreateCards();
    void providerErrorsStayVisibleAndStable();
    void providerErrorsStayVisibleAndStable_data();
    void attachmentsStayWithTheirMessageAndOpenExactFile();
    void repeatedJournalWindowKeepsOneAttachmentMessage();
    void delayedAttachmentAcknowledgementKeepsOneMessage();
    void voiceMessageRetainsAudioAndExpandableTranscript();
    void videoMessageIsOneCircleAndLoadsOnlyAfterTap();
    void voiceWaveformSeeksWithoutRelayout_data();
    void voiceWaveformSeeksWithoutRelayout();
    void voiceWaveformAppearance_data();
    void voiceWaveformAppearance();
    void recordingMetadataAppearance_data();
    void recordingMetadataAppearance();
    void attachmentThumbnailArrivesWithoutMovingTranscript();
    void liveCompactionProgressPreservesTranscriptAndClears();
    void sessionClearIsAColoredBoundaryAndSurvivesReload();
    void confirmedQuestionAnswerStaysInTimeline();
    void unmatchedContextPrecedesTimeline();
    void localDeliveryCards();
    void nativeQueueStaysSeparateFromHistoryAndTracksIdentity();
    void attachmentDeliveryKeepsThumbnailAndViewport();
    void contextCounterKeepsPhysicalRightAlignment();
    void contextCounterNamesTheReportedWindowSource();
    void failedMessageActionsFollowCardIdentity();
    void searchResultIsolatedFromLiveUpdatesAndReturnsToLatest();
    void searchResultClearsOnConversationChange();
    void searchToolExcerptIsExpandedAndClearlyMarked();
    void contentScaleScalesMarkupOnly();
    void contentScaleEnlargesTranscriptAndQueue();
    void agentMarkdownLooksLikeGitHub();
    void searchResultKeepsInlineCodeSearchable();
    void copyGivesVisibleText();
    void agentRepliesCopyAsMarkdown();
    void markdownFollowsThemeAndScale();
    void agentCardsKeepZerusSurface();
    void mouseSelectsProseAndPartOfInlineCode();
    void wrappedCodeIsDrawnAsARoundedChip();
    void resizingWithoutChipChangesKeepsTheJournal();
    void darkCardsUseNeutralTablesAndVisibleChips();
    void cardsKeepTheirSpacingAfterChips();
    void lastCardEndsTheJournal_data();
    void lastCardEndsTheJournal();
    void readingColumnCentersTranscriptAndQueue();
    void columnEdgesResizeSymmetrically();
    void contextMenuSwitchesFullWidth();
    void preview();
};

namespace {
QJsonArray longHistory()
{
    auto events = history(500);
    for (int i = 0; i < events.size(); ++i) {
        auto event = events[i].toObject();
        event["detail"] = event["detail"].toString() + QString(" The complete explanation remains available and selectable.").repeated(16);
        events[i] = event;
    }
    return events;
}
}

void TestActivityView::hiddenSnapshotsDoNotBuildDocuments()
{
    ActivityView view; view.resize(700, 400); view.setSessionKey("hidden");
    const int passes = view.renderPasses();
    view.setActivity({}, longHistory());
    view.setActivity({}, {journalEvent(1, "Stop", "Newest hidden snapshot")});
    QTest::qWait(30);
    QCOMPARE(view.renderPasses(), passes);
    QCOMPARE(view.browser()->document()->characterCount(), 1);
    view.show(); settle(view);
    QVERIFY(view.browser()->toPlainText().contains("Newest hidden snapshot"));
    QVERIFY(!view.browser()->toPlainText().contains("Message number"));
}

void TestActivityView::preparedWorkerLifetimeFollowsDocuments()
{
    const auto workers = [] { return qApp->findChildren<QThread *>("activityRenderWorker"); };
    QTRY_COMPARE_WITH_TIMEOUT(workers().size(), 0, 15000);
    auto *view = new ActivityView; view->resize(700, 400); view->show();
    auto events = history(20);
    for (int revision = 0; revision < 6; ++revision) {
        auto last = events.last().toObject(); last["detail"] = QString("Document revision %1").arg(revision);
        events[events.size() - 1] = last;
        const int passes = view->renderPasses(); view->setActivity({}, events);
        QTRY_VERIFY(view->renderPasses() > passes);
        QVERIFY(workers().size() <= 2);
        settle(*view);
        QVERIFY(view->browser()->toPlainText().contains(QString("Document revision %1").arg(revision)));
        QTRY_COMPARE(workers().size(), 1);
        QVERIFY(workers().first()->isRunning());
        // A first paint after preparation must retain its platform font faces.
        view->browser()->viewport()->repaint();
    }
    delete view;
    QTRY_COMPARE_WITH_TIMEOUT(workers().size(), 0, 15000);
}

void TestActivityView::longUpdatesKeepQueuedEditorInputResponsive()
{
    ActivityView view; view.resize(900, 500); view.show(); view.setSessionKey("responsive");
    settle(view);
    auto events = longHistory(); view.setActivity({}, events); settle(view);
    qint64 bestCpu = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3; ++run) {
        auto last = events.last().toObject(); last["detail"] = last["detail"].toString() + " Changed response."; events[events.size() - 1] = last;
        QLineEdit editor; editor.show();
        bool editorEvent = false; int beats = 0;
        QTimer heartbeat; heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, [&] { ++beats; }); heartbeat.start();
        const auto cpu = threadCpuMs();
        view.setActivity({}, events);
        QTimer::singleShot(0, &editor, [&] { QTest::keyClicks(&editor, "queued editor input"); editorEvent = true; });
        QTRY_VERIFY(editorEvent);
        QCOMPARE(editor.text(), QString("queued editor input"));
        QVERIFY(view.renderPending());
        settle(view); view.browser()->viewport()->repaint();
        QVERIFY(beats > 0);
        bestCpu = std::min(bestCpu, threadCpuMs() - cpu);
        QVERIFY(view.browser()->toPlainText().contains("Changed response."));
    }
    // Thread CPU excludes host scheduling stalls. The old GUI import/layout
    // consumes ~600 ms for this history; the commit and first paint are bounded.
    QVERIFY2(bestCpu < 200, qPrintable(QString("GUI CPU including first paint: %1 ms").arg(bestCpu)));
}

void TestActivityView::tableReplyScrollingKeepsQueuedInputResponsive_data()
{
    QTest::addColumn<bool>("dark");QTest::addColumn<double>("scale");
    for(bool dark:{false,true})for(double scale:{1.,1.25})
        QTest::newRow(qPrintable(QString("%1-%2").arg(dark?"dark":"light").arg(scale)))<<dark<<scale;
}
void TestActivityView::tableReplyScrollingKeepsQueuedInputResponsive()
{
    QFETCH(bool,dark);QFETCH(double,scale);
    ActivityView view;view.setStyleSheet("QTextBrowser { background:transparent; border:0; padding-top:12px; }");
    view.resize(900,700);view.show();view.setTheme(dark);view.setContentScale(scale);view.setSessionKey("synthetic-table");
    QString body="## Synthetic comparison\n\nA wrapped table keeps **readable results** and `tool settings`.\n\n"
        "| Scenario | Result | Notes |\n| --- | --- | --- |\n";
    for(int i=0;i<32;++i)body+=QString("| Scenario %1 | Preserve `field_%1` and `example/cache/catalog.json` beside wrapped prose. | "
        "**Reviewed** text with `native value` and a longer explanation. Текст для проверки переноса строк. |\n").arg(i);
    view.setActivity({}, {journalEvent(1,"Stop",body)});settle(view);
    auto *browser=view.browser();auto *bar=browser->verticalScrollBar();QVERIFY(bar->maximum()>700);
    QVERIFY(browser->viewport()->palette().brush(QPalette::Base).isOpaque());
    auto cursor=browser->document()->find("field_0");QVERIFY(!cursor.isNull());browser->setTextCursor(cursor);
    const auto selected=cursor.selectedText();
    const auto previews=qEnvironmentVariable("HGS_ACTIVITY_TABLE_PREVIEW");
    if(!previews.isEmpty()){QDir().mkpath(previews);QVERIFY(view.grab().save(previews+QString("/table-%1-%2.png").arg(dark?"dark":"light").arg(scale)));}
    QLineEdit editor;editor.show();qint64 bestCpu=std::numeric_limits<qint64>::max();int delivered=0;
    for(int run=0;run<3;++run){
        bar->setValue(bar->maximum()/4);const int before=bar->value();const auto start=threadCpuMs();
        for(int frame=0;frame<20;++frame){
            QTimer::singleShot(0,&editor,[&]{QTest::keyClicks(&editor,"x");++delivered;});
            QWheelEvent wheel(QPointF(40,200),browser->viewport()->mapToGlobal(QPoint(40,200)),{},QPoint(0,-60),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
            QApplication::sendEvent(browser->viewport(),&wheel);browser->viewport()->repaint();QCoreApplication::processEvents();
            QCOMPARE(delivered,run*20+frame+1);
        }
        bestCpu=std::min(bestCpu,threadCpuMs()-start);QVERIFY(bar->value()>before);
    }
    QCOMPARE(browser->textCursor().selectedText(),selected);QCOMPARE(editor.text(),QString(60,'x'));
    QVERIFY2(bestCpu<1000,qPrintable(QString("%1 ms GUI CPU for 20 table scroll/input frames").arg(bestCpu)));
}

void TestActivityView::richReplyScrollingKeepsQueuedInputResponsive_data()
{
    QTest::addColumn<bool>("paragraph");
    QTest::addColumn<bool>("workspaceStyle");
    QTest::newRow("bullets") << false << false;
    QTest::newRow("dense-paragraph") << true << false;
    QTest::newRow("styled-bullets") << false << true;
    QTest::newRow("styled-dense-paragraph") << true << true;
}

void TestActivityView::richReplyScrollingKeepsQueuedInputResponsive()
{
    QFETCH(bool, paragraph); QFETCH(bool, workspaceStyle);
    ActivityView view;
    // SessionsWindow's general browser style must not disable Activity's
    // opaque-canvas painting path (Qt gives transparent CSS a NoBrush base).
    if (workspaceStyle) view.setStyleSheet("QTextBrowser { background:transparent; border:0; padding-top:12px; }");
    view.resize(900, 700); view.show(); view.setSessionKey("rich-scroll"); view.setTheme(true);
    QString body = "## Configuration example\n\nReadable `tool settings` for the `current_session`.\n\n";
    for (int i = 0; i < 18; ++i)
        body += QString("- Preserve `option_%1` and `example/cache/model-catalog/long-catalog-file.json` beside longer prose "
                        "that wraps across lines. Selecting and scrolling this reply must keep the editor responsive.\n").arg(i);
    if (paragraph) { body.replace("\n- ", " "); body.replace("\n", " "); }
    QJsonArray events;
    for (int i = 0; i < 50; ++i) events.append(journalEvent(i + 1, "Stop", QString("Reply %1\n\n").arg(i) + body));
    view.setActivity({}, events); settle(view);
    auto *browser = view.browser(); auto *bar = browser->verticalScrollBar();
    QVERIFY(browser->viewport()->palette().brush(QPalette::Base).isOpaque());
    bar->setValue(bar->maximum() / 2); browser->viewport()->repaint();
    auto cursor = browser->cursorForPosition(QPoint(80, 150));
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 5);
    browser->setTextCursor(cursor);
    const auto selected = browser->textCursor().selectedText(); QVERIFY(!selected.isEmpty());
    const auto preview = qEnvironmentVariable("HGS_ACTIVITY_SCROLL_PREVIEW");
    if (!preview.isEmpty()) QVERIFY(view.grab().save(preview));
    const int scrollBefore = bar->value();
    QLineEdit editor; editor.show();
    qint64 bestCpu = std::numeric_limits<qint64>::max();
    for (int run = 0; run < 3; ++run) {
        const auto start = threadCpuMs();
        for (int frame = 0; frame < 20; ++frame) {
            bool inputDelivered = false;
            QTimer::singleShot(0, &editor, [&] { QTest::keyClicks(&editor, "x"); inputDelivered = true; });
            QWheelEvent wheel(QPointF(40, 200), browser->viewport()->mapToGlobal(QPoint(40, 200)), {}, QPoint(0, -120),
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
            QApplication::sendEvent(browser->viewport(), &wheel);
            browser->viewport()->repaint();
            QCoreApplication::processEvents();
            QVERIFY(inputDelivered);
        }
        bestCpu = std::min(bestCpu, threadCpuMs() - start);
    }
    QVERIFY(bar->value() > scrollBefore);
    QCOMPARE(editor.text(), QString(60, 'x'));
    QCOMPARE(browser->textCursor().selectedText(), selected);
    QVERIFY2(bestCpu < 1000, qPrintable(QString("%1 ms GUI CPU for 20 rich scroll/input frames").arg(bestCpu)));
}

void TestActivityView::staleIdentityAndPresentationResultsAreRejected()
{
    ActivityView view; view.resize(900, 500); view.show(); view.setSessionKey("old");
    view.setActivity({{"conversation_id", "old-conversation"}}, longHistory());
    QTest::qWait(20); QVERIFY(view.renderPending());
    view.setSessionKey("new");
    QVERIFY(view.browser()->toPlainText().isEmpty());
    view.setActivity({{"conversation_id", "new-conversation"}}, {journalEvent(1, "Stop", "Newest **identity** with `code`.")});
    view.setTheme(true); view.setContentScale(1.5); view.resize(550, 320); view.setColumnWidth(450);
    settle(view);
    QVERIFY(view.browser()->toPlainText().contains("Newest identity"));
    QVERIFY(!view.browser()->toPlainText().contains("Message number"));
    auto *source = view.browser()->document()->findChild<QTextDocument *>(); QVERIFY(source);
    QCOMPARE(source->textWidth(), qreal(view.browser()->viewport()->width()));
    QVERIFY(qAbs(fontPixels(view.browser(), "Newest") - ContentScale::px(13, 1.5)) <= 1);
    const auto code = codeRuns(view.browser()->document()); QVERIFY(!code.isEmpty());
    QVERIFY(code.first().foreground().color().lightness() > 128);
}

void TestActivityView::streamingSnapshotsMakeProgress()
{
    ActivityView view; view.resize(900, 500); view.show(); view.setSessionKey("streaming");
    auto events = longHistory(); view.setActivity({}, events); settle(view);
    int revision = 0, commitsDuringStream = 0;
    QTimer stream; stream.setInterval(10);
    connect(&stream, &QTimer::timeout, [&] {
        auto last = events.last().toObject(); last["detail"] = QString("Streaming revision %1 ").arg(++revision) + QString("complete transcript ").repeated(50);
        events[events.size() - 1] = last; view.setActivity({}, events);
    });
    connect(view.browser(), &QTextBrowser::textChanged, [&] {
        if (stream.isActive() && view.browser()->toPlainText().contains("Streaming revision")) ++commitsDuringStream;
    });
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(commitsDuringStream > 0, 10000);
    QVERIFY(stream.isActive()); QVERIFY(revision > 1);
    stream.stop(); settle(view);
    QVERIFY(view.browser()->toPlainText().contains(QString("Streaming revision %1 ").arg(revision)));
}

void TestActivityView::destroyingViewDoesNotWaitForWorker()
{
    auto *view = new ActivityView; view->resize(900, 500); view->show();
    view->setActivity({}, longHistory()); QTest::qWait(20); QVERIFY(view->renderPending());
    const auto cpu = threadCpuMs(); delete view;
    QVERIFY(threadCpuMs() - cpu < 100);
    bool heartbeat = false; QTimer::singleShot(0, this, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
}

void TestActivityView::applicationShutdownDrainsOutstandingWorkers()
{
    // Run just the destruction contract in a fresh QApplication, so its worker
    // is still active when Qt begins deleting global fonts and palettes.
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), {"destroyingViewDoesNotWaitForWorker"});
    QVERIFY(child.waitForStarted(10000));
    QVERIFY(child.waitForFinished(30000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QVERIFY2(child.exitCode() == 0, child.readAllStandardError().constData());
}

void TestActivityView::keyboardSelectionAndFindUseCommittedLineGeometry()
{
    SettledActivityView view; view.resize(450, 300);
    view.setActivity({}, {journalEvent(1, "Stop", QString("Readable wrapping prose with `inline code` and plain text. ").repeated(25) + "\n\n- first list item\n- second list item\n\nשלום עולם مرحبا بالعالم")});
    auto *browser = view.browser();
    // QTextBrowser's default interaction flags offer mouse selection. Opt into
    // keyboard selection here to exercise the adapter's complete line model.
    browser->setTextInteractionFlags(browser->textInteractionFlags() | Qt::TextSelectableByKeyboard);
    browser->setFocus();
    browser->moveCursor(QTextCursor::Start);
    QVERIFY(browser->find("wrapping"));
    QCOMPARE(browser->textCursor().selectedText(), QString("wrapping"));
    auto found = browser->textCursor(); found.clearSelection(); browser->setTextCursor(found);
    standardKey(browser, QKeySequence::MoveToEndOfLine);
    const auto atEnd = browser->textCursor();
    const auto block = atEnd.block();
    const auto line = block.layout()->lineForTextPosition(atEnd.position() - block.position());
    QVERIFY(line.isValid());
    const auto end = atEnd.position();
    standardKey(browser, QKeySequence::SelectPreviousChar);
    QCOMPARE(browser->textCursor().selectionStart(), end - 1);
    QCOMPARE(browser->textCursor().selectionEnd(), end);
    standardKey(browser, QKeySequence::MoveToStartOfLine);
    const auto home = browser->textCursor();
    const auto homeLine = home.block().layout()->lineForTextPosition(home.position() - home.block().position());
    QCOMPARE(home.position(), home.block().position() + homeLine.textStart());
    standardKey(browser, QKeySequence::SelectNextLine);
    QVERIFY(browser->textCursor().hasSelection());
    QVERIFY(browser->textCursor().selectedText().size() > 1);
    browser->moveCursor(QTextCursor::End);
    const int previous = browser->textCursor().position();
    standardKey(browser, QKeySequence::MoveToPreviousPage);
    QVERIFY(browser->textCursor().position() < previous);
    standardKey(browser, QKeySequence::SelectAll);
    QCOMPARE(browser->textCursor().selectionStart(), 0);
    QCOMPARE(browser->textCursor().selectionEnd(), browser->document()->characterCount() - 1);
    const auto selected = browser->textCursor().selectedText();
    view.setColumnWidth(350); settle(view);
    QCOMPARE(browser->textCursor().selectedText(), selected);
}

void TestActivityView::renderedModelMatchesLaidOutSource()
{
    SettledActivityView view; view.resize(420, 300);
    view.setActivity({}, {journalEvent(1, "Stop", "Long wrapped **formatted** prose with `inline code` repeated across lines. " + QString("More text ").repeated(30) + "\n\n- first\n- second\n\n| Name | Value |\n|---|---|\n| one | two |\n\nשלום עולם مرحبا بالعالم")});
    auto *document = view.browser()->document();
    auto *source = document->findChild<QTextDocument *>(); QVERIFY(source);
    QCOMPARE(document->characterCount(), source->characterCount());
    QCOMPARE(document->documentLayout()->paintDevice(), source->documentLayout()->paintDevice());
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        const auto original = source->findBlock(block.position());
        QCOMPARE(block.text(), original.text());
        const auto *actual = block.layout(), *expected = original.layout();
        QCOMPARE(actual->lineCount(), expected->lineCount());
        for (int i = 0; i < expected->lineCount(); ++i) {
            const auto a = actual->lineAt(i), e = expected->lineAt(i);
            QCOMPARE(a.textStart(), e.textStart()); QCOMPARE(a.textLength(), e.textLength());
            QCOMPARE(a.leadingIncluded(), e.leadingIncluded());
            for (int offset = e.textStart(); offset <= e.textStart() + e.textLength(); ++offset)
                QVERIFY2(qAbs(a.cursorToX(offset) - e.cursorToX(offset)) < .1,
                    qPrintable(QString("block %1 line %2 offset %3: %4 vs %5").arg(block.text()).arg(i).arg(offset).arg(a.cursorToX(offset)).arg(e.cursorToX(offset))));
            QVERIFY(qAbs(a.height() - e.height()) < .1);
        }
    }
}

void TestActivityView::toolDetailsUseAnInstalledMonospaceFamily()
{
    // The generic "monospace" is no macOS family: Qt spent ~60 ms populating
    // aliases and then fell back to the proportional system font.
    SettledActivityView view; view.resize(600, 500); view.setSessionKey("codex/session");
    view.setActivity({}, {journalEvent(4, "Stop", "Answer"), journalEvent(5, "PostToolUse", "printf fixture-detail", "Bash")});
    activate(view.browser(), "hgs-activity:group-5");
    const auto found = view.browser()->document()->find("printf fixture-detail");
    QVERIFY(!found.isNull());
    const auto families = found.charFormat().fontFamilies().toStringList();
    QCOMPARE(families.value(0), MarkdownHtml::monospaceFamily());
}

void TestActivityView::contentScaleScalesMarkupOnly()
{
    const QString source = "<html><head><style>body{font-size:13px;}p{margin:6px 0;line-height:135%;}</style></head><body>"
        "<p style='font-size:10px;margin:0px;'>style='font-size:12px' 14px</p>"
        "<table width='100%' cellspacing='0' cellpadding='10' style='border:1px solid #333;'><tr><td width='3'></td>"
        "<td><a href='https://example.com/icon-16px.png' title='width=\"4\" 18px'>link</a>"
        "<span style=\" font-family:'monospace'; margin-left:9px;\">code 20px</span></td></tr></table>"
        "<img src='hgs-thumbnail:key' width='96' height='64'></body></html>";
    QCOMPARE(ContentScale::html(source, 1.0), source);
    QCOMPARE(ContentScale::html(source, 2.0), QString("<html><head><style>body{font-size:26px;}p{margin:12px 0;line-height:135%;}</style></head><body>"
        "<p style='font-size:20px;margin:0px;'>style='font-size:12px' 14px</p>"
        "<table width='100%' cellspacing='0' cellpadding='20' style='border:2px solid #333;'><tr><td width='6'></td>"
        "<td><a href='https://example.com/icon-16px.png' title='width=\"4\" 18px'>link</a>"
        "<span style=\" font-family:'monospace'; margin-left:18px;\">code 20px</span></td></tr></table>"
        "<img src='hgs-thumbnail:key' width='192' height='128'></body></html>"));
    QCOMPARE(ContentScale::clamp(0.5), 0.75);
    QCOMPARE(ContentScale::clamp(4.0), 2.0);
}

void TestActivityView::contentScaleEnlargesTranscriptAndQueue()
{
    // The workspace sets this base font on every widget.
    QWidget workspace; workspace.setStyleSheet("QWidget { font-size:13px; }");
    auto *layout = new QVBoxLayout(&workspace); auto *view = new SettledActivityView; layout->addWidget(view);
    view->setTheme(false); workspace.resize(520, 700); workspace.show();
    view->setSessionKey("arch/scaled");
    const QString literal = "style='font-size:12px' and https://example.com/icon-16px.png";
    QJsonObject details{{"input_queue", QJsonObject{{"id", "queue"}, {"text", "queued words"}}}};
    const QJsonArray events{journalEvent(1, "UserPromptSubmit", literal),
        journalEvent(2, "Stop", "# Plan heading\n\nAgent body with [docs](https://example.com/icon-16px.png).")};
    view->setActivity(details, events);
    auto *browser = view->browser(); auto *queue = view->findChild<QLabel *>("queueText"); QVERIFY(queue);
    const int body = fontPixels(browser, "Agent body"), heading = fontPixels(browser, "Plan heading");
    const int queued = QFontInfo(queue->font()).pixelSize(), jump = QFontInfo(view->jumpButton()->font()).pixelSize();
    QCOMPARE(body, 14);
    const QSize documentSize = browser->document()->size().toSize();
    view->setContentScale(2.0);
    QCOMPARE(fontPixels(browser, "Agent body"), 28);
    QVERIFY2(fontPixels(browser, "Plan heading") >= 2 * heading - 1, qPrintable(QString::number(fontPixels(browser, "Plan heading"))));
    QCOMPARE(QFontInfo(queue->font()).pixelSize(), 26);
    QCOMPARE(QFontInfo(view->jumpButton()->font()).pixelSize(), jump);
    QVERIFY(browser->document()->size().height() > documentSize.height() * 1.5);
    // Literal text and link destinations are content, not markup.
    QVERIFY(browser->toPlainText().contains(literal));
    QVERIFY(links(browser).contains("https://example.com/icon-16px.png"));
    view->setActivity(details, events);
    QCOMPARE(fontPixels(browser, "Agent body"), 28);
    view->setContentScale(0.75);
    QCOMPARE(fontPixels(browser, "Agent body"), 11);
    QCOMPARE(QFontInfo(queue->font()).pixelSize(), 10);
    QCOMPARE(QFontInfo(view->jumpButton()->font()).pixelSize(), jump);
    QVERIFY(browser->toPlainText().contains(literal));
    QVERIFY(links(browser).contains("https://example.com/icon-16px.png"));
    view->setContentScale(1.0);
    QCOMPARE(fontPixels(browser, "Agent body"), body);
    QCOMPARE(fontPixels(browser, "Plan heading"), heading);
    QCOMPARE(QFontInfo(queue->font()).pixelSize(), queued);
    QCOMPARE(browser->document()->size().toSize(), documentSize);
}

void TestActivityView::agentMarkdownLooksLikeGitHub()
{
    SettledActivityView view; view.resize(560, 600); view.show();
    view.setActivity({}, {journalEvent(1, "Stop", "Use `ctest` and:\n\n- one\n- two\n\n```\ncode\n```")});
    auto *document = view.browser()->document();
    QStringList images;
    for (auto block = document->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (format.isImageFormat()) images.append(format.toImageFormat().name());
        }
    QCOMPARE(codeRuns(document).size(), 1);
    QVERIFY(images.contains("hgs-md:disc/light/100")); QVERIFY(images.contains("hgs-md:corner-tl/light/100?fill=e8eef2"));
    for (const auto &name : images) QVERIFY(!document->resource(QTextDocument::ImageResource, QUrl(name)).value<QImage>().isNull());
    QVERIFY(view.plainText().contains(QString::fromUtf8("Use ctest and:\n• one\n• two\ncode")));
}

void TestActivityView::searchResultKeepsInlineCodeSearchable()
{
    SettledActivityView view; view.resize(560, 400); view.show();
    view.showSearchResult(journalEvent(1, "AgentMessage", "Run `ctest -R markdown` now"), "ctest");
    QCOMPARE(codeRuns(view.browser()->document()).size(), 1);
    QVERIFY(!view.browser()->document()->find("ctest -R markdown").isNull());
    QVERIFY(!view.browser()->extraSelections().isEmpty());
}

void TestActivityView::copyGivesVisibleText()
{
    SettledActivityView view; view.resize(560, 400); view.show();
    view.setActivity({}, {journalEvent(1, "Stop", "Edit `src/cli.rs`:\n\n1. first\n2. second")});
    view.browser()->selectAll(); view.browser()->copy();
    const auto copied = QGuiApplication::clipboard()->text();
    QVERIFY2(copied.contains("Edit src/cli.rs:\n1. first\n2. second"), qPrintable(copied));
    QVERIFY(!copied.contains(QChar::ObjectReplacementCharacter));
}

void TestActivityView::agentRepliesCopyAsMarkdown()
{
    SettledActivityView view; view.setSessionKey("arch/copy"); view.resize(560, 500); view.show();
    const QString reply = "## Done\n\n- `src/cli.rs` — **fixed**\n\n```sh\nctest -R activityview\n```";
    view.setActivity({{"last_message", "An older *recorded* response"}},
        {journalEvent(1, "UserPromptSubmit", "Fix the launcher"), journalEvent(2, "AgentThinking", "Considering the fix."),
         journalEvent(3, "Stop", reply)});
    auto *browser = view.browser(); auto *viewport = browser->viewport();
    // Replies only: not the request, not thinking. The recorded response is a reply too.
    QList<QTextCursor> buttons; QStringList images; QTextCursor label;
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next()) {
        if (block.text().startsWith("Agent") && !block.text().contains("recorded")) label = QTextCursor(block);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (!format.isImageFormat() || !format.toImageFormat().name().startsWith("hgs-ui:copy/light/100")) continue;
            buttons.append(QTextCursor(browser->document())); buttons.last().setPosition(it.fragment().position());
            images.append(format.toImageFormat().name());
        }
    }
    QCOMPARE(buttons.size(), 2);
    // Images, not links: Tab and Enter keep moving between the journal's toggles.
    QVERIFY(links(browser).isEmpty());
    const auto centre = [&](const QTextCursor &at) {
        QTextCursor after(at); after.setPosition(at.position() + 1);
        const auto rect = browser->cursorRect(at);
        return QPoint((rect.left() + browser->cursorRect(after).left()) / 2, rect.center().y());
    };

    // The button sits at the right end of the header line, beside the label.
    QVERIFY(!label.isNull());
    const auto icon = centre(buttons.last());
    QVERIFY2(qAbs(icon.y() - browser->cursorRect(label).center().y()) <= 4,
             qPrintable(QString("%1 vs %2").arg(icon.y()).arg(browser->cursorRect(label).center().y())));
    QVERIFY2(icon.x() > viewport->width() - 40, qPrintable(QString::number(icon.x())));
    // A copied selection reads as before: the button gives no text.
    QVERIFY(view.plainText().contains("Fix the launcher"));
    QVERIFY(!view.plainText().contains(QChar::ObjectReplacementCharacter));

    QTest::mouseMove(viewport, icon);
    QTRY_COMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseMove(viewport, icon - QPoint(80, 0));
    QTRY_VERIFY(viewport->cursor().shape() != Qt::PointingHandCursor);

    const auto resource = [&] { return browser->document()->resource(QTextDocument::ImageResource, QUrl(images.last())).value<QImage>(); };
    const auto idle = resource();
    QVERIFY(!idle.isNull());
    browser->setTextCursor(browser->document()->find("Fix the launcher"));
    const auto revision = browser->document()->revision();
    QGuiApplication::clipboard()->clear();
    QTest::mouseClick(viewport, Qt::LeftButton, {}, icon);
    QCOMPARE(QGuiApplication::clipboard()->text(), reply);
    // The pressed button confirms in place, without editing the document or the selection, then returns.
    QVERIFY(resource() != idle);
    QCOMPARE(browser->document()->revision(), revision);
    QCOMPARE(browser->textCursor().selectedText(), QString("Fix the launcher"));
    QTRY_VERIFY_WITH_TIMEOUT(resource() == idle, 3000);

    QTest::mouseClick(viewport, Qt::LeftButton, {}, centre(buttons.first()));
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("An older *recorded* response"));
    // Beside the button is ordinary text.
    QGuiApplication::clipboard()->setText("unchanged");
    QTest::mouseClick(viewport, Qt::LeftButton, {}, icon - QPoint(80, 0));
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("unchanged"));
}

void TestActivityView::markdownFollowsThemeAndScale()
{
    SettledActivityView view; view.resize(560, 400); view.show();
    view.setActivity({}, {journalEvent(1, "Stop", "- item with `code`")});
    view.setTheme(true); view.setContentScale(2.0);
    QStringList images;
    for (auto block = view.browser()->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (format.isImageFormat()) images.append(format.toImageFormat().name());
        }
    const auto chips = codeRuns(view.browser()->document());
    QVERIFY(images.contains("hgs-md:disc/dark/200"));
    QCOMPARE(chips.size(), 1);
    QCOMPARE(chips.first().property(MarkdownObjects::ChipScale).toDouble(), 2.0);
    const auto disc = view.browser()->document()->resource(QTextDocument::ImageResource, QUrl("hgs-md:disc/dark/200")).value<QImage>();
    QCOMPARE(disc.size(), (QSizeF(56, 24) * view.browser()->devicePixelRatioF()).toSize());
}

void TestActivityView::agentCardsKeepZerusSurface()
{
    // GitHub styling applies to the Markdown, not to the Zerus card around it.
    for (bool dark : {false, true}) {
        SettledActivityView view; view.resize(560, 400); view.setTheme(dark); view.show();
        view.setActivity({}, {journalEvent(1, "Stop", "Reply prose\n\n```\nblock text\n```")});
        const auto cellColour = [&](const QString &text) {
            for (auto block = view.browser()->document()->begin(); block.isValid(); block = block.next())
                if (block.text().contains(text)) {
                    const QTextCursor cursor(block);
                    return cursor.currentTable()->cellAt(cursor).format().background().color().name();
                }
            return QString();
        };
        QCOMPARE(cellColour("Reply prose"), QString(dark ? "#242d36" : "#f3f6f8"));
        QCOMPARE(cellColour("block text"), QString(dark ? "#171e25" : "#e8eef2"));
    }
}

void TestActivityView::mouseSelectsProseAndPartOfInlineCode()
{
    // Inline code is text: a drag from prose may end on any character of the code.
    SettledActivityView view; view.resize(560, 300); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setActivity({}, {journalEvent(1, "Stop", "Please run `ctest --output-on-failure` today.")});
    auto *browser = view.browser(); auto *document = browser->document();
    const QTextCursor prose = document->find("Please"), code = document->find("ctest");
    QVERIFY(!prose.isNull() && !code.isNull());
    QTextCursor from(document), to(document);
    from.setPosition(prose.selectionStart() + 3); to.setPosition(code.selectionStart() + 3);
    const QPoint start = browser->cursorRect(from).center(), end = browser->cursorRect(to).center();
    QTest::mousePress(browser->viewport(), Qt::LeftButton, {}, start);
    QMouseEvent move(QEvent::MouseMove, end, browser->viewport()->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(browser->viewport(), &move);
    QTest::mouseRelease(browser->viewport(), Qt::LeftButton, {}, end);
    QCOMPARE(browser->textCursor().selectedText(), QString("ase run cte"));
    browser->copy();
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("ase run cte"));
}

void TestActivityView::wrappedCodeIsDrawnAsARoundedChip()
{
    // Long code stays text so that it can wrap; the journal still draws its chip,
    // rounded and padded, on the card below the scrolled transcript.
    SettledActivityView view; view.resize(420, 300); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    QJsonArray events = history(12);
    events.append(journalEvent(13, "Stop", "Then run `cargo test --workspace --all-features --no-fail-fast` again."));
    view.setActivity({}, events);
    auto *browser = view.browser(); auto *bar = browser->verticalScrollBar();
    QTRY_VERIFY(bar->maximum() > 0); QTRY_COMPARE(bar->value(), bar->maximum());
    const QTextCursor found = browser->document()->find("cargo test");
    QVERIFY(!found.isNull());
    QTextCursor inside(found); inside.setPosition(found.selectionStart() + 1);
    const QFontMetricsF metrics(inside.charFormat().font());
    const qreal pixels = inside.charFormat().font().pixelSize();
    const QTextBlock block = found.block();
    const QTextLine line = block.layout()->lineForTextPosition(found.selectionStart() - block.position());
    const QPointF origin = browser->document()->documentLayout()->blockBoundingRect(block).topLeft() - QPointF(0, bar->value());
    const qreal left = origin.x() + line.cursorToX(found.selectionStart() - block.position()) - 0.4 * pixels;
    const qreal top = origin.y() + line.y() + line.ascent() - metrics.ascent() - 0.2 * pixels;
    const QImage image = browser->viewport()->grab().toImage();
    const auto pixel = [&](qreal x, qreal y) { return image.pixelColor(qFloor(x * image.devicePixelRatio()), qFloor(y * image.devicePixelRatio())); };
    const QColor card("#f3f6f8");
    const auto distance = [&](const QColor &colour) { return qAbs(colour.red() - card.red()) + qAbs(colour.green() - card.green()) + qAbs(colour.blue() - card.blue()); };
    QVERIFY2(distance(pixel(left + 0.5, top + 0.5)) <= 6, qPrintable(pixel(left + 0.5, top + 0.5).name()));
    QVERIFY2(distance(pixel(left + 6, top + 1)) >= 20, qPrintable(pixel(left + 6, top + 1).name()));
    QVERIFY2(distance(pixel(left + 1, top + 0.2 * pixels + metrics.ascent() / 2)) >= 20, "padding before the code");
}

void TestActivityView::resizingWithoutChipChangesKeepsTheJournal()
{
    // Toggling the session panel widens the conversation once. Inline code wraps
    // with its text, so a re-render would only shift a transcript the reader follows.
    SettledActivityView view; view.resize(864, 400); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    QJsonArray events;
    for (int i = 1; i <= 40; ++i) events.append(journalEvent(i, i % 2 ? "UserPromptSubmit" : "Stop",
        QString("Message %1 runs `cargo test` in `src/file-%1.rs` with enough words to wrap across the pane.").arg(i)));
    view.setActivity({}, events);
    auto *bar = view.browser()->verticalScrollBar(); QTRY_VERIFY(bar->maximum() > 0); QTest::qWait(200);
    QCOMPARE(bar->value(), bar->maximum());
    QSignalSpy replaced(view.browser()->document(), &QTextDocument::contentsChanged);
    view.resize(1110, 400); QTest::qWait(400);
    QCOMPARE(replaced.count(), 0);
    QCOMPARE(bar->value(), bar->maximum());
}

void TestActivityView::darkCardsUseNeutralTablesAndVisibleChips()
{
    // On the grey Zerus card, zebra rows are a step lighter (not the near-black code
    // surface) and inline code is dense enough to stand out.
    SettledActivityView view; view.resize(560, 400); view.setTheme(true); view.show();
    view.setActivity({}, {journalEvent(1, "Stop", "Use `x`.\n\n| A |\n|---|\n| one |\n| two |")});
    QString stripe; double chipAlpha = 0;
    for (auto block = view.browser()->document()->begin(); block.isValid(); block = block.next()) {
        if (block.text().contains("two")) {
            const QTextCursor cursor(block);
            stripe = cursor.currentTable()->cellAt(cursor).format().background().color().name();
        }
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (MarkdownObjects::isInlineCode(it.fragment().charFormat()))
                chipAlpha = it.fragment().charFormat().colorProperty(MarkdownObjects::ChipFill).alphaF();
    }
    QCOMPARE(stripe, QString("#2a333d"));
    QVERIFY2(chipAlpha >= 0.3, qPrintable(QString::number(chipAlpha)));
}

void TestActivityView::lastCardEndsTheJournal_data()
{
    QTest::addColumn<bool>("toolsLast");
    QTest::newRow("reply") << false; QTest::newRow("tools") << true;
}

void TestActivityView::lastCardEndsTheJournal()
{
    // Cards are spaced apart, but nothing pads the journal below the last one.
    QFETCH(bool, toolsLast);
    SettledActivityView view; view.resize(540, 320); view.show();
    QJsonArray events{journalEvent(1, "UserPromptSubmit", "Please inspect the source"), journalEvent(2, "Stop", "Done.")};
    if (toolsLast) events.append(journalEvent(3, "PostToolUse", "command completed", "Bash"));
    view.setActivity({}, events); QTest::qWait(20);
    auto *document = view.browser()->document(); auto *layout = document->documentLayout();
    // Qt follows each table with an empty block laid over its last line: that block ends
    // where the card visibly ends. The table's frame rectangle includes cell padding.
    QList<qreal> tops, bottoms; bool afterCard = false;
    for (auto it = document->rootFrame()->begin(); !it.atEnd(); ++it) {
        if (auto *frame = it.currentFrame()) { tops << layout->frameBoundingRect(frame).top(); afterCard = true; }
        else if (afterCard) { bottoms << layout->blockBoundingRect(it.currentBlock()).bottom(); afterCard = false; }
    }
    QCOMPARE(tops.size(), toolsLast ? 3 : 2);
    for (qsizetype i = 1; i < tops.size(); ++i) QVERIFY2(tops[i] - bottoms[i - 1] >= 7, qPrintable(QString("gap before card %1").arg(i)));
    const qreal tail = layout->documentSize().height() - bottoms.last();
    QVERIFY2(tail <= document->documentMargin() + 6, qPrintable(QString("%1 px below the last card").arg(tail)));
}

void TestActivityView::cardsKeepTheirSpacingAfterChips()
{
    // Qt lays out a long journal lazily. Converting chips must not leave later
    // paragraphs, such as the spacers between cards, without layout and height.
    SettledActivityView view; view.resize(540, 320); view.show();
    QJsonArray events{journalEvent(1, "Stop", "Reply with `inline code`.")};
    for (int i = 0; i < 40; ++i) {
        events.append(journalEvent(2 * i + 2, "UserPromptSubmit", QString("Request %1").arg(i)));
        events.append(journalEvent(2 * i + 3, "Stop", QString("Reply %1.").arg(i)));
    }
    view.setActivity({}, events);
    for (auto block = view.browser()->document()->begin(); block.isValid(); block = block.next())
        QVERIFY2(block.layout()->lineCount() > 0, qPrintable(QString("block %1 after \"%2\"")
            .arg(block.blockNumber()).arg(block.previous().text())));
}

void TestActivityView::contextCounterKeepsPhysicalRightAlignment()
{
    SessionUsage::ContextButton button; button.setTheme(false);
    button.setData({{"context",QJsonObject{{"used",1500},{"limit",10000}}}});
    button.show(); button.clearFocus(); QTest::qWait(10);
    const auto image=button.grab().toImage(); int rightmost=-1;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x) {
        const auto pixel=image.pixelColor(x,y);
        if(pixel.green()>pixel.red()+15 && pixel.green()>pixel.blue()+10) rightmost=qMax(rightmost,x);
    }
    QVERIFY2(rightmost>=image.width()-qCeil(12*image.devicePixelRatio()),"Context text must end at the physical right edge");
}

void TestActivityView::contextCounterNamesTheReportedWindowSource()
{
    SessionUsage::ContextButton button;
    button.setData({{"context",QJsonObject{{"used",250000},{"limit",1000000},{"limit_source","native_statusline"},
        {"limit_model","claude-opus-5-5[1m]"},{"limit_observed_at",1791018100}}}});
    QVERIFY(button.property("contextPercent").isValid());
    QVERIFY(button.toolTip().contains("Window size that Claude Code's status line reports for claude-opus-5-5[1m]."));
    // Without the window, the warning stays and no source is named.
    button.setData({{"context",QJsonObject{{"used",250000}}}});
    QVERIFY(!button.toolTip().contains("status line"));
    QVERIFY(button.toolTip().contains("The provider has not reported the window size."));
}

void TestActivityView::attachmentDeliveryKeepsThumbnailAndViewport()
{
    SettledActivityView view; view.resize(540,380); view.show(); view.setSessionKey("arch/session");
    const auto events = history(30);
    QJsonObject details{{"conversation_id","conversation"}};
    QJsonObject file{{"name","screenshot.png"},{"mime","image/png"},{"bytes",120},
        {"reference","[Image #1]"},{"local_path","/cached/screenshot.png"}};
    QJsonObject local{{"id","outgoing-1"},{"text","[Image #1] Review this image"},{"status","sending"},
        {"submitted_at",1791018100},{"attachments",QJsonArray{file}}};
    QSignalSpy previews(&view,&ActivityView::attachmentPreviewRequested);
    view.setTimeline(details,events,{local}); QTRY_COMPARE(previews.size(),1);
    const QString key=previews[0][0].toString();
    QImage thumbnail(192,128,QImage::Format_ARGB32); thumbnail.fill(Qt::red);
    view.setAttachmentPreview(key,thumbnail);
    auto *browser=view.browser(); auto *bar=browser->verticalScrollBar();
    QTRY_COMPARE(bar->value(),bar->maximum());
    const auto top=browser->cursorForPosition(QPoint(2,2));
    const auto topText=top.block().text(); const auto topY=browser->cursorRect(top).top();
    // Read the sent card while its acknowledgement and native journal arrive.
    auto selection=browser->document()->find("Review this image");browser->setTextCursor(selection);
    local["status"]="sent";local["message_id"]="delivery-1";file["request_id"]="delivery-1";file["index"]=0;
    local["attachments"]=QJsonArray{file};
    view.setTimeline(details,events,{local});
    QCOMPARE(browser->textCursor().selectedText(),QString("Review this image"));
    file.remove("local_path");
    details["attachment_messages"]=QJsonArray{QJsonObject{{"type","UserPromptSubmit"},{"source","hgs_delivery"},
        {"message_id","delivery-1"},{"at",1791018100},{"detail",local["text"]},{"submitted_text",local["text"]},
        {"attachments",QJsonArray{file}}}};
    view.setTimeline(details,events,{});
    QCOMPARE(browser->textCursor().selectedText(),QString("Review this image"));
    auto nativeEvents=events;nativeEvents.append(QJsonObject{{"seq",31},{"at",1791018101},{"type","UserPromptSubmit"},{"detail",local["text"]}});
    view.setTimeline(details,nativeEvents,{});
    QTest::qWait(20);
    QCOMPARE(previews.size(),1);
    QCOMPARE(browser->textCursor().selectedText(),QString("Review this image"));
    const auto restoredTop=browser->cursorForPosition(QPoint(2,2));
    QCOMPARE(restoredTop.block().text(),topText);QCOMPARE(browser->cursorRect(restoredTop).top(),topY);
    QCOMPARE(browser->toPlainText().count("Review this image"),1);
    const auto resource=browser->document()->resource(QTextDocument::ImageResource,QUrl("hgs-thumbnail:"+key)).value<QImage>();
    QVERIFY(!resource.isNull());QCOMPARE(resource.pixelColor(30,30),QColor(Qt::red));
    QSignalSpy opened(&view,&ActivityView::attachmentActivated);
    activate(browser,"hgs-attachment:"+key);QCOMPARE(opened.size(),1);
    QCOMPARE(opened[0][0].toJsonObject(),file); // Opening uses the new durable reference.
}

void TestActivityView::nativeQueueStaysSeparateFromHistoryAndTracksIdentity()
{
    SettledActivityView view; view.resize(600,550); view.setSessionKey("queue-session"); view.show();
    auto events = history(15); QJsonObject details{{"conversation_id","conversation"}};
    view.setActivity(details,events);
    auto *send = view.findChild<QPushButton *>("queueSendNow");
    auto *panel = view.findChild<QWidget *>("activityInputQueue"); QVERIFY(!panel->isVisible());
    auto queue = QJsonObject{{"id","queue-1"},{"text","❯ first queued message"},{"can_send_now",true},{"hint","Native action"}};
    details["input_queue"] = queue; view.setActivity(details,events); QVERIFY(panel->isVisible()); QVERIFY(send->isEnabled());
    auto *browser = view.browser(); const auto revision = browser->document()->revision();
    browser->setTextCursor(browser->document()->find("Message number 1"));
    QSignalSpy sent(&view,&ActivityView::queueSendNowRequested);
    send->click(); QCOMPARE(sent.size(),1); QCOMPARE(sent[0][0].toString(),QString("queue-1"));
    queue["id"]="queue-2";queue["text"]="❯ newer queue";details["input_queue"]=queue;
    view.setActivity(details,events); QCOMPARE(browser->document()->revision(),revision);
    QCOMPARE(browser->textCursor().selectedText(),QString("Message number 1"));
    send->click(); QCOMPARE(sent[1][0].toString(),QString("queue-2"));
    details["queue_sending"]=true;view.setActivity(details,events);QVERIFY(!send->isEnabled());
    details.remove("input_queue");view.setActivity(details,events);QVERIFY(!panel->isVisible());
    QVERIFY(browser->toPlainText().contains("Sent"));
    view.setActivity({{"input_queue",queue}},events);view.setSessionKey("another");QVERIFY(!panel->isVisible());
}

void TestActivityView::updatesRemainPaintableBeforeTheNextEventLoop()
{
    class PaintCounter : public QObject {
    public:
        int paints = 0;
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Paint) ++paints;
            return false;
        }
    } counter;
    SettledActivityView view; view.resize(520, 340); view.show(); view.setSessionKey("arch/session");
    auto events = history(24); view.setActivity({{"conversation_id", "one"}}, events);
    QTest::qWait(30);
    auto *browser = view.browser(); browser->viewport()->installEventFilter(&counter);
    browser->verticalScrollBar()->setValue(0);
    auto selection = browser->document()->find("Message number 1"); browser->setTextCursor(selection);
    const auto selected = browser->textCursor().selectedText();
    const int scroll = browser->verticalScrollBar()->value();
    for (int i = 25; i <= 28; ++i) {
        events.append(journalEvent(i, "AgentMessage", QString("New response %1").arg(i)));
        view.setActivity({{"conversation_id", "one"}}, events);
        // Another widget can repaint the parent before queued callbacks run.
        // Activity must participate in that frame, never leave a blank rectangle.
        counter.paints = 0; browser->viewport()->repaint();
        QVERIFY2(counter.paints > 0, "Activity disappeared from the parent repaint while its update was pending");
        QCOMPARE(browser->textCursor().selectedText(), selected);
        QCOMPARE(browser->verticalScrollBar()->value(), scroll);
        QVERIFY(browser->toPlainText().contains(QString("New response %1").arg(i)));
    }
}

void TestActivityView::jumpOverlayPreservesViewportAndReadingPosition()
{
    SettledActivityView view;view.resize(700,420);view.show();view.setSessionKey("overlay");
    auto events=history(24);view.setActivity({},events);
    auto *browser=view.browser();auto *bar=browser->verticalScrollBar();
    auto *latest=view.findChild<QPushButton *>("activityJumpLatest");
    QTRY_VERIFY(bar->maximum()>0);QTRY_COMPARE(bar->value(),bar->maximum());
    const auto viewport=browser->viewport()->geometry();
    bar->setValue(bar->maximum()/2);QTRY_VERIFY(latest->isVisible());
    QCOMPARE(browser->viewport()->geometry(),viewport);
    auto cursor=browser->cursorForPosition(QPoint(8,8));const int position=cursor.position();const auto reading=browser->cursorRect(cursor).top();
    events.append(journalEvent(100,"AgentMessage","One new reply"));view.setActivity({},events);QTest::qWait(20);
    cursor=QTextCursor(browser->document());cursor.setPosition(position);
    QCOMPARE(browser->viewport()->geometry(),viewport);QCOMPARE(browser->cursorRect(cursor).top(),reading);
    QVERIFY(latest->text().contains("1 new"));
    for (const auto size:{QSize(430,310),QSize(820,500)}) {
        view.resize(size);QTest::qWait(20);
        const auto area=QRect(browser->viewport()->mapTo(&view,QPoint()),browser->viewport()->size());
        const auto button=QRect(latest->mapTo(&view,QPoint()),latest->size());
        QVERIFY(area.contains(button));QVERIFY(qAbs(area.center().x()-button.center().x())<=1);
        QCOMPARE(area.bottom()-button.bottom(),8);
    }
    const auto preview=qEnvironmentVariable("HGS_JUMP_PREVIEW");if(!preview.isEmpty()){view.setTheme(true);QTest::qWait(20);QVERIFY(view.grab().save(preview));}
    const auto beforeJump=browser->viewport()->geometry();latest->click();QTest::qWait(20);
    QVERIFY(latest->isHidden());QCOMPARE(browser->viewport()->geometry(),beforeJump);QCOMPARE(bar->value(),bar->maximum());
}

void TestActivityView::jumpToLatestKeepsFocusInActivity_data()
{
    QTest::addColumn<bool>("keyboard");
    QTest::addColumn<bool>("search");
    QTest::newRow("mouse") << false << false;
    QTest::newRow("keyboard") << true << false;
    QTest::newRow("mouse-from-search") << false << true;
    QTest::newRow("keyboard-from-search") << true << true;
}

void TestActivityView::jumpToLatestKeepsFocusInActivity()
{
    QFETCH(bool, keyboard); QFETCH(bool, search);
    WorkspaceFocus::install();
    QWidget window; QVBoxLayout layout(&window);
    SettledActivityView view; SessionUsage::ContextButton context; QLineEdit composer;
    context.setData({{"context", QJsonObject{{"used", 220000}, {"limit", 1000000}}}});
    layout.addWidget(&view, 1); layout.addWidget(&context); layout.addWidget(&composer);
    window.resize(520, 420); window.show(); window.activateWindow();
    QTRY_VERIFY(window.isActiveWindow());
    view.setSessionKey("arch/session"); view.setActivity({}, history(24));
    auto *browser = view.browser(); auto *bar = browser->verticalScrollBar();
    auto *latest = view.findChild<QPushButton *>("activityJumpLatest");
    QTRY_VERIFY(bar->maximum() > 0); QTRY_COMPARE(bar->value(), bar->maximum());
    if (search) view.showSearchResult(journalEvent(101, "AgentMessage", "Saved reply"), "reply");
    else bar->setValue(0);
    QTRY_VERIFY(latest->isVisible());
    if (keyboard) {
        latest->setFocus(Qt::TabFocusReason);
        QVERIFY(latest->hasFocus());
        QTest::keyClick(latest, Qt::Key_Space);
    } else QTest::mouseClick(latest, Qt::LeftButton);
    QTRY_VERIFY(latest->isHidden()); QTRY_COMPARE(bar->value(), bar->maximum());
    QVERIFY(browser->hasFocus()); QVERIFY(!context.hasFocus());
    QVERIFY(!browser->textCursor().hasSelection());

    // Context remains reachable via Tab, with a keyboard-only focus ring.
    QTest::keyClick(browser, Qt::Key_Tab);
    QVERIFY(context.hasFocus()); QVERIFY(context.property("keyboardFocus").toBool());
    QTest::mouseClick(&context, Qt::LeftButton);
    QVERIFY(!context.property("keyboardFocus").toBool());

    // Automatic following must not take focus away from a message draft.
    composer.setFocus(Qt::MouseFocusReason);
    view.setActivity({}, history(26));
    QTRY_COMPARE(bar->value(), bar->maximum()); QVERIFY(composer.hasFocus());
}

void TestActivityView::sessionClearIsAColoredBoundaryAndSurvivesReload()
{
    SettledActivityView view; view.resize(560, 420); view.show(); view.setSessionKey("codex/session");
    auto clear=journalEvent(3,"SessionCleared",{});clear["activity_key"]="confirmed-clear";
    const QJsonObject details{{"conversation_id","old"},{"session_clear",clear}};
    const QJsonArray events{journalEvent(1,"Stop","Previous answer"),journalEvent(2,"PostToolUse","Before clear","Read"),
        clear,journalEvent(4,"PostToolUse","After clear","Read")};
    for (bool dark:{false,true}) {
        view.setTheme(dark);view.setActivity(details,events,"Old request snapshot");
        const auto text=view.plainText();QCOMPARE(text.count("Session cleared"),1);
        QVERIFY(text.indexOf("Previous answer")<text.indexOf("Session cleared"));
        QVERIFY(!text.contains("Old request snapshot"));
        QVERIFY(!links(view.browser()).contains("hgs-toggle:group-confirmed-clear"));
        const auto cursor=view.browser()->document()->find("Session cleared");QVERIFY(!cursor.isNull());
        // Amber marks the boundary for attention; green stays with your messages.
        QCOMPARE(cursor.charFormat().foreground().color(),QColor(dark?"#edbd77":"#9b6216"));
        const auto revision=view.browser()->document()->revision();view.setActivity(details,events);
        QCOMPARE(view.browser()->document()->revision(),revision);
    }
    // The durable marker keeps the notice on a cold reader even after the
    // operation has fallen outside the bounded tool-event window.
    view.setSessionKey("codex/reopened");view.setActivity(details,{journalEvent(4,"PostToolUse","Later tool","Read")});
    QCOMPARE(view.plainText().count("Session cleared"),1);
    view.setSessionKey("codex/unrelated");view.setActivity({},{journalEvent(1,"SessionStart",{})});
    QVERIFY(!view.plainText().contains("Session cleared"));
    // The new conversation continues the same timeline. A clear first seen in
    // Terminal and confirmed by the later hook is still one boundary.
    view.setSessionKey("codex/continued");
    auto earlier=journalEvent(5,"PostToolUse","Earlier tool","Read");earlier["agent_id"]="";
    view.setActivity({{"conversation_id","old"}},{journalEvent(4,"Stop","Earlier answer"),earlier});
    QVERIFY(links(view.browser()).contains("hgs-activity:group-5"));activate(view.browser(),"hgs-activity:group-5");
    QVERIFY(view.plainText().contains("▾ Tool finished"));
    auto terminal=journalEvent(6,"SessionCleared",{});terminal["activity_key"]="clear:run:old:6";terminal["run_id"]="run";
    auto hook=terminal;hook["activity_key"]="clear:run:new:6";hook["seq"]=7;
    const QJsonObject cleared{{"conversation_id","new"},{"cleared_conversations",QJsonArray{"old"}},{"session_clear",hook}};
    view.setActivity(cleared,{journalEvent(4,"Stop","Earlier answer"),earlier,terminal,hook,journalEvent(8,"Stop","Fresh answer")});
    const auto continued=view.plainText();QCOMPARE(continued.count("Session cleared"),1);
    QVERIFY(continued.indexOf("Earlier answer")<continued.indexOf("Session cleared"));
    QVERIFY(continued.indexOf("Session cleared")<continued.indexOf("Fresh answer"));
    QVERIFY(continued.contains("▾ Tool finished"));
    const auto preview=qEnvironmentVariable("HGS_CLEAR_PREVIEW");
    if(!preview.isEmpty()) {
        view.setSessionKey("codex/session");
        for(bool dark:{false,true}) {view.setTheme(dark);view.setActivity(details,events);QTest::qWait(50);QVERIFY(view.grab().save(preview+(dark?"-dark.png":"-light.png")));}
    }
}

void TestActivityView::liveCompactionProgressPreservesTranscriptAndClears()
{
    SettledActivityView view; view.resize(520, 380); view.show(); view.setSessionKey("kimi/session");
    QJsonObject details{{"runtime_state","live"},{"process_state","running"},{"phase","working"}};
    const QJsonArray events{journalEvent(1,"UserPromptSubmit","Review this project"), journalEvent(2,"Stop","Previous response")};
    view.setActivity(details, events);
    auto *row = view.findChild<QWidget *>("activityCompaction"); QVERIFY(row); QVERIFY(row->isHidden());
    const auto revision = view.browser()->document()->revision();
    auto cursor = view.browser()->document()->find("Previous response"); view.browser()->setTextCursor(cursor);
    details["phase"] = "compacting"; view.setActivity(details, events);
    QVERIFY(!row->isHidden()); QCOMPARE(view.browser()->document()->revision(), revision);
    QCOMPARE(view.browser()->textCursor().selectedText(), QString("Previous response"));
    auto *progress=view.findChild<QProgressBar *>("compactionProgress"); QVERIFY(progress);
    QCOMPARE(progress->minimum(),0); QCOMPARE(progress->maximum(),0); QVERIFY(!progress->isTextVisible());
    QCOMPARE(view.findChild<QLabel *>("compactionCaption")->text(), QString("Compacting context…"));
    const auto preview=qEnvironmentVariable("HGS_COMPACTION_PREVIEW");
    if(!preview.isEmpty()) for(bool dark:{false,true}) {view.setTheme(dark);QTest::qWait(80);QVERIFY(view.grab().save(preview+(dark?"-dark.png":"-light.png")));}
    view.showSearchResult(events[0].toObject(), "Review"); QVERIFY(row->isHidden());
    view.clearSearchResult(); QVERIFY(!row->isHidden());
    for(const auto phase:{"working","tool","idle","error"}) {details["phase"]=phase;view.setActivity(details,events);QVERIFY(row->isHidden());}
    details["phase"]="compacting";
    for(const auto runtime:{"stopped","unavailable"}) {details["runtime_state"]=runtime;view.setActivity(details,events);QVERIFY(row->isHidden());}
    details["runtime_state"]="live";details["state"]="archived";view.setActivity(details,events);QVERIFY(row->isHidden());
    details.remove("state");details["process_state"]="exited";view.setActivity(details,events);QVERIFY(row->isHidden());
    details["process_state"]="running";view.setActivity(details,events);QVERIFY(!row->isHidden());
    view.setSessionKey("other/session");QVERIFY(row->isHidden());
}

void TestActivityView::searchResultIsolatedFromLiveUpdatesAndReturnsToLatest()
{
    SettledActivityView view; view.resize(520, 340); view.show(); view.setSessionKey("arch/session");
    view.setActivity({{"conversation_id", "one"}, {"last_message", "Latest live response"}}, history(24));
    view.setLocalMessages({QJsonObject{{"request_id", "draft"}, {"text", "Pending local message"}, {"state", "sending"}}});
    QTest::qWait(20);
    auto saved = journalEvent(101, "AgentMessage", "Saved **NEEDLE** response from history.");
    view.showSearchResult(saved, "needle");
    auto *browser = view.browser(); auto *latest = view.findChild<QPushButton *>("activityJumpLatest");
    QVERIFY(browser->toPlainText().contains("Saved NEEDLE response"));
    QVERIFY(!browser->toPlainText().contains("Latest live response"));
    QVERIFY(!browser->toPlainText().contains("Pending local message"));
    QVERIFY(!browser->toPlainText().contains("Message number"));
    QVERIFY(!browser->extraSelections().isEmpty());
    QCOMPARE(browser->extraSelections().first().cursor.selectedText(), "NEEDLE");
    QVERIFY(latest->isVisible()); QVERIFY(latest->text().contains("Back to latest"));
    // Polling keeps the saved message stable while the live timeline advances behind it.
    const int revision = browser->document()->revision();
    view.setActivity({{"conversation_id", "one"}, {"last_message", "New live response"}}, history(26));
    QCOMPARE(browser->document()->revision(), revision);
    QVERIFY(browser->toPlainText().contains("Saved NEEDLE response"));
    latest->click(); settle(view);
    QVERIFY(browser->toPlainText().contains("Message number 26"));
    QVERIFY(browser->toPlainText().contains("Pending local message"));
    QVERIFY(!browser->toPlainText().contains("Saved NEEDLE response"));
    QVERIFY(browser->extraSelections().isEmpty());
    QTRY_COMPARE(browser->verticalScrollBar()->value(), browser->verticalScrollBar()->maximum());
}
void TestActivityView::searchResultClearsOnConversationChange()
{
    SettledActivityView view; view.resize(520, 340); view.show(); view.setSessionKey("arch/session");
    view.setActivity({{"conversation_id", "one"}}, history(12));
    view.showSearchResult(journalEvent(100, "AgentMessage", "Needle from old conversation"), "needle");
    QVERIFY(view.browser()->toPlainText().contains("old conversation"));
    view.setActivity({{"conversation_id", "two"}}, history(14));
    QVERIFY(!view.browser()->toPlainText().contains("old conversation"));
    QVERIFY(view.browser()->extraSelections().isEmpty());
    QVERIFY(view.browser()->toPlainText().contains("Message number 14"));
    QTRY_COMPARE(view.browser()->verticalScrollBar()->value(), view.browser()->verticalScrollBar()->maximum());
    view.showSearchResult(journalEvent(100, "AgentMessage", "Needle from previous session"), "needle");
    view.setSessionKey("mac/another"); view.setActivity({}, history(2));
    QVERIFY(!view.browser()->toPlainText().contains("previous session"));
    QVERIFY(view.browser()->extraSelections().isEmpty());
}
void TestActivityView::searchToolExcerptIsExpandedAndClearlyMarked()
{
    SettledActivityView view; view.setSessionKey("arch/session"); view.setActivity({}, history(2));
    auto event = journalEvent(99, "PostToolUse", "A long log excerpt containing needle", "Bash");
    event["content_truncated"] = true;
    view.showSearchResult(event, "needle");
    QVERIFY(view.browser()->toPlainText().contains("A long log excerpt containing needle"));
    QVERIFY(view.browser()->toPlainText().contains("Excerpt around the match"));
    QVERIFY(view.browser()->toPlainText().contains("too long to display in full"));
    QVERIFY(!view.browser()->extraSelections().isEmpty());
    view.clearSearchResult();
    QVERIFY(!view.browser()->toPlainText().contains("Excerpt around the match"));
    QVERIFY(view.browser()->extraSelections().isEmpty());
}

void TestActivityView::kimiWireAnswerReplacesEmptyStop()
{
    SettledActivityView view; view.resize(520, 340); view.show(); view.setSessionKey("kimi/project");
    const QJsonArray events{journalEvent(1, "TurnStarted", "Please push"),
        journalEvent(2, "PostToolUse", "git push", "Bash"), journalEvent(3, "Stop", "")};
    const auto reply = QJsonObject{{"type", "AgentMessage"}, {"source", "kimi_wire"},
        {"at", 1791018003.0}, {"detail", "All pushed. **343 tests passed.**"}};
    view.setActivity({{"provider_messages", QJsonArray{reply}}}, events);
    const auto text = view.browser()->toPlainText();
    QCOMPARE(text.count("All pushed."), 1);
    QVERIFY(text.indexOf("All pushed.") > text.indexOf("Please push"));
    QVERIFY(!text.contains("Outside the available timeline"));
    view.setActivity({{"provider_messages", QJsonArray{reply}}}, events);
    QCOMPARE(view.browser()->toPlainText().count("All pushed."), 1);
    QTRY_COMPARE(view.browser()->verticalScrollBar()->value(), view.browser()->verticalScrollBar()->maximum());
}

void TestActivityView::progressCommentaryAppearsBeforeFinalResponse()
{
    for (const QString &source : {QString("codex_transcript"),QString("claude_transcript"),QString("kimi_wire")}) {
        SettledActivityView view; view.setSessionKey(source);
        auto progress=journalEvent(0,"AgentMessage","Checking the deployment.");progress["at"]=1791018002.0;progress["source"]=source;
        auto final=journalEvent(0,"AgentMessage","Deployment complete.");final["at"]=1791018004.0;final["source"]=source;
        QJsonArray hooks{journalEvent(1,"UserPromptSubmit","Deploy"),journalEvent(3,"PostToolUse","Check services","Bash")};
        view.setActivity({{"provider_messages",QJsonArray{progress}}},hooks);
        QCOMPARE(view.browser()->toPlainText().count("Checking the deployment."),1);
        hooks.append(journalEvent(4,"Stop","Deployment complete."));
        view.setActivity({{"provider_messages",QJsonArray{progress,final}}},hooks);
        const auto text=view.browser()->toPlainText();
        QCOMPARE(text.count("Checking the deployment."),1);QCOMPARE(text.count("Deployment complete."),1);
        QVERIFY(text.indexOf("Checking the deployment.")<text.indexOf("Deployment complete."));
    }
}

void TestActivityView::claudeThinkingStaysSeparateFromToolsAndReplies()
{
    SettledActivityView view;view.setSessionKey("claude/thinking");view.resize(700,500);view.show();
    const auto body=QString("Read all 24 pages. ")+QString("More recorded thinking. ").repeated(10)+"END_OF_THINKING";
    auto thinking=journalEvent(0,"AgentThinking",body);thinking["source"]="claude_transcript";thinking["message_id"]="thinking-one";thinking["at"]=1791018002.0;
    QJsonArray hooks{journalEvent(1,"PreToolUse","Read report","Read"),journalEvent(3,"PostToolUse","Inspect source","Bash"),journalEvent(4,"Stop","")};
    const QJsonObject details{{"provider_messages",QJsonArray{thinking}}};view.setActivity(details,hooks);
    auto *browser=view.browser();const auto text=browser->toPlainText();
    // Shown in full like a reply, between the tools, without a toggle.
    QVERIFY(text.contains("Thinking"));QCOMPARE(text.count("Read all 24 pages."),1);QCOMPARE(text.count("END_OF_THINKING"),1);
    QVERIFY(text.indexOf("Thinking")>text.indexOf("Read report"));QVERIFY(text.indexOf("Thinking")<text.indexOf("Bash"));
    for(const auto &link:links(browser))QVERIFY2(!link.contains("thinking-one"),qPrintable(link));
    // Quieter than a reply: page background instead of a card fill, dimmer text at full size.
    QTextCursor cursor(browser->document()->find("END_OF_THINKING"));
    QVERIFY(!cursor.isNull());QVERIFY(cursor.currentTable());
    QCOMPARE(cursor.currentTable()->cellAt(cursor).format().background().color().name(),QString("#ffffff"));
    QCOMPARE(cursor.charFormat().foreground().color().name(),QString("#424a53"));
    QVERIFY(QFontInfo(cursor.charFormat().font()).pixelSize()>=13);
    view.setActivity(details,hooks);QCOMPARE(browser->toPlainText().count("END_OF_THINKING"),1);
    const auto preview=qEnvironmentVariable("HGS_THINKING_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(view.grab().save(preview+"/claude-thinking.png"));
        view.setTheme(true);view.setActivity(details,hooks);QTest::qWait(40);QVERIFY(view.grab().save(preview+"/claude-thinking-dark.png"));}
}

void TestActivityView::kimiCommentaryDoesNotReplaceEmptyStop()
{
    SettledActivityView view; view.setSessionKey("kimi/project");
    auto started = journalEvent(2, "PreToolUse", "Writing the file", "Write");
    started["at"] = 1791018002.5;
    const QJsonArray events{journalEvent(1, "TurnStarted", "Build the file"), started,
        journalEvent(3, "PostToolUse", "Wrote the file", "Write"), journalEvent(4, "Stop", "")};
    const QJsonObject progress{{"type", "AgentMessage"}, {"source", "kimi_wire"},
        {"message_id", "kimi-step:progress"}, {"message_phase", "commentary"},
        {"at", 1791018002.0}, {"detail", "Now writing the HTML file."}};
    const QJsonObject final{{"type", "AgentMessage"}, {"source", "kimi_wire"},
        {"message_id", "kimi-step:final"}, {"message_phase", "final"},
        {"at", 1791018003.9}, {"detail", "File ready."}};
    for (const QJsonArray &messages : {QJsonArray{progress}, QJsonArray{progress, final}}) {
        view.setActivity({{"provider_messages", messages}}, events);
        const auto text = view.browser()->toPlainText();
        QCOMPARE(text.count("Now writing the HTML file."), 1);
        QVERIFY(text.indexOf("Now writing") < text.indexOf("1 tool call"));
        if (messages.size() == 2) {
            QCOMPARE(text.count("File ready."), 1);
            QVERIFY(text.indexOf("1 tool call") < text.indexOf("File ready."));
            QVERIFY(!text.contains("Response finished"));
        }
    }
}

void TestActivityView::emptyPromptHooksDoNotCreateCards()
{
    SettledActivityView view;
    QJsonArray events{journalEvent(1, "UserPromptSubmit", ""),
        journalEvent(2, "TurnStarted", "Please inspect this"),
        journalEvent(3, "StopFailure", "Provider failed"),
        journalEvent(4, "UserPromptSubmit", " \n "),
        journalEvent(5, "TurnStarted", "Try again")};
    view.setActivity({}, events);
    auto text = view.browser()->toPlainText();
    QVERIFY(!text.contains("UserPromptSubmit"));
    QCOMPARE(text.count("Please inspect this"), 1);
    QCOMPARE(text.count("Try again"), 1);
    QVERIFY(text.contains("Response failed"));
    QVERIFY(!links(view.browser()).contains("hgs-activity:group-1"));
    QVERIFY(!links(view.browser()).contains("hgs-activity:group-4"));
    // Submission hooks with actual content still supply the user message.
    auto attachment = journalEvent(6, "UserPromptSubmit", "");
    attachment["attachments"] = QJsonArray{QJsonObject{{"name", "screen.png"}}};
    events.append(attachment);
    events.append(journalEvent(7, "UserPromptSubmit", "A regular prompt"));
    view.setActivity({}, events);
    text = view.browser()->toPlainText();
    QCOMPARE(text.count("screen.png"), 1);
    QCOMPARE(text.count("A regular prompt"), 1);
    QVERIFY(!text.contains("UserPromptSubmit"));
}

void TestActivityView::providerErrorsStayVisibleAndStable_data()
{
    QTest::addColumn<QString>("source");
    QTest::newRow("native-log")<<QString("codex_native_log");
    QTest::newRow("native-completion")<<QString("codex_native_transcript");
}
void TestActivityView::providerErrorsStayVisibleAndStable()
{
    QFETCH(QString,source);
    SettledActivityView view;view.setSessionKey("codex/project/session");
    const QJsonObject error{{"type","StopFailure"},{"source",source},{"message_id","codex-error-1"},
        {"at",1791018002.5},{"detail","Selected model is at capacity. Please try a different model."}};
    const QJsonObject details{{"provider_errors",QJsonArray{error,error}}};
    QJsonArray events{journalEvent(1,"UserPromptSubmit","Start"),journalEvent(2,"PostToolUse","Success","Bash")};
    view.setActivity(details,events);
    auto text=view.browser()->toPlainText();QCOMPARE(text.count("Selected model is at capacity."),1);
    QVERIFY(text.contains("Response failed"));QVERIFY(text.indexOf("Selected model")>text.indexOf("Start"));
    const auto revision=view.browser()->document()->revision();view.setActivity(details,events);
    QCOMPARE(view.browser()->document()->revision(),revision);
    events.append(journalEvent(3,"UserPromptSubmit","Retry"));view.setActivity(details,events);
    text=view.browser()->toPlainText();QCOMPARE(text.count("Selected model is at capacity."),1);
    QVERIFY(text.indexOf("Selected model")<text.indexOf("Retry"));
    view.setSessionKey("codex/other");view.setActivity({},{});QVERIFY(!view.browser()->toPlainText().contains("Selected model"));
}

void TestActivityView::voiceMessageRetainsAudioAndExpandableTranscript()
{
    SettledActivityView view;view.setSessionKey("fixture/conversation");
    const QString id="aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa";
    const QJsonObject audio{{"request_id",id},{"index",0},{"name","voice.wav"},{"mime","audio/wav"},{"bytes",32044},{"duration_ms",1000}};
    QJsonObject voice{{"id",id},{"run_id","run"},{"conversation_id","conversation"},{"state","transcribing"},{"audio",audio}};
    QJsonObject row{{"type","UserMessage"},{"source","hgs_voice"},{"message_id",id},{"request_id",id},{"at",1791018002.0},{"voice",voice},{"attachments",QJsonArray{audio}}};
    view.setActivity({{"voice_messages",QJsonArray{row}}},{});
    auto imageLabel=[&](const QString &label){
        for(auto block=view.browser()->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it)
            if(it.fragment().charFormat().property(QTextFormat::ImageAltText).toString()==label)return true;
        return false;
    };
    auto text=view.browser()->toPlainText();QVERIFY(text.contains("You (voice)"));QVERIFY(!text.contains("Recognizing"));QVERIFY(imageLabel("Recognizing…"));
    bool accessiblePlayer=false;
    for(auto block=view.browser()->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it)
        accessiblePlayer|=it.fragment().charFormat().property(QTextFormat::ImageAltText).toString().contains("Play or pause voice message (0:01)");
    QVERIFY(accessiblePlayer);
    QSignalSpy played(&view,&ActivityView::voicePlaybackRequested);QString play;
    for(const auto &url:links(view.browser()))if(url.startsWith("hgs-voice:"))play=url;
    QVERIFY(!play.isEmpty());activate(view.browser(),play);QCOMPARE(played.count(),1);QCOMPARE(played[0][1].toJsonObject(),audio);
    // Mock attachment retrieval failure: no real media output, still actionable.
    view.playVoice(played[0][0].toString(),{},"Synthetic audio unavailable");settle(view);
    QVERIFY(view.browser()->toPlainText().contains("Synthetic audio unavailable"));
    voice["state"]="submitted";voice["transcript"]="Recognized voice fixture";row["voice"]=voice;row["detail"]="Recognized voice fixture";
    auto local=row;local["id"]="voice-local";local["submitted_at"]=1791018002.0;local["status"]="sent";
    view.setTimeline({{"voice_messages",QJsonArray{row}}},{row,journalEvent(3,"UserPromptSubmit","Recognized voice fixture")},{local});
    text=view.browser()->toPlainText();QCOMPARE(text.count("You (voice)"),1);QVERIFY(!text.contains("Show transcript"));QVERIFY(imageLabel("Show transcript"));QVERIFY(imageLabel("Sent to agent"));
    // Similar native text without an exact correlation remains a separate message.
    QCOMPARE(text.count("Recognized voice fixture"),1);
    QString toggle;for(const auto &url:links(view.browser()))if(url.startsWith("hgs-activity:voice-text-"))toggle=url;
    QVERIFY(!toggle.isEmpty());activate(view.browser(),toggle);settle(view);
    QCOMPARE(view.browser()->toPlainText().count("Recognized voice fixture"),2);QVERIFY(imageLabel("Hide transcript"));
    voice["native_binding"]=QJsonObject{{"journal_seq",3}};row["voice"]=voice;
    view.setTimeline({{"voice_messages",QJsonArray{row}},{"prompt","Recognized voice fixture"}},{journalEvent(3,"UserPromptSubmit","Recognized voice fixture")},{});
    QCOMPARE(view.browser()->toPlainText().count("You (voice)"),1);QCOMPARE(view.browser()->toPlainText().count("Recognized voice fixture"),1);
    view.setSessionKey("fixture/other");view.playVoice(played[0][0].toString(),{},"Stale recording error");settle(view);
    QVERIFY(!view.browser()->toPlainText().contains("Stale recording error"));
}

void TestActivityView::videoMessageIsOneCircleAndLoadsOnlyAfterTap()
{
    ActivityView view;view.resize(700,500);view.setSessionKey("fixture/video");view.show();
    const QString id="aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa";
    const QJsonObject audio{{"request_id",id},{"index",0},{"name","voice.wav"},{"mime","audio/wav"},{"bytes",32044},{"duration_ms",1000}};
    const QJsonObject video{{"request_id",id},{"index",1},{"name","video.mp4"},{"mime","video/mp4"},{"bytes",4000},{"duration_ms",1000},{"width",480},{"height",480}};
    const QJsonObject voice{{"id",id},{"run_id","run"},{"conversation_id","conversation"},{"state","submitted"},{"audio",audio},{"video",video},{"transcript","Synthetic video speech"}};
    const QJsonObject row{{"type","UserMessage"},{"source","hgs_voice"},{"message_id",id},{"request_id",id},{"at",1791018002.0},{"voice",voice},{"attachments",QJsonArray{audio,video}}};
    QSignalSpy played(&view,&ActivityView::voicePlaybackRequested);
    view.setActivity({{"voice_messages",QJsonArray{row}}},{});settle(view);QCOMPARE(played.count(),0);
    QCOMPARE(view.browser()->toPlainText().count("You (video)"),1);
    QString link;for(const auto &url:links(view.browser()))if(url.startsWith("hgs-voice:"))link=url;
    QVERIFY(!link.isEmpty());activate(view.browser(),link);QCOMPARE(played.count(),1);QCOMPARE(played[0][1].toJsonObject(),video);
    view.playVoice(played[0][0].toString(),{},"Synthetic video unavailable");settle(view);
    QVERIFY(view.browser()->toPlainText().contains("Synthetic video unavailable"));
}

namespace {
QJsonObject waveformMessage(bool envelope=true) {
    const QString id="aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa";
    QJsonObject audio{{"request_id",id},{"index",0},{"name","voice.wav"},{"mime","audio/wav"},{"bytes",320044},{"duration_ms",10000}};
    if(envelope){QJsonArray bins;for(int i=0;i<64;++i)bins.append(128);audio["waveform"]=bins;}
    const QJsonObject voice{{"id",id},{"run_id","run"},{"conversation_id","conversation"},{"state","submitted"},{"audio",audio},{"transcript","Synthetic voice recording for player verification."}};
    return {{"type","UserMessage"},{"source","hgs_voice"},{"message_id",id},{"request_id",id},{"at",1791018002.0},{"voice",voice},{"attachments",QJsonArray{audio}}};
}
QPair<QUrl,QRect> waveformImage(QTextBrowser *browser) {
    for(auto block=browser->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
        const auto fragment=it.fragment();const auto image=fragment.charFormat().toImageFormat();const QUrl url(image.name());
        if(url.scheme()!="hgs-voice-player")continue;
        QTextCursor cursor(browser->document());cursor.setPosition(fragment.position());
        return {url,QRect(browser->cursorRect(cursor).topLeft(),QSize(qRound(image.width()),qRound(image.height())))};
    }
    return {};
}
}

void TestActivityView::voiceWaveformSeeksWithoutRelayout_data()
{
    QTest::addColumn<double>("scale");QTest::newRow("normal")<<1.;QTest::newRow("large")<<1.5;
}
void TestActivityView::voiceWaveformSeeksWithoutRelayout()
{
    QFETCH(double,scale);SettledActivityView view;view.resize(560,360);view.setTheme(true);view.setContentScale(scale);view.setSessionKey("fixture/waveform");
    QJsonArray events;for(int i=0;i<12;++i)events.append(journalEvent(i+1,"Stop",QString("Earlier fixture reply %1").arg(i)));
    view.setActivity({{"voice_messages",QJsonArray{waveformMessage()}}},events);view.jumpToLatest();
    auto *browser=view.browser();const auto image=waveformImage(browser);QVERIFY(!image.first.isEmpty());
    browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->value()+image.second.top()-40);
    const auto bounds=waveformImage(browser).second;
    const auto area=VoicePlayerImage::waveformRect(bounds.width(),bounds.height()/52.);
    const QPoint seek=bounds.topLeft()+QPoint(qRound(area.left()+area.width()*.7),qRound(area.bottom()-4));
    const auto original=browser->document()->resource(QTextDocument::ImageResource,image.first).value<QImage>();
    QSignalSpy requested(&view,&ActivityView::voicePlaybackRequested);
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,seek);
    QCOMPARE(requested.count(),1);
    const auto loading=browser->document()->resource(QTextDocument::ImageResource,image.first).value<QImage>();
    QVERIFY(original!=loading);
    // Seeking while the immutable original is downloading does not fetch again.
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,seek-QPoint(40,0));QCOMPARE(requested.count(),1);
    // A silent fixture is loaded paused: this test never produces audible output.
    QTemporaryDir directory;QFile file(directory.filePath("silent.wav"));QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray wav(320044,'\0');wav.replace(0,4,"RIFF");wav.replace(8,8,"WAVEfmt ");wav.replace(36,4,"data");
    qToLittleEndian<quint32>(320036,wav.data()+4);qToLittleEndian<quint32>(16,wav.data()+16);
    qToLittleEndian<quint16>(1,wav.data()+20);qToLittleEndian<quint16>(1,wav.data()+22);qToLittleEndian<quint32>(16000,wav.data()+24);
    qToLittleEndian<quint32>(32000,wav.data()+28);qToLittleEndian<quint16>(2,wav.data()+32);qToLittleEndian<quint16>(16,wav.data()+34);qToLittleEndian<quint32>(320000,wav.data()+40);
    QCOMPARE(file.write(wav),wav.size());file.close();view.playVoice(requested[0][0].toString(),file.fileName());
    auto *player=view.findChild<QMediaPlayer *>();QVERIFY(player);view.findChild<QAudioOutput *>()->setMuted(true);
    // Loading media can finish after further clicks: latest seek and pause win.
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,seek);
    const QPoint button=bounds.topLeft()+QPoint(qRound(22*scale),qRound(22*scale));
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,button);
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,button);
    QVERIFY(player->playbackState()!=QMediaPlayer::PlayingState);
    QTest::qWait(100);QVERIFY(player->playbackState()!=QMediaPlayer::PlayingState);
#ifndef Q_OS_MAC
    // Offscreen macOS never finishes loading with Darwin's backend, which needs a
    // native event loop, and the bundled FFmpeg backend can finish at any later
    // turn; hit testing and resource updates below are still exercised.
    QTRY_VERIFY_WITH_TIMEOUT(player->mediaStatus()==QMediaPlayer::LoadedMedia || player->mediaStatus()==QMediaPlayer::BufferedMedia,5000);
    QVERIFY(qAbs(player->position()-7000)<100);
#endif
    QTest::mouseClick(browser->viewport(),Qt::LeftButton,Qt::NoModifier,seek);QVERIFY(player->playbackState()!=QMediaPlayer::PlayingState);
    settle(view);auto *document=browser->document();auto *layout=document->documentLayout();const auto size=layout->documentSize();const int passes=view.renderPasses();
    const int scroll=browser->verticalScrollBar()->value();
    QTextCursor selection(document);selection.setPosition(1);selection.setPosition(8,QTextCursor::KeepAnchor);browser->setTextCursor(selection);
    browser->verticalScrollBar()->setValue(scroll);
    const auto before=document->resource(QTextDocument::ImageResource,image.first).value<QImage>();
    // Drive the same native signal used by playback; there is no timer-driven HTML path.
    // The image updates synchronously: check it before an event-loop turn, where a late
    // load applies its pending seek and pause and may restore the original image.
    for(qint64 position=100;position<=5000;position+=100)player->positionChanged(position);
    player->playbackStateChanged(QMediaPlayer::PlayingState);
    QVERIFY(before!=document->resource(QTextDocument::ImageResource,image.first).value<QImage>());
    QCoreApplication::processEvents();
    QCOMPARE(browser->document(),document);QCOMPARE(document->documentLayout(),layout);QCOMPARE(layout->documentSize(),size);
    QCOMPARE(view.renderPasses(),passes);QCOMPARE(browser->verticalScrollBar()->value(),scroll);
    QCOMPARE(browser->textCursor().anchor(),selection.anchor());QCOMPARE(browser->textCursor().position(),selection.position());
    const auto screenshots=qEnvironmentVariable("ZERUS_VOICE_PLAYER_SCREENSHOTS");
    if(!screenshots.isEmpty())QVERIFY(view.grab().save(screenshots+"/progress-"+QString::fromLatin1(QTest::currentDataTag())+".png"));
    // A tick during asynchronous scaling keeps the committed image dimensions.
    const auto committed=document->resource(QTextDocument::ImageResource,image.first).value<QImage>().size();
    view.ActivityView::setContentScale(scale==1.?1.5:1.);
    player->positionChanged(6000);
    QCOMPARE(document->resource(QTextDocument::ImageResource,image.first).value<QImage>().size(),committed);
    settle(view);
    view.hide();QVERIFY(player->playbackState()!=QMediaPlayer::PlayingState);
    view.setSessionKey("fixture/another");view.playVoice(requested[0][0].toString(),file.fileName());QVERIFY(player->playbackState()!=QMediaPlayer::PlayingState);
}

void TestActivityView::voiceWaveformAppearance_data()
{
    QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");
    QTest::newRow("dark-normal")<<true<<620;QTest::newRow("dark-narrow")<<true<<340;
    QTest::newRow("light-normal")<<false<<620;QTest::newRow("light-narrow")<<false<<340;
}
void TestActivityView::voiceWaveformAppearance()
{
    QFETCH(bool,dark);QFETCH(int,width);SettledActivityView view;view.resize(width,290);view.setTheme(dark);view.setSessionKey("fixture/appearance");
    auto row=waveformMessage();auto voice=row["voice"].toObject();auto audio=voice["audio"].toObject();QJsonArray envelope;
    for(int i=0;i<64;++i)envelope.append(i<4 || (i>23 && i<28)?0:qRound(255*std::abs(std::sin(i*.27))*std::abs(std::sin(i*.091))));
    audio["waveform"]=envelope;voice["audio"]=audio;row["voice"]=voice;
    view.setActivity({{"voice_messages",QJsonArray{row}}},{});
    const auto image=waveformImage(view.browser());QVERIFY(!image.first.isEmpty());
    const auto rendered=view.browser()->document()->resource(QTextDocument::ImageResource,image.first).value<QImage>();QVERIFY(!rendered.isNull());
    QVERIFY(image.second.right()<=view.browser()->viewport()->width());
    auto invalid=audio;invalid["waveform"]=QJsonArray{1,2,3};QVERIFY(VoicePlayerImage::waveform(invalid).isEmpty());
    auto invalidBins=envelope;invalidBins[10]=1.5;invalid["waveform"]=invalidBins;QVERIFY(VoicePlayerImage::waveform(invalid).isEmpty());
    invalidBins[10]=256;invalid["waveform"]=invalidBins;QVERIFY(VoicePlayerImage::waveform(invalid).isEmpty());
    const auto placeholder=VoicePlayerImage::draw(240,1,1,dark,invalid,0,false,false);QVERIFY(!placeholder.isNull());
    const auto screenshots=qEnvironmentVariable("ZERUS_VOICE_PLAYER_SCREENSHOTS");
    if(!screenshots.isEmpty()){QDir().mkpath(screenshots);QVERIFY(view.grab().save(screenshots+"/"+QString::fromLatin1(QTest::currentDataTag())+".png"));}
}

void TestActivityView::recordingMetadataAppearance_data()
{
    QTest::addColumn<bool>("dark");QTest::addColumn<bool>("videoMode");QTest::addColumn<int>("width");QTest::addColumn<QString>("state");
    for(bool dark:{true,false})for(bool video:{true,false})for(int width:{340,620})for(const auto &state:QStringList{"submitted","transcribing","uncertain"})
        QTest::newRow(qPrintable(QString("%1-%2-%3-%4").arg(dark?"dark":"light",video?"video":"audio",QString::number(width),state)))<<dark<<video<<width<<state;
}
void TestActivityView::recordingMetadataAppearance()
{
    QFETCH(bool,dark);QFETCH(bool,videoMode);QFETCH(int,width);QFETCH(QString,state);
    SettledActivityView view;view.resize(width,540);view.setTheme(dark);view.setSessionKey("fixture/media-icons");view.show();
    auto row=waveformMessage();auto voice=row["voice"].toObject();voice["state"]=state;
    if(state=="uncertain")voice["error"]="Delivery is unconfirmed. Check the original conversation.";
    if(videoMode)voice["video"]=QJsonObject{{"request_id",voice["id"]},{"index",1},{"mime","video/mp4"},{"duration_ms",10000},{"width",480},{"height",480}};
    row["voice"]=voice;view.setActivity({{"voice_messages",QJsonArray{row}}},{});settle(view);
    const auto text=view.browser()->toPlainText();QVERIFY(!text.contains("Sent"));QVERIFY(!text.contains("Show transcript"));QVERIFY(!text.contains("Recognizing"));
    bool status=false,transcript=false;
    for(auto block=view.browser()->document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
        const auto format=it.fragment().charFormat();if(!format.isImageFormat())continue;
        const QUrl resource(format.toImageFormat().name());
        if(resource.scheme()=="hgs-voice-status"){
            status=true;QCOMPARE(format.property(QTextFormat::ImageAltText).toString(),state=="submitted"?"Sent to agent":state=="transcribing"?"Recognizing…":"Delivery not confirmed");
            QVERIFY(!view.browser()->document()->resource(QTextDocument::ImageResource,resource).value<QImage>().isNull());
        }
        if(resource.scheme()=="hgs-voice-transcript"){transcript=true;QCOMPARE(format.property(QTextFormat::ImageAltText).toString(),"Show transcript");}
    }
    QVERIFY(status && transcript);QVERIFY(view.browser()->horizontalScrollBar()->maximum()==0);
    if(state=="uncertain")QVERIFY(text.contains(voice["error"].toString()));
    const auto directory=qEnvironmentVariable("ZERUS_RECORDING_MESSAGE_SCREENSHOTS");
    if(!directory.isEmpty()){QDir().mkpath(directory);QVERIFY(view.grab().save(directory+"/"+QString::fromLatin1(QTest::currentDataTag())+".png"));}
}

void TestActivityView::attachmentsStayWithTheirMessageAndOpenExactFile()
{
    SettledActivityView view;view.setSessionKey("mac/project/session");
    const QJsonObject file{{"request_id","receipt-one"},{"index",0},{"name","screen.png"},{"mime","image/png"},{"bytes",2048}};
    QJsonArray events{journalEvent(1,"Stop","Earlier response"),journalEvent(4,"Stop","Later response")};
    view.setActivity({},events);view.setLocalMessages({QJsonObject{{"id","1"},{"text",""},{"status","sent"},{"submitted_at",1791018002.0},{"attachments",QJsonArray{file}}}});
    auto text=view.browser()->toPlainText();QVERIFY(text.indexOf("Earlier response")<text.indexOf("screen.png"));QVERIFY(text.indexOf("screen.png")<text.indexOf("Later response"));
    QSignalSpy opened(&view,&ActivityView::attachmentActivated);QString link;
    for(const auto &url:links(view.browser()))if(url.startsWith("hgs-attachment:")){link=url;break;}
    QVERIFY(!link.isEmpty());activate(view.browser(),link);QCOMPARE(opened.size(),1);QCOMPARE(opened.first().first().toJsonObject(),file);
    const auto native=journalEvent(2,"UserPromptSubmit","Inspect the attached image at \"/private/image.png\"");events.insert(1,native);
    const QJsonObject receipt{{"source","hgs_delivery"},{"type","UserPromptSubmit"},{"message_id","receipt-one"},{"at",1791018001.8},
        {"detail",""},{"submitted_text","\n\nInspect the attached image at \"/private/image.png\""},{"attachments",QJsonArray{file}}};
    events.insert(2,journalEvent(3,"TurnStarted","Inspect the attached image at \"/private/image.png\""));
    view.setLocalMessages({});view.setActivity({{"attachment_messages",QJsonArray{receipt}}, {"prompt","Inspect the attached image at \"/private/image.png\""}},events);
    text=view.browser()->toPlainText();QCOMPARE(text.count("screen.png"),1);QVERIFY(!text.contains("/private/image.png"));QVERIFY(text.indexOf("screen.png")<text.indexOf("Later response"));
    view.setSessionKey("other");activate(view.browser(),link);QCOMPARE(opened.size(),1);
}

void TestActivityView::repeatedJournalWindowKeepsOneAttachmentMessage()
{
    SettledActivityView view; view.setSessionKey("fixture/attachment");
    const QString text = QString::fromUtf8("Посмотри [Image #1] и [Image #2], затем сравни.");
    const QString submitted = QString::fromUtf8("Посмотри [Image #1] (Inspect the attached image at \"/synthetic/one.png\") и [Image #2] (Inspect the attached image at \"/synthetic/two.png\"), затем сравни.");
    QJsonArray files;
    for (int i = 0; i < 2; ++i) files.append(QJsonObject{{"name", i ? "two.png" : "one.png"},
        {"mime", "image/png"}, {"bytes", 120}, {"reference", QString("[Image #%1]").arg(i + 1)},
        {"request_id", "delivery"}, {"index", i}});
    const QJsonObject local{{"id", "outgoing"}, {"message_id", "delivery"}, {"text", text},
        {"submitted_text", submitted}, {"status", "sent"}, {"submitted_at", 1791018002.0}, {"attachments", files}};
    const QJsonObject receipt{{"source", "hgs_delivery"}, {"type", "UserPromptSubmit"},
        {"message_id", "delivery"}, {"at", 1791018002.0}, {"detail", text}, {"submitted_text", submitted}, {"attachments", files}};
    auto prompt = journalEvent(2, "UserPromptSubmit", submitted);
    const QJsonObject details{{"conversation_id", "conversation"}, {"prompt", submitted}, {"attachment_messages", QJsonArray{receipt}}};
    // Retain the outgoing card's identity when its durable receipt arrives.
    view.setTimeline(details, {prompt}, {local});
    // A conversation change fetches a full window which can repeat a sequence.
    view.setTimeline(details, {prompt, prompt}, {});
    auto plain = view.browser()->toPlainText();
    QCOMPARE(plain.count(QString::fromUtf8("Посмотри")), 1);
    QVERIFY(!plain.contains("Inspect the attached image"));
    QCOMPARE(plain.count("one.png"), 1); QCOMPARE(plain.count("two.png"), 1);
    QSignalSpy opened(&view, &ActivityView::attachmentActivated);
    QSet<int> indexes;
    for (const auto &link : links(view.browser())) if (link.startsWith("hgs-attachment:")) {
        activate(view.browser(), link);
        indexes.insert(opened.last()[0].toJsonObject().value("index").toInt());
    }
    QCOMPARE(indexes, QSet<int>({0, 1}));
    // Equal text with a different journal identity is a separate submission.
    auto repeated = prompt; repeated["seq"] = 4; repeated["at"] = 1791018004.0;
    view.setTimeline(details, {prompt, journalEvent(3, "Stop", "Compared"), repeated}, {});
    plain = view.browser()->toPlainText();
    QCOMPARE(plain.count(QString::fromUtf8("Посмотри")), 2);
}

void TestActivityView::delayedAttachmentAcknowledgementKeepsOneMessage()
{
    SettledActivityView view; view.setSessionKey("fixture/delayed-attachment");
    const QString text = "Compare these images";
    const QString submitted = text + "\n\nInspect the attached image at \"/synthetic/request-one/one.png\"";
    const QJsonArray files{QJsonObject{{"request_id", "receipt-one"}, {"index", 0}, {"name", "one.png"}, {"mime", "image/png"}, {"bytes", 120}}};
    const QJsonObject receipt{{"source", "hgs_delivery"}, {"type", "UserPromptSubmit"},
        {"message_id", "receipt-one"}, {"at", 1791018000.0}, {"detail", text}, {"submitted_text", submitted}, {"attachments", files}};
    const QJsonObject details{{"conversation_id", "conversation"}, {"attachment_messages", QJsonArray{receipt}}};
    auto native = journalEvent(2, "UserPromptSubmit", submitted); native["at"] = 1791018102.0;
    // A locally acknowledged send precedes durable attachment metadata.
    const QJsonObject local{{"id", "outgoing"}, {"message_id", "receipt-one"}, {"status", "sent"},
        {"text", text}, {"submitted_text", submitted}, {"submitted_at", 1791018000.0}, {"attachments", files}};
    view.setTimeline({{"conversation_id", "conversation"}}, {native}, {local});
    QCOMPARE(view.browser()->toPlainText().count(text),1);
    QVERIFY(!view.browser()->toPlainText().contains("/synthetic/"));
    view.setTimeline(details, {}, {}); // durable send exists while native queues input
    view.setTimeline(details, {journalEvent(1, "PreToolUse", "Running existing turn"), native}, {});
    auto plain = view.browser()->toPlainText();
    QCOMPARE(plain.count(text), 1); QCOMPARE(plain.count("one.png"), 1);
    QVERIFY(!plain.contains("Inspect the attached image")); QVERIFY(!plain.contains("/synthetic/"));
    // An independent equal-caption send uses different immutable staged files.
    auto repeated = journalEvent(3, "UserPromptSubmit", text + "\n\nInspect the attached image at \"/synthetic/request-two/two.png\"");
    repeated["at"] = 1791018103.0;
    view.setTimeline(details, {native, repeated}, {});
    plain = view.browser()->toPlainText();
    QCOMPARE(plain.count(text), 2); QCOMPARE(plain.count("one.png"), 1);
    QVERIFY(plain.contains("/synthetic/request-two/two.png"));
    // A cold ambiguous pair has no retained native binding to disambiguate it.
    view.setSessionKey("fixture/ambiguous-attachment");
    auto other = native; other["seq"] = 4; other["at"] = 1791018104.0;
    view.setTimeline(details, {native, other}, {});
    QCOMPARE(view.browser()->toPlainText().count(text), 3);
}

void TestActivityView::attachmentThumbnailArrivesWithoutMovingTranscript()
{
    SettledActivityView view;view.resize(640,420);view.show();view.setSessionKey("arch/session");
    QSignalSpy previews(&view,&ActivityView::attachmentPreviewRequested);
    const QJsonObject file{{"name","screen.png"},{"mime","image/png"},{"request_id","receipt"},{"index",0},{"bytes",38400}};
    auto events=history(12);
    auto message=journalEvent(13,"UserPromptSubmit","See this image");message["attachments"]=QJsonArray{file};events.append(message);
    view.setActivity({{"conversation_id","one"}},events);
    QTRY_COMPARE(previews.size(),1);const auto key=previews.first()[0].toString();
    auto *browser=view.browser();auto *bar=browser->verticalScrollBar();
    QTRY_COMPARE(bar->value(),bar->maximum());
    auto cursor=browser->document()->find("See this image");browser->setTextCursor(cursor);
    const auto revision=browser->document()->revision();const auto scroll=bar->value();
    QImage image(320,200,QImage::Format_RGB32);image.fill(QColor("#83cbb2"));
    view.setAttachmentPreview(key,image);QCoreApplication::processEvents();
    QCOMPARE(browser->document()->revision(),revision);QCOMPARE(bar->value(),scroll);
    QCOMPARE(browser->textCursor().selectedText(),QString("See this image"));
    const QUrl resource("hgs-thumbnail:"+key);
    QVERIFY(!browser->document()->resource(QTextDocument::ImageResource,resource).value<QImage>().isNull());
    events.append(journalEvent(14,"Stop","Image received"));view.setActivity({{"conversation_id","one"}},events);
    QCoreApplication::processEvents();QCOMPARE(previews.size(),1);
    QVERIFY(!browser->document()->resource(QTextDocument::ImageResource,resource).value<QImage>().isNull());
    const auto preview=qEnvironmentVariable("HGS_INLINE_ATTACHMENT_PREVIEW");
    if(!preview.isEmpty()){view.setTheme(true);view.jumpToLatest();QTest::qWait(40);QVERIFY(view.grab().save(preview));}
    view.setSessionKey("other/session");view.setActivity({},history(4));
    const auto otherRevision=browser->document()->revision();view.setAttachmentPreview(key,image);
    QCOMPARE(browser->document()->revision(),otherRevision);QVERIFY(!browser->toPlainText().contains("screen.png"));
}

void TestActivityView::followingPaintsAtTheFinalScrollRange()
{
    // A long journal is laid out lazily. Every rebuild must paint at its final
    // scroll range: the lazy estimate showed one frame past the end, a flicker.
    SettledActivityView view; view.resize(900, 700); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    auto events = history(400); view.setActivity({}, events); QTest::qWait(50);
    auto *bar = view.browser()->verticalScrollBar();
    struct Paints : QObject {
        QScrollBar *bar = nullptr; QList<int> maxima;
        bool eventFilter(QObject *object, QEvent *event) override
        { if (event->type() == QEvent::Paint) maxima << bar->maximum(); return QObject::eventFilter(object, event); }
    } paints;
    paints.bar = bar; view.browser()->viewport()->installEventFilter(&paints);
    for (int i = 401; i <= 403; ++i) {
        events.append(journalEvent(i, "Stop", QString("Update %1").arg(i)));
        paints.maxima.clear(); view.setActivity({}, events);
        const int range = bar->maximum(); QCOMPARE(bar->value(), range);
        QTest::qWait(60);
        QCOMPARE(bar->maximum(), range); QVERIFY(!paints.maxima.isEmpty());
        for (const int painted : paints.maxima) QCOMPARE(painted, range);
    }
}

void TestActivityView::unchangedPollsSkipRendering()
{
    // Polls repeat the same inspection with fresh live fields that Activity never
    // shows. They must not rebuild the journal, scroll, selection or expanded cards.
    SettledActivityView view; view.resize(640, 360); view.show(); view.setSessionKey("arch\ncodex/hgs/poll");
    QJsonObject details{{"tracked", true}, {"conversation_id", "poll"}, {"phase", "idle"}, {"goal_observed_at", 100.},
        {"cache_hint", QJsonObject{{"status", "warm"}}}, {"processes", QJsonObject{{"items", QJsonArray{QJsonObject{{"id", "job"}, {"cpu", 1.5}}}}}}};
    QJsonArray events;
    for (int i = 1; i <= 40; ++i) events.append(QJsonObject{{"seq", i}, {"type", i % 2 ? "UserPromptSubmit" : "Stop"},
        {"detail", QString("Message %1 with enough text to wrap across the reading column.").arg(i)}, {"at", double(i)}});
    view.setActivity(details, events, "Start", true); QTest::qWait(30);
    auto *scroll = view.browser()->verticalScrollBar(); QVERIFY(scroll->maximum() > 0);
    scroll->setValue(scroll->maximum() / 2); auto cursor = view.browser()->textCursor(); cursor.setPosition(5); cursor.setPosition(25, QTextCursor::KeepAnchor);
    view.browser()->setTextCursor(cursor);
    const int passes = view.renderPasses(), position = scroll->value(); QVERIFY(passes > 0);
    details["goal_observed_at"] = 103.; details["cache_hint"] = QJsonObject{{"status", "cold"}};
    details["processes"] = QJsonObject{{"items", QJsonArray{QJsonObject{{"id", "job"}, {"cpu", 7.25}}}}};
    view.setActivity(details, events, "Start", true); view.setTimeline(details, events, {}, "Start", true); QTest::qWait(20);
    QCOMPARE(view.renderPasses(), passes); QCOMPARE(scroll->value(), position);
    QCOMPARE(view.browser()->textCursor().selectionStart(), 5); QCOMPARE(view.browser()->textCursor().selectionEnd(), 25);
    // Every input that Activity shows still renders.
    const auto renders = [&](const std::function<void()> &change) { const int before = view.renderPasses(); change(); return view.renderPasses() > before; };
    QVERIFY(renders([&] { details["phase"] = "tool"; view.setActivity(details, events, "Start", true); }));
    QVERIFY(renders([&] { events.append(QJsonObject{{"seq", 41}, {"type", "Stop"}, {"detail", "New reply"}, {"at", 41.}}); view.setActivity(details, events, "Start", true); }));
    QVERIFY(renders([&] { view.setActivity(details, events, "Another prompt", true); }));
    QVERIFY(renders([&] { view.setActivity(details, events, "Another prompt", false); }));
    QVERIFY(renders([&] { details["processes"] = QJsonObject{{"items", QJsonArray{QJsonObject{{"id", "other"}}}}}; view.setActivity(details, events, "Another prompt", false); }));
    QVERIFY(renders([&] { view.setTimeline(details, events, QJsonArray{QJsonObject{{"id", "local"}, {"text", "Queued"}, {"submitted_at", 42.}, {"status", "sending"}}}, "Another prompt", false); }));
    QVERIFY(renders([&] { view.setTheme(true); }));
    QVERIFY(renders([&] { view.setContentScale(1.25); }));
    view.setSessionKey("arch\ncodex/hgs/other");
    QVERIFY(renders([&] { view.setActivity(details, events, "Another prompt", false); }));
}

void TestActivityView::startsAtLatestAndFollows()
{
    SettledActivityView view; view.resize(520, 340); view.show();
    view.setSessionKey("arch/one"); view.setActivity({}, {});
    QTest::qWait(20);
    auto *bar = view.browser()->verticalScrollBar();
    view.setActivity({}, history(24));
    QTRY_VERIFY(bar->maximum() > 0);
    QTRY_COMPARE(bar->value(), bar->maximum());
    QVERIFY(view.browser()->toPlainText().indexOf("Message number 1\n") < view.browser()->toPlainText().indexOf("Message number 24\n"));
    const int oldMaximum = bar->maximum();
    view.setActivity({}, history(26));
    QTRY_VERIFY(bar->maximum() > oldMaximum);
    QTRY_COMPARE(bar->value(), bar->maximum());
    view.resize(420, 290); QTest::qWait(20);
    QTRY_COMPARE(bar->value(), bar->maximum());
    bar->setValue(0);
    view.setSessionKey("mac/two"); view.setActivity({}, history(20));
    QTRY_COMPARE(bar->value(), bar->maximum());
    // A new conversation behind the same pane is a fresh history too.
    view.setActivity({{"conversation_id", "a"}}, history(20)); bar->setValue(0);
    view.setActivity({{"conversation_id", "b"}}, history(22));
    QTRY_COMPARE(bar->value(), bar->maximum());
}

void TestActivityView::preservesSelectionAndReadingPosition()
{
    SettledActivityView view; view.resize(560, 350); view.show();
    view.setSessionKey("arch/one"); view.setActivity({}, history(30));
    QTest::qWait(20);
    auto *browser = view.browser(); auto *bar = browser->verticalScrollBar();
    auto selection = browser->document()->find("Message number 12"); QVERIFY(!selection.isNull());
    browser->setTextCursor(selection);
    bar->setValue(bar->value() - 50); QTest::qWait(20);
    const auto top = browser->cursorForPosition(QPoint(4, 4));
    const QString topText = top.block().text(); const int topY = browser->cursorRect(top).top();
    const QString selected = browser->textCursor().selectedText();
    const int revision = browser->document()->revision();
    view.setActivity({}, history(30));
    QCOMPARE(browser->document()->revision(), revision); // unchanged polls never reset selection
    view.setActivity({}, history(32)); QTest::qWait(20);
    QCOMPARE(browser->textCursor().selectedText(), selected);
    const auto afterTop = browser->cursorForPosition(QPoint(4, 4));
    QCOMPARE(afterTop.block().text(), topText);
    QVERIFY(qAbs(browser->cursorRect(afterTop).top() - topY) < 3);
    QVERIFY(bar->value() < bar->maximum() - 30);
    auto *latest = view.findChild<QPushButton *>("activityJumpLatest");
    QVERIFY(latest->isVisible()); QVERIFY(latest->text().contains("2 new"));
    // Pruning old events keeps the selected event and the visible paragraph.
    auto pruned = history(33); for (int i = 0; i < 8; ++i) pruned.removeFirst();
    view.setActivity({}, pruned); QTest::qWait(20);
    QCOMPARE(browser->textCursor().selectedText(), selected);
    QCOMPARE(browser->cursorForPosition(QPoint(4, 4)).block().text(), topText);
    latest->click();
    QTRY_COMPARE(bar->value(), bar->maximum());
    QVERIFY(!latest->isVisible()); QVERIFY(!browser->textCursor().hasSelection());
}

void TestActivityView::idleNotificationsStayOutOfTheTimeline()
{
    // Claude sends an empty Notification a minute after every finished turn
    // ("waiting for your input"); it is not part of the conversation.
    SettledActivityView view; view.resize(540, 320); view.show();
    QJsonObject typed = journalEvent(4, "Notification", "Claude is waiting for your input");
    typed["notification_type"] = "idle_prompt";
    view.setActivity({}, {journalEvent(1, "UserPromptSubmit", "Do the work"), journalEvent(2, "Stop", "Done."),
                          journalEvent(3, "Notification", ""), typed});
    QVERIFY(!view.plainText().contains("Agent notification"));
    QVERIFY(!view.plainText().contains("activity events"));
    QVERIFY(!view.plainText().contains("waiting for your input"));
    QVERIFY(!links(view.browser()).join(' ').contains("hgs-activity:"));
    QVERIFY(view.plainText().contains("Done."));
}

void TestActivityView::groupsToolsAndKeepsExpansion()
{
    SettledActivityView view; view.resize(540, 320); view.show();
    view.setSessionKey("arch/one");
    QJsonArray events{journalEvent(1, "UserPromptSubmit", "Please inspect the source"),
        journalEvent(2, "PreToolUse", "hidden-command-first", "Bash"),
        journalEvent(3, "PostToolUse", "command completed", "Bash"),
        journalEvent(4, "Notification", "Waiting for tool results")};
    view.setActivity({}, events); QTest::qWait(20);
    auto *browser = view.browser();
    QVERIFY(browser->toPlainText().contains("1 tool call, 3 events"));
    QVERIFY(!browser->toPlainText().contains("hidden-command-first"));
    QVERIFY(links(browser).contains("hgs-activity:group-2"));
    activate(browser, "hgs-activity:group-2");
    QVERIFY(browser->toPlainText().contains("hidden-command-first"));
    events.append(journalEvent(5, "Stop", "Review **finished**.")); view.setActivity({}, events);
    QVERIFY(browser->toPlainText().contains("hidden-command-first"));
    activate(browser, "hgs-activity:group-2");
    QVERIFY(!browser->toPlainText().contains("hidden-command-first"));
    events.append(journalEvent(6, "PermissionRequest", "Approve protected operation")); view.setActivity({}, events);
    QVERIFY(browser->toPlainText().contains("Approve protected operation"));
    const auto expandedAttentionLength = browser->toPlainText().size();
    activate(browser, "hgs-activity:group-6");
    QVERIFY(browser->toPlainText().size() < expandedAttentionLength);
    view.setSessionKey("mac/new"); view.setActivity({}, events);
    QVERIFY(!browser->toPlainText().contains("hidden-command-first"));
}

void TestActivityView::rejectsMarkupResourcesAndUnsafeLinks()
{
    SettledActivityView view; view.resize(540, 320); view.show();
    const QString text = "**Good prose**\n\n<script>bad()</script><img src='file:///etc/passwd'>\n\n"
        "![tracking](https://example.invalid/tracker.png)\n\n[bad](javascript:alert(1)) [file](file:///etc/passwd) "
        "[forged](hgs-activity:group-2) [safe](https://example.com/docs)\n\n```sh\necho '<script>'\n```";
    view.setActivity({}, {journalEvent(1, "Stop", text), journalEvent(2, "PreToolUse", "literal <b>tool</b>", "<tool>")});
    auto *browser = view.browser();
    const auto allLinks = links(browser);
    QVERIFY(allLinks.contains("https://example.com/docs"));
    for (const auto &link : allLinks) QVERIFY(link.startsWith("https://") || link.startsWith("hgs-file:") || link == "hgs-activity:group-2");
    QVERIFY(browser->toPlainText().contains("[Image attachment]"));
    // Drawn decorations, and the reply's own copy button.
    QStringList buttons;
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            if (!it.fragment().charFormat().isImageFormat()) continue;
            const auto name = it.fragment().charFormat().toImageFormat().name();
            if (name.startsWith("hgs-ui:")) buttons.append(name); else QVERIFY(name.startsWith("hgs-md:"));
        }
    QCOMPARE(buttons.size(), 1); QVERIFY(buttons.first().startsWith("hgs-ui:copy/"));
    QVERIFY(browser->document()->resource(QTextDocument::ImageResource, QUrl("file:///etc/passwd")).value<QImage>().isNull());
    QSignalSpy opened(&view, &ActivityView::externalLinkActivated);
    activate(browser, "javascript:alert(1)"); activate(browser, "file:///etc/passwd");
    activate(browser, "https://user:password@example.com/private");
    QCOMPARE(opened.size(), 0);
    activate(browser, "hgs-activity:group-2");
    QVERIFY(browser->toPlainText().contains("literal <b>tool</b>"));
}

void TestActivityView::preservesLiteralUserMessages_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<bool>("dark");
    for (const auto &source : QStringList{"UserPromptSubmit", "UserPromptQueued", "TurnStarted",
             "QuestionAnswered", "sending", "sent", "error", "snapshot", "fallback", "search"}) {
        for (bool dark : {false, true})
            QTest::newRow(qPrintable(source + (dark ? "-dark" : "-light"))) << source << dark;
    }
}

void TestActivityView::preservesLiteralUserMessages()
{
    QFETCH(QString, source);
    QFETCH(bool, dark);
    const QString text = "> Вычеты из наших выплат за ошибки игр сейчас без потолка\n"
        "> нужна расшифровка\n\nраскрой что ты имеешь в виду?\n"
        "  >> Вложенная псевдо-цитата\n1. исходный номер\n# не заголовок\n"
        "**буквальные звёздочки** и `код`\n"
        "<b>буквальный HTML</b> & <img src='file:///etc/passwd'>\n"
        "[ссылка](https://example.com)  два пробела";
    SettledActivityView view; view.resize(420, 800); view.setTheme(dark); view.show();
    view.setSessionKey("arch/quoted-request");
    if (source == "snapshot") view.setActivity({{"prompt", text}}, {});
    else if (source == "fallback") view.setActivity({}, {}, text);
    else if (source == "search") view.showSearchResult(journalEvent(1, "UserPromptSubmit", text), "расшифровка");
    else if (QStringList{"sending", "sent", "error"}.contains(source)) {
        view.setActivity({}, {});
        view.setLocalMessages({QJsonObject{{"id", "quoted"}, {"text", text}, {"status", source}}});
    } else view.setActivity({}, {journalEvent(1, source, text)});
    auto *browser = view.browser();
    QVERIFY2(browser->toPlainText().contains(text), qPrintable(browser->toPlainText()));
    // Selection/copy must preserve the actual prompt as well as its appearance.
    QTextCursor cursor(browser->document());
    cursor.setPosition(browser->toPlainText().indexOf(text));
    cursor.setPosition(cursor.position() + text.size(), QTextCursor::KeepAnchor);
    auto copied = cursor.selectedText();
    copied.replace(QChar::ParagraphSeparator, '\n').replace(QChar::LineSeparator, '\n');
    QCOMPARE(copied, text);
    QVERIFY(!links(browser).contains("https://example.com"));
    for (auto block = browser->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) QVERIFY(!it.fragment().charFormat().isImageFormat());
    QTest::qWait(10);
    QCOMPARE(browser->horizontalScrollBar()->maximum(), 0);
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (!directory.isEmpty() && source == "UserPromptSubmit") {
        QDir().mkpath(directory);
        QVERIFY(view.grab().save(directory + (dark ? "/literal-user-dark.png" : "/literal-user-light.png")));
    }
    view.setActivity({}, {journalEvent(2, "Stop", "**Ответ агента** с `кодом`")});
    if (source == "search") view.clearSearchResult();
    QVERIFY(view.plainText().contains("Ответ агента с кодом"));
    QVERIFY(!browser->toPlainText().contains("**Ответ агента**"));
}

void TestActivityView::preservesSessionFileReferences()
{
    SettledActivityView view;
    const QString text = "[Package](/home/user/packages/setup.zip)\n\n"
        "[Instructions](files/README.txt) [Registry](<docs/client map.md:12>)\n\n"
        "[**Formatted** label](/tmp/result.txt#L4) [file URI](file:///tmp/report.txt)\n\n"
        "[forged](hgs-file:fake) [network](file://other-host/etc/passwd) [command](command:run)";
    view.setActivity({}, {journalEvent(1, "Stop", text)});
    auto *browser = view.browser();
    const auto rendered = browser->toPlainText();
    QVERIFY(rendered.contains("Package (/home/user/packages/setup.zip)"));
    QVERIFY(rendered.contains("Instructions (files/README.txt)"));
    QVERIFY(rendered.contains("Registry (docs/client map.md:12)"));
    QCOMPARE(rendered.count("/tmp/result.txt#L4"), 1);
    QSignalSpy files(&view, &ActivityView::fileReferenceActivated);
    QSignalSpy external(&view, &ActivityView::externalLinkActivated);
    const auto allLinks = links(browser);
    QVERIFY(!allLinks.contains("hgs-file:fake"));
    for (const auto &link : allLinks) if (link.startsWith("hgs-file:")) activate(browser, link);
    QVERIFY(files.size() >= 5); QCOMPARE(external.size(), 0);
    QVERIFY(files.contains(QList<QVariant>{QString("/home/user/packages/setup.zip")}));
    QVERIFY(files.contains(QList<QVariant>{QString("files/README.txt")}));
    const QString previousLink = allLinks.first();
    view.setSessionKey("other/session"); view.setActivity({}, {journalEvent(1, "Stop", "No files")});
    const auto opened = files.size(); activate(browser, previousLink); QCOMPARE(files.size(), opened);
    const auto target = SessionFileReference::parse("docs/client%20map.md:12");
    QCOMPARE(target.path, QString("docs/client map.md")); QCOMPARE(target.location, QString(":12"));
    QCOMPARE(SessionFileReference::resolve(target, "/remote/project"), QString("/remote/project/docs/client map.md"));
    QVERIFY(SessionFileReference::resolve(target, {}).isEmpty());
    QVERIFY(SessionFileReference::resolve(SessionFileReference::parse("~/result.txt"), "/remote/project").isEmpty());
    QVERIFY(!SessionFileReference::parse("//other-host/path").valid());
    QVERIFY(!SessionFileReference::parse("/tmp/file%0Acommand").valid());
    QCOMPARE(SessionFileReference::parse("LICENSE").path, QString("LICENSE"));
    QCOMPARE(SessionFileReference::parse("README.md#installation").location, QString("#installation"));
}

void TestActivityView::fileReferencesFollowTheGuiPlatform_data()
{
    // An empty path is an invalid reference.
    QTest::addColumn<bool>("windows");
    QTest::addColumn<QString>("href");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("location");
    // A Windows GUI reads drive paths with either separator, kept as written.
    QTest::newRow("windows drive") << true << "C:\\work\\a b.cpp:12:3" << "C:\\work\\a b.cpp" << ":12:3";
    QTest::newRow("windows drive slashes") << true << "C:/work/a.cpp:12" << "C:/work/a.cpp" << ":12";
    QTest::newRow("windows drive fragment") << true << "c:\\work\\notes.md#intro" << "c:\\work\\notes.md" << "#intro";
    QTest::newRow("windows drive root") << true << "D:\\" << "D:\\" << "";
    QTest::newRow("windows file URL") << true << "file:///C:/work/a.cpp" << "C:/work/a.cpp" << "";
    QTest::newRow("windows file URL decoded") << true << "file:///c:/work/x%20y.md#L4" << "c:/work/x y.md" << "#L4";
    QTest::newRow("windows peer path") << true << "/home/user/notes.md:3" << "/home/user/notes.md" << ":3";
    QTest::newRow("windows relative") << true << "docs/client%20map.md:12" << "docs/client map.md" << ":12";
    // Never a share (UNC), however it is written, nor a drive-relative path.
    QTest::newRow("windows share") << true << "\\\\server\\share\\a.txt" << "" << "";
    QTest::newRow("windows share slashes") << true << "//server/share/a.txt" << "" << "";
    QTest::newRow("windows share mixed") << true << "\\/server/share/a.txt" << "" << "";
    QTest::newRow("windows share URL") << true << "file://server/share/a.txt" << "" << "";
    QTest::newRow("windows share URL path") << true << "file:////server/share/a.txt" << "" << "";
    QTest::newRow("windows share encoded") << true << "file:///%5C%5Cserver/share/a.txt" << "" << "";
    QTest::newRow("windows device") << true << "\\\\?\\C:\\a.txt" << "" << "";
    QTest::newRow("windows drive relative") << true << "C:a.txt" << "" << "";
    QTest::newRow("windows drive control") << true << "C:\\work\\a%0A.txt" << "" << "";
    QTest::newRow("windows drive query") << true << "C:/work/a.txt?x=1" << "" << "";
    // POSIX GUIs read references as before.
    QTest::newRow("posix drive") << false << "C:\\work\\a.cpp" << "" << "";
    QTest::newRow("posix drive slashes") << false << "C:/work/a.cpp:12" << "" << "";
    QTest::newRow("posix file URL") << false << "file:///C:/work/a.cpp" << "/C:/work/a.cpp" << "";
    QTest::newRow("posix backslashes") << false << "\\\\server\\share\\a.txt" << "\\\\server\\share\\a.txt" << "";
    QTest::newRow("posix share slashes") << false << "//server/share/a.txt" << "" << "";
    QTest::newRow("posix path") << false << "/home/user/notes.md:3" << "/home/user/notes.md" << ":3";
}

void TestActivityView::fileReferencesFollowTheGuiPlatform()
{
    QFETCH(bool, windows); QFETCH(QString, href); QFETCH(QString, path); QFETCH(QString, location);
    using MachinePlatform::Style;
    const auto target = SessionFileReference::parse(href, windows ? Style::Windows : Style::Posix);
    QCOMPARE(target.valid(), !path.isEmpty());
    QCOMPARE(target.path, path);
    QCOMPARE(target.location, location);
    // References follow the platform the GUI runs on.
    if ((MachinePlatform::build() == Style::Windows) == windows)
        QCOMPARE(SessionFileReference::parse(href).path, path);
}

void TestActivityView::expandsMatchingSnapshotWithoutDuplicate()
{
    SettledActivityView view;
    view.setActivity({{"prompt", "Please implement this feature completely"}, {"last_message", "Finished the change and ran all tests."}},
        {journalEvent(1, "UserPromptSubmit", "Please implement…"), journalEvent(2, "Stop", "Finished the change…")});
    QCOMPARE(view.browser()->toPlainText().count("Please implement"), 1);
    QCOMPARE(view.browser()->toPlainText().count("Finished the change"), 1);
    QVERIFY(view.browser()->toPlainText().contains("ran all tests."));
    view.setActivity({{"last_message", "An older response"}}, {journalEvent(3, "UserPromptSubmit", "New request")});
    QVERIFY(view.browser()->toPlainText().contains("recorded response"));
    QVERIFY(view.browser()->toPlainText().indexOf("An older response") < view.browser()->toPlainText().indexOf("New request"));
    QVERIFY(view.browser()->toPlainText().contains("Outside the available timeline"));
}

void TestActivityView::crossSessionMessageIsAFullNoticeFromThatSession()
{
    SettledActivityView view; view.setTheme(true);
    auto message = journalEvent(2, "UserPromptSubmit", "Message from zerus-19");
    message["origin"] = "peer_message"; message["agent_id"] = ""; message["sender"] = "zerus-19";
    message["report"] = "SessionsWindow.cpp is free again.\n\n- **Re-apply** your edits on top of HEAD";
    view.setActivity({}, {journalEvent(1, "UserPromptSubmit", "Commit my work"), message, journalEvent(3, "Stop", "Done")});
    const auto plain = view.browser()->toPlainText();
    QCOMPARE(plain.count("You"), 1);
    QVERIFY(plain.contains("Message from zerus-19"));
    QVERIFY(plain.contains("SessionsWindow.cpp is free again."));
    QVERIFY(plain.contains("Re-apply your edits on top of HEAD"));
    QVERIFY(!plain.contains("**Re-apply**"));
    QVERIFY(links(view.browser()).filter(QRegularExpression("^hgs-activity:report-")).isEmpty());
    QVERIFY(view.browser()->toHtml().contains("#c5a8f5", Qt::CaseInsensitive));
}

void TestActivityView::subagentReportIsShortUntilExpanded()
{
    SettledActivityView view; view.setTheme(true); view.resize(630, 520); view.show();
    const auto preview = qEnvironmentVariable("HGS_REPORT_PREVIEW");
    auto report = journalEvent(2, "UserPromptSubmit", "Agent \"Review branch\" finished");
    report["origin"] = "subagent_report"; report["agent_id"] = ""; report["from_agent"] = "a15";
    report["report"] = "## Review: Markdown\n\n- **Critical**: tests fail at HEAD\n- Minor: spacing";
    view.setActivity({}, {journalEvent(1, "UserPromptSubmit", "Review the branch"), report, journalEvent(3, "Stop", "Fixing it")});
    auto plain = view.browser()->toPlainText();
    QCOMPARE(plain.count("You"), 1);
    QVERIFY(plain.contains("Subagent report"));
    QVERIFY(plain.contains("Agent \"Review branch\" finished"));
    QVERIFY(!plain.contains("tests fail at HEAD"));
    const auto toggle = links(view.browser()).filter(QRegularExpression("^hgs-activity:report-"));
    QCOMPARE(toggle.size(), 1);
    if (!preview.isEmpty()) { QDir().mkpath(preview); QTest::qWait(40); QVERIFY(view.grab().save(preview + "/report-collapsed.png")); }
    activate(view.browser(), toggle.first());
    if (!preview.isEmpty()) { QTest::qWait(40); QVERIFY(view.grab().save(preview + "/report-expanded.png")); }
    plain = view.browser()->toPlainText();
    QVERIFY(plain.contains("Review: Markdown"));
    QVERIFY(plain.contains("tests fail at HEAD"));
    QVERIFY(!plain.contains("## Review"));
    QVERIFY(plain.indexOf("Review: Markdown") < plain.indexOf("Fixing it"));
    // Polling keeps the reader's choice; a second click collapses it again.
    view.setActivity({}, {journalEvent(1, "UserPromptSubmit", "Review the branch"), report, journalEvent(3, "Stop", "Fixing it")});
    QVERIFY(view.browser()->toPlainText().contains("tests fail at HEAD"));
    activate(view.browser(), toggle.first());
    QVERIFY(!view.browser()->toPlainText().contains("tests fail at HEAD"));
}

void TestActivityView::taskNotificationIsAColoredNoticeNotYou()
{
    SettledActivityView view; view.setTheme(true);
    auto notice = journalEvent(2, "UserPromptSubmit", "Background command \"Run checks\" completed (exit code 0)");
    notice["origin"] = "task_notification"; notice["agent_id"] = "";
    view.setActivity({}, {journalEvent(1, "UserPromptSubmit", "Run checks in the background"), notice, journalEvent(3, "Stop", "Checks passed")});
    const auto plain = view.browser()->toPlainText();
    QCOMPARE(plain.count("You"), 1);
    QVERIFY(plain.contains("Background task"));
    QVERIFY(plain.contains("Background command \"Run checks\" completed (exit code 0)"));
    QCOMPARE(plain.count("Sent"), 1);
    QVERIFY(plain.indexOf("Background task") < plain.indexOf("Checks passed"));
    // The notice has its own color in both themes.
    QVERIFY(view.browser()->toHtml().contains("#c5a8f5", Qt::CaseInsensitive));
    view.setTheme(false); view.setActivity({}, {notice});
    QVERIFY(view.browser()->toHtml().contains("#6c43b8", Qt::CaseInsensitive));
}

void TestActivityView::kimiTurnStartedKeepsRequestBeforeTools()
{
    SettledActivityView view;
    const QString request = "Investigate the dashboard error";
    QJsonArray events{journalEvent(1, "SessionStart", {}), journalEvent(2, "UserPromptSubmit", {}),
        journalEvent(3, "TurnStarted", request), journalEvent(4, "PreToolUse", "Inspect the logs", "Bash"),
        journalEvent(5, "PostToolUse", "Log inspection finished", "Bash"), journalEvent(6, "Stop", "Found the cause")};
    view.setActivity({{"prompt", request}, {"last_message", "Found the cause"}}, events);
    auto plain = view.browser()->toPlainText();
    QCOMPARE(plain.count(request), 1);
    QVERIFY(!plain.contains("UserPromptSubmit"));
    QVERIFY(!plain.contains("RECORDED CONTEXT"));
    QVERIFY(plain.indexOf(request) < plain.indexOf("1 tool call"));
    QVERIFY(plain.indexOf("1 tool call") < plain.indexOf("Found the cause"));
    // Providers that populate both hooks must not duplicate the request.
    events[1] = journalEvent(2, "UserPromptSubmit", request);
    view.setActivity({{"prompt", request}}, events);
    QCOMPARE(view.browser()->toPlainText().count(request), 1);
    // A genuinely repeated request in a later turn remains visible.
    events.append(journalEvent(7, "TurnStarted", request));
    view.setActivity({{"prompt", request}}, events);
    QCOMPARE(view.browser()->toPlainText().count(request), 2);
    auto child = journalEvent(8, "TurnStarted", "Child task prompt"); child["agent_id"] = "worker";
    events.append(child); view.setActivity({}, events);
    QVERIFY(links(view.browser()).contains("hgs-activity:group-8"));
}

void TestActivityView::confirmedQuestionAnswerStaysInTimeline()
{
    SettledActivityView view;
    auto confirmed = journalEvent(3, "QuestionAnswered", "Which scope? → Documents");
    confirmed["seq"] = 5; // receipt is journaled after a fast subsequent agent response
    view.setActivity({{"prompt", "Ask about scope"}}, {
        journalEvent(1, "TurnStarted", "Ask about scope"),
        journalEvent(2, "PreToolUse", "", "AskUserQuestion"),
        journalEvent(4, "Stop", "I will inspect the documents."), confirmed});
    const auto plain = view.browser()->toPlainText();
    QVERIFY(plain.contains("You (answer)"));
    QVERIFY(plain.indexOf("AskUserQuestion") < plain.indexOf("Which scope?"));
    QVERIFY(plain.indexOf("Which scope?") < plain.indexOf("I will inspect"));
    QCOMPARE(plain.count("Which scope?"), 1);
}

void TestActivityView::nativeQuestionReplies_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("format");
    QTest::addColumn<bool>("dark");
    for (const auto &source : QStringList{"journal", "snapshot", "local", "search"})
        for (const auto &format : QStringList{"array", "object", "ide"})
            for (bool dark : {false, true})
                QTest::newRow(qPrintable(source + '-' + format + (dark ? "-dark" : "-light"))) << source << format << dark;
}

void TestActivityView::nativeQuestionReplies()
{
    QFETCH(QString, source);
    QFETCH(QString, format);
    QFETCH(bool, dark);
    const QString question = "Which review should run before the release?";
    const QString answer = "Review API compatibility and the migration notes.";
    const QJsonObject reply{{"questionItemId", "[\"request_user_input_async\",\"call_scope\",0]"},
        {"question", question}, {"answer", answer}};
    const QJsonObject second{{"questionItemId", "[\"request_user_input_async\",\"call_scope\",1]"},
        {"question", "Include macOS in the checks?"}, {"answer", "Yes, include the desktop client."}};
    const auto payload = format == "object" ? QJsonDocument(reply) : QJsonDocument(QJsonArray{reply, second});
    QString text = "<send_user_message_question_reply>\n" + QString::fromUtf8(payload.toJson(QJsonDocument::Compact))
        + "\n</send_user_message_question_reply>";
    if (format == "ide") text.prepend("# Context from my IDE setup:\nOpen files: src/example.rs\n## My request for Codex:\n");
    SettledActivityView view; view.resize(560, 500); view.setTheme(dark); view.setSessionKey("codex/sample/review"); view.show();
    if (source == "snapshot") view.setActivity({{"prompt", text}}, {});
    else if (source == "local") view.setTimeline({}, {}, {QJsonObject{{"id", "reply"}, {"text", text}, {"status", "sent"}}});
    else if (source == "search") view.showSearchResult(journalEvent(1, "UserPromptSubmit", text), "compatibility");
    else view.setActivity({{"prompt", text}}, {journalEvent(1, "UserPromptSubmit", text)});
    auto *browser = view.browser();
    const auto plain = browser->toPlainText();
    QVERIFY(plain.contains(source == "snapshot" ? "You (recorded answer)" : "You (answer)"));
    QCOMPARE(plain.count(question), 1); QCOMPARE(plain.count(answer), 1);
    QVERIFY(plain.indexOf(question) < plain.indexOf("Your answer"));
    QVERIFY(plain.indexOf("Your answer") < plain.indexOf(answer));
    if (format != "object") {
        QCOMPARE(plain.count("Include macOS in the checks?"), 1);
        QCOMPARE(plain.count("Yes, include the desktop client."), 1);
        QVERIFY(plain.indexOf(answer) < plain.indexOf("Include macOS"));
    }
    QVERIFY(!plain.contains("send_user_message_question_reply"));
    QVERIFY(!plain.contains("questionItemId")); QVERIFY(!plain.contains("call_scope"));
    QVERIFY(!plain.contains("Context from my IDE"));
    QVERIFY(links(browser).isEmpty());
    const auto revision = browser->document()->revision();
    if (source == "journal") {
        view.setActivity({{"prompt", text}}, {journalEvent(1, "UserPromptSubmit", text)});
        QCOMPARE(browser->document()->revision(), revision);
    }
    const auto preview = qEnvironmentVariable("HGS_QUESTION_REPLY_PREVIEW");
    if (!preview.isEmpty() && source == "journal" && format == "array") {
        QDir().mkpath(preview); QTest::qWait(30);
        QVERIFY(view.grab().save(preview + (dark ? "/question-reply-dark.png" : "/question-reply-light.png")));
    }
}

void TestActivityView::unknownQuestionReplyMessages_data()
{
    QTest::addColumn<QString>("text");
    const QString start = "<send_user_message_question_reply>\n";
    const QString end = "\n</send_user_message_question_reply>";
    const QString valid = "{\"questionItemId\":\"reply-1\",\"question\":\"Which scope?\",\"answer\":\"Full review\"}";
    QTest::newRow("ordinary-prose") << "Review **these** changes.\nKeep the original text.";
    QTest::newRow("invalid-json") << start + "not json" + end;
    QTest::newRow("truncated") << start + valid;
    QTest::newRow("empty-array") << start + "[]" + end;
    QTest::newRow("missing-id") << start + "{\"question\":\"Which scope?\",\"answer\":\"Full review\"}" + end;
    QTest::newRow("non-text-answer") << start + "{\"questionItemId\":\"reply-1\",\"question\":\"Which scope?\",\"answer\":[\"Full review\"]}" + end;
    QTest::newRow("mixed-validity") << start + '[' + valid + ",false]" + end;
    QTest::newRow("surrounding-prose") << "An example reply:\n" + start + valid + end;
    QTest::newRow("code-example") << "```text\n" + start + valid + end + "\n```";
}

void TestActivityView::unknownQuestionReplyMessages()
{
    QFETCH(QString, text);
    SettledActivityView view;
    view.setActivity({}, {journalEvent(1, "UserPromptSubmit", text)});
    const auto plain = view.browser()->toPlainText();
    QVERIFY2(plain.contains(text), qPrintable(plain));
    QVERIFY(!plain.contains("You (answer)"));
    QVERIFY(!plain.contains("Your answer"));
}

void TestActivityView::nativeQuestionRepliesPreserveLiteralText()
{
    const QString question = "<img src='file:///private/missing.png'>\nChoose **scope** & priority";
    const QString answer = "> Keep <b>literal markup</b> & `code`\n1. Keep numbering\n[Docs](https://example.com)";
    const auto payload = QJsonDocument(QJsonObject{{"questionItemId", "reply-literal"}, {"question", question}, {"answer", answer}});
    const QString text = "<send_user_message_question_reply>\n" + QString::fromUtf8(payload.toJson(QJsonDocument::Compact))
        + "\n</send_user_message_question_reply>";
    for (bool dark : {false, true}) {
        SettledActivityView view; view.setTheme(dark);
        view.setActivity({}, {journalEvent(1, "UserPromptSubmit", text)});
        const auto plain = view.browser()->toPlainText();
        QVERIFY(plain.contains(question)); QVERIFY(plain.contains(answer));
        QVERIFY(links(view.browser()).isEmpty());
        for (auto block = view.browser()->document()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) QVERIFY(!it.fragment().charFormat().isImageFormat());
    }
}

void TestActivityView::unmatchedContextPrecedesTimeline()
{
    SettledActivityView view; view.resize(560, 360); view.show(); view.setSessionKey("arch/one");
    view.setActivity({{"prompt", "Request missing from journal"}, {"last_message", "Response outside the journal"}}, history(30));
    QTest::qWait(20); // let QTextDocument finish its deferred layout
    auto *browser = view.browser(); auto *bar = browser->verticalScrollBar();
    QTRY_VERIFY(bar->maximum() > 0); QTRY_COMPARE(bar->value(), bar->maximum());
    const auto latest = browser->document()->find("Message number 30");
    QVERIFY(!latest.isNull());
    const QRect answerRect = browser->cursorRect(latest);
    QVERIFY(answerRect.top() >= 0); QVERIFY(answerRect.bottom() <= browser->viewport()->height());
    const auto plain = browser->toPlainText();
    QVERIFY(plain.indexOf("Request missing from journal") < plain.indexOf("Message number 1"));
    QVERIFY(plain.indexOf("Response outside the journal") < plain.indexOf("Message number 1"));
    QVERIFY(plain.indexOf("Message number 1") < plain.indexOf("Message number 30"));
}

void TestActivityView::failedMessageActionsFollowCardIdentity()
{
    SettledActivityView view;view.resize(600,350);view.show();view.setTheme(true);view.setSessionKey("session-one");
    QSignalSpy actions(&view,&ActivityView::messageActionRequested);
    const QJsonArray events{journalEvent(1,"Stop","Earlier response"),journalEvent(4,"Stop","Later response")};
    view.setActivity({},events);
    QJsonObject message{{"id","failed-one"},{"text","Original failed request"},{"status","error"},{"error","Terminal was not ready"},{"submitted_at",1791018002.0}};
    view.setLocalMessages({message});auto text=view.browser()->toPlainText();
    QVERIFY(text.indexOf("Original failed request")<text.indexOf("Later response"));QVERIFY(text.contains("Retry"));QVERIFY(text.contains("Delete"));
    QStringList commands;for(const auto &url:links(view.browser()))if(url.startsWith("hgs-message:"))commands.append(url);
    QCOMPARE(commands.size(),2);activate(view.browser(),commands[0]);QCOMPARE(actions.size(),1);
    QCOMPARE(actions[0][0].toString(),QString("failed-one"));QCOMPARE(actions[0][1].toString(),QString("retry"));
    activate(view.browser(),commands[1]);QCOMPARE(actions[1][1].toString(),QString("delete"));
    const auto preview=qEnvironmentVariable("HGS_FAILED_MESSAGE_PREVIEW");if(!preview.isEmpty()){QTest::qWait(30);QVERIFY(view.grab().save(preview));}
    message["uncertain"]=true;view.setLocalMessages({message});text=view.browser()->toPlainText();
    QVERIFY(text.contains("Delivery not confirmed"));QVERIFY(text.contains("Check Terminal"));QVERIFY(!text.contains("Retry"));
    activate(view.browser(),commands[0]);QCOMPARE(actions.size(),2); // Old Retry is no longer registered.
    view.setActivity({{"native_ui_available",true}},events);QVERIFY(view.browser()->toPlainText().contains("Check native UI"));
    view.setSessionKey("other");activate(view.browser(),commands[1]);QCOMPARE(actions.size(),2);
}

void TestActivityView::localDeliveryCards()
{
    SettledActivityView view; view.setSessionKey("arch/one"); view.setActivity({}, history(2));
    QJsonObject message{{"id", "outgoing-1"}, {"text", "Look at **this image**"}, {"status", "sending"},
        {"attachments", QJsonArray{QJsonObject{{"name", "screenshot.png"}}}}};
    view.setLocalMessages({message});
    QVERIFY(view.browser()->toPlainText().contains("Sending…"));
    QVERIFY(view.browser()->toPlainText().contains("Attachment: screenshot.png"));
    message["status"] = "sent"; view.setLocalMessages({message});
    QVERIFY(view.browser()->toPlainText().contains("Submitted to terminal"));
    message["status"] = "error"; message["error"] = "Connection interrupted"; view.setLocalMessages({message});
    QVERIFY(view.browser()->toPlainText().contains("Not sent"));
    QVERIFY(view.browser()->toPlainText().contains("Connection interrupted"));
    view.setSessionKey("mac/two"); view.setActivity({}, {});
    QVERIFY(!view.browser()->toPlainText().contains("screenshot.png"));
}

void TestActivityView::preview()
{
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (directory.isEmpty()) QSKIP("Set HGS_PREVIEW_DIR to render the activity preview");
    QDir().mkpath(directory);
    for (bool dark : {true, false}) {
        SettledActivityView view; view.resize(630, 750); view.setTheme(dark); view.show();
        view.setActivity({}, {
            journalEvent(1, "UserPromptSubmit", "Make the activity easier to read.\n\nKeep **messages** separate from tool output and show the latest event by default."),
            journalEvent(2, "PreToolUse", "rg -n renderDetails tray/src/SessionsWindow.cpp", "Bash"),
            journalEvent(3, "PostToolUse", "Found the activity rendering path", "Bash"),
            journalEvent(4, "PreToolUse", "Add ActivityView and preserve the reading position", "apply_patch"),
            journalEvent(5, "PostToolUse", "Updated ActivityView.cpp", "apply_patch"),
            QJsonObject{{"seq", 6}, {"at", 1791018006}, {"type", "UserPromptSubmit"}, {"agent_id", ""}, {"origin", "task_notification"},
                {"detail", "Background command \"Run focused checks\" completed (exit code 0)"}},
            journalEvent(7, "Stop", "## Summary\n\nFixed launching sessions with a space in the name:\n\n"
                "- `src/cli.rs` — `--new` now follows `rename`\n- `tests/test_hgs.sh` — new checks\n  - nested item with **bold** text\n"
                "- see [docs](https://example.com) and [client registry](docs/client.md:24)\n\n1. First step\n2. Second step\n\n"
                "| File | Lines | Status |\n|------|------:|--------|\n| `src/cli.rs` | 7 | changed |\n| `tests/test_hgs.sh` | 18 | added |\n| `README.md` | 0 | unchanged |\n\n"
                "> Remote machines must update `hgs`.\n\n```sh\nctest --test-dir tray/build -R activityview\n```\n\n---\n\n"
                "All focused checks passed; run `cargo test --workspace --all-features --no-fail-fast` again before pushing.")});
        QTest::qWait(40);
        QVERIFY(view.grab().save(directory + (dark ? "/activity-dark.png" : "/activity-light.png")));
    }
}

void TestActivityView::caughtUpActivityAcknowledgesEarlierReplyWithoutIntermediatePaint()
{
    SettledActivityView view;view.resize(560,250);view.show();view.setSessionKey("polling");
    QJsonObject details{{"conversation_id","same"},{"reply_id","10:100"},{"activity","busy"}};
    QJsonArray events{QJsonObject{{"seq",10},{"type","Stop"},{"at",100},{"detail","Completed the previous step."}}};
    for(int i=11;i<120;++i)events.append(QJsonObject{{"seq",i},{"at",100+i},{"type","AgentMessage"},{"detail",QString("Progress update %1").arg(i)}});
    view.setActivity(details,events);QTest::qWait(20);view.jumpToLatest();QTest::qWait(20);
    QVERIFY(view.replyVisible("10:100"));
    auto *browser=view.browser();browser->verticalScrollBar()->setValue(0);QVERIFY(!view.replyVisible("10:100"));
    auto *previousDocument = browser->document();
    events.append(QJsonObject{{"seq",120},{"at",220},{"type","AgentMessage"},{"detail","Another update"}});
    view.setActivity(details,events);QVERIFY(browser->document() != previousDocument);
    QTRY_VERIFY(browser->updatesEnabled());QCOMPARE(browser->verticalScrollBar()->value(),0);
    const auto revision=browser->document()->revision();view.setActivity(details,events);QCOMPARE(browser->document()->revision(),revision);
}

void TestActivityView::readingColumnCentersTranscriptAndQueue()
{
    SettledActivityView view; view.resize(1400, 400); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QJsonObject details{{"input_queue", QJsonObject{{"id", "queue"}, {"text", "queued words"}}}};
    view.setActivity(details, history(30));
    auto *browser = view.browser(); auto *viewport = browser->viewport(); auto *bar = browser->verticalScrollBar();
    auto *queue = view.findChild<QWidget *>("activityInputQueue"); QVERIFY(queue && queue->isVisible());
    QTRY_VERIFY(bar->isVisible());
    // Without a column the scroll bar takes its usual place beside the text.
    const int reserve = browser->width() - viewport->width(); QVERIFY(reserve > 0);
    QCOMPARE(queue->width(), 1400);
    view.setColumnWidth(800);
    QTRY_COMPARE(viewport->width(), 800);
    QCOMPARE(viewport->x(), 300);
    QTRY_COMPARE(queue->geometry().x(), 300); QCOMPARE(queue->width(), 800);
    // The scroll bar stays at the pane edge, outside the column.
    QCOMPARE(bar->mapTo(browser, QPoint(bar->width(), 0)).x(), browser->width());
    // The margins scroll the transcript as the column does.
    bar->setValue(0);
    QWheelEvent wheel(QPointF(40, 200), browser->mapToGlobal(QPointF(40, 200)), {}, QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(browser, &wheel);
    QVERIFY(bar->value() > 0);
    // A pane narrower than the column keeps its whole width.
    view.resize(700, 400);
    QTRY_COMPARE(viewport->width(), 700 - reserve); QCOMPARE(viewport->x(), 0);
    QTRY_COMPARE(queue->width(), 700);
    view.resize(1400, 400);
    QTRY_COMPARE(viewport->width(), 800); QCOMPARE(viewport->x(), 300);
    view.setColumnWidth(0);
    QTRY_COMPARE(viewport->width(), 1400 - reserve); QCOMPARE(viewport->x(), 0);
    QTRY_COMPARE(queue->width(), 1400);
}

void TestActivityView::columnEdgesResizeSymmetrically()
{
    SettledActivityView view; view.resize(1400, 400); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setActivity({}, history(30));
    // The owner stores the requested width and applies it to every column.
    connect(&view, &ActivityView::columnWidthRequested, &view, &ActivityView::setColumnWidth);
    QSignalSpy requested(&view, &ActivityView::columnWidthRequested), reset(&view, &ActivityView::columnResetRequested);
    auto *viewport = view.browser()->viewport();
    QCOMPARE(columnEdges(view).size(), 2);
    for (auto *edge : columnEdges(view)) QVERIFY(!edge->isVisible());
    view.setColumnWidth(800);
    QTRY_VERIFY(columnEdges(view)[0]->isVisible() && columnEdges(view)[1]->isVisible());
    // Each edge sits in the margin, right beside the column.
    QCOMPARE(columnEdges(view)[0]->geometry().right() + 1, viewport->x());
    QCOMPARE(columnEdges(view)[1]->x(), viewport->geometry().right() + 1);
    QCOMPARE(columnEdges(view)[1]->cursor().shape(), Qt::SizeHorCursor);
    // Both sides move together, so the column stays centered.
    dragEdge(columnEdges(view)[1], 50);
    QCOMPARE(requested.last().first().toInt(), 900); QCOMPARE(viewport->width(), 900); QCOMPARE(viewport->x(), 250);
    dragEdge(columnEdges(view)[0], -30);
    QCOMPARE(viewport->width(), 960); QCOMPARE(viewport->x(), 220);
    dragEdge(columnEdges(view)[1], -40);
    QCOMPARE(viewport->width(), 880);
    // The column keeps a readable minimum and its edges stay within the pane.
    dragEdge(columnEdges(view)[1], -1000);
    QCOMPARE(viewport->width(), 480);
    dragEdge(columnEdges(view)[0], -1000);
    QVERIFY2(viewport->width() > 1300 && viewport->width() < 1400, qPrintable(QString::number(viewport->width())));
    QVERIFY(columnEdges(view)[0]->isVisible() && columnEdges(view)[1]->isVisible());
    QCOMPARE(reset.count(), 0);
    const auto *edge = columnEdges(view)[1]; const QPoint at = edge->mapToGlobal(QPoint(edge->width() / 2, 50));
    mouse(columnEdges(view)[1], QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(reset.count(), 1);
    // A pane without margins leaves its edges to text selection.
    view.resize(1000, 400);
    QTRY_VERIFY(!columnEdges(view)[0]->isVisible() && !columnEdges(view)[1]->isVisible());
}

void TestActivityView::contextMenuSwitchesFullWidth()
{
    SettledActivityView view; view.resize(1400, 400); view.show(); QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setActivity({}, history(4));
    QSignalSpy full(&view, &ActivityView::fullWidthRequested), reset(&view, &ActivityView::columnResetRequested);
    view.setColumnWidth(800);
    auto *browser = view.browser();
    // The transcript keeps its own actions; its margins offer only the column.
    auto *menu = contextMenu(browser->viewport(), QPoint(20, 20)); QVERIFY(menu);
    const auto copies = [](QMenu *menu) {
        return std::any_of(menu->actions().cbegin(), menu->actions().cend(), [](QAction *action) { return action->text().startsWith("&Copy"); });
    };
    QVERIFY(copies(menu));
    auto *option = menuAction(menu, tr("Full-width Activity")); QVERIFY(option);
    QVERIFY(option->isCheckable() && !option->isChecked());
    QVERIFY(menuAction(menu, tr("Reset Activity width"))->isEnabled());
    option->trigger(); menu->close();
    QCOMPARE(full.count(), 1); QCOMPARE(full.last().first().toBool(), true);
    view.setColumnWidth(0);
    QTRY_COMPARE(browser->viewport()->x(), 0);
    view.setColumnWidth(800);
    QTRY_COMPARE(browser->viewport()->x(), 300);
    menu = contextMenu(browser, QPoint(40, 200)); QVERIFY(menu);
    QVERIFY(!copies(menu)); QVERIFY(menuAction(menu, tr("Full-width Activity")));
    menuAction(menu, tr("Reset Activity width"))->trigger(); menu->close();
    QCOMPARE(reset.count(), 1);
    view.setColumnWidth(0);
    menu = contextMenu(browser->viewport(), QPoint(20, 20)); QVERIFY(menu);
    option = menuAction(menu, tr("Full-width Activity")); QVERIFY(option->isChecked());
    option->trigger(); menu->close();
    QCOMPARE(full.count(), 2); QCOMPARE(full.last().first().toBool(), false);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QTemporaryDir settings;
    app.setOrganizationName("ZerusTests"); app.setApplicationName("ActivityView");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    TestActivityView test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_activityview.moc"
