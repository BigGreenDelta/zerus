#include "QuestionCard.h"
#include "ActivityWidth.h"
#include "MarkdownObjects.h"
#include "MarkdownText.h"
#include "MonospaceFont.h"

#include <QAbstractButton>
#include <QDir>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QTest>
#include <QtTest/qtestspontaneevent.h>
#include <QSemaphore>
#include <QScopeGuard>
#include <QThread>
#include <atomic>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QValidator>
#include "ComposerDraftStore.h"

namespace {
QJsonObject request(const QString &id = "tool-question", const QString &hash = "hash-one")
{
    return {{"question_id", id}, {"question_hash", hash}, {"run_id", "run"}, {"conversation_id", "conversation"},
        {"source", "hook"}, {"can_answer", true}, {"questions", QJsonArray{
            QJsonObject{{"id", "q_0"}, {"header", "Approach"}, {"question", "Which approach should I use?"}, {"allow_other", true},
                {"options", QJsonArray{
                    QJsonObject{{"id", "opt_0_0"}, {"label", "Keep it focused"}, {"description", "Implement the requested change with minimal dependencies."}},
                    QJsonObject{{"id", "opt_0_1"}, {"label", "Explore alternatives"}, {"description", "Compare multiple approaches before implementation."}}}}},
            QJsonObject{{"id", "q_1"}, {"header", "Validation"}, {"question", "Which checks should run?"}, {"multi_select", true}, {"allow_other", true},
                {"options", QJsonArray{
                    QJsonObject{{"id", "opt_1_0"}, {"label", "Unit tests"}}, QJsonObject{{"id", "opt_1_1"}, {"label", "Native UI preview"}}}}}}}};
}

QAbstractButton *option(QuestionCard &card, const QString &id)
{
    for (auto *button : card.findChildren<QAbstractButton *>("questionOption"))
        if (button->property("optionId").toString() == id) return button;
    return nullptr;
}
QAbstractButton *other(QuestionCard &card, const QString &id)
{
    for (auto *button : card.findChildren<QAbstractButton *>("questionOther"))
        if (button->property("questionId").toString() == id) return button;
    return nullptr;
}
QLineEdit *text(QuestionCard &card, const QString &id)
{
    for (auto *editor : card.findChildren<QLineEdit *>("questionFreeText"))
        if (editor->property("questionId").toString() == id) return editor;
    return nullptr;
}
QPushButton *submit(QuestionCard &card) { return card.findChild<QPushButton *>("submitQuestionAnswer"); }

// Stands in for the desktop browser: QDesktopServices hands web links here.
class UrlRecorder final : public QObject {
    Q_OBJECT
public:
    UrlRecorder() { for (const auto *scheme : {"https", "http"}) QDesktopServices::setUrlHandler(scheme, this, "record"); }
    ~UrlRecorder() override { for (const auto *scheme : {"https", "http"}) QDesktopServices::unsetUrlHandler(scheme); }
    QList<QUrl> urls;
public slots:
    void record(const QUrl &url) { urls << url; }
};

QStringList anchors(const MarkdownText *view)
{
    QStringList result;
    for (auto block = view->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isAnchor()) result << it.fragment().charFormat().anchorHref();
    result.removeDuplicates();
    return result;
}

QTextCharFormat formatOf(const MarkdownText *view, const QString &text)
{
    const auto found = view->document()->find(text);
    QTextCursor inside(view->document()); inside.setPosition(found.selectionStart() + 1);
    return inside.charFormat();
}

// Clicks the middle of the first occurrence of text, as a reader would.
bool clickText(MarkdownText *view, const QString &text)
{
    const auto found = view->document()->find(text);
    if (found.isNull()) return false;
    QTextCursor middle(view->document()); middle.setPosition((found.selectionStart() + found.selectionEnd()) / 2);
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, view->cursorRect(middle).center());
    return true;
}
void complete(QuestionCard &card) { option(card, "opt_0_0")->click(); option(card, "opt_1_0")->click(); }
}

namespace {
void setQuestionReady(QuestionCard &card, const QString &key, const QJsonObject &question, int count = 0)
{
    card.setQuestion(key, question, count); QTRY_VERIFY(!card.isDraftLoading());
}
bool setSendingReady(QuestionCard &card, const QString &key, const QString &id, bool sending = true)
{
    if (!sending) return card.setSending(key, id, false);
    bool finished = false, okay = false;
    card.prepareSendingAsync(key, id, [&](bool result) { okay = result; finished = true; });
    QElapsedTimer clock; clock.start();
    while (!finished && clock.elapsed() < 3000) QTest::qWait(1);
    return finished && okay;
}
// A case of the shared OpenCode form contract (tests/fixtures/opencode-form-contract.json).
QJsonObject openCodeCase(const QString &name)
{
    QFile file(QStringLiteral(ZERUS_OPENCODE_FORM_CONTRACT));
    if (!file.open(QIODevice::ReadOnly)) return {};
    for (const auto &value : QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray())
        if (value.toObject().value("name") == name) return value.toObject();
    return {};
}
// The case's card as hgs sends it: its card fields with its item as the only
// question. hgs sends an item's prompt as both prompt and question, and a hash
// of the native request, which the contract leaves out.
QJsonObject openCodeCard(const QJsonObject &contractCase)
{
    auto card = contractCase.value("card").toObject();
    auto item = card.take("item").toObject(); card.remove("question_count");
    item["question"] = item.value("prompt");
    card["questions"] = QJsonArray{item}; card["question_hash"] = contractCase.value("name");
    return card;
}
}

class TestQuestionCard : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("hgs-tests"); QCoreApplication::setApplicationName("question-card");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }
    void init() { ComposerDraftStore::flushWrites(); QVERIFY(QDir(ComposerDraftStore::directory()).removeRecursively()); }
    void stalledWriterKeepsQuestionNavigationAndSubmissionResponsive();
    void submittedReceiptSupersedesStalledPreparationWithoutReplay();
    void restoresUnsentAnswersAcrossRestartWithExactRequestIdentity();
    void interruptedAndSubmittedAnswersStayLockedAfterRestart();
    void allRequiredQuestionsAndStableAnswerIds();
    void customAndMultipleAnswers();
    void enterSubmitsFreeformAnswers_data();
    void enterSubmitsFreeformAnswers();
    void enterSubmitsSelectedChoices_data();
    void enterSubmitsSelectedChoices();
    void enterFromChoicesRespectsGuardsAndFocus();
    void enterFromChoicesKeepsNavigationAndApproval();
    void enterRespectsSubmissionGuards();
    void enterCannotBypassQuestionNavigationOrApproval();
    void draftsSurviveRefreshAndSessionSwitch();
    void claudeCapabilityRefreshPreservesCompletedOtherDraft();
    void replacementHashAndBackgroundReplyAreIsolated();
    void capabilityOfflineAndErrorStates();
    void disabledSubmitAlwaysExplainsItself();
    void freeformLimitsAndPlainText();
    void markdownLinksOpenOnlyOnClick();
    void approvalAndTrustDisclosuresStayLiteral();
    void longProviderTextKeepsCompactWidth();
    void contentHeightPreservesActivityAndPinsFreeformAnswer();
    void constrainedHeightDoesNotOverlapQuestionActions_data();
    void constrainedHeightDoesNotOverlapQuestionActions();
    void currentQuestionAndOtherAnswerResizeTheForm();
    void reviewCountdownAndNextNavigation();
    void nativeCountdownPreservesScrollChoiceAndReview();
    void terminalOnlyChoicesDoNotPretendToAcceptAnswers();
    void unanswerableOpenCodeFormPointsToOpenCode();
    void oversizedOpenCodeFormShowsReasonWithoutSubmit();
    void openCodeFormDefaultsSeedOnlyWithoutSavedDraft();
    void openCodeSeededDefaultsStayUnsavedUntilEdited();
    void openCodeOtherAnswerCountsTowardItemBounds();
    void openCodePermissionListsNativeChoicesAndConfirmsAlwaysAllow();
    void openCodeFormTextMayStartWithSlashOrBang();
    void claudeApprovalListsNativeOptionsAndConfirmsBroaderOnes();
    void kimiNativeChoicesReviewFeedbackAndReplacementGuards();
    void unreadableApprovalPointsToTerminal();
    void kimiPlanReviewShowsPlanAndNamedRevise();
    void optionalQuestionsHaveExplicitSkipAndKeepDrafts();
    void submittedAnswersRestoreAndIgnoreLateErrors();
    void submittedCallbacksAndMetadataKeepExactIdentity();
    void submissionRecognitionKeepsUnconfirmedAnswers();
    void hookTrustRequiresAnExplicitChoice();
    void claudeResumePreservesQuotaWarningAndExplicitChoice();
    void contentScaleEnlargesQuestionTextOnly();
    void preview();
private:
    QTemporaryDir m_settings;
};

void TestQuestionCard::contentScaleEnlargesQuestionTextOnly()
{
    QWidget workspace; workspace.setStyleSheet("QWidget { font-size:13px; }");
    auto *layout = new QVBoxLayout(&workspace); auto *card = new QuestionCard; layout->addWidget(card);
    card->setTheme(false); setQuestionReady(*card, "local", request()); workspace.resize(560, 700); workspace.show(); QTest::qWait(10);
    const auto pixels = [card](const QString &name) { auto *widget = card->findChild<QWidget *>(name); return widget ? QFontInfo(widget->font()).pixelSize() : -1; };
    const QStringList scaled{"questionPrompt", "questionChoiceTitle", "questionChoiceDescription", "questionFreeText"};
    QMap<QString, int> before; for (const auto &name : scaled) before.insert(name, pixels(name));
    const int submitFont = QFontInfo(submit(*card)->font()).pixelSize();
    QCOMPARE(before.value("questionPrompt"), 13);
    // The prompt's Markdown is drawn at the card's sizes, semibold like before.
    auto *prompt = card->findChild<MarkdownText *>("questionPrompt"); QVERIFY(prompt);
    auto *description = card->findChild<MarkdownText *>("questionChoiceDescription"); QVERIFY(description);
    QCOMPARE(formatOf(prompt, "approach").font().pixelSize(), 13); QCOMPARE(formatOf(description, "requested").font().pixelSize(), 11);
    QVERIFY(prompt->document()->defaultFont().weight() >= QFont::DemiBold);
    option(*card, "opt_0_1")->click(); text(*card, "q_0")->setText("custom");
    card->setContentScale(2.0); QTest::qWait(10);
    for (const auto &name : scaled) QCOMPARE(pixels(name), 2 * before.value(name));
    QCOMPARE(formatOf(prompt, "approach").font().pixelSize(), 26); QCOMPARE(formatOf(description, "requested").font().pixelSize(), 22);
    QCOMPARE(QFontInfo(submit(*card)->font()).pixelSize(), submitFont);
    QVERIFY(option(*card, "opt_0_1")->isChecked()); QCOMPARE(text(*card, "q_0")->text(), QString("custom"));
    card->setContentScale(1.0); QTest::qWait(10);
    for (const auto &name : scaled) QCOMPARE(pixels(name), before.value(name));
}

