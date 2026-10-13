#include "MessageComposer.h"
#include "AttachmentViewer.h"
#include "ComposerToolbar.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QClipboard>
#include <QTimeZone>
#include <QCryptographicHash>
#include <QBuffer>
#include <QEnterEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFontInfo>
#include <QFrame>
#include <QImage>
#include <QKeySequence>
#include <QInputMethodEvent>
#include <QLabel>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QLineEdit>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QSemaphore>
#include <QThread>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>
#include <QStyleOptionComboBox>
#include <QUrl>
#include <QVBoxLayout>
#include <atomic>
#include <thread>
#include <QToolTip>
#include <QVBoxLayout>

namespace {
void setSessionReady(MessageComposer &composer, const QString &key);
struct Submission {
    QString key, text;
    QList<MessageAttachment> attachments;
};

void observe(MessageComposer &composer, QList<Submission> &submissions)
{
    QObject::connect(&composer, &MessageComposer::sendRequested, &composer,
        [&submissions](const QString &key, const QString &text, const QList<MessageAttachment> &attachments) {
            submissions.append({key, text, attachments});
        });
}

QPushButton *sendButton(MessageComposer &composer) { return composer.findChild<QPushButton *>("sendMessage"); }

QByteArray pngBytes()
{
    QImage image(8, 6, QImage::Format_RGB32); image.fill(Qt::darkCyan);
    QByteArray data; QBuffer buffer(&data); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return data;
}

struct Window {
    QWidget widget; MessageComposer *composer = new MessageComposer;
    explicit Window(const QString &key) {
        auto *layout = new QVBoxLayout(&widget); layout->addWidget(composer);
        setSessionReady(*composer, key); composer->setAvailability(true); widget.resize(720, 260); widget.show();
    }
    ToolbarChip *chip() const { return composer->findChild<ToolbarChip *>("attachmentsChip"); }
};
QLabel *status(MessageComposer &composer) { return composer.findChild<QLabel *>("messageStatus"); }

// Exercise the editor's MIME insertion path without reading or overwriting the
// user's real clipboard. A drop forwards the same QMimeData into that path.
void drop(QPlainTextEdit *editor, const QMimeData &mime)
{
    const QPoint position = editor->cursorRect().center();
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(), &enter);
    QVERIFY2(enter.isAccepted(), "Editor should accept supported synthetic MIME data");
    QDragMoveEvent move(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(), &move);
    QVERIFY2(move.isAccepted(), "Editor should show the native drop insertion position");
    QDropEvent drop(QPointF(position), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor->viewport(), &drop);
    QVERIFY2(drop.isAccepted(), "Editor should accept the synthetic drop");
}
}

namespace {
void setSessionReady(MessageComposer &composer, const QString &key)
{
    composer.setSessionKey(key);
    QTRY_VERIFY(!composer.isDraftLoading(key));
}
bool setSendingReady(MessageComposer &composer, const QString &key, bool preserve = false)
{
    bool finished = false, okay = false;
    composer.prepareSendingAsync(key, preserve, [&](bool result) { okay = result; finished = true; });
    QElapsedTimer clock; clock.start();
    while (!finished && clock.elapsed() < 3000) QTest::qWait(1);
    return finished && okay;
}
}

class TestMessageComposer : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void voiceInsertionRequiresUnchangedDraftAndNavigation();
    void init() { ComposerDraftStore::flushWrites(); QVERIFY(QDir(ComposerDraftStore::directory()).removeRecursively()); }
    void crashRestoresLastEditAndAttachmentBytes();
    void restartKeepsIsolationRenamesAndAttachmentRemoval();
    void restoredDraftPresenceNeedsNoConversationOrAttachmentRead();
    void deliveryPersistence_data();
    void deliveryPersistence();
    void storageFailureKeepsDraftAndBlocksDispatch();
    void endedSessionDraftCanBeRecovered();
    void draftReplacementRequiresConfirmation();
    void renameCollisionKeepsBothDrafts();
    void typingDoesNotRewriteAttachmentSnapshots();
    void slowDraftWriteDoesNotBlockTypingAndSendOrdersLatestState();
    void sendPreparationWaitsForDurableLatestSnapshot();
    void stalledWriterAllowsNavigationAndDurableSend();
    void stalledRenameSurvivesEditorDestruction();
    void stalledRenameUpdatesPreviouslyLoadedDestination();
    void stalledRenameFailureKeepsSourceRecoverable();
    void cancelledPreparationCannotSettleNewAttempt();
    void stalledDurabilityFailureKeepsTextAndAttachments();
    void isolatesDraftsAcrossSessions();
    void enterSendsShiftEnterAddsLine();
    void sendingKeepsEditorFocus_data();
    void sendingKeepsEditorFocus();
    void deliveryDoesNotStealDeliberatelyMovedFocus();
    void inputMethodDoesNotAccidentallySubmit();
    void availabilityAndBusyGateSubmission();
    void syntheticImageAndFileDrop();
    void inlineReferencesFollowCursorAndSurviveEditing();
    void escapeReleasesFocusWithoutChangingDraft();
    void attachmentLimits();
    void textLimitCountsUtf8Bytes();
    void failureAndUncertaintyPreserveDraft();
    void sendingHidesPayloadAndRestoresFailedBackgroundDelivery();
    void waitingPayloadLocksEditsAndPreservesFocusAndSelection();
    void waitingIntentRestoresAsAnEditableDraftAfterRestart();
    void backgroundSuccessDoesNotClearVisibleDraft();
    void renamePreservesDraft();
    void retryOfAnotherMessagePreservesDraftAndUncertainty();
    void modelSettingsPreserveDraftAndDistinguishPending();
    void modelSettingsUnavailableExplainsWhy();
    void modelSettingsFitNarrowColumn();
    void sessionStatusPreservesGeometryAndLockedDraft_data();
    void sessionStatusPreservesGeometryAndLockedDraft();
    void cursorStaysWithDraftAcrossPollingSwitchesAndFailures();
    void attachmentDropReleasesVisualCaret_data();
    void attachmentDropReleasesVisualCaret();
    void unsupportedModelOffersTerminal();
    void contentScaleEnlargesOnlyTheMessageField();
    void draftStateFollowsUnsentContent();
    void attachmentsChipSummarizesAndRemoves();
    void programAttachmentRowIsShownInItsFolder();
    void droppedShortcutIsCheckedAsTheFileItReads();
    void attachmentsPopoverFollowsSessionAndSending();
    void attachmentRowOpensStoredCopyAndPreviews();
    void hiddenInputKeepsToolbar();
    void attachmentsPopoverShrinksAndTakesPreviewAlong();
    void keyboardRemovalKeepsFocusInTheList();
    void toolbarPreview();
private:
    QTemporaryDir m_settings;
};

void TestMessageComposer::initTestCase()
{
    QVERIFY(m_settings.isValid());
    QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("message-composer");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
}

void TestMessageComposer::crashRestoresLastEditAndAttachmentBytes()
{
    QProcess child; child.setProcessChannelMode(QProcess::SeparateChannels);
    child.start(QCoreApplication::applicationFilePath(), {"--draft-crash-child", m_settings.path()});
    QVERIFY(child.waitForStarted());
    // Diagnostics on stderr can arrive first; wait for the child's own report.
    QByteArray saved;
    QTRY_VERIFY2_WITH_TIMEOUT((saved += child.readAllStandardOutput()).contains('\n'),
                              qPrintable(QString::fromUtf8(child.readAllStandardError())), 10000);
    QCOMPARE(saved.trimmed(), QByteArray("saved"));
    child.kill(); QVERIFY(child.waitForFinished()); QCOMPARE(child.exitStatus(), QProcess::CrashExit);
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch\nended/session"); composer.setAvailability(true);
    QCOMPARE(composer.editor()->toPlainText(), QString::fromUtf8("[File #1] Last keystroke я\nsecond line"));
    QCOMPARE(composer.editor()->textCursor().position(), 8); QCOMPARE(composer.editor()->textCursor().anchor(), 3);
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 1);
    QCOMPARE(submissions[0].attachments.size(), 1); QCOMPARE(submissions[0].attachments[0].data, QByteArray("exact\0bytes", 11));
#ifdef Q_OS_UNIX
    // Windows keeps drafts private through the profile directory's ACL; Qt
    // reports no owner-only modes there.
    const auto root = ComposerDraftStore::directory();
    for (const auto &folder : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto permissions = QFileInfo(root + '/' + folder).permissions();
        QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
        for (const auto &file : QDir(root + '/' + folder).entryList(QDir::Files))
            QVERIFY(!(QFileInfo(root + '/' + folder + '/' + file).permissions() & (QFile::ReadGroup | QFile::ReadOther)));
    }
#endif
}

void TestMessageComposer::restartKeepsIsolationRenamesAndAttachmentRemoval()
{
    {
        MessageComposer composer; setSessionReady(composer, "arch\nsame"); composer.editor()->setPlainText("Local");
        QVERIFY(composer.addAttachment("local.txt", "text/plain", "snapshot"));
        composer.editor()->setPlainText("Local without marker");
        composer.findChild<QPushButton *>("removeAttachment")->click();
        setSessionReady(composer, "mac\nsame"); composer.editor()->setPlainText("Remote");
        composer.renameDraft("mac\nsame", "mac\nrenamed");
        setSessionReady(composer, "arch\nsame\nconversation\nchild"); composer.editor()->setPlainText("Child");
    }
    MessageComposer restored; setSessionReady(restored, "arch\nsame");
    QCOMPARE(restored.editor()->toPlainText(), "Local without marker");
    QCOMPARE(restored.findChildren<QPushButton *>("removeAttachment").size(), 0);
    setSessionReady(restored, "mac\nsame"); QVERIFY(restored.editor()->toPlainText().isEmpty());
    setSessionReady(restored, "mac\nrenamed"); QCOMPARE(restored.editor()->toPlainText(), "Remote");
    setSessionReady(restored, "arch\nsame\nconversation\nchild"); QCOMPARE(restored.editor()->toPlainText(), "Child");
    restored.setAvailability(false, "Machine unavailable"); setSessionReady(restored, {});
    setSessionReady(restored, "arch\nsame"); QCOMPARE(restored.editor()->toPlainText(), "Local without marker");
}