void TestQuestionCard::stalledWriterKeepsQuestionNavigationAndSubmissionResponsive()
{
    QuestionCard card; setQuestionReady(card, "stalled/session", request());
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    option(card, "opt_0_0")->click(); QVERIFY(entered.tryAcquire(1, 3000));
    option(card, "opt_1_0")->click(); QVERIFY(submit(card)->isEnabled());
    int transports = 0; card.prepareSendingAsync("stalled/session", "tool-question", [&](bool okay) { if (okay) ++transports; });
    card.setQuestion("stalled/other", request("other-question")); QVERIFY(card.isDraftLoading());
    bool heartbeat = false; QTimer::singleShot(0, &card, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    QCOMPARE(transports, 0); release.release(); QTRY_COMPARE(transports, 1); QTRY_VERIFY(!card.isDraftLoading());
    QCOMPARE(card.submissionSnapshot("stalled/other", "other-question").value("sending").toBool(), false);
    QuestionCard recovered; setQuestionReady(recovered, "stalled/session", request());
    QVERIFY(!submit(recovered)->isEnabled()); QVERIFY(option(recovered, "opt_0_0")->isChecked());
    QVERIFY(option(recovered, "opt_1_0")->isChecked());
}

void TestQuestionCard::submittedReceiptSupersedesStalledPreparationWithoutReplay()
{
    auto question = request(); question["source"] = "codex_async"; question["optional"] = true;
    QuestionCard card; setQuestionReady(card, "stalled/receipt", question); complete(card); ComposerDraftStore::flushWrites();
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    int transports = 0; bool completed = false;
    card.prepareSendingAsync("stalled/receipt", "tool-question", [&](bool okay) { if (okay) ++transports; completed = true; });
    QVERIFY(entered.tryAcquire(1, 3000)); card.setSubmitted("stalled/receipt", "tool-question");
    QVERIFY(card.hasSubmittedAnswer("stalled/receipt", question));
    bool heartbeat = false; QTimer::singleShot(0, &card, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    QVERIFY(!completed); release.release(); QTRY_VERIFY(completed); QCOMPARE(transports, 0);
    ComposerDraftStore::flushWrites();
    QuestionCard recovered; setQuestionReady(recovered, "stalled/receipt", question);
    QVERIFY(recovered.hasSubmittedAnswer("stalled/receipt", question)); QVERIFY(!submit(recovered)->isEnabled());
}

void TestQuestionCard::restoresUnsentAnswersAcrossRestartWithExactRequestIdentity()
{
    {
        QuestionCard card; setQuestionReady(card, "arch\nagent", request());
        other(card, "q_0")->click(); text(card, "q_0")->setText("Keep every typed character");
        option(card, "opt_1_1")->click(); card.findChild<QTabBar *>("questionTabs")->setCurrentIndex(1);
    }
    QuestionCard restored; QSignalSpy sent(&restored, &QuestionCard::answerRequested);
    setQuestionReady(restored, "arch\nagent", request());
    QCOMPARE(text(restored, "q_0")->text(), "Keep every typed character");
    QVERIFY(other(restored, "q_0")->isChecked()); QVERIFY(option(restored, "opt_1_1")->isChecked());
    QCOMPARE(restored.findChild<QTabBar *>("questionTabs")->currentIndex(), 1); QCOMPARE(sent.count(), 0);
    auto changed = request(); changed["run_id"] = "different-run";
    setQuestionReady(restored, "arch\nagent", changed); QVERIFY(text(restored, "q_0")->text().isEmpty());
    changed = request(); changed["conversation_id"] = "different-conversation";
    setQuestionReady(restored, "arch\nagent", changed); QVERIFY(text(restored, "q_0")->text().isEmpty());
    setQuestionReady(restored, "mac\nagent", request()); QVERIFY(text(restored, "q_0")->text().isEmpty());
    setQuestionReady(restored, "arch\nagent", request("tool-question", "different-hash")); QVERIFY(text(restored, "q_0")->text().isEmpty());
    setQuestionReady(restored, "arch\nagent", request()); QCOMPARE(text(restored, "q_0")->text(), "Keep every typed character");
}

void TestQuestionCard::interruptedAndSubmittedAnswersStayLockedAfterRestart()
{
    {
        QuestionCard card; setQuestionReady(card, "arch\nagent", request()); complete(card);
        QVERIFY(setSendingReady(card, "arch\nagent", "tool-question"));
    }
    QuestionCard restored; setQuestionReady(restored, "arch\nagent", request());
    QVERIFY(!submit(restored)->isEnabled());
    restored.findChild<QPushButton *>("questionAllowRetry")->click(); QVERIFY(submit(restored)->isEnabled());
    restored.setSubmitted("arch\nagent", "tool-question");
    QuestionCard submitted; setQuestionReady(submitted, "arch\nagent", request());
    QVERIFY(!submit(submitted)->isEnabled()); QCOMPARE(submit(submitted)->text(), "Submitted");
    QVERIFY(option(submitted, "opt_0_0")->isChecked());
}

void TestQuestionCard::submittedAnswersRestoreAndIgnoreLateErrors()
{
    QuestionCard card; auto question=request();question["source"]="codex_async";question["optional"]=true;question["can_skip"]=true;
    setQuestionReady(card, "local",question);card.show();
    complete(card);setSendingReady(card, "local","tool-question",true);
    question["can_answer"]=false;question["can_skip"]=false;
    question["answer_delivery"]=QJsonObject{{"status","submitted"},{"question_id","tool-question"},{"question_hash","hash-one"},
        {"run_id","run"},{"conversation_id","conversation"},{"answers",QJsonArray{
            QJsonObject{{"question_id","q_0"},{"selected_option_ids",QJsonArray{}},{"text","Saved queued answer"}},
            QJsonObject{{"question_id","q_1"},{"selected_option_ids",QJsonArray{"opt_1_1"}},{"text",""}}}}};
    setQuestionReady(card, "local",question);
    QVERIFY(card.hasSubmittedAnswer("local",question));
    QCOMPARE(text(card,"q_0")->text(),QString("Saved queued answer"));QVERIFY(other(card,"q_0")->isChecked());
    QVERIFY(option(card,"opt_1_1")->isChecked());QVERIFY(!option(card,"opt_1_0")->isChecked());
    QVERIFY(!text(card,"q_0")->isEnabled());QVERIFY(!option(card,"opt_1_1")->isEnabled());
    QVERIFY(!submit(card)->isEnabled());QCOMPARE(submit(card)->text(),QString("Submitted"));
    QCOMPARE(card.findChild<QLabel *>("questionProgress")->text(),QString("Awaiting agent"));
    QVERIFY(!card.findChild<QPushButton *>("skipQuestion")->isEnabled());
    card.setError("local","tool-question","Old timeout",true);
    QVERIFY(card.findChild<QPushButton *>("questionAllowRetry")->isHidden());
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("Waiting for Codex"));
    QSignalSpy sent(&card,&QuestionCard::answerRequested);submit(card)->click();
    card.findChild<QPushButton *>("skipQuestion")->click();QCOMPARE(sent.size(),0);
    setQuestionReady(card, "other",request());complete(card);QVERIFY(submit(card)->isEnabled());
    setQuestionReady(card, "local",question);QVERIFY(!submit(card)->isEnabled());
    QuestionCard fresh;setQuestionReady(fresh, "local",question);
    QCOMPARE(text(fresh,"q_0")->text(),QString("Saved queued answer"));QVERIFY(!submit(fresh)->isEnabled());
    question.remove("answer_delivery");
    QVERIFY(fresh.hasSubmittedAnswer("local",question)); // stale metadata cannot reoffer a confirmed answer
}

void TestQuestionCard::submittedCallbacksAndMetadataKeepExactIdentity()
{
    auto question=request();question["source"]="codex_async";question["optional"]=true;
    QuestionCard card;setQuestionReady(card, "local",question);complete(card);setSendingReady(card, "local","tool-question",true);
    auto replacement=question;replacement["question_hash"]="new-hash";
    setQuestionReady(card, "local",replacement);card.setSubmitted("local","tool-question");
    QVERIFY(!card.hasSubmittedAnswer("local",replacement));
    QVERIFY(submit(card)->text()!=QString("Submitted"));complete(card);
    QVERIFY(submit(card)->isEnabled());
    setQuestionReady(card, "local",question);QVERIFY(!submit(card)->isEnabled());QCOMPARE(submit(card)->text(),QString("Submitted"));
    QVERIFY(card.hasSubmittedAnswer("local",question));
    setQuestionReady(card, "remote",question);complete(card);QVERIFY(submit(card)->isEnabled());
    QVERIFY(!card.hasSubmittedAnswer("remote",question));
    for(const auto &field:{"run_id","conversation_id"}) {
        auto unrelated=question;unrelated[field]="other-identity";
        setQuestionReady(card, "local",unrelated);complete(card);QVERIFY(submit(card)->isEnabled());
        QVERIFY(!card.hasSubmittedAnswer("local",unrelated));
    }
    const QJsonObject delivery{{"status","submitted"},{"question_id","tool-question"},{"question_hash","hash-one"},
        {"run_id","run"},{"conversation_id","conversation"},{"answers",QJsonArray{}}};
    for(const auto &field:{"question_id","question_hash","run_id","conversation_id"}) {
        QuestionCard fresh;auto stale=delivery;stale[field]="unrelated";auto snapshot=question;snapshot["answer_delivery"]=stale;
        const auto unsubmitted = QString("unsubmitted-%1").arg(field);
        setQuestionReady(fresh, unsubmitted,snapshot);complete(fresh);QVERIFY(submit(fresh)->isEnabled());
        QVERIFY(!fresh.hasSubmittedAnswer(unsubmitted,snapshot));
        QuestionCard known; setQuestionReady(known, "local",snapshot); QVERIFY(!submit(known)->isEnabled());
        QVERIFY(known.hasSubmittedAnswer("local",snapshot));
    }
    QuestionCard unsupported;auto snapshot=question;snapshot["source"]="other";snapshot["answer_delivery"]=delivery;
    setQuestionReady(unsupported, "unsupported",snapshot);complete(unsupported);QVERIFY(submit(unsupported)->isEnabled());
    QVERIFY(!unsupported.hasSubmittedAnswer("unsupported",snapshot));

}

void TestQuestionCard::submissionRecognitionKeepsUnconfirmedAnswers()
{
    auto question=request();question["source"]="codex_async";question["optional"]=true;
    QuestionCard card;setQuestionReady(card, "local",question);complete(card);
    setSendingReady(card, "local","tool-question");QVERIFY(!card.hasSubmittedAnswer("local",question));
    card.setError("local","tool-question","No receipt",true);QVERIFY(!card.hasSubmittedAnswer("local",question));
    card.setSubmitted("local","tool-question");QVERIFY(card.hasSubmittedAnswer("local",question));
    // A persisted receipt can be recognized before the question is displayed.
    question["answer_delivery"]=QJsonObject{{"status","submitted"},{"question_id","tool-question"},
        {"question_hash","hash-one"},{"run_id","run"},{"conversation_id","conversation"}};
    QuestionCard fresh;QVERIFY(fresh.hasSubmittedAnswer("local",question));
    auto unsupported=question;unsupported["optional"]=false;QVERIFY(!fresh.hasSubmittedAnswer("local",unsupported));
    unsupported=question;unsupported["source"]="other";QVERIFY(!fresh.hasSubmittedAnswer("local",unsupported));
    question.remove("answer_delivery");QVERIFY(fresh.hasSubmittedAnswer("local",question));

}

void TestQuestionCard::claudeResumePreservesQuotaWarningAndExplicitChoice()
{
    QuestionCard card; auto question = request("claude-resume:exact", "exact");
    question["source"] = "claude_resume"; question["answer_transport"] = "claude_tui";
    const QString warning = "This conversation has been inactive for 1h 25m and is 293.6k tokens long. Resuming it will use about 5% of your 5-hour usage limit.";
    question["questions"] = QJsonArray{QJsonObject{{"id", "resume"}, {"question", "Resume this conversation?"},
        {"body", warning}, {"allow_other", false}, {"options", QJsonArray{
            QJsonObject{{"id", "resume"}, {"label", "Resume"}},
            QJsonObject{{"id", "new_conversation"}, {"label", "Start a new conversation"},
                {"description", "Claude can look this one up if you refer to it later"}}}}}};
    setQuestionReady(card, "claude/session", question); card.resize(700, 350); card.show();
    QVERIFY(!submit(card)->isEnabled());
    QVERIFY(!option(card, "resume")->isChecked()); QVERIFY(!option(card, "new_conversation")->isChecked());
    QVERIFY(card.findChildren<QLineEdit *>("questionFreeText").isEmpty());
    bool visibleWarning = false;
    for (auto *label : card.findChildren<QLabel *>()) visibleWarning |= label->text().contains(warning);
    QVERIFY(visibleWarning);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    option(card, "new_conversation")->click(); QVERIFY(submit(card)->isEnabled()); QCOMPARE(sent.size(), 0);
    submit(card)->click(); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject()["selected_option_ids"].toArray(), QJsonArray{"new_conversation"});
}

void TestQuestionCard::hookTrustRequiresAnExplicitChoice()
{
    QuestionCard card;auto hooks=request();hooks["trust_request"]=true;hooks["conversation_id"]=QJsonValue::Null;
    hooks["questions"]=QJsonArray{QJsonObject{{"id","hooks_trust"},{"header","Hook trust"},{"question","Hooks need review"},
        {"body","4 hooks are new or changed. Hooks can run outside the sandbox after you trust them."},{"allow_other",false},
        {"options",QJsonArray{QJsonObject{{"id","review"},{"label","Review hooks"}},QJsonObject{{"id","trust"},{"label","Trust all and continue"}},
            QJsonObject{{"id","continue_without_trusting"},{"label","Continue without trusting (hooks won't run)"}}}}}};
    setQuestionReady(card, "startup",hooks);card.resize(700,450);card.show();
    QCOMPARE(card.findChild<QLabel *>("questionHeading")->text(),QString("Agent needs your approval"));
    QVERIFY(card.findChild<QPushButton *>("skipQuestion")->isHidden());
    QVERIFY(!submit(card)->isEnabled());QSignalSpy sent(&card,&QuestionCard::answerRequested);
    option(card,"trust")->click();QVERIFY(submit(card)->isEnabled());QCOMPARE(sent.size(),0);
    submit(card)->click();QCOMPARE(sent.size(),1);
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject()["selected_option_ids"].toArray(),QJsonArray{"trust"});
    const auto preview=qEnvironmentVariable("ZERUS_HOOKS_PREVIEW");
    if(!preview.isEmpty()) {QDir().mkpath(preview);QVERIFY(card.grab().save(preview+"/hooks.png"));}
}

void TestQuestionCard::reviewCountdownAndNextNavigation()
{
    QuestionCard card;card.resize(700,450);setQuestionReady(card, "session",request());card.show();card.activateWindow();QApplication::setActiveWindow(&card);
    auto *next=card.findChild<QPushButton *>("questionNext"),*back=card.findChild<QPushButton *>("questionPrevious");
    QVERIFY(next->isVisible());QVERIFY(!back->isEnabled());QVERIFY(submit(card)->isHidden());
    option(card,"opt_0_0")->click();next->click();
    QCOMPARE(card.findChild<QTabBar *>("questionTabs")->currentIndex(),1);QVERIFY(next->isHidden());QVERIFY(submit(card)->isVisible());
    option(card,"opt_1_0")->click();back->click();QVERIFY(option(card,"opt_0_0")->isChecked());
    auto approval=request("approval","command-one");approval["approval"]=true;
    approval["questions"]=QJsonArray{QJsonObject{{"id","approval"},{"question","Approve this Bash command?"},{"body","printf fixture > fixture.txt"},
        {"options",QJsonArray{QJsonObject{{"id","allow"},{"label","Approve once"}},QJsonObject{{"id","deny"},{"label","Deny"}}}}}};
    setQuestionReady(card, "session",approval);
    // Approval commands read as code: an installed fixed-pitch family, never the
    // generic "monospace" that macOS lacks and resolves to its proportional font.
    const auto *command = card.findChild<QLabel *>("questionBody"); QVERIFY(command);
    QCOMPARE(command->font().family(), monospaceFamily());
    auto *allow=card.findChild<QPushButton *>("approveToolRequest");auto *deny=card.findChild<QPushButton *>("denyToolRequest");
    QSignalSpy sent(&card,&QuestionCard::answerRequested);
    QVERIFY(!allow->isEnabled());QVERIFY(deny->isEnabled());QVERIFY(allow->text().contains("(3)"));
    QTest::qWait(1100);setQuestionReady(card, "session",approval);QVERIFY(!allow->isEnabled());
    card.hide();QTest::qWait(150);card.show();card.activateWindow();QApplication::setActiveWindow(&card);QVERIFY(!allow->isEnabled());
    // The real 3 s review delay, end to end; a short one for the next question.
    QTRY_VERIFY_WITH_TIMEOUT(allow->isEnabled(),4000);QCOMPARE(sent.size(),0);
    approval["question_hash"]="command-two";setQuestionReady(card, "session",approval);QVERIFY(!allow->isEnabled());
    card.setReviewDelayForTests(200);QTRY_VERIFY(allow->isEnabled());allow->click();QCOMPARE(sent.size(),1);
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject().value("selected_option_ids").toArray(),QJsonArray{"allow"});
    const auto preview=qEnvironmentVariable("HGS_APPROVAL_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(card.grab().save(preview+"/approval.png"));}
}

void TestQuestionCard::claudeApprovalListsNativeOptionsAndConfirmsBroaderOnes()
{
    QuestionCard card;card.resize(700,450);card.show();card.activateWindow();QApplication::setActiveWindow(&card);
    auto approval=request("claude-approval:one","read-one");approval["approval"]=true;approval["approval_choices"]=true;approval["source"]="claude_tool_approval";
    approval["questions"]=QJsonArray{QJsonObject{{"id","approval"},{"question","Allow this read outside the working directories?"},
        {"body","Read outside the working directories\n\nRead(/home/user/.codex/image.png)"},{"allow_other",false},{"options",QJsonArray{
            QJsonObject{{"id","1"},{"label","Yes, and keep allowing any reads outside the working directories"},{"scope","broader"}},
            QJsonObject{{"id","2"},{"label","No, and block reads outside the working directories from now on"},{"scope","broader"}},
            QJsonObject{{"id","3"},{"label","No, and ask again next time"},{"scope","once"}},
            QJsonObject{{"id","4"},{"label","Yes, but ask again next time"},{"scope","once"}}}}}};
    setQuestionReady(card, "session",approval);
    QCOMPARE(card.findChild<QLabel *>("questionHeading")->text(),QString("Agent needs your approval"));
    QVERIFY(card.findChild<QPushButton *>("approveToolRequest")->isHidden());QVERIFY(card.findChild<QPushButton *>("denyToolRequest")->isHidden());
    QVERIFY(option(card,"1")->isVisible());QVERIFY(option(card,"4")->isVisible());
    QCOMPARE(option(card,"1")->accessibleDescription(),QString("Applies beyond this request. Asks for confirmation."));
    QVERIFY(option(card,"4")->accessibleDescription().isEmpty());
    QSignalSpy sent(&card,&QuestionCard::answerRequested);
    option(card,"1")->click();QVERIFY(!submit(card)->isEnabled());QVERIFY(submit(card)->text().contains("(3)"));
    // reviewCountdownAndNextNavigation waits out the real review delay.
    card.setReviewDelayForTests(200);QTRY_VERIFY(submit(card)->isEnabled());
    // A broader option needs a second click; changing the choice resets it.
    submit(card)->click();QCOMPARE(sent.size(),0);QCOMPARE(submit(card)->text(),QString("Confirm"));
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("beyond this request"));
    option(card,"4")->click();QCOMPARE(submit(card)->text(),QString("Submit answer"));
    option(card,"1")->click();submit(card)->click();QCOMPARE(sent.size(),0);submit(card)->click();QCOMPARE(sent.size(),1);
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject().value("selected_option_ids").toArray(),QJsonArray{"1"});
    approval["question_hash"]="read-two";setQuestionReady(card, "session",approval);QVERIFY(!submit(card)->isEnabled());
    option(card,"4")->click();QTRY_VERIFY(submit(card)->isEnabled());submit(card)->click();QCOMPARE(sent.size(),2);
    QCOMPARE(sent.last()[2].toJsonArray().first().toObject().value("selected_option_ids").toArray(),QJsonArray{"4"});
    const auto preview=qEnvironmentVariable("HGS_APPROVAL_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(card.grab().save(preview+"/claude-approval.png"));}
}

void TestQuestionCard::kimiNativeChoicesReviewFeedbackAndReplacementGuards()
{
    QuestionCard card; card.resize(700, 500); card.show(); card.activateWindow(); QApplication::setActiveWindow(&card);
    auto approval = request("kimi-approval:fixture", "first");
    approval["approval"] = true; approval["approval_choices"] = true; approval["source"] = "kimi_tool_approval";
    approval["questions"] = QJsonArray{QJsonObject{{"id", "approval"}, {"question", "Run this synthetic command?"},
        {"body", "printf fixture # *literal*"}, {"allow_other", true}, {"other_label", "Reject with feedback"},
        {"options", QJsonArray{QJsonObject{{"id", "1"}, {"label", "Approve once"}, {"scope", "once"}},
            QJsonObject{{"id", "2"}, {"label", "Approve for this session"}, {"description", "Exact command for this session only"}, {"scope", "broader"}},
            QJsonObject{{"id", "3"}, {"label", "Reject"}, {"scope", "once"}}}}}};
    setQuestionReady(card, "session", approval);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    QVERIFY(option(card, "2")); QTest::mouseClick(option(card, "2"), Qt::LeftButton);
    QVERIFY(!submit(card)->isEnabled());
    // Exercise the unchanged production delay, rather than setReviewDelayForTests.
    QTest::qWait(1000); QVERIFY(!submit(card)->isEnabled()); QCOMPARE(sent.size(), 0);
    card.hide(); QTest::qWait(150); card.show(); card.activateWindow(); QApplication::setActiveWindow(&card);
    QTest::qWait(1000); QVERIFY(!submit(card)->isEnabled()); QCOMPARE(sent.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(submit(card)->isEnabled(), 4000);
    QTest::mouseClick(submit(card), Qt::LeftButton);
    QCOMPARE(sent.size(), 0); QCOMPARE(submit(card)->text(), QString("Confirm"));
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("beyond this request"));
    // Selecting another native choice cancels the broader second step, without sending it.
    QTest::mouseClick(option(card, "3"), Qt::LeftButton);
    QCOMPARE(submit(card)->text(), QString("Submit answer")); QCOMPARE(sent.size(), 0);
    QTest::mouseClick(option(card, "2"), Qt::LeftButton); QTest::mouseClick(submit(card), Qt::LeftButton);
    QCOMPARE(sent.size(), 0); QCOMPARE(submit(card)->text(), QString("Confirm"));
    // A replacement card cannot consume the old confirmation or its elapsed review.
    approval["question_hash"] = "replacement"; setQuestionReady(card, "session", approval);
    QVERIFY(!submit(card)->isEnabled()); QCOMPARE(sent.size(), 0);
    QTest::mouseClick(option(card, "2"), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(submit(card)->isEnabled(), 4000);
    QTest::mouseClick(submit(card), Qt::LeftButton); QCOMPARE(sent.size(), 0);
    QCOMPARE(submit(card)->text(), QString("Confirm"));
    QTest::mouseClick(submit(card), Qt::LeftButton); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject().value("selected_option_ids").toArray(), QJsonArray{"2"});
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject().value("text").toString(), QString());

    approval["question_hash"] = "feedback"; setQuestionReady(card, "session", approval);
    QVERIFY(other(card, "approval")); QTest::mouseClick(other(card, "approval"), Qt::LeftButton);
    auto *editor = text(card, "approval"); QVERIFY(editor); editor->setFocus();
    QTest::keyClicks(editor, "Use a safer command"); QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(sent.size(), 1); QVERIFY(!submit(card)->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(submit(card)->isEnabled(), 4000);
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 2);
    const auto feedback = sent.last()[2].toJsonArray().first().toObject();
    QCOMPARE(feedback.value("selected_option_ids").toArray(), QJsonArray());
    QCOMPARE(feedback.value("text").toString(), QString("Use a safer command"));
    // Read-only replacement retains literal choices but cannot emit a callback, including Enter.
    approval["question_hash"] = "readonly"; approval["can_answer"] = false;
    approval["answer_unavailable_reason"] = "Synthetic request is no longer current";
    setQuestionReady(card, "session", approval);
    QVERIFY(!submit(card)->isEnabled()); QVERIFY(!option(card, "1")->isEnabled());
    QTest::keyClick(&card, Qt::Key_Return); QCOMPARE(sent.size(), 2);
}

void TestQuestionCard::nativeCountdownPreservesScrollChoiceAndReview()
{
    QuestionCard card; card.resize(700, 450); card.show(); card.activateWindow(); QApplication::setActiveWindow(&card);
    card.setReviewDelayForTests(600);
    auto approval = request("claude-approval:child", "child-request-one");
    approval["source"] = "claude_tool_approval"; approval["approval"] = true; approval["approval_choices"] = true;
    QString body = "Bash command · from the \"fixture-checks\" workflow\n\n";
    for (int i = 0; i < 45; ++i) body += QString("printf fixture-%1\n").arg(i);
    const auto items = [&](int seconds) { return QJsonArray{QJsonObject{{"id", "approval"}, {"question", "Do you want to proceed?"},
        {"body", body + QString("⚠ Claude Code will automatically deny this request in %1:%2, to avoid blocking progress on an unattended session").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'))}, {"allow_other", false},
        {"options", QJsonArray{QJsonObject{{"id", "1"}, {"label", "Yes"}, {"scope", "once"}},
                               QJsonObject{{"id", "2"}, {"label", "No"}, {"scope", "once"}}}}}}; };
    approval["questions"] = items(120); setQuestionReady(card, "session", approval);
    auto *scroll = card.findChild<QScrollArea *>("questionContent"); QVERIFY(scroll);
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 100);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    auto *choice = option(card, "2"); QVERIFY(choice); choice->setFocus();
    QTest::mouseClick(choice, Qt::LeftButton);
    QVERIFY(choice->isChecked()); QVERIFY(!submit(card)->isEnabled());
    const int position = scroll->verticalScrollBar()->value();
    auto *bodyLabel = card.findChild<QLabel *>("questionBody"); QVERIFY(bodyLabel);
    bodyLabel->setSelection(bodyLabel->text().indexOf("printf fixture-30"), 17);
    const auto selectedText = bodyLabel->selectedText();
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < 1200) {
        approval["questions"] = items(119 - int(elapsed.elapsed() / 100)); card.setQuestion("session", approval);
        QCOMPARE(card.findChild<QScrollArea *>("questionContent"), scroll);
        QCOMPARE(option(card, "2"), choice); QVERIFY(choice->isChecked());
        QCOMPARE(scroll->verticalScrollBar()->value(), position);
        QCOMPARE(bodyLabel->selectedText(), selectedText);
        QTest::qWait(100);
    }
    QVERIFY(submit(card)->isEnabled()); QCOMPARE(sent.size(), 0);
    const auto preview = qEnvironmentVariable("HGS_APPROVAL_PREVIEW");
    if (!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(card.grab().save(preview + "/workflow-choice.png")); }
    QTest::mouseClick(submit(card), Qt::LeftButton); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[2].toJsonArray().first().toObject().value("selected_option_ids").toArray(), QJsonArray{"2"});
    // A real changed warning/command gets a new identity and a fresh review.
    approval["question_hash"] = "child-request-two"; setQuestionReady(card, "session", approval);
    QVERIFY(!option(card, "2")->isChecked()); QVERIFY(!submit(card)->isEnabled());
}

void TestQuestionCard::terminalOnlyChoicesDoNotPretendToAcceptAnswers()
{
    QuestionCard card; card.show();
    auto question = request(); question["source"] = "claude_terminal_choice"; question["can_answer"] = false;
    question["answer_unavailable_reason"] = "Answer this in Terminal.";
    setQuestionReady(card, "session", question);
    QVERIFY(!option(card, "opt_0_0")->isEnabled()); QVERIFY(submit(card)->isHidden());
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    option(card, "opt_0_0")->click(); QVERIFY(!option(card, "opt_0_0")->isChecked()); QCOMPARE(sent.size(), 0);
    QVERIFY(card.findChild<QPushButton *>("questionOpenTerminal")->isEnabled());
}

// OpenCode reports why a form cannot be answered in Zerus on the card itself.
void TestQuestionCard::unanswerableOpenCodeFormPointsToOpenCode()
{
    QuestionCard card; card.resize(700, 450); card.show();
    auto form = request("opencode-form", "form-hash"); form["source"] = "opencode_native"; form["kind"] = "opencode_form";
    form["answer_transport"] = "native_opencode"; form["can_answer"] = false; form["answerable"] = false;
    form["unavailable_reason"] = "Answer this form in OpenCode";
    setQuestionReady(card, "opencode/session", form);
    QVERIFY(submit(card)->isHidden() || !submit(card)->isEnabled());
    QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), QString("Answer this form in OpenCode"));
    // A specific answer reason still wins, and only without either is Terminal named.
    form["answer_unavailable_reason"] = "Answer this one elsewhere.";
    setQuestionReady(card, "opencode/session", form);
    QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), QString("Answer this one elsewhere."));
    form.remove("answer_unavailable_reason"); form.remove("unavailable_reason"); form["question_hash"] = "form-hash-2";
    setQuestionReady(card, "opencode/session", form);
    QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), QString("Answer this request in Terminal."));
}

// hgs empties an OpenCode form whose text is too large to show; the card
// still explains where to answer it and offers no Submit.
void TestQuestionCard::oversizedOpenCodeFormShowsReasonWithoutSubmit()
{
    QuestionCard card; card.resize(700, 450); card.show();
    QJsonObject form{{"question_id", "que_large"}, {"question_hash", "large-hash"}, {"run_id", "run"}, {"conversation_id", "ses_one"},
        {"kind", "opencode_form"}, {"source", "opencode_native"}, {"answer_transport", "native_opencode"}, {"can_answer", false},
        {"answerable", false}, {"questions", QJsonArray{}}, {"unavailable_reason", "Answer this form in OpenCode"}};
    setQuestionReady(card, "opencode/session", form);
    QVERIFY(card.isVisible()); QVERIFY(submit(card)->isHidden());
    QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), QString("Answer this form in OpenCode"));
}