void TestMessageComposer::restoredDraftPresenceNeedsNoConversationOrAttachmentRead()
{
    ComposerDraftStore store; ComposerDraft text; text.text = "Restored text";
    QVERIFY(store.save("\ntext", text));
    ComposerDraft attachment; attachment.attachments = {{"notes.txt", "text/plain", "snapshot", {}}};
    QVERIFY(store.save("mac\nattachment", attachment));
    ComposerDraft whitespace; whitespace.text = " \n\t"; QVERIFY(store.save("\nblank", whitespace));
    MessageComposer restored;
    QTRY_VERIFY(restored.hasDraft("\ntext")); QVERIFY(restored.hasDraft("mac\nattachment"));
    QVERIFY(!restored.hasDraft("\nblank")); QVERIFY(!restored.hasDraft("\nmissing"));
    QVERIFY(restored.sessionKey().isEmpty()); QVERIFY(restored.editor()->toPlainText().isEmpty());
    // Presence uses metadata: removing the bytes after saving does not require
    // reading them merely to decide whether a conversation belongs in Active.
    for (const auto &folder : QDir(ComposerDraftStore::directory()).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        for (const auto &name : QDir(ComposerDraftStore::directory() + '/' + folder).entryList({"*.bin"}, QDir::Files))
            QVERIFY(QFile::remove(ComposerDraftStore::directory() + '/' + folder + '/' + name));
    MessageComposer metadata; QTRY_VERIFY(metadata.hasDraft("mac\nattachment"));
    setSessionReady(restored, "\ntext"); restored.renameDraft("\ntext", "\nrenamed");
    QTRY_VERIFY(!restored.hasDraft("\ntext")); QTRY_VERIFY(restored.hasDraft("\nrenamed"));
    restored.editor()->clear(); QVERIFY(!restored.hasDraft("\nrenamed"));
}

void TestMessageComposer::deliveryPersistence_data()
{
    QTest::addColumn<QString>("outcome");
    for (const auto &outcome : {"success", "failure", "uncertain", "interrupted", "retry-old-message"}) QTest::newRow(outcome) << QString(outcome);
}

void TestMessageComposer::deliveryPersistence()
{
    QFETCH(QString, outcome);
    {
        MessageComposer composer; setSessionReady(composer, "arch\none"); composer.editor()->setPlainText("Never lose this");
        QVERIFY(composer.addAttachment("keep.txt", "text/plain", "keep"));
        QVERIFY(setSendingReady(composer, "arch\none", outcome == "retry-old-message"));
        if (outcome != "interrupted") composer.deliveryFinished("arch\none", outcome == "success" || outcome == "retry-old-message", "Failed", outcome == "uncertain");
    }
    MessageComposer restored; QList<Submission> submissions; observe(restored, submissions);
    setSessionReady(restored, "arch\none"); restored.setAvailability(true);
    const bool success = outcome == "success";
    QCOMPARE(restored.editor()->toPlainText(), success ? QString() : QString("[File #1] Never lose this"));
    QCOMPARE(restored.findChildren<QPushButton *>("removeAttachment").size(), success ? 0 : 1);
    QVERIFY(!restored.isSending("arch\none"));
    const bool uncertain = outcome == "interrupted" || outcome == "uncertain";
    QCOMPARE(sendButton(restored)->isEnabled(), !success && !uncertain);
    sendButton(restored)->click(); QCOMPARE(submissions.size(), !success && !uncertain ? 1 : 0);
    if (uncertain) { restored.findChild<QPushButton *>("allowMessageRetry")->click(); QVERIFY(sendButton(restored)->isEnabled()); }
}

void TestMessageComposer::storageFailureKeepsDraftAndBlocksDispatch()
{
    QVERIFY(QDir().mkpath(QFileInfo(ComposerDraftStore::directory()).absolutePath()));
    QFile blocker(ComposerDraftStore::directory()); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
    MessageComposer composer; setSessionReady(composer, "one"); composer.setAvailability(true);
    composer.editor()->setPlainText("Still editable");
    QTRY_VERIFY(status(composer)->text().contains("not saved"));
    QVERIFY(!composer.setWaitingToSend("one", "Waiting for compaction."));
    QVERIFY(!composer.isWaitingToSend("one")); QVERIFY(!composer.editor()->isReadOnly());
    QVERIFY(!setSendingReady(composer, "one")); QVERIFY(!composer.editor()->isReadOnly());
    QCOMPARE(composer.editor()->toPlainText(), "Still editable");
    QVERIFY(blocker.remove()); composer.editor()->insertPlainText("X");
    QTRY_VERIFY(status(composer)->text().isEmpty()); QVERIFY(status(composer)->isHidden());
    MessageComposer restored; setSessionReady(restored, "one"); QCOMPARE(restored.editor()->toPlainText(), composer.editor()->toPlainText());
}

void TestMessageComposer::endedSessionDraftCanBeRecovered()
{
    MessageComposer composer; setSessionReady(composer, "lost\nended"); composer.editor()->setPlainText("Recover my work");
    QVERIFY(composer.addAttachment("keep.txt", "text/plain", "important"));
    setSessionReady(composer, "arch\nnew"); composer.setAvailability(true);
    QList<Submission> submissions; observe(composer, submissions);
    QTimer::singleShot(0, &composer, [&] {
        auto *dialog = composer.findChild<QDialog *>("savedDraftsDialog"); QVERIFY(dialog);
        auto *list = dialog->findChild<QListWidget *>("savedDraftList"); QTRY_COMPARE(list->count(), 1);
        QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), "lost\nended");
        dialog->findChild<QPushButton *>("restoreSavedDraft")->click();
    });
    composer.showSavedDrafts(); QCOMPARE(submissions.size(), 0);
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] Recover my work");
    sendButton(composer)->click(); QCOMPARE(submissions[0].attachments[0].data, QByteArray("important"));
    QVERIFY(ComposerDraftStore().keys().contains("lost\nended"));
}

void TestMessageComposer::draftReplacementRequiresConfirmation()
{
    MessageComposer composer; setSessionReady(composer, "arch\nsource"); composer.editor()->setPlainText("Saved source");
    setSessionReady(composer, "mac\ntarget"); composer.editor()->setPlainText("Current draft");
    QTimer::singleShot(0, &composer, [&] {
        auto *dialog = composer.findChild<QDialog *>("savedDraftsDialog"); QVERIFY(dialog);
        auto *list = dialog->findChild<QListWidget *>("savedDraftList"); QTRY_VERIFY(list->count() > 0); list->setCurrentRow(0);
        dialog->findChild<QPushButton *>("copySavedDraft")->click(); QVERIFY(dialog->isVisible());
        QTimer::singleShot(0, &composer, [&] {
            auto *confirm = composer.findChild<QMessageBox *>("replaceDraftConfirm"); QVERIFY(confirm);
            QCOMPARE(confirm->defaultButton(), confirm->button(QMessageBox::Cancel)); confirm->reject();
        });
        dialog->findChild<QPushButton *>("restoreSavedDraft")->click(); QVERIFY(dialog->isVisible()); dialog->reject();
    });
    composer.showSavedDrafts(); QCOMPARE(composer.editor()->toPlainText(), "Current draft");
    MessageComposer restarted; setSessionReady(restarted, "mac\ntarget"); QCOMPARE(restarted.editor()->toPlainText(), "Current draft");
}

void TestMessageComposer::renameCollisionKeepsBothDrafts()
{
    MessageComposer composer; setSessionReady(composer, "old"); composer.editor()->setPlainText("Original");
    setSessionReady(composer, "new"); composer.editor()->setPlainText("Other saved draft");
    setSessionReady(composer, "old"); composer.renameDraft("old", "new");
    QTRY_COMPARE(composer.editor()->toPlainText(), "Other saved draft");
    MessageComposer restarted; setSessionReady(restarted, "old"); QCOMPARE(restarted.editor()->toPlainText(), "Original");
    setSessionReady(restarted, "new"); QCOMPARE(restarted.editor()->toPlainText(), "Other saved draft");
}

void TestMessageComposer::typingDoesNotRewriteAttachmentSnapshots()
{
    MessageComposer composer; setSessionReady(composer, "one");
    QVERIFY(composer.addAttachment("large.bin", "application/octet-stream", QByteArray(10 * 1024 * 1024, 'x')));
    ComposerDraftStore::flushWrites();
    const auto folders = QDir(ComposerDraftStore::directory()).entryList(QDir::Dirs | QDir::NoDotAndDotDot); QCOMPARE(folders.size(), 1);
    const QDir folder(ComposerDraftStore::directory() + '/' + folders[0]); const auto files = folder.entryList({"*.bin"}, QDir::Files); QCOMPARE(files.size(), 1);
    QFile snapshot(folder.filePath(files[0])); const auto timestamp = QDateTime::fromSecsSinceEpoch(1000000000, QTimeZone::UTC);
    QVERIFY(snapshot.open(QIODevice::ReadWrite)); QVERIFY(snapshot.setFileTime(timestamp, QFileDevice::FileModificationTime)); snapshot.close();
    QTest::keyClicks(composer.editor(), "Every character is saved");
    // In UTC: Windows file times converted through local time may disagree with Qt
    // on historic daylight saving time.
    QCOMPARE(QFileInfo(snapshot).lastModified(QTimeZone::UTC), timestamp);
    QVERIFY(composer.addAttachment("second.bin", "application/octet-stream", "second"));
    const auto before = composer.editor()->toPlainText();
    MessageComposer restored; setSessionReady(restored, "one"); QCOMPARE(restored.editor()->toPlainText(), before);
    QVERIFY(restored.addAttachment("third.bin", "application/octet-stream", "third"));
    QVERIFY(restored.editor()->toPlainText().contains("[File #3]"));
}

void TestMessageComposer::slowDraftWriteDoesNotBlockTypingAndSendOrdersLatestState()
{
    MessageComposer composer; setSessionReady(composer, "slow\nexact"); composer.setAvailability(true);
    composer.resize(440, 220); composer.show(); QTest::qWait(20);
    QVERIFY(status(composer)->isHidden()); const auto quietSize = composer.sizeHint();
    QSemaphore entered, release;
    std::atomic<int> writes = 0;
    QThread *writer = nullptr;
    ComposerDraftStore::setWriteObserver([&] {
        writer = QThread::currentThread();
        if (++writes == 1) { entered.release(); release.acquire(); }
    });
    auto cleanup = qScopeGuard([&] {
        release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({});
    });
    composer.editor()->setPlainText("First");
    QVERIFY(entered.tryAcquire(1, 3000));
    QVERIFY(writer != QThread::currentThread());
    composer.editor()->moveCursor(QTextCursor::End);
    // The writer stays stalled throughout these real editor events.
    QTest::keyClicks(composer.editor(), " latest edits");
    const auto latest = composer.editor()->toPlainText();
    QVERIFY(latest.endsWith(" latest edits"));
    bool heartbeat = false;
    QTimer::singleShot(0, &composer, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    QVERIFY(status(composer)->text().isEmpty()); QVERIFY(status(composer)->isHidden());
    QCOMPARE(composer.sizeHint(), quietSize);
    release.release(); ComposerDraftStore::flushWrites();
    QCOMPARE(ComposerDraftStore().load("slow\nexact").text, latest);
    QVERIFY(writes <= 2); // At most the in-flight and coalesced latest edit.
    QVERIFY(setSendingReady(composer, "slow\nexact"));
    QVERIFY(status(composer)->text().isEmpty()); QVERIFY(status(composer)->isHidden());
    QVERIFY(ComposerDraftStore().load("slow\nexact").uncertain);
    composer.deliveryFinished("slow\nexact", true, {});
    QCoreApplication::processEvents(); // Obsolete async acknowledgements cannot restore the text.
    QVERIFY(ComposerDraftStore().load("slow\nexact").text.isEmpty());
    QVERIFY(composer.editor()->toPlainText().isEmpty());
    QVERIFY(status(composer)->text().isEmpty()); QVERIFY(status(composer)->isHidden());
    QCOMPARE(composer.sizeHint(), quietSize);
}

void TestMessageComposer::sendPreparationWaitsForDurableLatestSnapshot()
{
    MessageComposer composer; setSessionReady(composer, "submit\nexact"); composer.setAvailability(true);
    QSemaphore entered, release; std::atomic<int> writes = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++writes == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] {
        release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({});
    });
    composer.editor()->setPlainText("Latest payload");
    QVERIFY(entered.tryAcquire(1, 3000));
    bool checkpoint = false; int submissions = 0;
    connect(&composer, &MessageComposer::sendRequested, &composer, [&](const QString &, const QString &text, const QList<MessageAttachment> &) {
        ++submissions;
        const auto folders = QDir(ComposerDraftStore::directory()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (folders.size() != 1) return;
        QFile manifest(ComposerDraftStore::directory() + '/' + folders[0] + "/draft.json");
        if (manifest.open(QIODevice::ReadOnly)) checkpoint = QJsonDocument::fromJson(manifest.readAll()).object().value("text").toString() == text;
    });
    sendButton(composer)->click();
    QCOMPARE(submissions, 1); // Intent signal; the owner performs the durability transition.
    QVERIFY(!checkpoint);
    bool durable = false;
    composer.prepareSendingAsync("submit\nexact", false, [&](bool okay) { durable = okay; });
    bool heartbeat = false; QTimer::singleShot(0, &composer, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat); QVERIFY(!durable);
    release.release(); QTRY_VERIFY(durable);
    QVERIFY(ComposerDraftStore().load("submit\nexact").uncertain);
}