// OpenCode form defaults seed a new answer; a saved draft for the same
// question wins, even an intentionally empty text. Nothing is sent by itself.
void TestQuestionCard::openCodeFormDefaultsSeedOnlyWithoutSavedDraft()
{
    const auto form = [](const QString &hash, const QString &source = "opencode_native") {
        QJsonObject card{{"question_id", "que_defaults"}, {"question_hash", hash}, {"run_id", "run"}, {"conversation_id", "ses_one"},
            {"kind", "opencode_form"}, {"source", source}, {"answer_transport", "native_opencode"}, {"can_answer", true}, {"answerable", true}};
        card["questions"] = QJsonArray{
            QJsonObject{{"id", "name"}, {"header", "Name"}, {"question", "Your name"}, {"multi_select", false}, {"required", true},
                {"allow_other", true}, {"input_kind", "text"}, {"options", QJsonArray{}}, {"default_text", "Ada Lovelace"}, {"max_length", 3}},
            QJsonObject{{"id", "confirm"}, {"header", "Confirm"}, {"question", "Proceed?"}, {"multi_select", false}, {"required", true},
                {"allow_other", false}, {"input_kind", "choice"}, {"default_option_ids", QJsonArray{"yes"}},
                {"options", QJsonArray{QJsonObject{{"id", "yes"}, {"label", "Yes"}}, QJsonObject{{"id", "no"}, {"label", "No"}}}}},
            QJsonObject{{"id", "tags"}, {"header", "Tags"}, {"question", "Pick tags"}, {"multi_select", true}, {"allow_multiple", true},
                {"required", true}, {"allow_other", false}, {"input_kind", "choice"}, {"default_option_ids", QJsonArray{"a", "c", "missing"}},
                {"min_items", 1}, {"max_items", 2},
                {"options", QJsonArray{QJsonObject{{"id", "a"}, {"label", "Alpha"}}, QJsonObject{{"id", "b"}, {"label", "Beta"}}, QJsonObject{{"id", "c"}, {"label", "Gamma"}}}}}};
        return card;
    };
    const auto modes = [](QuestionCard &card) { QStringList texts; for (auto *label : card.findChildren<QLabel *>("questionMode")) texts << label->text(); return texts; };
    {
        QuestionCard card; QSignalSpy sent(&card, &QuestionCard::answerRequested);
        setQuestionReady(card, "opencode\nsession", form("hash-one"));
        QCOMPARE(text(card, "name")->text(), QString("Ada")); QVERIFY(text(card, "name")->validator());
        { QString longer = "Adam"; int position = 4; QCOMPARE(text(card, "name")->validator()->validate(longer, position), QValidator::Invalid); }
        QVERIFY(option(card, "yes")->isChecked()); QVERIFY(!option(card, "no")->isChecked());
        QVERIFY(option(card, "a")->isChecked()); QVERIFY(!option(card, "b")->isChecked()); QVERIFY(option(card, "c")->isChecked());
        QVERIFY2(modes(card).contains("Select 1 to 2"), qPrintable(modes(card).join(" | ")));
        QVERIFY(submit(card)->isEnabled()); QTest::qWait(50); QCOMPARE(sent.size(), 0);
        // An intentionally cleared answer is a saved choice.
        text(card, "name")->clear();
    }
    {
        QuestionCard restored; setQuestionReady(restored, "opencode\nsession", form("hash-one"));
        QCOMPARE(text(restored, "name")->text(), QString()); QVERIFY(option(restored, "yes")->isChecked());
        // A changed question starts again from its defaults.
        setQuestionReady(restored, "opencode\nsession", form("hash-two"));
        QCOMPARE(text(restored, "name")->text(), QString("Ada")); QVERIFY(option(restored, "c")->isChecked());
    }
    // A card without defaults, and another source's card, start empty as before.
    auto legacy = form("hash-three"); auto items = legacy["questions"].toArray();
    for (int i = 0; i < items.size(); ++i) {
        auto item = items[i].toObject();
        for (const auto *field : {"default_text", "default_option_ids", "max_length", "min_items", "max_items"}) item.remove(field);
        items[i] = item;
    }
    legacy["questions"] = items;
    for (const auto &card : {legacy, form("hash-four", "hook")}) {
        QuestionCard plain; setQuestionReady(plain, "opencode\nsession", card);
        QVERIFY(text(plain, "name")->text().isEmpty()); QCOMPARE(text(plain, "name")->maxLength(), 4096); QVERIFY(!text(plain, "name")->validator());
        for (const auto *id : {"yes", "no", "a", "b", "c"}) QVERIFY2(!option(plain, id)->isChecked(), id);
        QVERIFY(!modes(plain).contains("Select 1 to 2"));
    }
}

// Seeded defaults are shown but never become a stored (unsent) draft until the
// user edits; navigating between questions saves only the page.
void TestQuestionCard::openCodeSeededDefaultsStayUnsavedUntilEdited()
{
    QJsonObject form{{"question_id", "que_seeded"}, {"question_hash", "seeded-hash"}, {"run_id", "run"}, {"conversation_id", "ses_one"},
        {"kind", "opencode_form"}, {"source", "opencode_native"}, {"answer_transport", "native_opencode"}, {"can_answer", true}, {"answerable", true}};
    form["questions"] = QJsonArray{
        QJsonObject{{"id", "name"}, {"header", "Name"}, {"question", "Your name"}, {"multi_select", false}, {"required", true},
            {"allow_other", true}, {"input_kind", "text"}, {"options", QJsonArray{}}, {"default_text", QString::fromUtf8("Ada \xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80")}, {"max_length", 6}},
        QJsonObject{{"id", "confirm"}, {"header", "Confirm"}, {"question", "Proceed?"}, {"multi_select", false}, {"required", true},
            {"allow_other", false}, {"input_kind", "choice"}, {"default_option_ids", QJsonArray{"yes"}},
            {"options", QJsonArray{QJsonObject{{"id", "yes"}, {"label", "Yes"}}, QJsonObject{{"id", "no"}, {"label", "No"}}}}}};
    const auto stored = [] { ComposerDraftStore::flushWrites(); QStringList keys; for (const auto &key : ComposerDraftStore().keys()) if (key.startsWith("question\n")) keys << key; return keys; };
    {
        QuestionCard card; card.show(); setQuestionReady(card, "opencode\nsession", form);
        // The limit counts characters, and a clipped default keeps whole characters.
        QCOMPARE(text(card, "name")->text(), QString::fromUtf8("Ada \xF0\x9F\x98\x80\xF0\x9F\x98\x80"));
        auto *tabs = card.findChild<QTabBar *>("questionTabs"); QVERIFY(tabs); QCOMPARE(tabs->count(), 2);
        tabs->setCurrentIndex(1); tabs->setCurrentIndex(0); tabs->setCurrentIndex(1);
        QCOMPARE(text(card, "name")->text(), QString::fromUtf8("Ada \xF0\x9F\x98\x80\xF0\x9F\x98\x80")); QVERIFY(option(card, "yes")->isChecked());
        QVERIFY2(stored().isEmpty(), qPrintable(stored().join(" | ")));
    }
    {
        // Still nothing saved: a new card seeds again and keeps the page.
        QuestionCard card; card.show(); setQuestionReady(card, "opencode\nsession", form);
        QCOMPARE(text(card, "name")->text(), QString::fromUtf8("Ada \xF0\x9F\x98\x80\xF0\x9F\x98\x80")); QVERIFY(option(card, "yes")->isChecked());
        QVERIFY(stored().isEmpty());
        // An edit stores the answer.
        auto *field = text(card, "name"); field->setText("Ada"); QTRY_COMPARE(stored().size(), 1);
        // Typing beyond the limit in characters is refused.
        auto value = QString::fromUtf8("Ada \xF0\x9F\x98\x80\xF0\x9F\x98\x80\xF0\x9F\x98\x80"); int position = value.size();
        QVERIFY(field->validator()); QCOMPARE(field->validator()->validate(value, position), QValidator::Invalid);
        value = QString::fromUtf8("Ada \xF0\x9F\x98\x80\xF0\x9F\x98\x80"); position = value.size();
        QCOMPARE(field->validator()->validate(value, position), QValidator::Acceptable);
    }
}

// hgs counts the selected options and an Other answer against the item bounds.
void TestQuestionCard::openCodeOtherAnswerCountsTowardItemBounds()
{
    QJsonObject form{{"question_id", "que_bounds"}, {"question_hash", "bounds-hash"}, {"run_id", "run"}, {"conversation_id", "ses_one"},
        {"kind", "opencode_form"}, {"source", "opencode_native"}, {"answer_transport", "native_opencode"}, {"can_answer", true}, {"answerable", true}};
    form["questions"] = QJsonArray{QJsonObject{{"id", "tags"}, {"header", "Tags"}, {"question", "Pick tags"}, {"multi_select", true},
        {"allow_multiple", true}, {"required", true}, {"allow_other", true}, {"input_kind", "choice"}, {"min_items", 2}, {"max_items", 2},
        {"options", QJsonArray{QJsonObject{{"id", "a"}, {"label", "Alpha"}}, QJsonObject{{"id", "b"}, {"label", "Beta"}}}}}};
    QuestionCard card; card.show(); setQuestionReady(card, "opencode\nsession", form);
    option(card, "a")->click(); QVERIFY(!submit(card)->isEnabled());
    other(card, "tags")->click(); text(card, "tags")->setText("Gamma"); QVERIFY(submit(card)->isEnabled());
    option(card, "b")->click(); QVERIFY(!submit(card)->isEnabled());
    option(card, "a")->click(); QVERIFY(submit(card)->isEnabled());
    option(card, "b")->click(); text(card, "tags")->setText("Gamma"); QVERIFY(!submit(card)->isEnabled());
}

// The contract's permission cases. hgs accepts only OpenCode's own decisions
// (once, always, reject) and refuses the synthesized allow/deny ids.
void TestQuestionCard::openCodePermissionListsNativeChoicesAndConfirmsAlwaysAllow()
{
    QuestionCard card; card.resize(700, 450); card.show(); card.activateWindow(); QApplication::setActiveWindow(&card);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    const auto status = [&card] { return card.findChild<QLabel *>("questionStatus")->text(); };
    auto *approve = card.findChild<QPushButton *>("approveToolRequest"), *deny = card.findChild<QPushButton *>("denyToolRequest");
    const auto refused = openCodeCase("permission-allow-refused");
    QCOMPARE(refused.value("error").toString(), QString("Unknown answer option"));
    // Every option id the card emits during this test, from any control.
    QStringList emitted;
    connect(&card, &QuestionCard::answerRequested, &card, [&emitted](const QString &, const QString &, const QJsonArray &answers) {
        for (const auto &answer : answers) for (const auto &id : answer.toObject().value("selected_option_ids").toArray()) emitted << id.toString();
    });
    for (const QString name : {"permission-once", "permission", "permission-reject"}) {
        const auto contractCase = openCodeCase(name); QVERIFY2(!contractCase.isEmpty(), qPrintable(name));
        const auto permission = openCodeCard(contractCase); const auto expected = contractCase.value("answers").toArray();
        QCOMPARE(permission.value("questions").toArray().size(), contractCase["card"]["question_count"].toInt());
        const auto choice = expected.first()["selected_option_ids"][0].toString();
        card.setReviewDelayForTests(3000); setQuestionReady(card, "opencode\nsession", permission); sent.clear();
        // OpenCode's own choices, listed in its order, with nothing preselected.
        QStringList ids;
        for (const auto &value : permission["questions"][0]["options"].toArray()) {
            const auto item = value.toObject(); auto *button = option(card, item.value("id").toString());
            QVERIFY2(button, qPrintable(item.value("id").toString()));
            QVERIFY2(button->isVisible(), qPrintable(item.value("id").toString() + " is not listed"));
            QCOMPARE(button->accessibleName(), item.value("label").toString()); QVERIFY(!button->isChecked());
            ids << item.value("id").toString();
        }
        QCOMPARE(ids, QStringList({"once", "always", "reject"}));
        // Neither allow, which hgs refuses (permission-allow-refused), nor deny is offered.
        for (const auto &synthesized : {refused["answers"][0]["selected_option_ids"][0].toString(), QString("deny")})
            QVERIFY2(!option(card, synthesized), qPrintable(synthesized));
        QVERIFY(approve->isHidden()); QVERIFY(deny->isHidden());
        QCOMPARE(option(card, "reject")->accessibleDescription(), QString("Rejects every pending request in this session"));
        QStringList descriptions;
        for (auto *label : card.findChildren<QLabel *>("questionChoiceDescription")) if (label->isVisible()) descriptions << label->text();
        QVERIFY2(descriptions.contains("Rejects every pending request in this session"), qPrintable(descriptions.join(" | ")));
        // Always allow shows what it saves, as reported, and that it asks for confirmation.
        QString saves;
        for (const auto &value : permission["questions"][0]["options"].toArray())
            if (value.toObject().value("id") == "always") saves = value.toObject().value("description").toString();
        QVERIFY2(saves.contains("printf *"), qPrintable(saves));
        const QString always = saves + "\nApplies beyond this request. Asks for confirmation.";
        QCOMPARE(option(card, "always")->accessibleDescription(), always);
        QStringList alwaysText;
        for (auto *label : option(card, "always")->parentWidget()->findChildren<QLabel *>()) {
            QCOMPARE(label->textFormat(), Qt::PlainText); if (label->isVisible()) alwaysText << label->text();
        }
        QCOMPARE(alwaysText, QStringList({"Always allow", always}));
        QVERIFY(option(card, "always")->parentWidget()->findChildren<MarkdownText *>().isEmpty());
        QCOMPARE(card.findChild<QLabel *>("questionHeading")->text(), QString("Agent needs your approval"));
        QCOMPARE(card.findChild<QLabel *>("questionPrompt")->text(), QString("shell\nprintf native-tool-output"));
        QCOMPARE(status(), QString("Review the request, then choose one of the agent's options."));
        QVERIFY(!submit(card)->isEnabled());
        // Submit waits for the review countdown.
        option(card, choice)->click(); QVERIFY(!submit(card)->isEnabled()); QVERIFY(submit(card)->text().startsWith("Submit answer ("));
        submit(card)->click(); QCOMPARE(sent.size(), 0);
        card.setReviewDelayForTests(200); QTRY_VERIFY(submit(card)->isEnabled());
        // The hidden synthesized buttons send nothing.
        approve->click(); deny->click(); QCOMPARE(sent.size(), 0);
        if (choice == "always") {
            // Always allow reaches past this request: it needs a second, explicit click.
            submit(card)->click(); QCOMPARE(sent.size(), 0); QCOMPARE(submit(card)->text(), QString("Confirm"));
            QCOMPARE(status(), QString("“Always allow” changes the agent's permissions beyond this request. Select Confirm to send it."));
            const auto preview = qEnvironmentVariable("HGS_APPROVAL_PREVIEW");
            if (!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(card.grab().save(preview + "/opencode-permission.png")); }
        } else QCOMPARE(submit(card)->text(), QString("Submit answer"));
        submit(card)->click(); QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.first()[0].toString(), QString("opencode\nsession")); QCOMPARE(sent.first()[1].toString(), QString("per_1"));
        QCOMPARE(sent.first()[2].toJsonArray(), expected);
    }
    // Only OpenCode's own decisions were ever emitted, never allow or deny.
    QCOMPARE(emitted, QStringList({"once", "always", "reject"}));
    QCOMPARE(QJsonDocument(openCodeCase("permission-once").value("answers").toArray()).toJson(QJsonDocument::Compact),
        QByteArray(R"([{"question_id":"permission","selected_option_ids":["once"],"text":""}])"));
}

// OpenCode form answers go through its form reply API, not a terminal, so they
// may start with / or !. Every other card keeps refusing them.
void TestQuestionCard::openCodeFormTextMayStartWithSlashOrBang()
{
    QuestionCard card; card.resize(700, 450); card.show(); card.activateWindow(); QApplication::setActiveWindow(&card);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    const auto status = [&card] { return card.findChild<QLabel *>("questionStatus")->text(); };
    auto *tabs = card.findChild<QTabBar *>("questionTabs");
    for (const QString name : {"plain-string-slash", "plain-string-bang"}) {
        const auto contractCase = openCodeCase(name); QVERIFY2(!contractCase.isEmpty(), qPrintable(name));
        const auto form = openCodeCard(contractCase); const auto expected = contractCase.value("answers").toArray();
        const auto id = form["questions"][0]["id"].toString(), answer = expected.first()["text"].toString();
        QVERIFY(answer.startsWith('/') || answer.startsWith('!'));
        setQuestionReady(card, "opencode\nsession", form); sent.clear();
        text(card, id)->setText(answer);
        QVERIFY2(submit(card)->isEnabled(), qPrintable(status()));
        QVERIFY2(tabs->tabText(0).startsWith(QStringLiteral("✓ ")), qPrintable(tabs->tabText(0)));
        QCOMPARE(status(), QString("Your choices are ready to send."));
        submit(card)->click(); QCOMPARE(sent.size(), 1); QCOMPARE(sent.first()[2].toJsonArray(), expected);
        // The other limits still apply.
        text(card, id)->setText(answer + QChar(1)); QVERIFY(!submit(card)->isEnabled());
        QCOMPARE(status(), QString("Remove control characters from your custom answer."));
        text(card, id)->setText(answer + QString(4096, QChar(0x044f)).left(4096 - answer.size()));
        QVERIFY(!submit(card)->isEnabled()); QCOMPARE(status(), QString("Keep your custom answer under 4 KiB."));
        // Terminal agents, and an OpenCode approval, keep the existing refusal.
        for (const QString source : {"codex_async", "hook", "opencode_approval"}) {
            auto variant = form; variant["question_hash"] = name + "/" + source;
            if (source == "opencode_approval") { variant["approval"] = true; variant["approval_choices"] = true; }
            else variant["source"] = source;
            setQuestionReady(card, "opencode\nsession", variant); sent.clear();
            text(card, id)->setText(answer); card.setReviewDelayForTests(0); QTest::qWait(150);
            QVERIFY2(!submit(card)->isEnabled(), qPrintable(source));
            QVERIFY2(!tabs->tabText(0).startsWith(QStringLiteral("✓ ")), qPrintable(source));
            QCOMPARE(status(), QString("Use Terminal for answers starting with / or !."));
            submit(card)->click(); QCOMPARE(sent.size(), 0);
        }
    }
    // A choice item that also takes its own answer accepts such text as Other.
    const auto custom = openCodeCase("question-custom-text"); QVERIFY(!custom.isEmpty());
    const auto choiceForm = openCodeCard(custom); const auto choiceId = choiceForm["questions"][0]["id"].toString();
    QVERIFY(choiceForm["questions"][0]["allow_other"].toBool()); QVERIFY(!choiceForm["questions"][0]["options"].toArray().isEmpty());
    for (const QString answer : {"/x", "!x"}) {
        auto variant = choiceForm; variant["question_hash"] = "question-custom-text" + answer;
        setQuestionReady(card, "opencode\nsession", variant); sent.clear();
        other(card, choiceId)->click(); text(card, choiceId)->setText(answer);
        QVERIFY2(submit(card)->isEnabled(), qPrintable(status()));
        QVERIFY2(tabs->tabText(0).startsWith(QStringLiteral("✓ ")), qPrintable(tabs->tabText(0)));
        QCOMPARE(status(), QString("Your choices are ready to send."));
        submit(card)->click(); QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.first()[2].toJsonArray(),
            (QJsonArray{QJsonObject{{"question_id", choiceId}, {"selected_option_ids", QJsonArray{}}, {"text", answer}}}));
        // The same item from a terminal agent keeps the refusal.
        variant["source"] = "hook"; variant["question_hash"] = "question-custom-text/hook" + answer;
        setQuestionReady(card, "opencode\nsession", variant); sent.clear();
        other(card, choiceId)->click(); text(card, choiceId)->setText(answer);
        QVERIFY(!submit(card)->isEnabled()); QCOMPARE(status(), QString("Use Terminal for answers starting with / or !."));
    }
}

void TestQuestionCard::unreadableApprovalPointsToTerminal()
{
    QuestionCard card;card.resize(700,450);card.show();
    auto approval=request("claude-approval:clipped","clipped");approval["approval"]=true;approval["can_answer"]=false;
    approval["answer_unavailable_reason"]="Zerus cannot read this approval completely. Enlarge Terminal or answer it there.";
    approval["questions"]=QJsonArray{QJsonObject{{"id","approval"},{"question","Claude asks to use Read"},{"body","Read\n\n/home/user/notes.txt"},
        {"allow_other",false},{"options",QJsonArray{}}}};
    setQuestionReady(card, "session",approval);
    QVERIFY(card.isVisible());QVERIFY(submit(card)->isHidden());
    QVERIFY(card.findChild<QPushButton *>("approveToolRequest")->isHidden());QVERIFY(card.findChild<QPushButton *>("denyToolRequest")->isHidden());
    QVERIFY(card.findChild<QPushButton *>("questionOpenTerminal")->isVisible());
    QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(),approval["answer_unavailable_reason"].toString());
}

namespace {
QJsonObject planReview()
{
    return {{"question_id", "approval_plan"}, {"question_hash", "plan-hash"}, {"run_id", "run"}, {"conversation_id", "conversation"},
        {"source", "kimi_plan_review"}, {"plan_review", true}, {"can_answer", true}, {"questions", QJsonArray{
            QJsonObject{{"id", "plan"}, {"header", "Plan"}, {"question", "Ready to build with this plan?"},
                {"body", "# Rotate fixture keys\n\n1. **Add** the `rotate.py` script.\n2. Verify the rotation."},
                {"allow_other", true}, {"other_label", "Revise"}, {"other_description", "Send feedback and keep planning"},
                {"options", QJsonArray{QJsonObject{{"id", "approve"}, {"label", "Approve"}, {"description", ""}},
                    QJsonObject{{"id", "reject"}, {"label", "Reject"}, {"description", ""}}}}}}}};
}
}

void TestQuestionCard::kimiPlanReviewShowsPlanAndNamedRevise()
{
    QuestionCard card; card.resize(620, 480); card.show(); setQuestionReady(card, "session", planReview());
    QCOMPARE(card.findChild<QLabel *>("questionHeading")->text(), QString("Agent needs your approval"));
    // The agent's plan is its Markdown, not a literal disclosure.
    auto *body = card.findChild<MarkdownText *>("questionBody"); QVERIFY(body);
    QVERIFY(body->toPlainText().contains("Rotate fixture keys")); QVERIFY(!body->toPlainText().contains("**"));
    QVERIFY(card.findChild<QPushButton *>("approveToolRequest")->isHidden());
    auto *revise = other(card, "plan"); QVERIFY(revise);
    QStringList titles; for (auto *title : card.findChildren<QLabel *>("questionChoiceTitle")) titles << title->text();
    QCOMPARE(titles, QStringList({"Approve", "Reject", "Revise"}));
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    option(card, "approve")->click(); QVERIFY(submit(card)->isEnabled()); submit(card)->click();
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent[0][2].toJsonArray()[0].toObject()["selected_option_ids"].toArray(), QJsonArray{"approve"});
    QuestionCard feedback; feedback.show(); setQuestionReady(feedback, "other-session", planReview());
    QSignalSpy revised(&feedback, &QuestionCard::answerRequested);
    other(feedback, "plan")->click(); QVERIFY(!submit(feedback)->isEnabled());
    text(feedback, "plan")->setText("Add a rollback step"); submit(feedback)->click();
    QCOMPARE(revised.size(), 1);
    const auto answer = revised[0][2].toJsonArray()[0].toObject();
    QVERIFY(answer["selected_option_ids"].toArray().isEmpty()); QCOMPARE(answer["text"].toString(), QString("Add a rollback step"));
}

void TestQuestionCard::allRequiredQuestionsAndStableAnswerIds()
{
    QuestionCard card; setQuestionReady(card, "arch/session", request());
    QSignalSpy submitted(&card, &QuestionCard::answerRequested);
    QVERIFY(!submit(card)->isEnabled());
    option(card, "opt_0_1")->click(); QVERIFY(!submit(card)->isEnabled());
    QVERIFY(card.findChild<QTabBar *>("questionTabs")->tabText(0).startsWith(QStringLiteral("✓")));
    QCOMPARE(card.findChild<QLabel *>("questionProgress")->text(), "1 of 2 answered");
    card.findChild<QTabBar *>("questionTabs")->setCurrentIndex(1);
    option(card, "opt_1_1")->click(); QVERIFY(submit(card)->isEnabled());
    submit(card)->click(); QCOMPARE(submitted.size(), 1);
    QCOMPARE(submitted[0][0].toString(), "arch/session"); QCOMPARE(submitted[0][1].toString(), "tool-question");
    const auto answers = submitted[0][2].toJsonArray(); QCOMPARE(answers.size(), 2);
    QCOMPARE(answers[0].toObject()["question_id"].toString(), "q_0");
    QCOMPARE(answers[0].toObject()["selected_option_ids"].toArray(), QJsonArray{"opt_0_1"});
    QCOMPARE(answers[1].toObject()["selected_option_ids"].toArray(), QJsonArray{"opt_1_1"});
    QVERIFY(answers[0].toObject()["text"].toString().isEmpty());
}

void TestQuestionCard::customAndMultipleAnswers()
{
    QuestionCard card; setQuestionReady(card, "arch/session", request());
    QSignalSpy submitted(&card, &QuestionCard::answerRequested);
    complete(card); other(card, "q_0")->click();
    QVERIFY(!option(card, "opt_0_0")->isChecked()); QVERIFY(!submit(card)->isEnabled());
    text(card, "q_0")->setText("  My approach  "); QVERIFY(submit(card)->isEnabled());
    option(card, "opt_1_1")->click(); other(card, "q_1")->click(); QVERIFY(!submit(card)->isEnabled());
    text(card, "q_1")->setText("Also check keyboard navigation");
    submit(card)->click(); QCOMPARE(submitted.size(), 1);
    const auto answers = submitted[0][2].toJsonArray();
    QVERIFY(answers[0].toObject()["selected_option_ids"].toArray().isEmpty());
    QCOMPARE(answers[0].toObject()["text"].toString(), "My approach");
    QCOMPARE(answers[1].toObject()["selected_option_ids"].toArray(), QJsonArray({"opt_1_0", "opt_1_1"}));
    QCOMPARE(answers[1].toObject()["text"].toString(), "Also check keyboard navigation");
}

void TestQuestionCard::enterSubmitsFreeformAnswers_data()
{
    QTest::addColumn<int>("key"); QTest::addColumn<bool>("hasOptions");
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter})
        for (const bool hasOptions : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(key).arg(hasOptions))) << int(key) << hasOptions;
}