void TestMessageComposer::stalledWriterAllowsNavigationAndDurableSend()
{
    MessageComposer composer; setSessionReady(composer, "stalled/original"); composer.setAvailability(true);
    QVERIFY(composer.addAttachment("fixture.txt", "text/plain", QByteArray("immutable attachment")));
    ComposerDraftStore::flushWrites();
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    composer.editor()->insertPlainText("Accepted payload"); QVERIFY(entered.tryAcquire(1, 3000));
    const auto text = composer.editor()->toPlainText();
    int transports = 0, duplicatesRejected = 0;
    composer.prepareSendingAsync("stalled/original", false, [&](bool okay) { if (okay) ++transports; });
    composer.prepareSendingAsync("stalled/original", false, [&](bool okay) { if (!okay) ++duplicatesRejected; });
    QCOMPARE(duplicatesRejected, 1); QVERIFY(composer.editor()->isReadOnly());
    QElapsedTimer navigation; navigation.start(); composer.setSessionKey("stalled/unrelated");
    QVERIFY(navigation.elapsed() < 100); QVERIFY(composer.isDraftLoading("stalled/unrelated"));
    bool heartbeat = false; QTimer::singleShot(0, &composer, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat); QCOMPARE(transports, 0);
    release.release(); QTRY_COMPARE(transports, 1); QTRY_VERIFY(!composer.isDraftLoading("stalled/unrelated"));
    QCOMPARE(composer.sessionKey(), QString("stalled/unrelated")); QVERIFY(composer.editor()->toPlainText().isEmpty());
    const auto recovered = ComposerDraftStore().load("stalled/original");
    QCOMPARE(recovered.text, text); QVERIFY(recovered.uncertain); QCOMPARE(recovered.attachments.size(), 1);
    QCOMPARE(recovered.attachments.first().data, QByteArray("immutable attachment"));
    composer.deliveryFinished("stalled/original", true);
    QVERIFY(ComposerDraftStore().load("stalled/original").text.isEmpty());
}

void TestMessageComposer::stalledRenameSurvivesEditorDestruction()
{
    auto *composer = new MessageComposer; setSessionReady(*composer, "rename/source"); composer->setAvailability(true);
    QVERIFY(composer->addAttachment("fixture.txt", "text/plain", QByteArray("snapshot")));
    ComposerDraftStore::flushWrites();
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    composer->editor()->insertPlainText("Latest rename payload"); const auto text = composer->editor()->toPlainText();
    QVERIFY(entered.tryAcquire(1, 3000)); composer->renameDraft("rename/source", "rename/destination");
    composer->setSessionKey("rename/destination"); QVERIFY(composer->isDraftLoading("rename/destination"));
    QElapsedTimer deletion; deletion.start(); delete composer; QVERIFY(deletion.elapsed() < 100);
    bool heartbeat = false; QTimer::singleShot(0, this, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    release.release(); ComposerDraftStore::flushWrites();
    QVERIFY(ComposerDraftStore().load("rename/source").text.isEmpty());
    const auto moved = ComposerDraftStore().load("rename/destination"); QCOMPARE(moved.text, text);
    QCOMPARE(moved.attachments.size(), 1); QCOMPARE(moved.attachments.first().data, QByteArray("snapshot"));
    MessageComposer recovered; setSessionReady(recovered, "rename/destination"); QCOMPARE(recovered.editor()->toPlainText(), text);
}

void TestMessageComposer::stalledRenameUpdatesPreviouslyLoadedDestination()
{
    MessageComposer composer; setSessionReady(composer, "rename/loaded-source"); composer.setAvailability(true);
    composer.editor()->setPlainText("Move this exact draft"); QVERIFY(composer.addAttachment("fixture.txt", "text/plain", "exact bytes"));
    setSessionReady(composer, "rename/loaded-destination"); setSessionReady(composer, "rename/loaded-source");
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    composer.editor()->moveCursor(QTextCursor::End); composer.editor()->insertPlainText(" latest");
    QVERIFY(entered.tryAcquire(1, 3000)); const auto text = composer.editor()->toPlainText();
    composer.renameDraft("rename/loaded-source", "rename/loaded-destination"); composer.setSessionKey("rename/loaded-destination");
    QVERIFY(composer.editor()->isReadOnly()); QVERIFY(composer.hasDraft("rename/loaded-destination"));
    bool heartbeat = false; QTimer::singleShot(0, &composer, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    release.release(); QTRY_VERIFY(!composer.isDraftLoading("rename/loaded-destination"));
    QCOMPARE(composer.editor()->toPlainText(), text); QVERIFY(!composer.hasDraft("rename/loaded-source"));
    composer.editor()->moveCursor(QTextCursor::End); composer.editor()->insertPlainText(" next edit");
    const auto saved = ComposerDraftStore().load("rename/loaded-destination");
    QCOMPARE(saved.text, text + " next edit"); QCOMPARE(saved.attachments.size(), 1); QCOMPARE(saved.attachments.first().data, QByteArray("exact bytes"));
}

void TestMessageComposer::stalledRenameFailureKeepsSourceRecoverable()
{
    MessageComposer composer; setSessionReady(composer, "rename/failure-source"); composer.setAvailability(true);
    composer.editor()->setPlainText("Recover from rename failure"); ComposerDraftStore::flushWrites();
    const auto digest = QCryptographicHash::hash(QByteArray("rename/failure-destination"), QCryptographicHash::Sha256).toHex();
    QFile blocker(ComposerDraftStore::directory() + '/' + QString::fromLatin1(digest)); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    composer.editor()->moveCursor(QTextCursor::End); composer.editor()->insertPlainText(" latest");
    QVERIFY(entered.tryAcquire(1, 3000)); const auto text = composer.editor()->toPlainText();
    composer.renameDraft("rename/failure-source", "rename/failure-destination");
    bool heartbeat = false; QTimer::singleShot(0, &composer, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    release.release(); QTRY_VERIFY(!composer.isDraftLoading("rename/failure-source"));
    QCOMPARE(composer.sessionKey(), QString("rename/failure-source")); QCOMPARE(composer.editor()->toPlainText(), text);
    QVERIFY(!composer.editor()->isReadOnly()); QCOMPARE(ComposerDraftStore().load("rename/failure-source").text, text);
    QVERIFY(blocker.remove());
}

void TestMessageComposer::cancelledPreparationCannotSettleNewAttempt()
{
    MessageComposer composer; setSessionReady(composer, "superseded/send"); composer.setAvailability(true);
    composer.editor()->setPlainText("First intent"); ComposerDraftStore::flushWrites();
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    int transports = 0; bool firstCompleted = false, secondCompleted = false;
    composer.prepareSendingAsync("superseded/send", false, [&](bool okay) { if (okay) ++transports; firstCompleted = true; });
    QVERIFY(entered.tryAcquire(1, 3000)); composer.deliveryFinished("superseded/send", false, "Cancelled before transport");
    composer.editor()->setPlainText("Replacement intent");
    composer.prepareSendingAsync("superseded/send", false, [&](bool okay) { if (okay) ++transports; secondCompleted = true; });
    bool heartbeat = false; QTimer::singleShot(0, &composer, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    QVERIFY(!firstCompleted); QVERIFY(!secondCompleted); QVERIFY(composer.editor()->isReadOnly());
    release.release(); QTRY_VERIFY(firstCompleted); QTRY_VERIFY(secondCompleted); QCOMPARE(transports, 1);
    const auto saved = ComposerDraftStore().load("superseded/send"); QCOMPARE(saved.text, QString("Replacement intent")); QVERIFY(saved.uncertain);
}

void TestMessageComposer::stalledDurabilityFailureKeepsTextAndAttachments()
{
    MessageComposer composer; setSessionReady(composer, "failed/original"); composer.setAvailability(true);
    ComposerDraftStore::flushWrites();
    QVERIFY(QDir().mkpath(QFileInfo(ComposerDraftStore::directory()).absolutePath()));
    QFile blocker(ComposerDraftStore::directory()); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    composer.editor()->setPlainText("Never dispatched"); QVERIFY(entered.tryAcquire(1, 3000));
    QVERIFY(composer.addAttachment("fixture.txt", "text/plain", QByteArray("recover me")));
    int transports = 0; bool finished = false;
    composer.prepareSendingAsync("failed/original", false, [&](bool okay) { if (okay) ++transports; finished = true; });
    composer.setSessionKey("failed/unrelated");
    bool heartbeat = false; QTimer::singleShot(0, &composer, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    QVERIFY(!finished); release.release(); QTRY_VERIFY(finished); QCOMPARE(transports, 0);
    setSessionReady(composer, "failed/original"); QVERIFY(!composer.editor()->isReadOnly());
    QVERIFY(composer.editor()->toPlainText().contains("Never dispatched")); QVERIFY(composer.hasDraft("failed/original"));
    QVERIFY(blocker.remove()); composer.editor()->insertPlainText(" saved");
    const auto draft = ComposerDraftStore().load("failed/original"); QCOMPARE(draft.attachments.size(), 1);
    QCOMPARE(draft.attachments.first().data, QByteArray("recover me")); QVERIFY(!draft.uncertain);
}

void TestMessageComposer::contentScaleEnlargesOnlyTheMessageField()
{
    QWidget workspace; workspace.setStyleSheet("QWidget { font-size:13px; }");
    auto *layout = new QVBoxLayout(&workspace); auto *composer = new MessageComposer; layout->addWidget(composer);
    composer->setTheme(false); setSessionReady(*composer, "local/scale"); composer->setAvailability(true); composer->editor()->setPlainText("draft survives");
    workspace.resize(520, 300); workspace.show(); QTest::qWait(10);
    auto *send = composer->findChild<QPushButton *>("sendMessage"); QVERIFY(send);
    const int height = composer->editor()->height(), sendFont = QFontInfo(send->font()).pixelSize();
    QCOMPARE(QFontInfo(composer->editor()->font()).pixelSize(), 13);
    composer->setContentScale(2.0); QTest::qWait(10);
    QCOMPARE(QFontInfo(composer->editor()->font()).pixelSize(), 26);
    QCOMPARE(composer->editor()->height(), 2 * height);
    QCOMPARE(QFontInfo(send->font()).pixelSize(), sendFont);
    QCOMPARE(composer->toolbar()->height(), ComposerToolbar::RowHeight);
    QCOMPARE(composer->editor()->toPlainText(), QString("draft survives"));
    composer->setTheme(true);
    QCOMPARE(QFontInfo(composer->editor()->font()).pixelSize(), 26);
    composer->setContentScale(1.0); QTest::qWait(10);
    QCOMPARE(QFontInfo(composer->editor()->font()).pixelSize(), 13);
    QCOMPARE(composer->editor()->height(), height);
}

void TestMessageComposer::attachmentDropReleasesVisualCaret_data()
{
    QTest::addColumn<bool>("image");
    QTest::newRow("image") << true;
    QTest::newRow("file") << false;
}

void TestMessageComposer::attachmentDropReleasesVisualCaret()
{
    QFETCH(bool, image);
    const int flashTime = QApplication::cursorFlashTime();
    const auto restoreFlash = qScopeGuard([flashTime] { QApplication::setCursorFlashTime(flashTime); });
    QApplication::setCursorFlashTime(0);
    MessageComposer composer; composer.resize(900, 240); setSessionReady(composer, "one"); composer.setAvailability(true);
    composer.show(); QVERIFY(QTest::qWaitForWindowExposed(&composer)); composer.activateWindow();
    auto *editor = composer.editor(); editor->setFocus(); QTRY_VERIFY(editor->hasFocus());
    editor->insertPlainText(QString::fromUtf8("Проверь релиз и документ"));
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QMimeData mime;
    if (image) {
        QImage pixels(4, 4, QImage::Format_RGB32); pixels.fill(Qt::red); mime.setImageData(pixels);
    } else {
        QFile file(dir.filePath("review.docx")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("document"); file.close();
        mime.setUrls({QUrl::fromLocalFile(file.fileName())});
    }
    drop(editor, mime);
    const QString draft = editor->toPlainText();
    const QRect end = editor->cursorRect();
    const auto endImage = editor->viewport()->grab().toImage();
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->textCursor().position(), 0);
    const auto homeImage = editor->viewport()->grab().toImage();
    // Checking textCursor() alone misses Qt's separate drag-feedback cursor:
    // it can override the painted caret even while insertion moves correctly.
    QVERIFY2(homeImage != endImage, "The painted caret must move with Home after an attachment drop");
    QDragLeaveEvent cleanup; QApplication::sendEvent(editor->viewport(), &cleanup);
    QCOMPARE(editor->viewport()->grab().toImage(), homeImage);
    QTest::keyClicks(editor, "X"); QCOMPARE(editor->toPlainText(), "X" + draft);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, end.center());
    const int insertion = editor->textCursor().position(); QVERIFY(insertion > 1);
    const auto clickImage = editor->viewport()->grab().toImage();
    QTest::keyClick(editor, Qt::Key_Home);
    QVERIFY(editor->viewport()->grab().toImage() != clickImage);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, end.center());
    auto expected = editor->toPlainText(); expected.insert(editor->textCursor().position(), "Y");
    QTest::keyClicks(editor, "Y"); QCOMPARE(editor->toPlainText(), expected);
}

void TestMessageComposer::cursorStaysWithDraftAcrossPollingSwitchesAndFailures()
{
    MessageComposer composer; composer.resize(700,200); setSessionReady(composer, "one"); composer.setAvailability(true);composer.show();QVERIFY(QTest::qWaitForWindowExposed(&composer));composer.activateWindow();
    auto *editor=composer.editor();editor->setFocus();QTRY_VERIFY(editor->hasFocus());
    QVERIFY(composer.addAttachment("review.docx","application/octet-stream","document"));
    editor->insertPlainText(QString::fromUtf8("Изучи этот документ и подготовь ответ"));
    auto cursor=editor->textCursor();cursor.setPosition(16);cursor.setPosition(20,QTextCursor::KeepAnchor);editor->setTextCursor(cursor);
    const auto draft=editor->toPlainText();const auto rectangle=editor->cursorRect();
    for (int i=0;i<3;++i) {setSessionReady(composer, "one");composer.setAvailability(true);composer.setModelSettings("claude-opus-5-5","high",{},false,"Use Terminal");QCoreApplication::processEvents();}
    QCOMPARE(editor->textCursor().position(),20);QCOMPARE(editor->textCursor().anchor(),16);QCOMPARE(editor->cursorRect(),rectangle);
    setSessionReady(composer, "two");editor->setPlainText("Other draft");setSessionReady(composer, "one");
    QCOMPARE(editor->toPlainText(),draft);QCOMPARE(editor->textCursor().position(),20);QCOMPARE(editor->textCursor().anchor(),16);
    setSendingReady(composer, "one");composer.deliveryFinished("one",false,"Terminal is busy");
    QCOMPARE(editor->toPlainText(),draft);QCOMPARE(editor->textCursor().position(),20);QCOMPARE(editor->textCursor().anchor(),16);
    QTest::keyClick(editor,Qt::Key_Right);QTest::keyClick(editor,Qt::Key_Home);
    QCOMPARE(editor->textCursor().position(),0);
    QTest::keyClicks(editor,"X");QCOMPARE(editor->toPlainText(),"X"+draft);
    const auto target=editor->cursorRect();QVERIFY(target.x()<rectangle.x());
    QTest::mouseClick(editor->viewport(),Qt::LeftButton,Qt::NoModifier,rectangle.center());
    const int insertion=editor->textCursor().position();const auto before=editor->toPlainText();
    QTest::keyClicks(editor,"Y");auto expected=before;expected.insert(insertion,"Y");QCOMPARE(editor->toPlainText(),expected);
    QVERIFY(composer.findChild<QPushButton *>("removeAttachment"));
}

void TestMessageComposer::unsupportedModelOffersTerminal()
{
    MessageComposer composer;setSessionReady(composer, "claude/one");composer.setAvailability(true);composer.show();
    composer.editor()->setPlainText("Keep draft");composer.setSettingsTerminalAvailable(true);
    composer.setModelSettings("claude-opus-5-5","high",{},false,"Change model with /model and effort with /effort in Terminal.");
    QSignalSpy terminal(&composer,&MessageComposer::settingsTerminalRequested);
    composer.findChild<QPushButton *>("sessionModelSettings")->click();
    auto *button=composer.findChild<QPushButton *>("sessionSettingsTerminal");QVERIFY(button->isVisible());
    QVERIFY(!composer.findChild<QComboBox *>("sessionModelChoice")->isVisible());
    QVERIFY(!composer.findChild<QPushButton *>("applySessionSettings")->isVisible());
    button->click();QCOMPARE(terminal.size(),1);QCOMPARE(terminal[0][0].toString(),QString("claude/one"));QCOMPARE(composer.editor()->toPlainText(),QString("Keep draft"));
}

void TestMessageComposer::escapeReleasesFocusWithoutChangingDraft()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "one"); composer.setAvailability(true); composer.show(); composer.activateWindow();
    composer.editor()->setPlainText("Keep this message");
    QVERIFY(composer.addAttachment("screen.png", "image/png", "image"));
    const auto draft = composer.editor()->toPlainText();
    composer.editor()->setFocus(); QTRY_VERIFY(composer.editor()->hasFocus());
    QTest::keyClick(composer.editor(), Qt::Key_Escape);
    QVERIFY(!composer.editor()->hasFocus()); QVERIFY(composer.isVisible());
    QCOMPARE(composer.editor()->toPlainText(), draft);
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 1);
    QVERIFY(submissions.isEmpty());
}