void TestQuestionCard::enterSubmitsFreeformAnswers()
{
    QFETCH(int, key); QFETCH(bool, hasOptions);
    QuestionCard card; auto data = request(); data["optional"] = true; data["can_skip"] = true;
    auto question = data["questions"].toArray().first().toObject();
    if (!hasOptions) question["options"] = QJsonArray{};
    data["questions"] = QJsonArray{question}; setQuestionReady(card, "fixture/session", data); card.show();
    if (hasOptions) other(card, "q_0")->click();
    auto *editor = text(card, "q_0"); editor->setText("  Keyboard answer  "); editor->setFocus();
    QVERIFY(submit(card)->isVisible()); QVERIFY(submit(card)->isEnabled());
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    connect(&card, &QuestionCard::answerRequested, &card, [&card](const QString &session, const QString &id) {
        setSendingReady(card, session, id);
    });
    QTest::keyClick(editor, Qt::Key(key)); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[0].toString(), QString("fixture/session"));
    QCOMPARE(sent.first()[1].toString(), QString("tool-question"));
    QCOMPARE(sent.first()[2].toJsonArray(), QJsonArray{QJsonObject({{"question_id", "q_0"},
        {"selected_option_ids", QJsonArray{}}, {"text", "Keyboard answer"}})});
    QTest::keyClick(editor, Qt::Key(key)); QCOMPARE(sent.size(), 1);
    card.setSubmitted("fixture/session", "tool-question");
    QTest::keyClick(editor, Qt::Key(key)); QCOMPARE(sent.size(), 1);
}

void TestQuestionCard::enterSubmitsSelectedChoices_data()
{
    QTest::addColumn<int>("key"); QTest::addColumn<QJsonObject>("data"); QTest::addColumn<QStringList>("selected");
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        for (const auto &source : {QString("codex_folder_trust"), QString("kimi_folder_trust"),
                                  QString("codex_hooks_trust"), QString("hook"), QString("multiple")}) {
            auto data = request(); auto question = data["questions"].toArray().first().toObject();
            data["source"] = source == "multiple" ? "hook" : source;
            question["allow_other"] = false;
            QStringList selected{"opt_0_1"};
            if (source.endsWith("folder_trust")) {
                question["id"] = "trust";
                question["options"] = QJsonArray{QJsonObject{{"id", "trust_0"}, {"label", "Trust and continue"}},
                    QJsonObject{{"id", "trust_1"}, {"label", "Quit"}}};
                selected = {"trust_0"};
            } else if (source == "codex_hooks_trust") {
                data["trust_request"] = true; question["id"] = "hooks_trust";
                question["options"] = QJsonArray{QJsonObject{{"id", "review"}, {"label", "Review hooks"}},
                    QJsonObject{{"id", "trust"}, {"label", "Trust all and continue"}}};
                selected = {"trust"};
            } else if (source == "multiple") {
                question["multi_select"] = true; selected = {"opt_0_0", "opt_0_1"};
            }
            data["questions"] = QJsonArray{question};
            QTest::newRow(qPrintable(QString("%1-%2").arg(source).arg(key))) << int(key) << data << selected;
        }
    }
}

void TestQuestionCard::enterSubmitsSelectedChoices()
{
    QFETCH(int, key); QFETCH(QJsonObject, data); QFETCH(QStringList, selected);
    QuestionCard card; setQuestionReady(card, "fixture/session", data); card.resize(700, 450); card.show();
    card.activateWindow(); QApplication::setActiveWindow(&card);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    connect(&card, &QuestionCard::answerRequested, &card, [&card](const QString &session, const QString &id) {
        setSendingReady(card, session, id);
    });
    QJsonArray expected;
    for (const auto &id : selected) {
        auto *choice = option(card, id); QVERIFY(choice);
        // A click on the row leaves keyboard focus on its radio/checkbox.
        QTest::mouseClick(choice->parentWidget(), Qt::LeftButton);
        QVERIFY(choice->isChecked()); QTRY_VERIFY(choice->hasFocus()); expected.append(id);
    }
    QVERIFY(submit(card)->isVisible()); QVERIFY(submit(card)->isEnabled()); QCOMPARE(sent.size(), 0);
    auto *focused = option(card, selected.last());
    QTest::keyClick(focused, Qt::Key(key)); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[0].toString(), QString("fixture/session"));
    QCOMPARE(sent.first()[1].toString(), data["question_id"].toString());
    QCOMPARE(sent.first()[2].toJsonArray(), QJsonArray{QJsonObject({
        {"question_id", data["questions"].toArray().first().toObject()["id"]},
        {"selected_option_ids", expected}, {"text", ""}})});
    QTest::keyClick(focused, Qt::Key(key)); QCOMPARE(sent.size(), 1);
    card.setSubmitted("fixture/session", data["question_id"].toString());
    QTest::keyClick(focused, Qt::Key(key)); QCOMPARE(sent.size(), 1);
}

void TestQuestionCard::enterFromChoicesRespectsGuardsAndFocus()
{
    QWidget window; auto *layout = new QVBoxLayout(&window);
    auto *outside = new QLineEdit; auto *card = new QuestionCard;
    layout->addWidget(outside); layout->addWidget(card);
    auto data = request(); data["questions"] = QJsonArray{data["questions"].toArray().first()};
    setQuestionReady(*card, "fixture/session", data); window.resize(700, 500); window.show();
    window.activateWindow(); QApplication::setActiveWindow(&window);
    QSignalSpy sent(card, &QuestionCard::answerRequested);
    auto press = [&] { auto *choice = option(*card, "opt_0_1"); choice->setFocus(); QTest::keyClick(choice, Qt::Key_Return); };
    press(); QCOMPARE(sent.size(), 0); // Nothing selected.
    option(*card, "opt_0_1")->click();
    outside->setFocus(); QTest::keyClick(outside, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    data["can_answer"] = false; setQuestionReady(*card, "fixture/session", data);
    press(); QCOMPARE(sent.size(), 0);
    data["can_answer"] = true; setQuestionReady(*card, "fixture/session", data);
    card->setAvailability(false, "Offline"); press(); QCOMPARE(sent.size(), 0);
    card->setAvailability(true); card->setError("fixture/session", "tool-question", "Check delivery", true);
    press(); QCOMPARE(sent.size(), 0);
    card->findChild<QPushButton *>("questionAllowRetry")->click();
    auto *choice = option(*card, "opt_0_1");
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, {}, true);
    QApplication::sendEvent(choice, &repeat); QCOMPARE(sent.size(), 0);
    QTest::keyClick(choice, Qt::Key_Return, Qt::ShiftModifier); QCOMPARE(sent.size(), 0);
    // Selecting Other keeps focus there; its already typed text is also an answer.
    auto *custom = other(*card, "q_0"); custom->click(); custom->setFocus();
    QTest::keyClick(custom, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    text(*card, "q_0")->setText("Keyboard answer");
    QTest::keyClick(custom, Qt::Key_Return); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[2].toJsonArray(), QJsonArray{QJsonObject({{"question_id", "q_0"},
        {"selected_option_ids", QJsonArray{}}, {"text", "Keyboard answer"}})});
}

void TestQuestionCard::enterFromChoicesKeepsNavigationAndApproval()
{
    QuestionCard card; setQuestionReady(card, "fixture/session", request()); card.resize(700, 500); card.show();
    card.activateWindow(); QApplication::setActiveWindow(&card); complete(card);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    QVERIFY(submit(card)->isEnabled()); QVERIFY(submit(card)->isHidden());
    QTest::keyClick(option(card, "opt_0_0"), Qt::Key_Return); QCOMPARE(sent.size(), 0);
    card.findChild<QTabBar *>("questionTabs")->setCurrentIndex(1);
    QTest::keyClick(option(card, "opt_1_0"), Qt::Key_Enter); QCOMPARE(sent.size(), 1);

    auto data = request("approval", "keyboard-review"); data["approval"] = true;
    data["approval_choices"] = true; data["source"] = "claude_tool_approval";
    data["questions"] = QJsonArray{QJsonObject{{"id", "approval"}, {"question", "Allow this request?"},
        {"options", QJsonArray{QJsonObject{{"id", "allow"}, {"label", "Allow for this session"}, {"scope", "broader"}}}}}};
    setQuestionReady(card, "fixture/session", data); auto *choice = option(card, "allow");
    choice->click(); choice->setFocus(); QVERIFY(!submit(card)->isEnabled());
    QTest::keyClick(choice, Qt::Key_Return); QCOMPARE(sent.size(), 1);
    card.setReviewDelayForTests(150); QTRY_VERIFY(submit(card)->isEnabled());
    QTest::keyClick(choice, Qt::Key_Return); QCOMPARE(sent.size(), 1); QCOMPARE(submit(card)->text(), QString("Confirm"));
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, {}, true);
    QApplication::sendEvent(choice, &repeat); QCOMPARE(sent.size(), 1);
    QTest::keyClick(choice, Qt::Key_Return); QCOMPARE(sent.size(), 2);
    QCOMPARE(sent.last()[2].toJsonArray(), QJsonArray{QJsonObject({{"question_id", "approval"},
        {"selected_option_ids", QJsonArray{"allow"}}, {"text", ""}})});
    data["question_hash"] = "read-only"; data["can_answer"] = false;
    setQuestionReady(card, "fixture/session", data);
    QVERIFY(!option(card, "allow")->isEnabled());
    QTest::keyClick(option(card, "allow"), Qt::Key_Return); QCOMPARE(sent.size(), 2);
}

void TestQuestionCard::enterRespectsSubmissionGuards()
{
    QuestionCard card; auto data = request();
    data["questions"] = QJsonArray{QJsonObject{{"id", "free"}, {"question", "Your answer?"}, {"allow_other", true}}};
    setQuestionReady(card, "fixture/session", data); card.show();
    auto *editor = text(card, "free"); editor->setFocus();
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    editor->setText("/invalid-answer"); QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    editor->setText("Valid answer");
    data["can_answer"] = false; setQuestionReady(card, "fixture/session", data);
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    data["can_answer"] = true; setQuestionReady(card, "fixture/session", data);
    card.setAvailability(false, "Offline");
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    card.setAvailability(true); card.setError("fixture/session", "tool-question", "Check delivery", true);
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 0);
    card.findChild<QPushButton *>("questionAllowRetry")->click();
    QTest::keyClick(editor, Qt::Key_Return); QCOMPARE(sent.size(), 1);
}

void TestQuestionCard::enterCannotBypassQuestionNavigationOrApproval()
{
    QuestionCard card; auto data = request();
    data["questions"] = QJsonArray{
        QJsonObject{{"id", "first"}, {"question", "First answer?"}, {"allow_other", true}},
        QJsonObject{{"id", "last"}, {"question", "Last answer?"}, {"allow_other", true}}};
    setQuestionReady(card, "fixture/session", data); card.show();
    text(card, "first")->setText("First"); text(card, "last")->setText("Last");
    QVERIFY(submit(card)->isEnabled()); QVERIFY(submit(card)->isHidden());
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    QTest::keyClick(text(card, "first"), Qt::Key_Return); QCOMPARE(sent.size(), 0);
    card.findChild<QTabBar *>("questionTabs")->setCurrentIndex(1);
    QTest::keyClick(text(card, "last"), Qt::Key_Return); QCOMPARE(sent.size(), 1);
    data["approval"] = true; data["questions"] = QJsonArray{data["questions"].toArray().first()};
    setQuestionReady(card, "fixture/session", data);
    QVERIFY(submit(card)->isHidden());
    QTest::keyClick(text(card, "first"), Qt::Key_Return); QCOMPARE(sent.size(), 1);
}

void TestQuestionCard::draftsSurviveRefreshAndSessionSwitch()
{
    QuestionCard card; setQuestionReady(card, "arch/session", request()); complete(card);
    other(card, "q_0")->click(); text(card, "q_0")->setText("Remember this");
    card.findChild<QTabBar *>("questionTabs")->setCurrentIndex(1);
    auto *editor = text(card, "q_0"); editor->setSelection(0, 8);
    setQuestionReady(card, "arch/session", request());
    QCOMPARE(text(card, "q_0"), editor); QCOMPARE(editor->selectedText(), "Remember");
    setQuestionReady(card, "mac/session", request()); QVERIFY(!submit(card)->isEnabled());
    QVERIFY(text(card, "q_0")->text().isEmpty());
    setQuestionReady(card, "arch/session", request());
    QCOMPARE(text(card, "q_0")->text(), "Remember this"); QVERIFY(submit(card)->isEnabled());
    QCOMPARE(card.findChild<QTabBar *>("questionTabs")->currentIndex(), 1);
    setQuestionReady(card, {}, {}); QVERIFY(card.isHidden());
    setQuestionReady(card, "arch/session", request()); QVERIFY(submit(card)->isEnabled());
}

void TestQuestionCard::claudeCapabilityRefreshPreservesCompletedOtherDraft()
{
    QWidget activity; activity.resize(700, 800);
    auto *layout = new QVBoxLayout(&activity);
    layout->addWidget(new QTextBrowser, 1);
    auto *card = new QuestionCard; layout->addWidget(card);
    auto data = request(); data["source"] = "hook"; data["can_answer"] = false;
    data["answer_unavailable_reason"] = "Finish the existing Other draft in Terminal.";
    auto questions = data["questions"].toArray(); auto last = questions[1].toObject();
    last["question"] = QString("Which checks should run? Review the synthetic fixture details.\n").repeated(80);
    questions[1] = last; data["questions"] = questions;
    setQuestionReady(*card, "fixture/claude", data); activity.show();
    option(*card, "opt_0_1")->click();
    auto *tabs = card->findChild<QTabBar *>("questionTabs"); tabs->setCurrentIndex(1);
    other(*card, "q_1")->click();
    auto *editor = text(*card, "q_1");
    const QString draft = "Keep this completed Other answer exactly, including Unicode: Привет.";
    editor->setText(draft); editor->setFocus(); editor->setSelection(5, 14);
    const int cursor = editor->cursorPosition();
    const QString selection = editor->selectedText();
    auto *pages = card->findChild<QStackedWidget *>("questionPages");
    auto *page = pages->currentWidget();
    auto *scroll = page->findChild<QScrollArea *>("questionContent")->verticalScrollBar();
    QTRY_VERIFY(scroll->maximum() > 100);
    scroll->setValue(50); QCOMPARE(scroll->value(), 50);
    QVERIFY(submit(*card)->isVisible()); QVERIFY(!submit(*card)->isEnabled());
    QCOMPARE(card->findChild<QLabel *>("questionProgress")->text(), QString("2 of 2 answered"));
    QSignalSpy sent(card, &QuestionCard::answerRequested);

    data["can_answer"] = true; data.remove("answer_unavailable_reason");
    setQuestionReady(*card, "fixture/claude", data);
    QTRY_VERIFY(submit(*card)->isEnabled());
    QCOMPARE(text(*card, "q_1"), editor); QCOMPARE(editor->text(), draft);
    QCOMPARE(editor->cursorPosition(), cursor); QCOMPARE(editor->selectedText(), selection);
    QCOMPARE(tabs->currentIndex(), 1); QCOMPARE(pages->currentWidget(), page);
    QCOMPARE(scroll->value(), 50); QVERIFY(other(*card, "q_1")->isChecked());
    QVERIFY(option(*card, "opt_0_1")->isChecked()); QCOMPARE(sent.size(), 0);
    // A later poll still cannot submit; only the user's explicit action does.
    setQuestionReady(*card, "fixture/claude", data); QCOMPARE(sent.size(), 0);
    submit(*card)->click(); QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first()[2].toJsonArray(), QJsonArray({
        QJsonObject{{"question_id", "q_0"}, {"selected_option_ids", QJsonArray{"opt_0_1"}}, {"text", ""}},
        QJsonObject{{"question_id", "q_1"}, {"selected_option_ids", QJsonArray{}}, {"text", draft}}}));
}