void TestMessageComposer::inlineReferencesFollowCursorAndSurviveEditing()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "one"); composer.setAvailability(true);
    composer.editor()->setPlainText(QString::fromUtf8("До\nпосле"));
    auto cursor = composer.editor()->textCursor(); cursor.setPosition(3); composer.editor()->setTextCursor(cursor);
    QVERIFY(composer.addAttachment("same.png", "image/png", "first"));
    QCOMPARE(composer.editor()->toPlainText(), QString::fromUtf8("До\n[Image #1] после"));
    cursor = composer.editor()->textCursor(); cursor.movePosition(QTextCursor::End); composer.editor()->setTextCursor(cursor);
    QVERIFY(composer.addAttachment("same.png", "image/png", "second"));
    QCOMPARE(composer.editor()->toPlainText(), QString::fromUtf8("До\n[Image #1] после [Image #2] "));
    const auto preview = qEnvironmentVariable("HGS_COMPOSER_PREVIEW");
    if (!preview.isEmpty()) {
        composer.resize(760, 200); composer.show(); QTest::qWait(30);
        QVERIFY(composer.grab().save(preview));
    }
    // Moving labels changes their position without renumbering the attachments.
    composer.editor()->setPlainText("Second [Image #2]\nfirst [Image #1]");
    setSessionReady(composer, "two"); setSessionReady(composer, "one");
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 1);
    QCOMPARE(submissions[0].text, QString("Second [Image #2]\nfirst [Image #1]"));
    QCOMPARE(submissions[0].attachments[0].reference, QString("[Image #1]"));
    QCOMPARE(submissions[0].attachments[1].reference, QString("[Image #2]"));
    setSendingReady(composer, "one"); composer.deliveryFinished("one", false, "Try again");
    QCOMPARE(composer.editor()->toPlainText(), submissions[0].text);
    composer.findChildren<QPushButton *>("removeAttachment").first()->click();
    QVERIFY(!composer.editor()->toPlainText().contains("[Image #1]"));
    QVERIFY(composer.editor()->toPlainText().contains("[Image #2]"));
    QVERIFY(composer.addAttachment("third.png", "image/png", "third"));
    QVERIFY(composer.editor()->toPlainText().contains("[Image #3]"));
    QVERIFY(!composer.editor()->toPlainText().contains("[Image #1]"));
    sendButton(composer)->click();
    QCOMPARE(submissions.last().attachments[0].reference, QString("[Image #2]"));
    QCOMPARE(submissions.last().attachments[1].reference, QString("[Image #3]"));
    setSendingReady(composer, "one"); composer.deliveryFinished("one", true);
    QVERIFY(composer.addAttachment("new.png", "image/png", "new"));
    QCOMPARE(composer.editor()->toPlainText(), QString("[Image #1] "));
}

void TestMessageComposer::retryOfAnotherMessagePreservesDraftAndUncertainty()
{
    MessageComposer composer;setSessionReady(composer, "one");composer.setAvailability(true);
    composer.editor()->setPlainText("Unrelated draft");QVERIFY(composer.addAttachment("next.txt","text/plain","next"));
    const QList<MessageAttachment> files{{"next.txt","text/plain","next","[File #1]"}};
    QVERIFY(composer.draftMatches("one","[File #1] Unrelated draft",files));
    composer.deliveryFinished("one",false,"Check the previous delivery",true);
    setSendingReady(composer, "one",true);setSessionReady(composer, "two");composer.editor()->setPlainText("Other session draft");
    composer.deliveryFinished("one",true);QCOMPARE(composer.editor()->toPlainText(),QString("Other session draft"));
    setSessionReady(composer, "one");QVERIFY(composer.draftMatches("one","[File #1] Unrelated draft",files));
    QCOMPARE(composer.editor()->toPlainText(),QString("[File #1] Unrelated draft"));QVERIFY(!sendButton(composer)->isEnabled());
    QVERIFY(status(composer)->text().contains("Check the previous delivery"));
}

void TestMessageComposer::modelSettingsPreserveDraftAndDistinguishPending()
{
    MessageComposer composer; composer.resize(640, 220); setSessionReady(composer, "arch/session");
    const QJsonArray options{QJsonObject{{"id", "model-one"}, {"effort_options", QJsonArray{"low", "high", "xhigh"}}},
        QJsonObject{{"id", "model-two"}, {"label", "Second model"}, {"effort_options", QJsonArray{"low", "high"}}}};
    composer.setModelSettings("model-one", "xhigh", options, true, {}, {}, {}, "resume");
    composer.editor()->setPlainText("Keep this draft"); QVERIFY(composer.addAttachment("keep.txt", "text/plain", "keep"));
    QSignalSpy changes(&composer, &MessageComposer::settingsRequested);
    composer.show(); QTest::qWait(20);
    auto *button = composer.findChild<QPushButton *>("sessionModelSettings"); QVERIFY(button); button->click();
    auto *models = composer.findChild<QComboBox *>("sessionModelChoice");
    auto *efforts = composer.findChild<QComboBox *>("sessionEffortChoice");
    auto *apply = composer.findChild<QPushButton *>("applySessionSettings");
    auto *cancel = composer.findChild<QPushButton *>("cancelSessionSettings"); QVERIFY(cancel);
    auto *popup = composer.findChild<QFrame *>("sessionSettingsPopup");
    QCOMPARE(efforts->currentData().toString(), QString("xhigh")); QVERIFY(!apply->isEnabled());
    models->setCurrentIndex(models->findData("model-two")); cancel->click();
    QVERIFY(!popup->isVisible()); QCOMPARE(changes.size(), 0);
    button->click(); QCOMPARE(models->currentData().toString(), QString("model-one"));
    QCOMPARE(efforts->currentData().toString(), QString("xhigh"));
    models->setCurrentIndex(models->findData("model-two")); QTest::keyClick(efforts, Qt::Key_Escape);
    QVERIFY(!popup->isVisible()); QCOMPARE(changes.size(), 0);
    button->click(); QCOMPARE(models->currentData().toString(), QString("model-one"));
    models->setCurrentIndex(models->findData("model-two"));
    QCOMPARE(efforts->count(), 2); QVERIFY(efforts->findData("xhigh") < 0);
    efforts->setCurrentIndex(efforts->findData("high")); QVERIFY(apply->isEnabled()); QCOMPARE(apply->text(), QString("Save for resume"));
    apply->click(); QCOMPARE(changes.size(), 1); QCOMPARE(changes[0], QVariantList({"arch/session", "model-two", "high"}));
    QCOMPARE(composer.editor()->toPlainText(), QString("[File #1] Keep this draft")); QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 1);
    composer.setModelSettings("model-one", "xhigh", options, true, {}, "model-two", "high", "ready");
    QVERIFY(button->text().contains("model-one")); QVERIFY(button->toolTip().contains("Pending: model-two  high"));
    button->click(); QCOMPARE(models->currentData().toString(), QString("model-two")); QCOMPARE(efforts->currentData().toString(), QString("high"));
    QVERIFY(composer.findChild<QLabel *>("sessionSettingsHint")->text().contains("Messages sent while it is working use the current model"));
    setSessionReady(composer, "mac/other"); QVERIFY(!composer.findChild<QFrame *>("sessionSettingsPopup")->isVisible());
    apply->click(); QCOMPARE(changes.size(), 1);
    setSessionReady(composer, "arch/session"); QCOMPARE(composer.editor()->toPlainText(), QString("[File #1] Keep this draft"));
}

void TestMessageComposer::modelSettingsUnavailableExplainsWhy()
{
    MessageComposer composer; setSessionReady(composer, "arch/session"); composer.resize(440, 220); composer.show();
    composer.setModelSettings("observed-model", "high", {}, false, "Machine is unavailable");
    auto *button = composer.findChild<QPushButton *>("sessionModelSettings"); QVERIFY(button->isEnabled()); button->click();
    QVERIFY(composer.findChild<QFrame *>("sessionSettingsPopup")->isVisible());
    QVERIFY(composer.findChild<QLabel *>("sessionSettingsHint")->text().contains("Machine is unavailable"));
    QVERIFY(!composer.findChild<QComboBox *>("sessionModelChoice")->isEnabled());
    QVERIFY(!composer.findChild<QPushButton *>("applySessionSettings")->isEnabled());
}

void TestMessageComposer::modelSettingsFitNarrowColumn()
{
    MessageComposer composer; setSessionReady(composer, "arch/session");
    composer.setModelSettings("a-very-long-model-name-in-the-catalog", "xhigh", {}, false, "Unavailable");
    composer.resize(280, 220); composer.show(); QTest::qWait(20);
    const auto *button = composer.findChild<QPushButton *>("sessionModelSettings");
    const auto *attach = composer.findChild<QPushButton *>("attachMessageFile");
    const auto *send = composer.findChild<QPushButton *>("sendMessage");
    QCOMPARE(composer.width(), 280); QVERIFY(attach->geometry().right() < button->geometry().left());
    QVERIFY(button->geometry().right() < send->geometry().left()); QVERIFY(send->geometry().right() < composer.width());
    QVERIFY(status(composer)->isHidden());
    composer.deliveryFinished("arch/session", false, "Check terminal before retrying", true); QTest::qWait(20);
    QVERIFY(status(composer)->isVisible()); QVERIFY(button->isHidden());
    const auto *retry = composer.findChild<QPushButton *>("allowMessageRetry"); QVERIFY(retry->isVisible());
    QCOMPARE(composer.width(), 280); QCOMPARE(retry->geometry().center().y(), attach->geometry().center().y());
    QVERIFY(retry->geometry().right() < composer.width()); QVERIFY(send->geometry().right() < composer.width());
}

void TestMessageComposer::sessionStatusPreservesGeometryAndLockedDraft_data()
{
    QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
    QTest::newRow("wide-dark") << 900 << true;
    QTest::newRow("narrow-light") << 280 << false;
}

void TestMessageComposer::sessionStatusPreservesGeometryAndLockedDraft()
{
    QFETCH(int, width); QFETCH(bool, dark);
    MessageComposer composer; setSessionReady(composer, "remote/session"); composer.setAvailability(true); composer.setTheme(dark);
    composer.resize(width, 220); composer.show(); composer.editor()->setPlainText("Keep this exact draft");
    auto cursor = composer.editor()->textCursor(); cursor.setPosition(5); cursor.setPosition(15, QTextCursor::KeepAnchor);
    composer.editor()->setTextCursor(cursor); composer.editor()->setFocus(); QTest::qWait(20);
    const auto before = composer.editor()->geometry(); const auto height = composer.sizeHint().height();
    const QString error = "Could not refresh activity: timed out. Refresh this session to try again. " + QString(1200, 'x');
    QSignalSpy sends(&composer, &MessageComposer::sendRequested), refresh(&composer, &MessageComposer::sessionRefreshRequested);
    composer.setSessionStatus(error, true, true); QTest::qWait(20);
    QCOMPARE(composer.editor()->geometry(), before); QCOMPARE(composer.sizeHint().height(), height);
    QVERIFY(composer.editor()->isReadOnly()); QVERIFY(composer.editor()->property("sessionBlocked").toBool());
    QCOMPARE(composer.editor()->toPlainText(), QString("Keep this exact draft"));
    QCOMPARE(composer.editor()->textCursor().position(), 15); QCOMPARE(composer.editor()->textCursor().anchor(), 5);
    QCOMPARE(QApplication::focusWidget(), composer.editor());
    auto *attach = composer.findChild<QPushButton *>("attachMessageFile");
    auto *retry = composer.findChild<QPushButton *>("refreshSessionStatus");
    QVERIFY(!attach->isEnabled()); QVERIFY(!sendButton(composer)->isEnabled());
    QVERIFY(!composer.addAttachment("fixture.txt", "text/plain", "fixture"));
    QTest::keyClicks(composer.editor(), "rejected"); QTest::keyClick(composer.editor(), Qt::Key_Return);
    QCOMPARE(composer.editor()->toPlainText(), QString("Keep this exact draft")); QCOMPARE(sends.size(), 0);
    QCOMPARE(status(composer)->text(), error); QCOMPARE(status(composer)->accessibleName(), error);
    QCOMPARE(status(composer)->geometry().center().y(), attach->geometry().center().y());
    QVERIFY(status(composer)->geometry().right() < retry->geometry().left());
    QVERIFY(retry->geometry().right() < composer.width()); QVERIFY(sendButton(composer)->geometry().right() < composer.width());
    retry->click(); QCOMPARE(refresh.size(), 1); QCOMPARE(refresh[0][0].toString(), QString("remote/session"));
    composer.setSessionStatus({}); QTest::qWait(20);
    QVERIFY(!composer.editor()->isReadOnly()); QCOMPARE(composer.editor()->geometry(), before);
    QCOMPARE(composer.editor()->textCursor().position(), 15); QCOMPARE(composer.editor()->textCursor().anchor(), 5);
    composer.setSessionStatus("Copy completed", false, false); QVERIFY(!composer.editor()->isReadOnly());
    setSessionReady(composer, "remote/other"); QVERIFY(!composer.editor()->isReadOnly()); QVERIFY(status(composer)->isHidden());
}

void TestMessageComposer::isolatesDraftsAcrossSessions()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    composer.setAvailability(true);
    setSessionReady(composer, "arch\ncodex/project/first");
    composer.editor()->setPlainText("First session draft");
    QVERIFY(composer.addAttachment("first.txt", "text/plain", "first-file"));
    setSessionReady(composer, "mac\nclaude/project/second");
    QVERIFY(composer.editor()->toPlainText().isEmpty());
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 0);
    composer.editor()->setPlainText("Second session draft");
    QVERIFY(composer.addAttachment("second.txt", "text/plain", "second-file"));
    setSessionReady(composer, "arch\ncodex/project/first");
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] First session draft");
    sendButton(composer)->click();
    QCOMPARE(submissions.size(), 1); QCOMPARE(submissions.last().key, "arch\ncodex/project/first");
    QCOMPARE(submissions.last().attachments.size(), 1); QCOMPARE(submissions.last().attachments[0].data, "first-file");
    setSessionReady(composer, "mac\nclaude/project/second");
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] Second session draft");
    sendButton(composer)->click();
    QCOMPARE(submissions.last().attachments[0].name, "second.txt");
    setSessionReady(composer, {});
    QVERIFY(!composer.editor()->isEnabled()); QVERIFY(!sendButton(composer)->isEnabled());
}

void TestMessageComposer::enterSendsShiftEnterAddsLine()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch/one"); composer.setAvailability(true); composer.show();
    auto *editor = composer.editor(); editor->setFocus();
    QTest::keyClicks(editor, "First line");
    QTest::keyClick(editor, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClicks(editor, "Second line");
    QCOMPARE(submissions.size(), 0); QCOMPARE(editor->toPlainText(), "First line\nSecond line");
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(submissions.size(), 1); QCOMPARE(submissions.last().text, "First line\nSecond line");
    // The owner confirms/locks delivery; emitting a request never drops a draft.
    QCOMPARE(editor->toPlainText(), "First line\nSecond line");
}

void TestMessageComposer::sendingKeepsEditorFocus_data()
{
    QTest::addColumn<bool>("button");
    QTest::newRow("enter") << false;
    QTest::newRow("send-button") << true;
}

void TestMessageComposer::sendingKeepsEditorFocus()
{
    QFETCH(bool, button);
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    connect(&composer, &MessageComposer::sendRequested, &composer, [&](const QString &key) { setSendingReady(composer, key); });
    setSessionReady(composer, "arch/one"); composer.setAvailability(true); composer.setInterruptAvailability(true, true);
    composer.show(); composer.activateWindow();
    auto *editor = composer.editor(); editor->setPlainText("Queue this message"); editor->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(), editor);
    if (button) { sendButton(composer)->setFocus(); sendButton(composer)->click(); }
    else QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(submissions.size(), 1); QCOMPARE(submissions.first().text, QString("Queue this message"));
    QCOMPARE(QApplication::focusWidget(), editor);
    QVERIFY(editor->isEnabled()); QVERIFY(editor->isReadOnly()); QVERIFY(editor->toPlainText().isEmpty());
    QTest::keyClicks(editor, "Must not change the outgoing payload");
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(submissions.size(), 1); QVERIFY(editor->toPlainText().isEmpty());
    composer.deliveryFinished("arch/one", true);
    QCOMPARE(QApplication::focusWidget(), editor); QVERIFY(!editor->isReadOnly());
    QVERIFY(!composer.findChild<QPushButton *>("interruptAgent")->hasFocus());
    QTest::keyClicks(editor, "Next draft"); QCOMPARE(editor->toPlainText(), QString("Next draft"));
}

void TestMessageComposer::deliveryDoesNotStealDeliberatelyMovedFocus()
{
    QWidget window; auto *layout = new QVBoxLayout(&window);
    auto *composer = new MessageComposer; auto *other = new QLineEdit;
    layout->addWidget(composer); layout->addWidget(other);
    setSessionReady(*composer, "arch/one"); composer->setAvailability(true); composer->setInterruptAvailability(true, true);
    window.show(); window.activateWindow(); composer->editor()->setPlainText("Recover this draft");
    composer->editor()->setFocus(); QTRY_COMPARE(QApplication::focusWidget(), composer->editor());
    setSendingReady(*composer, "arch/one"); other->setFocus();
    composer->deliveryFinished("arch/one", false, "Rejected");
    QCOMPARE(QApplication::focusWidget(), other); QCOMPARE(composer->editor()->toPlainText(), QString("Recover this draft"));
    setSendingReady(*composer, "arch/one"); setSessionReady(*composer, "mac/two");
    composer->editor()->setPlainText("Other session draft"); composer->editor()->setFocus();
    composer->deliveryFinished("arch/one", true);
    QCOMPARE(QApplication::focusWidget(), composer->editor());
    QCOMPARE(composer->editor()->toPlainText(), QString("Other session draft")); QVERIFY(!composer->editor()->isReadOnly());
}

void TestMessageComposer::inputMethodDoesNotAccidentallySubmit()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch/one"); composer.setAvailability(true); composer.show();
    auto *editor = composer.editor(); editor->setPlainText("Draft "); editor->setFocus();
    QInputMethodEvent preedit(QString::fromUtf8("に"), {});
    QApplication::sendEvent(editor, &preedit);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(submissions.size(), 0);
    QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("に"));
    QApplication::sendEvent(editor, &commit);
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(submissions.size(), 1);
}