void TestQuestionCard::replacementHashAndBackgroundReplyAreIsolated()
{
    QuestionCard card; setQuestionReady(card, "arch/session", request()); complete(card);
    setSendingReady(card, "arch/session", "tool-question");
    QVERIFY(!submit(card)->isEnabled()); QVERIFY(!option(card, "opt_0_0")->isEnabled());
    setQuestionReady(card, "mac/session", request()); complete(card);
    card.setAnswered("arch/session", "tool-question"); QVERIFY(submit(card)->isEnabled());
    setQuestionReady(card, "arch/session", request()); QVERIFY(!submit(card)->isEnabled());
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("Answer sent"));
    setQuestionReady(card, "arch/session", request("tool-question", "hash-two"));
    QVERIFY(!option(card, "opt_0_0")->isChecked()); complete(card);
    setSendingReady(card, "arch/session", "tool-question");
    setQuestionReady(card, "arch/session", request("tool-question", "hash-three"));
    // Keep a replacement request locked until the old hash's callback arrives.
    QVERIFY(!option(card, "opt_0_0")->isEnabled());
    card.setAnswered("arch/session", "tool-question");
    QVERIFY(option(card, "opt_0_0")->isEnabled()); complete(card); QVERIFY(submit(card)->isEnabled());
}

void TestQuestionCard::capabilityOfflineAndErrorStates()
{
    QuestionCard card; auto unsupported = request(); unsupported["can_answer"] = false;
    unsupported["answer_unavailable_reason"] = "This agent requires Terminal.";
    setQuestionReady(card, "arch/session", unsupported); complete(card);
    QVERIFY(!submit(card)->isEnabled()); QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), "This agent requires Terminal.");
    QSignalSpy terminal(&card, &QuestionCard::openTerminalRequested);
    card.findChild<QPushButton *>("questionOpenTerminal")->click(); QCOMPARE(terminal.size(), 1);
    setQuestionReady(card, "arch/session", request()); QVERIFY(submit(card)->isEnabled());
    card.setAvailability(false, "Machine is offline."); QVERIFY(!submit(card)->isEnabled());
    QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), "Machine is offline.");
    card.setAvailability(true);
    setSendingReady(card, "arch/session", "tool-question");
    card.setError("arch/session", "tool-question", "Cannot confirm delivery", true);
    QVERIFY(!submit(card)->isEnabled()); QVERIFY(option(card, "opt_0_0")->isChecked());
    QVERIFY(card.findChild<QPushButton *>("questionAllowRetry")->isVisible());
    card.findChild<QPushButton *>("questionAllowRetry")->click(); QVERIFY(submit(card)->isEnabled());
    setSendingReady(card, "arch/session", "tool-question"); card.setError("arch/session", "tool-question", "Request changed");
    QVERIFY(submit(card)->isEnabled()); QCOMPARE(card.findChild<QLabel *>("questionStatus")->text(), "Request changed");
}

void TestQuestionCard::disabledSubmitAlwaysExplainsItself()
{
    auto status = [](QuestionCard &card) { return card.findChild<QLabel *>("questionStatus")->text(); };
    QuestionCard card; auto unsupported = request(); unsupported["can_answer"] = false;
    unsupported["answer_unavailable_reason"] = "";
    setQuestionReady(card, "arch/reason", unsupported); complete(card);
    QVERIFY(!submit(card)->isEnabled()); QCOMPARE(status(card), "Answer this request in Terminal.");
    // A saved answer still loading holds Submit and says so.
    setQuestionReady(card, "arch/stall", request());
    QSemaphore entered, release; std::atomic<int> operations = 0;
    ComposerDraftStore::setWriteObserver([&] { if (++operations == 1) { entered.release(); release.acquire(); } });
    auto cleanup = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    option(card, "opt_0_0")->click(); QVERIFY(entered.tryAcquire(1, 3000));
    card.setQuestion("arch/loading", request("loading-question")); QVERIFY(card.isDraftLoading());
    QVERIFY(!submit(card)->isEnabled());
    QCOMPARE(status(card), "Loading your saved answer…");
    release.release(); QTRY_VERIFY(!card.isDraftLoading());
}

void TestQuestionCard::freeformLimitsAndPlainText()
{
    QuestionCard card; auto data = request();
    data["questions"] = QJsonArray{QJsonObject{{"id", "free"}, {"question", "<img src='file:///private'>"}, {"allow_other", true}, {"options", QJsonArray{}}}};
    setQuestionReady(card, "arch/session", data);
    // Raw HTML in the agent's Markdown stays text: nothing is loaded or drawn.
    const auto *prompt = card.findChild<MarkdownText *>("questionPrompt"); QVERIFY(prompt);
    QCOMPARE(prompt->toPlainText(), QString("<img src='file:///private'>"));
    for (auto block = prompt->document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) QVERIFY(!it.fragment().charFormat().isImageFormat());
    QVERIFY(!submit(card)->isEnabled());
    text(card, "free")->setText(QString(2048, QChar(0x044f))); QVERIFY(submit(card)->isEnabled());
    text(card, "free")->setText(QString(2049, QChar(0x044f))); QVERIFY(!submit(card)->isEnabled());
    text(card, "free")->setText("one\ntwo"); QVERIFY(!submit(card)->isEnabled());
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("one line"));
    text(card, "free")->setText(" /exit"); QVERIFY(!submit(card)->isEnabled());
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("Terminal"));
    text(card, "free")->setText(QStringLiteral("before") + QChar(1) + "after"); QVERIFY(!submit(card)->isEnabled());
    QVERIFY(card.findChild<QLabel *>("questionStatus")->text().contains("control characters"));
    text(card, "free")->setText("single line"); QVERIFY(submit(card)->isEnabled());
}

void TestQuestionCard::markdownLinksOpenOnlyOnClick()
{
    UrlRecorder browser;
    const QString token = "https://github.com/settings/personal-access-tokens/new?name=Zerus+mirror&target_name=example";
    auto data = request(); data["optional"] = true; data["can_skip"] = true;
    data["questions"] = QJsonArray{QJsonObject{{"id", "token"}, {"header", "Token"},
        {"question", "Open [token settings](" + token + ") and run `python3 save-token.py`. Never paste the **token** here."},
        {"body", "See [the notes](docs/token.md:12), [mail](mailto:owner@example.com) and [script](javascript:alert(1)).\n\n"
                 "1. Choose **Only select repositories**\n2. Generate the token\n\n```sh\npython3 save-token.py\n```"},
        {"allow_other", true}, {"options", QJsonArray{
            QJsonObject{{"id", "saved"}, {"label", "Saved"}, {"description", "Stored with `save-token.py`, see [docs](https://example.com/docs)."}},
            QJsonObject{{"id", "later"}, {"label", "Later"}}}}}};
    QuestionCard card; card.resize(640, 520); card.setQuestion("arch/session", data); card.show();
    QVERIFY(QTest::qWaitForWindowExposed(&card));
    QSignalSpy external(&card, &QuestionCard::externalLinkActivated), files(&card, &QuestionCard::fileReferenceActivated);
    QSignalSpy sent(&card, &QuestionCard::answerRequested);
    auto *prompt = card.findChild<MarkdownText *>("questionPrompt"); QVERIFY(prompt);
    QCOMPARE(prompt->toPlainText(), QString("Open token settings and run python3 save-token.py. Never paste the token here."));
    QCOMPARE(anchors(prompt), QStringList{token});
    QVERIFY(MarkdownObjects::isInlineCode(formatOf(prompt, "save-token")));
    QVERIFY(formatOf(prompt, "token here").font().bold() || formatOf(prompt, "token here").fontWeight() >= QFont::DemiBold);
    // Text outside links stays selectable and opens nothing.
    QVERIFY(clickText(prompt, "Never paste")); QCOMPARE(external.size(), 0);
    QVERIFY(clickText(prompt, "token settings"));
    QCOMPARE(external.size(), 1); QCOMPARE(browser.urls, QList<QUrl>{QUrl(token)});
    // Only web links and file references become links; other schemes stay text.
    auto *body = card.findChild<MarkdownText *>("questionBody"); QVERIFY(body);
    QCOMPARE(anchors(body).size(), 1); QVERIFY(anchors(body).first().startsWith("hgs-file:"));
    QVERIFY(body->toPlainText().contains("mail and script")); QVERIFY(body->toPlainText().contains("Only select repositories"));
    // Lists and code blocks are laid out in full: the view is exactly as tall as its text.
    for (auto *view : card.findChildren<MarkdownText *>())
        QCOMPARE(view->height(), qCeil(view->document()->size().height()));
    QVERIFY(clickText(body, "the notes"));
    QCOMPARE(files.size(), 1); QCOMPARE(files.first().first().toString(), QString("docs/token.md:12"));
    QCOMPARE(external.size(), 1);
    // A link in an option's description opens without choosing the option;
    // the rest of the description selects it like the row around it.
    auto *description = card.findChild<MarkdownText *>("questionChoiceDescription"); QVERIFY(description);
    QCOMPARE(description->viewport()->palette().color(QPalette::Base), QColor("#192228"));
    QVERIFY(clickText(description, "docs"));
    QCOMPARE(browser.urls.size(), 2); QCOMPARE(browser.urls.last(), QUrl("https://example.com/docs"));
    QVERIFY(!option(card, "saved")->isChecked());
    QVERIFY(clickText(description, "Stored with"));
    QVERIFY(option(card, "saved")->isChecked());
    // Inline code chips are drawn on the row's own background.
    QCOMPARE(description->viewport()->palette().color(QPalette::Base), QColor("#293d35"));
    QCOMPARE(sent.size(), 0);
    // Theme changes redraw the Markdown in place.
    card.setTheme(false);
    QCOMPARE(card.findChild<MarkdownText *>("questionPrompt"), prompt); QCOMPARE(anchors(prompt), QStringList{token});
    QCOMPARE(formatOf(prompt, "Never").foreground().color(), QColor("#1a2733"));
    QCOMPARE(description->viewport()->palette().color(QPalette::Base), QColor("#e4f3ec"));
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (!directory.isEmpty()) {
        QTest::qWait(20); QVERIFY(card.grab().save(directory + "/question-markdown-light.png"));
        card.setTheme(true); QTest::qWait(20); QVERIFY(card.grab().save(directory + "/question-markdown-dark.png"));
    }
}

void TestQuestionCard::approvalAndTrustDisclosuresStayLiteral()
{
    auto trust = request(); trust["trust_request"] = true;
    const QString prompt = "Trust **/work/[example](https://example.com)**?", body = "Folder `/work/example`\n\n- [x] tools";
    trust["questions"] = QJsonArray{QJsonObject{{"id", "trust"}, {"question", prompt}, {"body", body}, {"allow_other", false},
        {"options", QJsonArray{QJsonObject{{"id", "yes"}, {"label", "Yes"}, {"description", "Trust [it](https://example.com)"}}}}}};
    auto approval = trust; approval.remove("trust_request"); approval["approval"] = true; approval["approval_choices"] = true;
    // A folder trust panel read from Claude's terminal carries no trust flag.
    auto panel = trust; panel.remove("trust_request"); panel["source"] = "claude_folder_trust";
    for (const auto &data : {trust, approval, panel}) {
        QuestionCard card; card.setQuestion("arch/session", data);
        QVERIFY(!card.findChild<MarkdownText *>());
        QCOMPARE(card.findChild<QLabel *>("questionPrompt")->text(), prompt);
        QCOMPARE(card.findChild<QLabel *>("questionBody")->text(), body);
        QCOMPARE(card.findChild<QLabel *>("questionChoiceDescription")->text(), QString("Trust [it](https://example.com)"));
    }
}

void TestQuestionCard::longProviderTextKeepsCompactWidth()
{
    auto data = request();
    const QString token(8000, QChar('x'));
    data["questions"] = QJsonArray{QJsonObject{{"id", "long"}, {"question", token}, {"body", "body" + token},
        {"options", QJsonArray{QJsonObject{{"id", "long-option"}, {"label", token}, {"description", token}}}}}};
    QuestionCard card; card.resize(480, 360); setQuestionReady(card, "arch/session", data); card.show();
    QTest::qWait(25); QCOMPARE(card.width(), 480);
    const auto *scroll = card.findChild<QScrollArea *>(); QVERIFY(scroll);
    QVERIFY(scroll->widget()->width() <= scroll->viewport()->width());
    for (const auto *label : card.findChildren<QLabel *>())
        if (label->isVisible()) QVERIFY(label->width() < card.width());
    for (const auto *view : card.findChildren<MarkdownText *>())
        if (view->isVisible()) QVERIFY(view->width() < card.width());
    card.setError("arch/session", "tool-question", token); QTest::qWait(25);
    QCOMPARE(card.width(), 480);
    QVERIFY(card.findChild<QLabel *>("questionStatus")->width() < card.width());
}

void TestQuestionCard::constrainedHeightDoesNotOverlapQuestionActions_data()
{
    QTest::addColumn<double>("scale");
    QTest::newRow("normal") << 1.0;
    QTest::newRow("large") << 1.5;
    QTest::newRow("largest") << 2.0;
}

void TestQuestionCard::constrainedHeightDoesNotOverlapQuestionActions()
{
    QFETCH(double, scale);
    QWidget activity; activity.setFixedSize(720, 550);
    auto *layout = new QVBoxLayout(&activity); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    // Queued input and the composer reserve real space outside the question card.
    auto *history = new QTextBrowser; history->setMinimumHeight(150);
    history->setPlainText(QString("Earlier reply and queued input preview\n").repeated(20));
    layout->addWidget(history, 1);
    auto *column = new ActivityWidth::ColumnLayout; column->setColumnWidth(600); layout->addLayout(column);
    auto *card = new QuestionCard; column->addWidget(card);
    auto *composer = new QLineEdit("Unsent message stays here"); composer->setFixedHeight(160); column->addWidget(composer);
    auto data = request(); data["optional"] = true; data["can_skip"] = true;
    auto question = data["questions"].toArray().first().toObject();
    question["question"] = "Choose how to organize the next step. The explanation wraps across lines when the question uses larger text.";
    data["questions"] = QJsonArray{question};
    card->setContentScale(scale); setQuestionReady(*card, "constrained-fixture", data); activity.show();
    auto *pages = card->findChild<QStackedWidget *>("questionPages");
    auto *status = card->findChild<QLabel *>("questionStatus");
    const auto inCard = [card](QWidget *widget) { return QRect(widget->mapTo(card, QPoint()), widget->size()); };
    QTRY_VERIFY(card->isVisible());
    const auto preview = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (!preview.isEmpty()) { QDir().mkpath(preview); QTest::qWait(50); QVERIFY(activity.grab().save(preview + QString("/question-constrained-%1.png").arg(scale))); }
    QTRY_VERIFY2(inCard(pages).bottom() < inCard(status).top(), "Question body overlaps status text");
    QVERIFY2(inCard(status).bottom() < inCard(submit(*card)).top(), "Question status overlaps action buttons");
    QVERIFY(card->rect().contains(inCard(submit(*card))));
    QVERIFY(card->geometry().bottom() < composer->geometry().top());
    auto *scroll = pages->currentWidget()->findChild<QScrollArea *>("questionContent");
    QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
    option(*card, "opt_0_0")->click();
    QVERIFY(submit(*card)->isEnabled());
    QCOMPARE(composer->text(), QString("Unsent message stays here"));
    QSignalSpy answer(card, &QuestionCard::answerRequested);
    QCOMPARE(answer.size(), 0);
    // The last choice remains reachable inside the scrolling body, above pinned actions.
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QVERIFY(inCard(pages).bottom() < inCard(status).top());
    QVERIFY(card->rect().contains(inCard(submit(*card))));
    const int shortHeight = card->height();
    activity.setFixedSize(820, 720);
    QTRY_VERIFY(card->height() > shortHeight);
    activity.setFixedSize(720, 550);
    QTRY_COMPARE(card->height(), shortHeight);
    QVERIFY(option(*card, "opt_0_0")->isChecked());
    QVERIFY(inCard(pages).bottom() < inCard(status).top());
    QVERIFY(inCard(status).bottom() < inCard(submit(*card)).top());
    QVERIFY(submit(*card)->visibleRegion().contains(submit(*card)->rect().center()));
    QCOMPARE(QApplication::widgetAt(submit(*card)->mapToGlobal(submit(*card)->rect().center())), submit(*card));
    QTest::mouseClick(submit(*card), Qt::LeftButton);
    QCOMPARE(answer.size(), 1);
    QCOMPARE(answer.first()[2].toJsonArray().first().toObject().value("selected_option_ids").toArray(), QJsonArray{"opt_0_0"});
}

void TestQuestionCard::contentHeightPreservesActivityAndPinsFreeformAnswer()
{
    QWidget activity; activity.resize(820, 720);
    auto *layout = new QVBoxLayout(&activity); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto *history = new QTextBrowser; history->setPlainText(QString("Previous agent context\n").repeated(100));
    auto *card = new QuestionCard;
    layout->addWidget(history, 1); layout->addWidget(card);
    auto data = request(); data["optional"] = true; data["can_skip"] = true;
    data["created_at"] = double(QDateTime::currentSecsSinceEpoch());
    auto question = QJsonObject{{"id", "free"}, {"question", "Which folder should I use for the next step?"}, {"allow_other", true}};
    data["questions"] = QJsonArray{question}; setQuestionReady(*card, "fixture", data); activity.show();
    QTRY_VERIFY(card->height() < 240);
    QTRY_VERIFY(history->height() > activity.height() * 2 / 3);
    auto *editor = text(*card, "free"); editor->setText("Keep this draft");
    auto *scroll = card->findChild<QScrollArea *>("questionContent");
    QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
    question["question"] = QString("Long question with enough text to wrap and scroll.\n").repeated(80);
    data["questions"] = QJsonArray{question}; setQuestionReady(*card, "fixture", data);
    editor = text(*card, "free"); scroll = card->findChild<QScrollArea *>("questionContent");
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    QTRY_COMPARE(card->height(), activity.height() * 60 / 100);
    QTRY_VERIFY(history->height() >= activity.height() * 40 / 100);
    // The prompt never scrolls by itself: the wheel over it scrolls the card.
    auto *prompt = card->findChild<MarkdownText *>("questionPrompt"); QVERIFY(prompt);
    QCOMPARE(scroll->verticalScrollBar()->value(), 0);
    QWheelEvent wheel(QPointF(20, 20), prompt->viewport()->mapToGlobal(QPointF(20, 20)), QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    // Only spontaneous wheel events propagate to parents, as from a real mouse.
    QSpontaneKeyEvent::setSpontaneous(&wheel); qApp->notify(prompt->viewport(), &wheel);
    QVERIFY(scroll->verticalScrollBar()->value() > 0);
    scroll->verticalScrollBar()->setValue(0);
    QVERIFY(editor->isVisible()); QVERIFY(!scroll->isAncestorOf(editor));
    QCOMPARE(editor->text(), QString("Keep this draft"));
    QVERIFY(card->rect().contains(submit(*card)->geometry()));
    history->verticalScrollBar()->setValue(50); QCOMPARE(history->verticalScrollBar()->value(), 50);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCOMPARE(history->verticalScrollBar()->value(), 50);
    const int tallHeight = card->height();
    activity.resize(480, 460);
    QTRY_VERIFY(card->height() < tallHeight);
    QTRY_COMPARE(card->height(), activity.height() * 60 / 100);
    QVERIFY(history->height() >= activity.height() * 40 / 100);
    QVERIFY(editor->isVisible()); QCOMPARE(editor->text(), QString("Keep this draft"));
    QVERIFY(card->rect().contains(submit(*card)->geometry()));
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (!directory.isEmpty()) QVERIFY(activity.grab().save(directory + "/question-long-in-activity.png"));
    question["question"] = "Which folder should I use for the next step?";
    data["questions"] = QJsonArray{question}; setQuestionReady(*card, "fixture", data);
    activity.resize(820, 720);
    QTRY_VERIFY(card->height() < 240);
    if (!directory.isEmpty()) QVERIFY(activity.grab().save(directory + "/question-short-in-activity.png"));
}

void TestQuestionCard::currentQuestionAndOtherAnswerResizeTheForm()
{
    QWidget activity; activity.resize(700, 1100);
    auto *layout = new QVBoxLayout(&activity); layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QTextBrowser, 1);
    auto *card = new QuestionCard; layout->addWidget(card);
    auto data = request(); auto questions = data["questions"].toArray();
    auto first = questions[0].toObject();
    first["question"] = QString("Long option question\n").repeated(80); questions[0] = first;
    auto second = questions[1].toObject(); second["options"] = QJsonArray{}; questions[1] = second;
    data["questions"] = questions; setQuestionReady(*card, "fixture", data); activity.show();
    auto *tabs = card->findChild<QTabBar *>("questionTabs");
    auto *pages = card->findChild<QStackedWidget *>("questionPages");
    QTRY_VERIFY(pages->currentWidget()->findChild<QScrollArea *>("questionContent")->verticalScrollBar()->maximum() > 0);
    QTRY_COMPARE(card->height(), 600);
    const int longHeight = card->height();
    tabs->setCurrentIndex(1);
    QTRY_VERIFY(card->height() < longHeight);
    QVERIFY(text(*card, "q_1")->isVisible());
    tabs->setCurrentIndex(0); other(*card, "q_0")->click();
    QTRY_VERIFY(text(*card, "q_0")->isVisible());
    QTRY_VERIFY(card->height() <= 600);
    QVERIFY(card->rect().contains(QRect(text(*card,"q_0")->mapTo(card,QPoint()),text(*card,"q_0")->size())));
}

void TestQuestionCard::preview()
{
    const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (directory.isEmpty()) QSKIP("Set HGS_PREVIEW_DIR to render question form previews");
    QDir().mkpath(directory);
    for (const bool dark : {true, false}) {
        QuestionCard card; card.resize(620, 420); card.setTheme(dark); setQuestionReady(card, "arch/session", request());
        complete(card); card.show(); QTest::qWait(25);
        QVERIFY(card.styleSheet().contains(dark ? "#202a2c" : "#f4f9f7"));
        QCOMPARE(card.grab().toImage().pixelColor(600, 345), QColor(dark ? "#202a2c" : "#f4f9f7"));
        QVERIFY(card.grab().save(directory + (dark ? "/question-dark.png" : "/question-light.png")));
        card.resize(480, 360); card.findChild<QTabBar *>("questionTabs")->setCurrentIndex(1); QTest::qWait(20);
        QVERIFY(card.grab().save(directory + (dark ? "/question-dark-compact.png" : "/question-light-compact.png")));
        auto optional=request("queued");optional["optional"]=true;optional["can_skip"]=true;
        optional["created_at"]=double(QDateTime(QDate(2026,10,7),QTime(12,34,56)).toSecsSinceEpoch());
        optional["questions"]=QJsonArray{optional["questions"].toArray().first()};
        setQuestionReady(card, "arch/session",optional,3);card.setQueueNavigation(1,3);QTest::qWait(20);
        QVERIFY(card.grab().save(directory + (dark ? "/question-dark-queue.png" : "/question-light-queue.png")));
        QuestionCard plan; plan.resize(620, 460); plan.setTheme(dark); setQuestionReady(plan, "kimi/session", planReview());
        plan.show(); QTest::qWait(25);
        QVERIFY(plan.grab().save(directory + (dark ? "/question-dark-plan.png" : "/question-light-plan.png")));
    }
}

QTEST_MAIN(TestQuestionCard)
#include "test_questioncard.moc"

void TestQuestionCard::optionalQuestionsHaveExplicitSkipAndKeepDrafts()
{
    QuestionCard card;auto data=request();data["optional"]=true;data["can_skip"]=true;
    auto item=data["questions"].toArray().first().toObject();item["required"]=false;
    const auto created=QDateTime(QDate(2026,10,7),QTime(12,34,56)).toSecsSinceEpoch();data["created_at"]=double(created);
    data["questions"]=QJsonArray{item};setQuestionReady(card, "session",data,3);card.show();
    auto *asked=card.findChild<QLabel *>("questionAskedAt"),*pending=card.findChild<QLabel *>("questionPendingCount");
    QVERIFY(asked->isVisible());QVERIFY(asked->text().contains("7 Oct"));QVERIFY(asked->text().contains("12:34:56"));QCOMPARE(pending->text(),QString("3 pending"));
    const auto timestamp=asked->text();
    auto *skip=card.findChild<QPushButton *>("skipQuestion");QVERIFY(skip->isVisible());QVERIFY(skip->isEnabled());
    QVERIFY(!submit(card)->isEnabled());QSignalSpy sent(&card,&QuestionCard::answerRequested);
    other(card,"q_0")->click();text(card,"q_0")->setText("Draft answer");
    setQuestionReady(card, "session",data,2);QCOMPARE(text(card,"q_0")->text(),QString("Draft answer"));
    QCOMPARE(asked->text(),timestamp);QCOMPARE(pending->text(),QString("2 pending"));
    data["created_at"]=0;setQuestionReady(card, "session",data,2);QVERIFY(asked->isHidden());QVERIFY(asked->text().isEmpty());
    QCOMPARE(text(card,"q_0")->text(),QString("Draft answer"));
    data["can_answer"]=false;setQuestionReady(card, "session",data);QVERIFY(skip->isEnabled());QVERIFY(!submit(card)->isEnabled());
    card.resize(620,420);QTest::qWait(1);
    const auto submitBounds=submit(card)->geometry(),skipBounds=skip->geometry();
    skip->click();QCOMPARE(sent.size(),1);QCOMPARE(sent.first()[2].toJsonArray(),QJsonArray{QJsonObject({{"question_id","q_0"},{"skip",true}})});
    setSendingReady(card, "session",data["question_id"].toString());QTest::qWait(1);
    QCOMPARE(submit(card)->text(),QString("Submitting…"));
    QCOMPARE(submit(card)->geometry(),submitBounds);QCOMPARE(skip->geometry(),skipBounds);
    card.setError("session",data["question_id"].toString(),"Try again");QTest::qWait(1);
    QCOMPARE(submit(card)->geometry(),submitBounds);QCOMPARE(skip->geometry(),skipBounds);
    card.setAvailability(false,"Offline");QVERIFY(!skip->isEnabled());
    card.setAvailability(true);setQuestionReady(card, "session",request());QVERIFY(skip->isHidden());
}