void TestMessageComposer::availabilityAndBusyGateSubmission()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch/one"); composer.show();
    composer.setAvailability(false, "Agent needs approval; use the terminal.");
    composer.editor()->setPlainText("A saved draft");
    QVERIFY(composer.addAttachment("existing.txt", "text/plain", "saved"));
    QVERIFY(composer.editor()->isEnabled()); QVERIFY(!sendButton(composer)->isEnabled());
    QTest::keyClick(composer.editor(), Qt::Key_Return); QCOMPARE(submissions.size(), 0);
    QCOMPARE(status(composer)->text(), "Agent needs approval; use the terminal.");
    QCOMPARE(QToolTip::text(), QString("Agent needs approval; use the terminal."));
    QCOMPARE(composer.editor()->toPlainText(), QString("[File #1] A saved draft"));
    composer.setAvailability(true);
    QVERIFY(sendButton(composer)->isEnabled());
    setSendingReady(composer, "arch/one");
    QVERIFY(composer.isSending("arch/one")); QVERIFY(composer.editor()->isEnabled()); QVERIFY(composer.editor()->isReadOnly());
    QVERIFY(!sendButton(composer)->isEnabled());
    QVERIFY(!composer.findChild<QPushButton *>("attachMessageFile")->isEnabled());
    QVERIFY(composer.editor()->toPlainText().isEmpty());
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 0);
    QVERIFY(!composer.addAttachment("late.txt", "text/plain", "late"));
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 0);
    composer.deliveryFinished("arch/one", false, "Agent was busy.");
    QVERIFY(composer.editor()->isEnabled()); QVERIFY(!composer.editor()->isReadOnly()); QVERIFY(sendButton(composer)->isEnabled());
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] A saved draft");
}

void TestMessageComposer::syntheticImageAndFileDrop()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch/one"); composer.setAvailability(true); composer.resize(640, 250); composer.show();
    QTest::qWait(10);
    QImage image(32, 24, QImage::Format_ARGB32); image.fill(QColor("#8bdfc0"));
    QMimeData imageMime; imageMime.setImageData(image); drop(composer.editor(), imageMime);
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 1);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString path = directory.filePath("sample file.txt"); QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("file payload"), 12); file.close();
    QMimeData fileMime; fileMime.setUrls({QUrl::fromLocalFile(path)}); drop(composer.editor(), fileMime);
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 2);
    QMimeData textMime; textMime.setText("Please inspect these attachments"); drop(composer.editor(), textMime);
    sendButton(composer)->click();
    QCOMPARE(submissions.size(), 1); QVERIFY(submissions[0].text.contains("Please inspect these attachments"));
    QVERIFY(submissions[0].text.contains("[Image #1]")); QVERIFY(submissions[0].text.contains("[File #2]"));
    QCOMPARE(submissions[0].attachments.size(), 2);
    QCOMPARE(submissions[0].attachments[0].name, "pasted-image.png");
    QCOMPARE(submissions[0].attachments[0].mime, "image/png");
    QVERIFY(submissions[0].attachments[0].data.startsWith("\x89PNG\r\n\x1a\n"));
    QCOMPARE(QImage::fromData(submissions[0].attachments[0].data).size(), image.size());
    QCOMPARE(submissions[0].attachments[1].name, "sample file.txt");
    QCOMPARE(submissions[0].attachments[1].data, "file payload");
    composer.findChildren<QPushButton *>("removeAttachment").first()->click();
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 1);
}

void TestMessageComposer::attachmentLimits()
{
    MessageComposer composer; setSessionReady(composer, "count"); composer.setAvailability(true);
    for (int i = 0; i < 8; ++i) QVERIFY(composer.addAttachment(QString("file-%1.txt").arg(i), "text/plain", "small"));
    QVERIFY(!composer.addAttachment("ninth.txt", "text/plain", "small"));
    QVERIFY(status(composer)->text().contains("8 files"));
    setSessionReady(composer, "size");
    QVERIFY(!composer.addAttachment("empty.txt", "text/plain", {}));
    QVERIFY(status(composer)->text().contains("empty"));
    QVERIFY(!sendButton(composer)->isEnabled());
    QVERIFY(!composer.addAttachment("large.bin", "application/octet-stream", QByteArray(10 * 1024 * 1024 + 1, 'x')));
    QVERIFY(status(composer)->text().contains("10 MiB"));
    const QByteArray maxFile(10 * 1024 * 1024, 'x');
    QVERIFY(composer.addAttachment("a.bin", "application/octet-stream", maxFile));
    QVERIFY(composer.addAttachment("b.bin", "application/octet-stream", maxFile));
    QVERIFY(!composer.addAttachment("overflow.txt", "text/plain", "x"));
    QVERIFY(status(composer)->text().contains("20 MiB"));
    setSessionReady(composer, "pixels"); composer.show();
    // Monochrome storage keeps this boundary test small in memory.
    QImage oversized(8000, 4001, QImage::Format_Mono); oversized.fill(0);
    QMimeData mime; mime.setImageData(oversized); drop(composer.editor(), mime);
    QVERIFY(status(composer)->text().contains("32 megapixels"));
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 0);
}

void TestMessageComposer::textLimitCountsUtf8Bytes()
{
    MessageComposer composer; setSessionReady(composer, "arch/one"); composer.setAvailability(true);
    composer.editor()->setPlainText(QString(32768, QChar(0x044f)));
    QVERIFY(sendButton(composer)->isEnabled()); // exactly 64 KiB in UTF-8
    composer.editor()->insertPlainText(QString(QChar(0x044f)));
    QVERIFY(!sendButton(composer)->isEnabled()); QVERIFY(status(composer)->text().contains("64 KiB"));
    composer.editor()->setPlainText(" \n\t"); QVERIFY(!sendButton(composer)->isEnabled());
    QVERIFY(composer.addAttachment("only.txt", "text/plain", "attachment without text"));
    QVERIFY(sendButton(composer)->isEnabled());
}

void TestMessageComposer::failureAndUncertaintyPreserveDraft()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch/one"); composer.setAvailability(true); composer.show();
    composer.editor()->setPlainText("Keep this draft"); composer.addAttachment("keep.txt", "text/plain", "keep");
    setSendingReady(composer, "arch/one"); composer.deliveryFinished("arch/one", false, "Rejected before sending");
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] Keep this draft");
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 1);
    QVERIFY(sendButton(composer)->isEnabled());
    setSendingReady(composer, "arch/one"); composer.deliveryFinished("arch/one", false, "Delivery uncertain. Check terminal.", true);
    auto *retry = composer.findChild<QPushButton *>("allowMessageRetry");
    QVERIFY(retry->isVisible()); QVERIFY(!sendButton(composer)->isEnabled());
    QTest::keyClick(composer.editor(), Qt::Key_Return); QCOMPARE(submissions.size(), 0);
    QVERIFY(composer.addAttachment("another.txt", "text/plain", "additional context"));
    QCOMPARE(status(composer)->text(), "Delivery uncertain. Check terminal.");
    QVERIFY(!sendButton(composer)->isEnabled());
    setSessionReady(composer, "mac/other"); setSessionReady(composer, "arch/one");
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] [File #2] Keep this draft");
    QVERIFY(retry->isVisible()); QVERIFY(!sendButton(composer)->isEnabled());
    retry->click(); QVERIFY(sendButton(composer)->isEnabled());
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 1);
    QCOMPARE(submissions[0].attachments[0].data, "keep");
}

void TestMessageComposer::sendingHidesPayloadAndRestoresFailedBackgroundDelivery()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    connect(&composer, &MessageComposer::sendRequested, &composer,
        [&](const QString &key) { setSendingReady(composer, key); });
    setSessionReady(composer, "arch/one"); composer.setAvailability(true);
    composer.editor()->setPlainText("Message with attachment");
    QVERIFY(composer.addAttachment("notes.txt", "text/plain", "retained contents"));
    sendButton(composer)->click();
    QCOMPARE(submissions.size(), 1); QCOMPARE(submissions[0].text, QString("[File #1] Message with attachment"));
    QCOMPARE(submissions[0].attachments[0].data, QByteArray("retained contents"));
    QVERIFY(composer.isSending("arch/one")); QVERIFY(composer.editor()->toPlainText().isEmpty());
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 0);
    // Revisiting while delivery is pending must not redisplay the outgoing text.
    setSessionReady(composer, "mac/two"); composer.editor()->setPlainText("Separate draft");
    setSessionReady(composer, "arch/one"); QVERIFY(composer.editor()->toPlainText().isEmpty());
    setSessionReady(composer, "mac/two");
    composer.deliveryFinished("arch/one", false, "Connection failed");
    QCOMPARE(composer.editor()->toPlainText(), QString("Separate draft"));
    setSessionReady(composer, "arch/one");
    QCOMPARE(composer.editor()->toPlainText(), QString("[File #1] Message with attachment"));
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 1);
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 2);
    QCOMPARE(submissions[1].attachments[0].data, QByteArray("retained contents"));
    QVERIFY(composer.editor()->toPlainText().isEmpty());
    composer.deliveryFinished("arch/one", true);
    QVERIFY(composer.editor()->toPlainText().isEmpty());
}

void TestMessageComposer::waitingPayloadLocksEditsAndPreservesFocusAndSelection()
{
    Window window("arch/waiting"); auto &composer = *window.composer;
    QList<Submission> submissions; observe(composer, submissions);
    composer.editor()->setPlainText("Exact accepted message");
    QVERIFY(composer.addAttachment("notes.txt", "text/plain", "original bytes"));
    const auto accepted = composer.editor()->toPlainText();
    auto *editor = composer.editor(); window.widget.activateWindow(); editor->setFocus(); QTRY_VERIFY(editor->hasFocus());
    const QString reason = "Waiting for compaction. This exact message will be sent automatically. Cancel send to edit.";
    QVERIFY(composer.setWaitingToSend("arch/waiting", reason));
    QCOMPARE(QApplication::focusWidget(), editor); QVERIFY(editor->isEnabled()); QVERIFY(editor->isReadOnly());
    QCOMPARE(editor->toPlainText(), accepted); QVERIFY(!composer.isSending("arch/waiting"));
    QCOMPARE(sendButton(composer)->text(), QString("Waiting to send…")); QVERIFY(!sendButton(composer)->isEnabled());
    QVERIFY(status(composer)->text().contains("exact message")); QVERIFY(!composer.canAttachFiles());
    QVERIFY(!composer.addAttachment("extra.txt", "text/plain", "extra bytes"));
    auto *remove = composer.findChild<QPushButton *>("removeAttachment"); QVERIFY(remove); QVERIFY(!remove->isEnabled());
    QTest::keyClicks(editor, "accidental edit"); QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), accepted); QCOMPARE(submissions.size(), 0);
    editor->selectAll(); QTest::keySequence(editor, QKeySequence(QKeySequence::Copy));
    QCOMPARE(QApplication::clipboard()->text(), accepted);
    QApplication::clipboard()->setText("replacement"); QTest::keySequence(editor, QKeySequence(QKeySequence::Paste));
    QCOMPARE(editor->toPlainText(), accepted);
    setSessionReady(composer, "mac/other"); QVERIFY(!editor->isReadOnly()); editor->setPlainText("Separate draft");
    setSessionReady(composer, "arch/waiting"); QVERIFY(editor->isReadOnly()); QCOMPARE(editor->toPlainText(), accepted);
    QTimer::singleShot(0, &composer, [&] {
        auto *dialog = composer.findChild<QDialog *>("savedDraftsDialog"); QVERIFY(dialog);
        QVERIFY(!dialog->findChild<QPushButton *>("restoreSavedDraft")->isEnabled()); dialog->reject();
    });
    composer.showSavedDrafts(); QCOMPARE(editor->toPlainText(), accepted);
    composer.deliveryFinished("arch/waiting", false, "Pending send cancelled. Your draft is unchanged.");
    QVERIFY(!editor->isReadOnly()); QVERIFY(composer.canAttachFiles()); QCOMPARE(sendButton(composer)->text(), QString("Send"));
    QCOMPARE(editor->toPlainText(), accepted);
    auto cursor = editor->textCursor(); cursor.movePosition(QTextCursor::End); editor->setTextCursor(cursor);
    QTest::keyClicks(editor, " updated"); sendButton(composer)->click(); QCOMPARE(submissions.size(), 1);
    QCOMPARE(submissions[0].text, accepted + " updated"); QCOMPARE(submissions[0].attachments[0].data, QByteArray("original bytes"));
}

void TestMessageComposer::waitingIntentRestoresAsAnEditableDraftAfterRestart()
{
    {
        MessageComposer composer; setSessionReady(composer, "arch/pending");
        composer.editor()->setPlainText("Recover after restart");
        QVERIFY(composer.addAttachment("keep.txt", "text/plain", "accepted bytes"));
        QVERIFY(composer.setWaitingToSend("arch/pending", "Waiting for the session to start."));
    }
    MessageComposer restored; setSessionReady(restored, "arch/pending"); restored.setAvailability(true);
    QCOMPARE(restored.editor()->toPlainText(), QString("[File #1] Recover after restart"));
    QVERIFY(!restored.editor()->isReadOnly()); QVERIFY(!restored.isWaitingToSend("arch/pending"));
    QCOMPARE(sendButton(restored)->text(), QString("Send"));
    QList<Submission> submissions; observe(restored, submissions); sendButton(restored)->click();
    QCOMPARE(submissions.size(), 1); QCOMPARE(submissions[0].attachments[0].data, QByteArray("accepted bytes"));
}

void TestMessageComposer::backgroundSuccessDoesNotClearVisibleDraft()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch/one"); composer.setAvailability(true);
    composer.editor()->setPlainText("Sending first"); composer.addAttachment("first.txt", "text/plain", "first");
    setSendingReady(composer, "arch/one");
    setSessionReady(composer, "mac/two"); composer.editor()->setPlainText("Still editing second");
    composer.addAttachment("second.txt", "text/plain", "second");
    composer.deliveryFinished("arch/one", true);
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] Still editing second");
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 1);
    QCOMPARE(submissions[0].key, "mac/two"); QCOMPARE(submissions[0].attachments[0].data, "second");
    setSessionReady(composer, "arch/one"); QVERIFY(composer.editor()->toPlainText().isEmpty());
    QCOMPARE(composer.findChildren<QPushButton *>("removeAttachment").size(), 0);
    QVERIFY(!composer.isSending("arch/one"));
}

void TestMessageComposer::renamePreservesDraft()
{
    MessageComposer composer; QList<Submission> submissions; observe(composer, submissions);
    setSessionReady(composer, "arch\ncodex/project/old"); composer.setAvailability(true);
    composer.editor()->setPlainText("Rename retains this draft"); composer.addAttachment("draft.txt", "text/plain", "draft");
    composer.renameDraft("arch\ncodex/project/old", "arch\ncodex/project/new");
    QTRY_COMPARE(composer.sessionKey(), QString("arch\ncodex/project/new"));
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] Rename retains this draft");
    sendButton(composer)->click(); QCOMPARE(submissions.size(), 1);
    QCOMPARE(submissions[0].key, "arch\ncodex/project/new");
    QCOMPARE(submissions[0].attachments[0].data, "draft");
    setSessionReady(composer, "mac/other");
    composer.renameDraft("arch\ncodex/project/new", "arch\ncodex/project/final");
    QVERIFY(composer.editor()->toPlainText().isEmpty());
    setSessionReady(composer, "arch\ncodex/project/final");
    QCOMPARE(composer.editor()->toPlainText(), "[File #1] Rename retains this draft");
}

void TestMessageComposer::draftStateFollowsUnsentContent()
{
    // The session list marks sessions whose composer still holds an unsent message.
    MessageComposer composer; composer.setAvailability(true);
    QSignalSpy changed(&composer, &MessageComposer::draftChanged);
    setSessionReady(composer, "arch/one");
    QVERIFY(!composer.hasDraft("arch/one"));
    composer.editor()->setPlainText("   \n");
    QVERIFY(!composer.hasDraft("arch/one")); QCOMPARE(changed.size(), 0);
    composer.editor()->setPlainText("Ask about the tests");
    QVERIFY(composer.hasDraft("arch/one"));
    QCOMPARE(changed.size(), 1); QCOMPARE(changed.last().at(0).toString(), QString("arch/one"));
    composer.editor()->setPlainText("Ask about the tests again");
    QCOMPARE(changed.size(), 1);   // only a change between empty and unsent is reported
    setSessionReady(composer, "arch/two");
    QVERIFY(composer.hasDraft("arch/one")); QVERIFY(!composer.hasDraft("arch/two"));
    setSendingReady(composer, "arch/one");
    QVERIFY(!composer.hasDraft("arch/one")); QCOMPARE(changed.size(), 2);
    composer.deliveryFinished("arch/one", false, "Rejected before sending");
    QVERIFY(composer.hasDraft("arch/one")); QCOMPARE(changed.size(), 3);   // failed delivery is still unsent
    setSendingReady(composer, "arch/one"); composer.deliveryFinished("arch/one", true);
    QVERIFY(!composer.hasDraft("arch/one"));
    QVERIFY(composer.addAttachment("note.txt", "text/plain", "context"));
    QVERIFY(composer.hasDraft("arch/two"));
    composer.renameDraft("arch/two", "arch/renamed");
    QTRY_VERIFY(!composer.hasDraft("arch/two")); QVERIFY(composer.hasDraft("arch/renamed"));
    QVERIFY(composer.offerDraft("arch/offered", "Continue the interrupted turn"));
    QVERIFY(composer.hasDraft("arch/offered"));
}

void TestMessageComposer::droppedShortcutIsCheckedAsTheFileItReads()
{
    // Qt describes a Windows .lnk by its target (here a folder); the composer
    // attaches the shortcut file it reads, so it must check that file.
    Window window("local/shortcut"); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    QTemporaryDir dir; QVERIFY(dir.isValid()); const auto shortcut = dir.filePath("folder.lnk");
#ifdef Q_OS_WIN
    QVERIFY(QFile::link(dir.path(), shortcut));
#else
    { QFile file(shortcut); QVERIFY(file.open(QIODevice::WriteOnly)); QVERIFY(file.write("shortcut bytes") > 0); }
#endif
    window.composer->attachDroppedFiles({shortcut, dir.path()});
    QCOMPARE(window.chip()->fullLabel(), QString("1 attached")); QVERIFY(window.chip()->toolTip().contains("folder.lnk"));
}

void TestMessageComposer::attachmentsChipSummarizesAndRemoves()
{
    Window window("local/chip"); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    auto *chip = window.chip(); QVERIFY(chip); QVERIFY(chip->isHidden());
    QVERIFY(window.composer->addAttachment("one.png", "image/png", pngBytes()));
    QVERIFY(window.composer->addAttachment("notes.md", "text/markdown", "# notes"));
    QVERIFY(chip->isVisible()); QCOMPARE(chip->fullLabel(), QString("2 attached")); QCOMPARE(chip->shortLabel(), QString("2"));
    QVERIFY(chip->property("flashing").toBool());
    QVERIFY(chip->toolTip().contains("[Image #1] one.png")); QVERIFY(chip->toolTip().contains("[File #2] notes.md"));
    QCOMPARE(window.composer->editor()->toPlainText(), QString("[Image #1] [File #2] "));
    QTest::mouseClick(chip, Qt::LeftButton); QTRY_VERIFY(chip->popover()->isVisible());
    QCOMPARE(chip->popover()->findChildren<QWidget *>("attachmentRow").size(), 2);
    chip->popover()->findChildren<QPushButton *>("removeAttachment").first()->click();
    QCOMPARE(chip->fullLabel(), QString("1 attached")); QVERIFY(chip->popover()->isVisible());
    QVERIFY(!window.composer->editor()->toPlainText().contains("[Image #1]"));
    QCOMPARE(chip->popover()->findChildren<QWidget *>("attachmentRow").size(), 1);
    chip->popover()->findChildren<QPushButton *>("removeAttachment").first()->click();
    QVERIFY(chip->isHidden()); QVERIFY(!chip->popover()->isVisible());
    QVERIFY(window.composer->findChildren<QPushButton *>("removeAttachment").isEmpty());
}

void TestMessageComposer::attachmentsPopoverFollowsSessionAndSending()
{
    Window window("local/a"); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    QVERIFY(window.composer->addAttachment("a.txt", "text/plain", "a"));
    setSessionReady(*window.composer, "local/b");
    QVERIFY(window.composer->addAttachment("b.txt", "text/plain", "b")); QVERIFY(window.composer->addAttachment("c.txt", "text/plain", "c"));
    auto *chip = window.chip(); QTest::mouseClick(chip, Qt::LeftButton); QTRY_VERIFY(chip->popover()->isVisible());
    // Switching session never shows the previous session's list.
    setSessionReady(*window.composer, "local/a");
    QVERIFY(!chip->popover()->isVisible()); QCOMPARE(chip->fullLabel(), QString("1 attached"));
    QTest::mouseClick(chip, Qt::LeftButton); QTRY_VERIFY(chip->popover()->isVisible());
    QVERIFY(setSendingReady(*window.composer, "local/a"));
    QVERIFY(!chip->popover()->isVisible()); QVERIFY(chip->isHidden());
    window.composer->deliveryFinished("local/a", false, "Network down");
    QVERIFY(chip->isVisible()); QCOMPARE(chip->fullLabel(), QString("1 attached"));
}

void TestMessageComposer::programAttachmentRowIsShownInItsFolder()
{
    // Opening a draft attachment never runs a program; its copy is revealed.
    Window window("local/program"); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    QList<QUrl> opened; window.composer->openUrl = [&opened](const QUrl &url) { opened << url; return true; };
    QStringList revealed; ExternalFile::setRevealHandler([&revealed](const QString &path) { revealed << path; return true; });
    const auto restore = qScopeGuard([] { ExternalFile::setRevealHandler({}); });
    QVERIFY(window.composer->addAttachment("setup.exe", "application/octet-stream", "MZ program fixture"));
    auto *chip = window.chip(); QTest::mouseClick(chip, Qt::LeftButton); QTRY_VERIFY(chip->popover()->isVisible());
    auto *row = chip->popover()->findChild<QWidget *>("attachmentRow"); QVERIFY(row);
    QTest::mouseClick(row, Qt::LeftButton, Qt::NoModifier, QPoint(row->width() / 2, row->height() / 2));
    QTRY_COMPARE(revealed.size(), 1); QVERIFY(revealed.first().endsWith("/setup.exe")); QVERIFY(opened.isEmpty());
    auto *status = window.composer->findChild<QLabel *>("messageStatus");
    QVERIFY(status); QTRY_VERIFY(status->text().contains("setup.exe"));
    QFile::remove(revealed.first());
}

void TestMessageComposer::attachmentRowOpensStoredCopyAndPreviews()
{
    Window window("local/open"); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    QList<QUrl> opened; window.composer->openUrl = [&opened](const QUrl &url) { opened << url; return true; };
    const QByteArray png = pngBytes(); QVERIFY(window.composer->addAttachment("shot.png", "image/png", png));
    auto *chip = window.chip(); QTest::mouseClick(chip, Qt::LeftButton); QTRY_VERIFY(chip->popover()->isVisible());
    auto *row = chip->popover()->findChild<QWidget *>("attachmentRow"); QVERIFY(row);
    QEnterEvent enter(QPointF(4, 4), row->mapToGlobal(QPointF(4, 4)), row->mapToGlobal(QPointF(4, 4)));
    QApplication::sendEvent(row, &enter);
    auto *preview = window.composer->findChild<QLabel *>("attachmentPreview");
    QVERIFY(preview->isVisible()); QVERIFY(!preview->pixmap().isNull());
    QEvent leave(QEvent::Leave); QApplication::sendEvent(row, &leave); QVERIFY(preview->isHidden());
    QTest::mouseClick(row, Qt::LeftButton, Qt::NoModifier, QPoint(row->width() / 2, row->height() / 2));
    QTRY_COMPARE(opened.size(), 1); QVERIFY(opened[0].isLocalFile());
    QFile stored(opened[0].toLocalFile()); QVERIFY(stored.open(QIODevice::ReadOnly)); QCOMPARE(stored.readAll(), png);
    stored.close();   // Windows cannot remove an open file.
    QVERIFY(!chip->popover()->isVisible());
    QVERIFY(QDir(AttachmentFiles::root()).removeRecursively());
}

void TestMessageComposer::attachmentsPopoverShrinksAndTakesPreviewAlong()
{
    Window window("local/rows"); window.widget.move(40, 300); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    for (const auto *name : {"a.png", "b.png", "c.png"}) QVERIFY(window.composer->addAttachment(name, "image/png", pngBytes()));
    auto *chip = window.chip(); QTest::mouseClick(chip, Qt::LeftButton); QTRY_VERIFY(chip->popover()->isVisible());
    auto *popover = chip->popover(); QTRY_VERIFY(popover->height() > 0); const int three = popover->height();
    const int chipTop = chip->mapToGlobal(QPoint(0, 0)).y();
    popover->findChildren<QPushButton *>("removeAttachment").first()->click();
    QTRY_VERIFY(popover->height() < three); QTRY_VERIFY(popover->geometry().bottom() < chipTop);
    // The hover preview never outlives the list it belongs to.
    auto *row = popover->findChild<QWidget *>("attachmentRow"); QVERIFY(row);
    QEnterEvent enter(QPointF(4, 4), row->mapToGlobal(QPointF(4, 4)), row->mapToGlobal(QPointF(4, 4)));
    QApplication::sendEvent(row, &enter);
    auto *preview = window.composer->findChild<QLabel *>("attachmentPreview"); QVERIFY(preview->isVisible());
    chip->closePopover(); QVERIFY(preview->isHidden());
}

void TestMessageComposer::keyboardRemovalKeepsFocusInTheList()
{
    Window window("local/keys"); window.widget.move(40, 300); window.widget.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window.widget));
    for (const auto *name : {"a.txt", "b.txt", "c.txt"}) QVERIFY(window.composer->addAttachment(name, "text/plain", name));
    auto *chip = window.chip(); chip->setFocus(Qt::TabFocusReason); QTRY_VERIFY(chip->hasFocus());
    QTest::keyClick(chip, Qt::Key_Return); QTRY_VERIFY(chip->popover()->isVisible());
    const auto removes = [&] { return chip->popover()->findChildren<QPushButton *>("removeAttachment"); };
    // Removing a middle row moves focus to the row that took its place.
    removes()[1]->setFocus(); QTRY_VERIFY(removes()[1]->hasFocus());
    QTest::keyClick(removes()[1], Qt::Key_Space);
    QCOMPARE(removes().size(), 2); QTRY_VERIFY(removes()[1]->hasFocus());
    QVERIFY(removes()[1]->accessibleName().contains("c.txt"));
    // Removing the last row moves focus to the one before it.
    QTest::keyClick(removes()[1], Qt::Key_Space);
    QCOMPARE(removes().size(), 1); QTRY_VERIFY(removes()[0]->hasFocus());
    // Removing the only row closes the list and returns to the message field.
    QTest::keyClick(removes()[0], Qt::Key_Space);
    QVERIFY(!chip->popover()->isVisible()); QTRY_VERIFY(window.composer->editor()->hasFocus());
}

void TestMessageComposer::hiddenInputKeepsToolbar()
{
    Window window("local/question"); QVERIFY(QTest::qWaitForWindowExposed(&window.widget));
    QVERIFY(window.composer->isInputVisible());
    window.composer->setInputVisible(false);
    QVERIFY(!window.composer->isInputVisible()); QVERIFY(window.composer->isVisible());
    QVERIFY(window.composer->toolbar()->isVisible());
    QVERIFY(!window.composer->editor()->isVisible()); QVERIFY(!sendButton(*window.composer)->isVisible());
    window.composer->setInputVisible(true);
    QVERIFY(window.composer->editor()->isVisible()); QVERIFY(sendButton(*window.composer)->isVisible());
}

void TestMessageComposer::toolbarPreview()
{
    const auto directory = qEnvironmentVariable("HGS_COMPOSER_PREVIEW");
    if (directory.isEmpty()) QSKIP("Set HGS_COMPOSER_PREVIEW to write screenshots");
    QVERIFY(QDir().mkpath(directory));
    for (const bool dark : {true, false}) {
        QWidget window; window.setAutoFillBackground(true);
        QPalette palette = window.palette(); palette.setColor(QPalette::Window, QColor(dark ? "#1b2129" : "#ffffff")); window.setPalette(palette);
        auto *layout = new QVBoxLayout(&window); layout->setContentsMargins(16, 12, 16, 12);
        auto *composer = new MessageComposer; layout->addWidget(composer);
        setSessionReady(*composer, QString("local/preview-%1").arg(dark ? "dark" : "light")); composer->setAvailability(true);
        const auto chip = [&](ComposerToolbar::Slot slot, const char *name, const QString &full, const QString &shortText, const QString &icon, ChipTone tone) {
            auto *item = new ToolbarChip; item->setObjectName(name); item->setLabels(full, shortText); item->setIconName(icon); item->setTone(tone);
            composer->toolbar()->add(slot, item); return item;
        };
        auto *read = chip(ComposerToolbar::Slot::MarkRead, "read", "Mark as read", {}, "read-all", ChipTone::Quiet);
        auto *recovery = chip(ComposerToolbar::Slot::Recovery, "recovery", "Retry in 42 s", "42 s", "refresh", ChipTone::Warning);
        auto *cache = chip(ComposerToolbar::Slot::Cache, "cache", "Cache ~4m", "~4m", {}, ChipTone::Success);
        auto *limit = chip(ComposerToolbar::Slot::UsageLimit, "limit", "Limit reached · 2h 14m", "Limit", "attention", ChipTone::Danger);
        auto *context = new QPushButton("70,1k"); context->setFlat(true); context->setFixedHeight(24); composer->toolbar()->add(ComposerToolbar::Slot::Context, context);
        // Stand-ins for the window's compaction indicator and Cancel send link.
        auto *compaction = new QLabel("Compacting context…"); compaction->hide(); composer->toolbar()->add(ComposerToolbar::Slot::Compaction, compaction);
        auto *cancel = new QPushButton("Cancel send"); cancel->setFlat(true); cancel->setFixedHeight(20); cancel->hide();
        composer->toolbar()->add(ComposerToolbar::Slot::CompactionCancel, cancel);
        auto *panel = new QWidget; auto *panelLayout = new QVBoxLayout(panel); panelLayout->setContentsMargins(14, 10, 14, 10);
        panelLayout->addWidget(new QLabel("Retry in 42 s (1/4 attempts)")); auto *actions = new QHBoxLayout;
        for (const auto *label : {"Retry now", "Cancel retry", "Attempts…", "Settings…"}) actions->addWidget(new QPushButton(label));
        panelLayout->addLayout(actions); recovery->setPopoverContent(panel);
        composer->setTheme(dark); cache->setActive(true);
        window.resize(760, 200); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto save = [&](const QString &name) {
            QTest::qWait(80); QVERIFY(window.grab().save(QString("%1/%2-%3.png").arg(directory, name, dark ? "dark" : "light")));
        };
        save("1-normal");
        read->setActive(true);
        for (const auto *name : {"shot.png", "screen.png"}) QVERIFY(composer->addAttachment(name, "image/png", pngBytes()));
        QVERIFY(composer->addAttachment("notes.md", "text/markdown", "# notes"));
        save("2-attached-unread");
        auto *attachments = composer->findChild<ToolbarChip *>("attachmentsChip"); attachments->openPopover(); QTest::qWait(80);
        QVERIFY(attachments->popover()->grab().save(QString("%1/3-attachments-popover-%2.png").arg(directory, dark ? "dark" : "light")));
        attachments->closePopover();
        compaction->show(); cancel->show(); save("4-compaction"); compaction->hide(); cancel->hide();
        cache->setLabels("Cold cache", "Cold"); cache->setTone(ChipTone::Danger); cache->setIconName("context-warning");
        recovery->setActive(true); save("5-recovery");
        window.move(40, 360); recovery->openPopover(); QTest::qWait(80);
        QVERIFY(recovery->popover()->grab().save(QString("%1/5-recovery-popover-%2.png").arg(directory, dark ? "dark" : "light")));
        recovery->closePopover();
        recovery->setActive(false); limit->setActive(true); save("6-limit");
        recovery->setActive(true); window.resize(420, 200); save("7-narrow");
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (app.arguments().contains("--draft-crash-child")) {
        QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("message-composer");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, app.arguments().last());
        auto *composer = new MessageComposer;
        setSessionReady(*composer, "arch\nended/session"); composer->editor()->setPlainText("Last keystroke");
        QFile original(QDir(app.arguments().last()).filePath("vanished.txt"));
        if (!original.open(QIODevice::WriteOnly)) return 2;
        original.write(QByteArray("exact\0bytes", 11)); original.close();
        composer->attachDroppedFiles({original.fileName()});
        if (!original.remove()) return 2;
        composer->editor()->moveCursor(QTextCursor::End); composer->editor()->insertPlainText(QString::fromUtf8(" я\nsecond line"));
        auto cursor = composer->editor()->textCursor(); cursor.setPosition(3); cursor.setPosition(8, QTextCursor::KeepAnchor); composer->editor()->setTextCursor(cursor);
        ComposerDraftStore::flushWrites(); // Acknowledge the actual durable checkpoint before SIGKILL.
        QFile output; if (!output.open(stdout, QIODevice::WriteOnly)) return 2;
        output.write("saved\n"); output.flush();
        return app.exec(); // Parent sends SIGKILL, so no destructor/close handler runs.
    }
    TestMessageComposer test; return QTest::qExec(&test, argc, argv);
}
void TestMessageComposer::voiceInsertionRequiresUnchangedDraftAndNavigation()
{
    MessageComposer composer; setSessionReady(composer, "fixture\nvoice"); composer.setAvailability(true);
    composer.editor()->setPlainText("First"); composer.editor()->moveCursor(QTextCursor::End);
    QSignalSpy sent(&composer, &MessageComposer::sendRequested);
    const auto original = composer.voiceTarget();
    composer.editor()->insertPlainText(" edit"); composer.editor()->undo();
    QVERIFY(!composer.insertVoiceText(original, "stale words", [](bool) {}));
    const auto beforeNavigation = composer.voiceTarget();
    setSessionReady(composer, "fixture\nother"); setSessionReady(composer, "fixture\nvoice");
    QVERIFY(!composer.insertVoiceText(beforeNavigation, "wrong destination", [](bool) {}));
    const auto beforeLock = composer.voiceTarget();
    QVERIFY(composer.setWaitingToSend(composer.sessionKey(), "Waiting"));
    QVERIFY(!composer.insertVoiceText(beforeLock, "locked words", [](bool) {}));
    QVERIFY(composer.setWaitingToSend(composer.sessionKey(), {}));
    composer.editor()->moveCursor(QTextCursor::End); bool finished = false, durable = false;
    QVERIFY(composer.insertVoiceText(composer.voiceTarget(), "spoken words", [&](bool saved) { finished = true; durable = saved; }));
    QTRY_VERIFY(finished); QVERIFY(durable); QCOMPARE(sent.count(), 0);
    QCOMPARE(composer.editor()->toPlainText(), "First spoken words");
    QCOMPARE(ComposerDraftStore().load(composer.sessionKey()).text, "First spoken words");
}

#include "test_messagecomposer.moc"
