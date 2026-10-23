#include "QuestionCard.h"
#include "MonospaceFont.h"
#include "ContentScale.h"
#include "MarkdownText.h"
#include "SessionFileReference.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QDateTime>
#include <QEvent>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QValidator>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOption>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// hgs counts an OpenCode text limit in Unicode characters, not UTF-16 units.
class CharacterLimit final : public QValidator {
public:
    CharacterLimit(int limit, QObject *parent) : QValidator(parent), m_limit(limit) {}
    State validate(QString &input, int &) const override { return input.toUcs4().size() <= m_limit ? Acceptable : Invalid; }
private:
    int m_limit;
};
// The first characters of text, never splitting a surrogate pair.
QString clipCharacters(const QString &text, int limit)
{
    qsizetype units = 0;
    for (int count = 0; units < text.size() && count < limit; ++count)
        units += text.at(units).isHighSurrogate() && units + 1 < text.size() && text.at(units + 1).isLowSurrogate() ? 2 : 1;
    return text.left(units);
}
// Reserve the same space for every submit label, including the shorter busy
// label used while Skip is being delivered. Let Qt size it for the current
// font and style instead of fixing a pixel width.
class SubmitButton final : public QPushButton {
public:
    explicit SubmitButton(const QStringList &labels) : QPushButton(labels.first()), m_labels(labels)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override
    {
        ensurePolished();
        QStyleOptionButton option; initStyleOption(&option);
        QSize size;
        for (const auto &label : m_labels) {
            option.text = label;
            size = size.expandedTo(style()->sizeFromContents(QStyle::CT_PushButton, &option,
                fontMetrics().size(Qt::TextShowMnemonic, label), this));
        }
        return size;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
private:
    QStringList m_labels;
};

// The card's colours; setTheme() styles the widgets with them.
struct CardColors {
    QColor surface, text, border, selected, accent, onAccent, muted, field;
};

CardColors cardColors(bool dark)
{
    return {QColor(dark ? "#202a2c" : "#f4f9f7"), QColor(dark ? "#e8edf4" : "#1a2733"), QColor(dark ? "#40544e" : "#cbded4"),
            QColor(dark ? "#293d35" : "#e4f3ec"), QColor(dark ? "#8bdfc0" : "#167357"), QColor(dark ? "#10231b" : "#ffffff"),
            QColor(dark ? "#a2adbc" : "#627082"), QColor(dark ? "#192228" : "#ffffff")};
}

bool webLink(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
        && (url.scheme() == "https" || url.scheme() == "http");
}

// Terminal agents read an answer starting with / or ! as a command. OpenCode
// form answers go through its form reply API instead, so they may.
bool formReplyCard(const QJsonObject &question)
{
    return question.value("source") == "opencode_native" && !question.value("approval").toBool();
}

QString customAnswerProblem(const QString &text, bool formReply)
{
    if (text.toUtf8().size() > 4096) return QObject::tr("Keep your custom answer under 4 KiB.");
    if (text.contains('\n') || text.contains('\r')) return QObject::tr("Use one line for a custom answer. Multiline input is available in Terminal.");
    for (const auto ch : text) if (ch.unicode() < 0x20 || (ch.unicode() >= 0x7f && ch.unicode() <= 0x9f))
        return QObject::tr("Remove control characters from your custom answer.");
    if (!formReply && (text.trimmed().startsWith('/') || text.trimmed().startsWith('!')))
        return QObject::tr("Use Terminal for answers starting with / or !.");
    return {};
}

// Keep a preferred body height while allowing the parent to reserve room for
// queued input and the composer. A fixed height makes Qt overlap the card's
// status and action rows when the complete Activity pane is constrained.
class QuestionPages final : public QStackedWidget {
public:
    QSize sizeHint() const override {
        auto size = QStackedWidget::sizeHint(); size.setHeight(m_preferredHeight); return size;
    }
    QSize minimumSizeHint() const override {
        auto size = QStackedWidget::minimumSizeHint(); size.setHeight(0); return size;
    }
    void setBodyHeight(int preferred, int minimum) {
        if (m_preferredHeight == preferred && minimumHeight() == minimum) return;
        m_preferredHeight = preferred;
        setMinimumHeight(minimum); setMaximumHeight(preferred); updateGeometry();
    }
private:
    int m_preferredHeight = 0;
};

class Choice final : public QFrame {
public:
    // A Markdown description replaces the plain one.
    Choice(bool multi, const QString &label, const QString &description, QWidget *parent, MarkdownText *markdown = nullptr)
        : QFrame(parent), button(multi ? static_cast<QAbstractButton *>(new QCheckBox(this)) : new QRadioButton(this))
    {
        setObjectName("questionChoice"); setProperty("checked", false);
        auto *row = new QHBoxLayout(this); row->setContentsMargins(10, 9, 12, 9); row->setSpacing(8);
        button->setFixedWidth(20); button->setAccessibleName(label);
        button->setAccessibleDescription(description);
        row->addWidget(button, 0, Qt::AlignTop);
        auto *copy = new QVBoxLayout; copy->setSpacing(3); copy->setContentsMargins(0, 0, 0, 0);
        auto *title = new QLabel(label, this); title->setObjectName("questionChoiceTitle");
        title->setTextFormat(Qt::PlainText); title->setWordWrap(true); title->setAttribute(Qt::WA_TransparentForMouseEvents);
        title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        copy->addWidget(title);
        if (markdown) {
            // Clicks outside its links select the option, like the rest of the row.
            markdown->setClickThrough(true); copy->addWidget(markdown);
        } else if (!description.isEmpty()) {
            auto *detail = new QLabel(description, this); detail->setObjectName("questionChoiceDescription");
            detail->setTextFormat(Qt::PlainText); detail->setWordWrap(true); detail->setAttribute(Qt::WA_TransparentForMouseEvents);
            detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            copy->addWidget(detail);
        }
        row->addLayout(copy, 1);
        connect(button, &QAbstractButton::toggled, this, [this, markdown](bool checked) {
            setProperty("checked", checked); style()->unpolish(this); style()->polish(this);
            // The description's background follows the row's state.
            if (markdown) { markdown->style()->unpolish(markdown); markdown->style()->polish(markdown); }
            update();
        });
    }
    QAbstractButton *button;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && button->isEnabled()) {
            button->setFocus(Qt::MouseFocusReason); button->click(); event->accept(); return;
        }
        QFrame::mousePressEvent(event);
    }
};
}

QuestionCard::QuestionCard(QWidget *parent) : QWidget(parent)
{
    setObjectName("questionCard"); setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    if (parent) parent->installEventFilter(this);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(14, 12, 14, 12); layout->setSpacing(9);
    auto *header = new QHBoxLayout; header->setContentsMargins(0, 0, 0, 0);
    m_heading = new QLabel(tr("Agent needs your answer")); m_heading->setObjectName("questionHeading");
    m_heading->setTextFormat(Qt::PlainText); header->addWidget(m_heading, 1);
    m_progress = new QLabel; m_progress->setObjectName("questionProgress"); header->addWidget(m_progress);
    layout->addLayout(header);
    auto *metadata=new QHBoxLayout;metadata->setContentsMargins(0,0,0,0);
    m_askedAt=new QLabel;m_askedAt->setObjectName("questionAskedAt");m_askedAt->setTextFormat(Qt::PlainText);
    m_pendingCount=new QLabel;m_pendingCount->setObjectName("questionPendingCount");m_pendingCount->setTextFormat(Qt::PlainText);
    m_pendingCount->setToolTip(tr("Questions waiting for an answer, including the questions shown in this card."));
    m_queuePrevious=new QToolButton;m_queuePrevious->setObjectName("questionQueuePrevious");m_queuePrevious->setArrowType(Qt::LeftArrow);
    m_queueNext=new QToolButton;m_queueNext->setObjectName("questionQueueNext");m_queueNext->setArrowType(Qt::RightArrow);
    m_queuePrevious->setAccessibleName(tr("Previous queued question"));m_queueNext->setAccessibleName(tr("Next queued question"));
    for(auto *button:{m_queuePrevious,m_queueNext}) {button->setFixedSize(26,24);button->hide();}
    connect(m_queuePrevious,&QToolButton::clicked,this,[this]{emit queueNavigationRequested(-1);});
    connect(m_queueNext,&QToolButton::clicked,this,[this]{emit queueNavigationRequested(1);});
    metadata->addWidget(m_askedAt);metadata->addStretch();metadata->addWidget(m_pendingCount);
    metadata->addWidget(m_queuePrevious);metadata->addWidget(m_queueNext);layout->addLayout(metadata);
    m_tabs = new QTabBar; m_tabs->setObjectName("questionTabs");
    m_tabs->setExpanding(false); m_tabs->setDrawBase(false); m_tabs->setUsesScrollButtons(true); m_tabs->setElideMode(Qt::ElideNone);
    layout->addWidget(m_tabs);
    m_pages = new QuestionPages; m_pages->setObjectName("questionPages");
    m_pages->setMaximumHeight(0); layout->addWidget(m_pages);
    m_status = new QLabel; m_status->setObjectName("questionStatus"); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(m_status);
    auto *footer = new QHBoxLayout; footer->setContentsMargins(0, 0, 0, 0);
    m_terminal = new QPushButton(tr("Open Terminal")); m_terminal->setObjectName("questionOpenTerminal");
    m_retry = new QPushButton(tr("Allow retry")); m_retry->setObjectName("questionAllowRetry");
    m_retry->setToolTip(tr("Check the agent in Terminal before retrying to avoid submitting twice."));
    m_submit = new SubmitButton({tr("Submit answers"), tr("Submit answer"), tr("Submitting…"), tr("Submit answer (%1)").arg(3), tr("Confirm")});
    m_submit->setObjectName("submitQuestionAnswer");
    m_previous = new QPushButton(tr("← Back")); m_previous->setObjectName("questionPrevious");
    m_next = new QPushButton(tr("Next →")); m_next->setObjectName("questionNext");
    m_approve = new QPushButton(tr("Approve once (3)"));m_approve->setObjectName("approveToolRequest");
    m_deny = new QPushButton(tr("Deny"));m_deny->setObjectName("denyToolRequest");
    m_skip = new QPushButton(tr("Skip")); m_skip->setObjectName("skipQuestion");
    m_skip->setToolTip(tr("Dismiss this optional question without sending an answer. The agent can continue."));
    connect(m_skip,&QPushButton::clicked,this,[this]{
        if(!m_skip->isEnabled())return;
        QJsonArray result;for(const auto &form:m_forms)result.append(QJsonObject{{"question_id",form.id},{"skip",true}});
        emit answerRequested(m_session,m_id,result);
    });
    footer->addWidget(m_terminal); footer->addStretch(); footer->addWidget(m_skip); footer->addWidget(m_retry); footer->addWidget(m_previous); footer->addWidget(m_next); footer->addWidget(m_submit); layout->addLayout(footer);
    connect(m_previous,&QPushButton::clicked,this,[this]{m_tabs->setCurrentIndex(m_tabs->currentIndex()-1);});
    connect(m_next,&QPushButton::clicked,this,[this]{m_tabs->setCurrentIndex(m_tabs->currentIndex()+1);});
    footer->addWidget(m_deny);footer->addWidget(m_approve);
    for(auto *button:{m_approve,m_deny})connect(button,&QPushButton::clicked,this,[this,button]{
        if(!button->isEnabled()||!m_question.value("approval").toBool())return;
        const auto question=m_question.value("questions").toArray().first().toObject();
        emit answerRequested(m_session,m_id,QJsonArray{QJsonObject{{"question_id",question.value("id")},
            {"selected_option_ids",QJsonArray{button==m_approve?"allow":"deny"}},{"text",""}}});
    });
    m_reviewTimer.setInterval(100);connect(&m_reviewTimer,&QTimer::timeout,this,[this]{
        if(!m_question.value("approval").toBool())return;
        if(!isVisible()||!window()->isActiveWindow()||visibleRegion().isEmpty()||!m_available) {
            m_reviewClock.invalidate();m_reviewSeconds=reviewSecondsLeft(0);
        } else {
            if(!m_reviewClock.isValid())m_reviewClock.start();
            m_reviewSeconds=reviewSecondsLeft(m_reviewClock.elapsed());
        }
        updateControls();
    });m_reviewTimer.start();
    connect(m_terminal, &QPushButton::clicked, this, &QuestionCard::openTerminalRequested);
    connect(m_submit, &QPushButton::clicked, this, &QuestionCard::submit);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        m_pages->setCurrentIndex(index);
        if (!m_loading && !m_key.isEmpty() && !m_drafts.value(m_key).sending
            && !m_drafts.value(m_key).submitted && !m_drafts.value(m_key).answered
            && !m_sendingKeys.contains(identity(m_session, m_id))) { m_drafts[m_key].page = index; saveDraft(m_key, true); }
        updateControls();
    });
    connect(m_retry, &QPushButton::clicked, this, [this] {
        if (m_key.isEmpty()) return;
        auto &draft = m_drafts[m_key]; draft.uncertain = false; draft.error = false; draft.notice.clear(); saveDraft(m_key); updateControls();
    });
    auto *retrySave = new QTimer(this); retrySave->setInterval(2000);
    connect(retrySave, &QTimer::timeout, this, [this] {
        const auto pending = m_failedSaves;
        for (const auto &key : pending) saveDraft(key, true);
        if (!pending.isEmpty()) updateControls();
    }); retrySave->start();
    connect(this, &QuestionCard::externalLinkActivated, this, [](const QUrl &url) { QDesktopServices::openUrl(url); });
    setTheme(true); hide();
}

QString QuestionCard::identity(const QString &sessionKey, const QString &questionId, const QString &hash)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{sessionKey, questionId, hash}).toJson(QJsonDocument::Compact));
}

QString QuestionCard::draftIdentity(const QString &sessionKey, const QJsonObject &question)
{
    if (question.value("question_id").toString().isEmpty()) return {};
    return identity(sessionKey, question.value("question_id").toString(), question.value("question_hash").toString())
        + '\n' + question.value("run_id").toString() + '\n' + question.value("conversation_id").toString();
}

void QuestionCard::ensureDraft(const QString &key, const QString &sessionKey)
{
    if (key.isEmpty() || m_drafts.contains(key)) return;
    Draft draft; draft.label = tr("%1 (question answer)").arg(QString(sessionKey).replace('\n', " / "));
    m_drafts.insert(key, draft); m_loadingKeys.insert(key);
    const auto revision = m_saveRevision.value(key);
    m_draftStore.loadAsync("question\n" + key, this, [this, key, revision](const ComposerDraft &saved) {
        if (!m_loadingKeys.contains(key) || m_saveRevision.value(key) != revision) return;
        m_loadingKeys.remove(key);
        const auto label = m_drafts[key].label; m_drafts[key] = restoredDraft(saved); m_drafts[key].label = label;
        if (m_key == key) {
            auto *focus = QApplication::focusWidget();
            const bool hadFormFocus = focus && m_pages->isAncestorOf(focus);
            const auto name = hadFormFocus ? focus->objectName() : QString();
            const auto questionId = hadFormFocus ? focus->property("questionId") : QVariant();
            const auto optionId = hadFormFocus ? focus->property("optionId") : QVariant();
            rebuild(); updateControls();
            if (hadFormFocus) {
                for (auto *control : m_pages->findChildren<QWidget *>())
                    if (control->objectName() == name && control->property("questionId") == questionId
                        && control->property("optionId") == optionId && control->isEnabled()) {
                        control->setFocus(Qt::OtherFocusReason); break;
                    }
            }
        }
        emit draftRestored();
    });
}

bool QuestionCard::restoreSubmittedAnswer(const QString &key, const QJsonObject &question)
{
    bool changed = false;
    const auto delivery = question.value("answer_delivery").toObject();
    if (!key.isEmpty() && question.value("source") == "codex_async" && question.value("optional").toBool()
        && delivery.value("status") == "submitted"
        && delivery.value("question_id") == question.value("question_id")
        && delivery.value("question_hash") == question.value("question_hash")
        && delivery.value("run_id") == question.value("run_id")
        && delivery.value("conversation_id") == question.value("conversation_id")) {
        m_loadingKeys.remove(key);
        auto &draft = m_drafts[key];
        if (!draft.submitted) {
            draft.answers.clear();
            for (const auto &value : delivery.value("answers").toArray()) {
                const auto answer = value.toObject(); Answer saved;
                for (const auto &option : answer.value("selected_option_ids").toArray()) saved.options.insert(option.toString());
                saved.text = answer.value("text").toString(); saved.other = saved.options.isEmpty();
                draft.answers.insert(answer.value("question_id").toString(), saved);
            }
            changed = true;
        }
        draft.submitted = true; draft.sending = false; draft.error = false; draft.uncertain = false;
        draft.notice = tr("Answer submitted. Waiting for Codex to record it. You can check the queue above or open Terminal.");
        if (changed) saveDraft(key);
    }
    return changed;
}

QJsonObject QuestionCard::submissionSnapshot(const QString &sessionKey, const QString &questionId) const
{
    if (sessionKey != m_session || questionId != m_id || !m_drafts.contains(m_key)) return {};
    const auto &draft = m_drafts[m_key];
    QJsonArray forms;
    for (const auto &form : m_forms) {
        QStringList options;
        for (auto it = form.options.cbegin(); it != form.options.cend(); ++it)
            if (it.value()->isChecked()) options.append(it.key());
        options.sort();
        forms.append(QJsonObject{{"id", form.id}, {"options", QJsonArray::fromStringList(options)},
            {"other", form.other && form.other->isChecked()}, {"text", form.text ? form.text->text() : QString()}});
    }
    return {{"identity", m_key}, {"forms", forms}, {"page", m_tabs->currentIndex()},
        {"confirm", draft.confirm}, {"sending", draft.sending || m_sendingKeys.values().contains(m_key)},
        {"submitted", draft.submitted}, {"answered", draft.answered}, {"uncertain", draft.uncertain},
        {"storage_error", draft.storageError}};
}

bool QuestionCard::hasSubmittedAnswer(const QString &sessionKey, const QJsonObject &question)
{
    if (question.value("source") != "codex_async" || !question.value("optional").toBool()) return false;
    const auto key = draftIdentity(sessionKey, question);
    ensureDraft(key, sessionKey);
    // Remember verified snapshots even when the submitted card is never shown.
    // A later stale poll must not offer the same request again.
    restoreSubmittedAnswer(key, question);
    return m_drafts.value(key).submitted;
}

void QuestionCard::setQuestion(const QString &sessionKey, const QJsonObject &question, int pendingQuestions)
{
    const QString id = question.value("question_id").toString();
    const QString key = draftIdentity(sessionKey, question);
    ensureDraft(key, sessionKey);
    const bool restored = restoreSubmittedAnswer(key, question);
    const bool changed = restored || key != m_key || (question.value("questions") != m_question.value("questions")
        && !refreshNativeBody(question));
    m_session = sessionKey; m_id = id; m_key = key; m_question = question;
    const double created=question.value("created_at").toDouble();
    const auto asked=created>0 && created<253402300800. ? QDateTime::fromSecsSinceEpoch(qint64(created)).toLocalTime() : QDateTime();
    m_askedAt->setVisible(asked.isValid());
    m_askedAt->setText(asked.isValid()?tr("Asked %1").arg(asked.toString(asked.date().year()==QDate::currentDate().year()?"d MMM HH:mm:ss":"d MMM yyyy HH:mm:ss")):QString());
    m_askedAt->setToolTip(asked.isValid()?asked.toString("d MMM yyyy HH:mm:ss t"):QString());
    const int count=qMax(pendingQuestions,question.value("questions").toArray().size());
    m_pendingCount->setText(tr("%1 pending").arg(count));m_pendingCount->setVisible(count>0);
    if (!key.isEmpty()) m_latestKeys.insert(identity(sessionKey, id), key);
    if (changed) {m_queueIndex=-1;m_queueCount=0;m_reviewClock.invalidate();m_reviewSeconds=reviewSecondsLeft(0);rebuild();}
    updateControls(); setVisible(!m_key.isEmpty());
}

void QuestionCard::setAvailability(bool available, const QString &reason)
{
    m_available = available; m_unavailableReason = reason; updateControls();
}

void QuestionCard::setQueueNavigation(int index, int count)
{
    m_queueIndex=index;m_queueCount=count;
    const bool visible=m_question.value("optional").toBool() && count>1 && index>=0 && index<count;
    m_queuePrevious->setVisible(visible);m_queueNext->setVisible(visible);
    m_queuePrevious->setEnabled(visible && index>0);m_queueNext->setEnabled(visible && index+1<count);
    const QString position=tr("Request %1 of %2, newest first. Draft answers are kept when switching.").arg(index+1).arg(count);
    m_queuePrevious->setToolTip(tr("Previous queued question (newer)")+"\n"+position);
    m_queueNext->setToolTip(tr("Next queued question (older)")+"\n"+position);
    updateControls();
}

QString QuestionCard::callbackKey(const QString &sessionKey, const QString &questionId) const
{
    const QString pair = identity(sessionKey, questionId);
    return m_sendingKeys.value(pair, m_latestKeys.value(pair));
}

bool QuestionCard::setSending(const QString &sessionKey, const QString &questionId, bool sending)
{
    if (!sending) {
        const auto key = callbackKey(sessionKey, questionId);
        if (key.isEmpty()) return false;
        m_drafts[key].sending = false; m_sendingKeys.remove(identity(sessionKey, questionId)); saveDraft(key); updateControls(); return true;
    }
    const auto key = callbackKey(sessionKey, questionId);
    if (key.isEmpty() || m_loadingKeys.contains(key) || m_drafts.value(key).sending
        || m_drafts.value(key).submitted || m_drafts.value(key).answered || m_drafts.value(key).uncertain
        || m_sendingKeys.contains(identity(sessionKey, questionId))) return false;
    prepareSendingAsync(sessionKey, questionId, [](bool) {}); return true;
}

void QuestionCard::prepareSendingAsync(const QString &sessionKey, const QString &questionId, std::function<void(bool)> done)
{
    const QString pair = identity(sessionKey, questionId), key = callbackKey(sessionKey, questionId);
    if (key.isEmpty() || m_loadingKeys.contains(key)) { done(false); return; }
    auto &draft = m_drafts[key];
    if (draft.sending || draft.submitted || draft.answered || draft.uncertain || m_sendingKeys.contains(pair)) { done(false); return; }
    draft.sending = true; draft.error = false; draft.notice = tr("Saving before submitting…");
    m_sendingKeys.insert(pair, key); updateControls();
    saveDraft(key, false, [this, key, pair, done = std::move(done)](bool okay) {
        if (!okay) {
            if (!m_drafts[key].submitted && !m_drafts[key].answered) {
                m_drafts[key].sending = false; m_drafts[key].notice.clear();
            }
            if (m_sendingKeys.value(pair) == key) m_sendingKeys.remove(pair);
        }
        if (key == m_key) updateControls(); done(okay);
    });
}

void QuestionCard::setError(const QString &sessionKey, const QString &questionId, const QString &detail, bool uncertain)
{
    const QString key = callbackKey(sessionKey, questionId);
    if (key.isEmpty()) return;
    auto &draft = m_drafts[key];
    if (draft.submitted || draft.answered) {
        draft.sending = false; saveDraft(key); m_sendingKeys.remove(identity(sessionKey, questionId)); updateControls(); return;
    }
    draft.sending = false; draft.error = true; draft.uncertain = uncertain;
    draft.notice = uncertain ? tr("Delivery is not confirmed. Check Terminal before retrying. %1").arg(detail) : detail;
    saveDraft(key); m_sendingKeys.remove(identity(sessionKey, questionId)); updateControls();
}

void QuestionCard::setAnswered(const QString &sessionKey, const QString &questionId)
{
    const QString key = callbackKey(sessionKey, questionId);
    if (key.isEmpty()) return;
    auto &draft = m_drafts[key]; draft.sending = false; draft.answered = true; draft.error = false; draft.uncertain = false;
    draft.notice = m_question.value("optional").toBool() ? tr("Response recorded.") : tr("Answer sent. Waiting for the agent…");
    saveDraft(key);
    m_sendingKeys.remove(identity(sessionKey, questionId)); updateControls();
}

void QuestionCard::setSubmitted(const QString &sessionKey, const QString &questionId)
{
    const QString key = callbackKey(sessionKey, questionId);
    if (key.isEmpty()) return;
    auto &draft = m_drafts[key]; draft.sending = false; draft.submitted = true; draft.error = false; draft.uncertain = false;
    draft.notice = tr("Answer submitted. Waiting for Codex to record it. You can check the queue above or open Terminal.");
    saveDraft(key);
    m_sendingKeys.remove(identity(sessionKey, questionId)); updateControls();
}

// These native cards hash every decision-bearing row. Only Claude's exact
// unattended timer is excluded by the backend. Keep the existing form mounted
// for a same-identity body refresh: selection, reading position and the review
// delay must survive every tick. Changes to choices or other schema rebuild.
bool QuestionCard::refreshNativeBody(const QJsonObject &question)
{
    static const QSet<QString> sources{"claude_tool_approval", "claude_terminal_safety", "claude_terminal_choice"};
    if (!sources.contains(question.value("source").toString()) || question.value("source") != m_question.value("source")
        || question.value("question_hash").toString().isEmpty()
        || question.value("question_hash") != m_question.value("question_hash")) return false;
    const auto oldItems = m_question.value("questions").toArray(), items = question.value("questions").toArray();
    if (oldItems.size() != items.size() || items.size() != m_pages->count()) return false;
    QList<QLabel *> bodies;
    for (int i = 0; i < items.size(); ++i) {
        auto oldItem = oldItems[i].toObject(), item = items[i].toObject();
        const auto body = item.take("body"), oldBody = oldItem.take("body");
        if (item != oldItem) return false;
        auto *label = m_pages->widget(i)->findChild<QLabel *>("questionBody");
        if (body != oldBody && (!label || body.toString().isEmpty())) return false;
        bodies.append(label);
    }
    for (int i = 0; i < items.size(); ++i) {
        auto *body = bodies[i]; const auto text = items[i].toObject().value("body").toString();
        if (!body || body->text() == text) continue;
        const int start = body->selectionStart(); const auto selected = body->selectedText();
        body->setText(text);
        if (start >= 0 && !selected.isEmpty() && text.mid(start, selected.size()) == selected)
            body->setSelection(start, selected.size());
    }
    return true;
}

void QuestionCard::rebuild()
{
    m_loading = true; m_forms.clear(); m_fileLinks.clear();
    // Questions the agent wrote are its Markdown. Approval and trust disclosures
    // and panels read from a terminal, such as a folder path, stay as reported.
    static const QSet<QString> authored{"hook", "codex_async", "kimi_wire", "kimi_plan_review", "dsh_native", "opencode_native", "claude_native_question"};
    const bool literal = m_question.value("approval").toBool() || m_question.value("trust_request").toBool()
        || !authored.contains(m_question.value("source").toString());
    while (m_pages->count()) { auto *page = m_pages->widget(0); m_pages->removeWidget(page); delete page; }
    while (m_tabs->count()) m_tabs->removeTab(0);
    const auto draft = m_drafts.value(m_key);
    // OpenCode reports its form defaults and bounds. Defaults only seed an answer
    // that has no saved draft yet (an intentionally emptied one stays empty) and
    // are never sent by themselves.
    const bool openCode = m_question.value("source") == "opencode_native";
    const bool seed = openCode && !m_key.isEmpty() && !m_loadingKeys.contains(m_key) && !draft.sending && !draft.submitted && !draft.answered;
    int index = 0;
    for (const auto &value : m_question.value("questions").toArray()) {
        const auto question = value.toObject(); Form form;
        form.id = question.value("id").toString(); form.multi = question.value("multi_select").toBool();
        form.allowOther = question.value("allow_other").toBool(); form.required = question.value("required").toBool(true);
        const int maxLength = openCode ? qBound(0, question.value("max_length").toInt(), 4096) : 0;
        if (openCode && form.multi) {
            form.minItems = qMax(0, question.value("min_items").toInt()); form.maxItems = qMax(0, question.value("max_items").toInt());
        }
        auto answer = draft.answers.value(form.id);
        if (seed && !form.id.isEmpty() && !draft.answers.contains(form.id)) {
            Answer seeded;
            QSet<QString> offered;
            for (const auto &option : question.value("options").toArray()) offered.insert(option.toObject().value("id").toString());
            for (const auto &id : question.value("default_option_ids").toArray()) {
                if (!offered.contains(id.toString()) || (!form.multi && !seeded.options.isEmpty())) continue;
                seeded.options.insert(id.toString());
            }
            if (form.allowOther && offered.isEmpty()) seeded.text = clipCharacters(question.value("default_text").toString(), maxLength > 0 ? maxLength : 4096);
            // Shown and submitted, but saved only once the user edits (capture) or submits.
            if (!seeded.options.isEmpty() || !seeded.text.isEmpty()) {
                answer = seeded; m_drafts[m_key].answers.insert(form.id, seeded); m_drafts[m_key].seeded.insert(form.id);
            }
        }
        auto *formPage = new QWidget;
        auto *formLayout = new QVBoxLayout(formPage); formLayout->setContentsMargins(0, 0, 0, 0); formLayout->setSpacing(7);
        auto *scroll = new QScrollArea; scroll->setObjectName("questionContent");
        scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        formLayout->addWidget(scroll, 1);
        auto *page = new QWidget; page->setObjectName("questionPage");
        auto *column = new QVBoxLayout(page); column->setContentsMargins(0, 0, 7, 0); column->setSpacing(7);
        const QString promptText = question.value("question").toString();
        if (literal) {
            auto *prompt = new QLabel(promptText); prompt->setObjectName("questionPrompt");
            prompt->setTextFormat(Qt::PlainText); prompt->setWordWrap(true); prompt->setTextInteractionFlags(Qt::TextSelectableByMouse);
            prompt->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            column->addWidget(prompt);
        } else column->addWidget(markdownText("questionPrompt", promptText));
        const QString body = question.value("body").toString();
        if (!body.isEmpty() && body != promptText) {
            if (literal) {
                auto *detail = new QLabel(body); detail->setObjectName("questionBody"); detail->setTextFormat(Qt::PlainText); detail->setWordWrap(true);
                detail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
                if(m_question.value("approval").toBool()) {auto font=detail->font();font.setFamily(monospaceFamily());detail->setFont(font);}
                column->addWidget(detail);
            } else column->addWidget(markdownText("questionBody", body));
        }
        const QString multiMode = form.minItems > 0 && form.maxItems > 0 ? tr("Select %1 to %2").arg(form.minItems).arg(form.maxItems)
            : form.minItems > 0 ? tr("Select at least %1").arg(form.minItems) : form.maxItems > 0 ? tr("Select up to %1").arg(form.maxItems) : tr("Select one or more");
        auto *mode = new QLabel(form.multi ? multiMode : tr("Select one")); mode->setObjectName("questionMode"); column->addWidget(mode);
        auto *group = new QButtonGroup(page); group->setExclusive(!form.multi);
        // Claude approvals list native options verbatim. Hide only the
        // synthesized Approve/Deny pairs, which have their own buttons.
        const bool listed = !m_question.value("approval").toBool() || m_question.value("approval_choices").toBool();
        for (const auto &optionValue : question.value("options").toArray()) {
            const auto option = optionValue.toObject();
            // A broader option keeps its own description as reported, such as the
            // patterns OpenCode's Always allow saves, and says it asks for confirmation.
            const bool broader = option.value("scope") == "broader";
            const QString own = option.value("description").toString(), confirmation = tr("Applies beyond this request. Asks for confirmation.");
            const QString description = !broader ? own : own.isEmpty() ? confirmation : own + '\n' + confirmation;
            auto *choice = new Choice(form.multi, option.value("label").toString(), description, page,
                literal || broader || description.isEmpty() ? nullptr : markdownText("questionChoiceDescription", description));
            choice->button->installEventFilter(this);
            choice->button->setObjectName("questionOption"); choice->button->setProperty("optionId", option.value("id").toString());
            choice->button->setProperty("questionId", form.id); group->addButton(choice->button);
            choice->button->setChecked(answer.options.contains(option.value("id").toString()));
            form.options.insert(option.value("id").toString(), choice->button); column->addWidget(choice);
            choice->setVisible(listed);
            connect(choice->button, &QAbstractButton::toggled, this, [this] { capture(); });
        }
        if (form.allowOther) {
            if (!form.options.isEmpty()) {
                // A provider can name its own free-text choice, such as Kimi's plan Revise.
                const auto otherLabel = question.value("other_label").toString().simplified();
                const auto otherDescription = question.value("other_description").toString().simplified();
                auto *other = new Choice(form.multi, otherLabel.isEmpty() ? tr("Other") : otherLabel,
                    otherDescription.isEmpty() ? tr("Write your own answer") : otherDescription, page);
                form.other = other->button; form.other->installEventFilter(this); form.other->setObjectName("questionOther"); form.other->setProperty("questionId", form.id);
                group->addButton(form.other); form.other->setChecked(answer.other); column->addWidget(other);
                connect(form.other, &QAbstractButton::toggled, this, [this] { capture(); });
            }
            form.text = new QLineEdit; form.text->setObjectName("questionFreeText"); form.text->setProperty("questionId", form.id);
            form.text->setPlaceholderText(tr("Write your answer…")); form.text->setAccessibleName(tr("Your answer to %1").arg(promptText));
            form.text->setMinimumHeight(35); form.text->setMaxLength(maxLength > 0 ? 2 * maxLength : 4096); form.text->setText(answer.text);
            if (maxLength > 0) form.text->setValidator(new CharacterLimit(maxLength, form.text));
            // Keep the answer reachable while a long prompt or option list scrolls.
            formLayout->addWidget(form.text); connect(form.text, &QLineEdit::textChanged, this, [this] { capture(); });
            connect(form.text, &QLineEdit::returnPressed, this, [this] {
                if (m_submit->isVisible()) submit();
            });
        }
        mode->setVisible(!form.options.isEmpty()&&listed&&!m_question.value("plan_review").toBool()); column->addStretch(); scroll->setWidget(page);
        m_pages->addWidget(formPage); m_forms.append(form);
        const auto header = question.value("header").toString().simplified();
        const auto title = header.isEmpty() ? tr("Question %1").arg(++index) : QString::number(++index) + QStringLiteral(": ") + header.left(32);
        const int tab = m_tabs->addTab(title); m_tabs->setTabData(tab, title);
    }
    m_tabs->setVisible(m_tabs->count() > 1);
    if (m_tabs->count()) m_tabs->setCurrentIndex(qBound(0, draft.page, m_tabs->count() - 1));
    m_loading = false;
}

MarkdownText *QuestionCard::markdownText(const QString &name, const QString &source)
{
    auto *text = new MarkdownText; text->setObjectName(name);
    connect(text, &QTextBrowser::anchorClicked, this, &QuestionCard::activateLink);
    renderMarkdown(text, source);
    return text;
}

void QuestionCard::renderMarkdown(MarkdownText *text, const QString &source)
{
    const auto colors = cardColors(m_dark);
    const bool prompt = text->objectName() == "questionPrompt";
    // GitHub's 14 px Markdown at the card's type sizes: 13 px prompts, 11 px details.
    auto theme = MarkdownTheme::github(m_dark, text->objectName() == "questionChoiceDescription" ? colors.field : colors.surface,
                                       m_scale * (prompt ? 13 : 11) / 14.0);
    theme.fg = prompt ? colors.text : colors.muted;
    // Activity's code surface and denser dark chips keep code visible on the card.
    theme.subtle = QColor(m_dark ? "#171e25" : "#e8eef2"); theme.stripe = m_dark ? QColor("#2a333d") : theme.subtle;
    if (m_dark) theme.chip.setAlphaF(0.33f);
    text->setMarkdownSource(source, theme, [this](const QString &href) { return link(href); });
}

MarkdownLink QuestionCard::link(const QString &href)
{
    const auto file = SessionFileReference::parse(href);
    if (file.valid()) {
        const QString key = QString::fromLatin1(QCryptographicHash::hash(href.toUtf8(), QCryptographicHash::Sha256).toHex());
        m_fileLinks.insert(key, href);
        return {"hgs-file:" + key, file.path + file.location, file.path + file.location};
    }
    if (webLink(QUrl(href))) return {href, {}, {}};
    return {};
}

void QuestionCard::activateLink(const QUrl &url)
{
    if (url.scheme() == "hgs-file" && m_fileLinks.contains(url.path())) emit fileReferenceActivated(m_fileLinks.value(url.path()));
    else if (webLink(url)) emit externalLinkActivated(url);
}

QuestionCard::~QuestionCard() = default;

void QuestionCard::capture()
{
    if (m_loading || m_key.isEmpty() || m_loadingKeys.contains(m_key)) return;
    if (m_drafts.value(m_key).sending || m_drafts.value(m_key).submitted || m_drafts.value(m_key).answered
        || m_sendingKeys.contains(identity(m_session, m_id))) return;
    auto &draft = m_drafts[m_key]; draft.confirm.clear(); draft.seeded.clear();
    for (const auto &form : m_forms) {
        auto &answer = draft.answers[form.id]; answer.options.clear();
        for (auto it = form.options.cbegin(); it != form.options.cend(); ++it) if (it.value()->isChecked()) answer.options.insert(it.key());
        answer.other = form.other && form.other->isChecked();
        if (form.text) answer.text = form.text->text();
    }
    saveDraft(m_key, true); updateControls();
}

QuestionCard::Draft QuestionCard::restoredDraft(const ComposerDraft &saved) const
{
    Draft draft;
    draft.storageError = saved.storageError; draft.uncertain = saved.uncertain; draft.error = saved.uncertain;
    draft.notice = saved.notice;
    draft.page = saved.formState.value("page").toInt();
    draft.answered = saved.formState.value("answered").toBool(); draft.submitted = saved.formState.value("submitted").toBool();
    if (draft.submitted) draft.notice = tr("Answer submitted. Waiting for the agent to record it. Check Activity or Terminal.");
    if (draft.answered) draft.notice = tr("Response recorded.");
    for (const auto &value : saved.formState.value("answers").toArray()) {
        const auto object = value.toObject(); Answer answer;
        answer.text = object.value("text").toString(); answer.other = object.value("other").toBool();
        for (const auto &option : object.value("options").toArray()) answer.options.insert(option.toString());
        draft.answers.insert(object.value("id").toString(), answer);
    }
    return draft;
}

bool QuestionCard::saveDraft(const QString &key, bool asynchronous, std::function<void(bool)> done)
{
    if (key.isEmpty() || m_loadingKeys.contains(key)) { if (done) done(false); return false; }
    auto &draft = m_drafts[key];
    if (asynchronous && draft.sending) return false;
    ComposerDraft saved;
    saved.label = draft.label; saved.sending = draft.sending; saved.uncertain = draft.uncertain;
    QJsonArray answers; QStringList text;
    auto ids = draft.answers.keys(); ids.sort();
    for (const auto &id : ids) {
        // Seeded defaults the user has not touched are not a draft.
        if (draft.seeded.contains(id)) continue;
        const auto answer = draft.answers.value(id); QJsonArray options;
        auto selected = answer.options.values(); selected.sort(); for (const auto &option : selected) options.append(option);
        answers.append(QJsonObject{{"id", id}, {"text", answer.text}, {"other", answer.other}, {"options", options}});
        if (!answer.text.isEmpty()) text.append(answer.text);
    }
    if (!draft.answered && !draft.submitted) saved.text = text.join('\n');
    saved.formState = {{"answers", draft.answered ? QJsonArray() : answers}, {"page", draft.page}, {"answered", draft.answered}, {"submitted", draft.submitted}};
    const auto revision = ++m_saveRevision[key];
    const auto completed = [this, key, revision, done = std::move(done)](bool okay, const ComposerDraft &written) {
        if (m_saveRevision.value(key) != revision || !m_drafts.contains(key)) { if (done) done(false); return; }
        m_drafts[key].storageError = written.storageError;
        if (okay) m_failedSaves.remove(key); else m_failedSaves.insert(key);
        if (key == m_key) updateControls();
        if (done) done(okay);
    };
    if (asynchronous) m_draftStore.saveAsync("question\n" + key, saved, this, completed);
    else m_draftStore.saveDurableAsync("question\n" + key, saved, this, completed);
    return true;
}

QJsonArray QuestionCard::answers(bool *valid) const
{
    bool complete = !m_forms.isEmpty(); QJsonArray result;
    const auto draft = m_drafts.value(m_key);
    for (const auto &form : m_forms) {
        const auto answer = draft.answers.value(form.id); QJsonArray selected;
        // Return source option order, rather than hash/set iteration order.
        for (const auto &value : m_question.value("questions").toArray()) {
            const auto question = value.toObject(); if (question.value("id").toString() != form.id) continue;
            for (const auto &option : question.value("options").toArray()) {
                const auto id = option.toObject().value("id").toString(); if (answer.options.contains(id)) selected.append(id);
            }
        }
        const bool custom = form.allowOther && (form.options.isEmpty() || answer.other);
        const QString text = custom ? answer.text.trimmed() : QString();
        // hgs counts the chosen options and an Other answer against the bounds.
        const auto count = selected.size() + (text.isEmpty() ? 0 : 1);
        if ((selected.isEmpty() && text.isEmpty()) || (custom && text.isEmpty())
            || (!form.multi && selected.size() > 1) || !customAnswerProblem(text, formReplyCard(m_question)).isEmpty() || form.id.isEmpty()
            || (form.minItems > 0 && count < form.minItems) || (form.maxItems > 0 && count > form.maxItems)) complete = false;
        result.append(QJsonObject{{"question_id", form.id}, {"selected_option_ids", selected}, {"text", text}});
    }
    if (valid) *valid = complete;
    return result;
}

void QuestionCard::updateControls()
{
    const auto draft = m_drafts.value(m_key);
    const bool inFlight = m_sendingKeys.contains(identity(m_session, m_id));
    const bool locked = draft.sending || inFlight || draft.submitted || draft.answered || m_loadingKeys.contains(m_key);
    // Disabling the focused answer control would move Qt focus out to the
    // context button. Hold it here until the card resolves or the user moves it.
    auto *focus = QApplication::focusWidget();
    if (locked && focus && (focus == m_submit || focus==m_approve || focus==m_deny || m_pages->isAncestorOf(focus)))
        setFocus(Qt::OtherFocusReason);
    bool complete = false; answers(&complete);
    const bool supported = m_question.value("can_answer").toBool();
    static const QSet<QString> terminalOnly{"claude_tool_approval", "kimi_tool_approval", "claude_terminal_choice", "claude_native_question", "claude_terminal_safety", "claude_usage_limit", "startup_terminal_choice"};
    const bool readOnly = !supported && terminalOnly.contains(m_question.value("source").toString());
    for (const auto &form : m_forms) {
        for (auto *button : form.options) button->setEnabled(!locked && !readOnly);
        if (form.other) form.other->setEnabled(!locked && !readOnly);
        if (form.text) {
            const bool custom = form.options.isEmpty() || (form.other && form.other->isChecked());
            form.text->setVisible(custom); form.text->setEnabled(custom && !locked && !readOnly);
        }
    }
    const bool approval=m_question.value("approval").toBool();
    const bool choices=approval&&m_question.value("approval_choices").toBool();
    const QString broader=choices?broaderSelection():QString();
    const bool confirming=!broader.isEmpty()&&draft.confirm==broader;
    m_submit->setEnabled(!m_key.isEmpty() && m_available && supported && complete && !locked && !draft.uncertain
        && (!choices || m_reviewSeconds == 0));
    m_submit->setText(draft.submitted && !draft.answered ? tr("Submitted") : draft.sending || inFlight ? tr("Submitting…")
        : choices && m_reviewSeconds > 0 ? tr("Submit answer (%1)").arg(m_reviewSeconds) : confirming ? tr("Confirm")
        : m_forms.size() == 1 ? tr("Submit answer") : tr("Submit answers"));
    m_retry->setVisible(draft.uncertain && !locked);
    const int page=m_tabs->currentIndex();
    m_previous->setVisible(m_forms.size()>1);m_previous->setEnabled(page>0&&!locked);
    m_next->setVisible(page>=0&&page<m_forms.size()-1);
    m_next->setEnabled(!locked);
    const bool optional=m_question.value("optional").toBool();
    const bool planReview=m_question.value("plan_review").toBool();
    m_heading->setText((approval||m_question.value("trust_request").toBool()||planReview)?tr("Agent needs your approval"):optional?tr("Optional question"):tr("Agent needs your answer"));
    if(optional && !approval && m_queueCount>1 && m_queueIndex>=0 && m_queueIndex<m_queueCount)
        m_heading->setText(tr("Optional question #%1/%2").arg(m_queueIndex+1).arg(m_queueCount));
    m_skip->setVisible(optional);
    m_skip->setEnabled(optional && m_available && m_question.value("can_skip").toBool() && !locked && !draft.uncertain);
    // Without native options, an approval can only be answered in Terminal.
    const bool buttons=approval&&!choices&&supported&&!m_forms.isEmpty()&&!m_forms.first().options.isEmpty();
    // A card without questions (an oversized OpenCode form) only explains itself.
    m_submit->setVisible(!readOnly&&!m_forms.isEmpty()&&(!approval||(choices&&supported))&&page==m_forms.size()-1);
    m_approve->setVisible(buttons);m_deny->setVisible(buttons);
    const bool canApprove=buttons&&m_available&&supported&&!locked&&!draft.uncertain;
    m_approve->setEnabled(canApprove&&m_reviewSeconds==0);m_deny->setEnabled(canApprove);
    m_approve->setText(m_reviewSeconds>0?tr("Approve once (%1)").arg(m_reviewSeconds):tr("Approve once"));
    int answered = 0;
    for (int i = 0; i < m_forms.size(); ++i) {
        const auto &form = m_forms[i]; const auto answer = draft.answers.value(form.id);
        const bool custom = form.allowOther && (form.options.isEmpty() || answer.other);
        const QString text = custom ? answer.text.trimmed() : QString();
        const bool filled = (!answer.options.isEmpty() || !text.isEmpty())
            && (!custom || !text.isEmpty()) && customAnswerProblem(text, formReplyCard(m_question)).isEmpty();
        if (filled) ++answered;
        m_tabs->setTabText(i, (filled ? QStringLiteral("✓ ") : QString()) + m_tabs->tabData(i).toString());
    }
    m_progress->setText(draft.submitted && !draft.answered ? tr("Awaiting agent") : m_forms.isEmpty() ? QString() : tr("%1 of %2 answered").arg(answered).arg(m_forms.size()));
    m_progress->setVisible(!approval&&(!planReview||(draft.submitted&&!draft.answered)));
    QString notice = draft.storageError.isEmpty() ? draft.notice : draft.storageError;
    if (notice.isEmpty() && !m_available) notice = m_unavailableReason;
    // OpenCode states its reason on the card ("Answer this form in OpenCode").
    if (notice.isEmpty() && !supported) for (const auto *field : {"answer_unavailable_reason", "unavailable_reason"}) {
        notice = m_question.value(field).toString().trimmed();
        if (!notice.isEmpty()) break;
    }
    // A disabled Submit always says why, even when an older host sends no reason.
    if (notice.isEmpty() && !supported) notice = tr("Answer this request in Terminal.");
    if (notice.isEmpty() && m_loadingKeys.contains(m_key)) notice = tr("Loading your saved answer…");
    if (notice.isEmpty()) for (const auto &form : m_forms) {
        const auto answer = draft.answers.value(form.id);
        if (!form.allowOther || (!form.options.isEmpty() && !answer.other)) continue;
        notice = customAnswerProblem(answer.text.trimmed(), formReplyCard(m_question));
        if (!notice.isEmpty()) break;
    }
    if (notice.isEmpty() && confirming)
        notice = tr("“%1” changes the agent's permissions beyond this request. Select Confirm to send it.").arg(broader);
    if (notice.isEmpty() && choices) notice = tr("Review the request, then choose one of the agent's options.");
    if (notice.isEmpty() && planReview && !complete) notice = tr("Review the plan, then choose how the agent continues.");
    if (notice.isEmpty()) notice = approval?tr("Approval applies only to this command. Review it before continuing."):
        optional ? tr("The agent can continue. Answer when ready or skip this question.") :
        complete ? tr("Your choices are ready to send.") : tr("Answer each question to continue.");
    m_status->setText(notice.left(600));
    m_status->setStyleSheet(QString("color:%1;font-size:11px;").arg(draft.error || !draft.storageError.isEmpty() ? (m_dark ? "#f4ab9b" : "#a13224") : (m_dark ? "#a2adbc" : "#627082")));
    scheduleSizing();
}

void QuestionCard::scheduleSizing()
{
    if (m_sizingPending) return;
    m_sizingPending = true;
    QTimer::singleShot(0, this, [this] { m_sizingPending = false; sizeToContent(); });
}

void QuestionCard::sizeToContent()
{
    auto *page = m_pages->currentWidget();
    auto *scroll = page ? page->findChild<QScrollArea *>("questionContent") : nullptr;
    if (!scroll || !scroll->widget()) return;
    const auto margins = layout()->contentsMargins();
    const int pageWidth = qMax(1, width() - margins.left() - margins.right());
    // Reserve a scrollbar width when measuring wrapped text, avoiding oscillation
    // at the height limit as the scrollbar appears and disappears.
    const int contentWidth = qMax(1, pageWidth - scroll->verticalScrollBar()->sizeHint().width());
    auto *contentLayout = scroll->widget()->layout();
    const int contentHeight = contentLayout->hasHeightForWidth()
        ? contentLayout->totalHeightForWidth(contentWidth) : contentLayout->totalSizeHint().height();
    const auto *editor = page->findChild<QLineEdit *>("questionFreeText");
    const int editorHeight = editor && !editor->isHidden()
        ? qMax(editor->minimumHeight(), editor->sizeHint().height()) + page->layout()->spacing() : 0;
    layout()->activate();
    const int chrome = layout()->totalHeightForWidth(width()) - m_pages->sizeHint().height();
    const int limit = parentWidget() ? qMin(600, parentWidget()->height() * 60 / 100) : 600;
    const int bodyHeight = qMin(contentHeight + editorHeight, qMax(editorHeight + 40, limit - chrome));
    static_cast<QuestionPages *>(m_pages)->setBodyHeight(bodyHeight, qMin(bodyHeight, editorHeight + 40));
}

bool QuestionCard::event(QEvent *event)
{
    const bool result = QWidget::event(event);
    if (event->type() == QEvent::ParentChange && parentWidget()) parentWidget()->installEventFilter(this);
    if (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest
        || event->type() == QEvent::Show || event->type() == QEvent::FontChange)
        scheduleSizing();
    return result;
}

bool QuestionCard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) scheduleSizing();
    if (event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        auto *choice = qobject_cast<QAbstractButton *>(watched);
        if (choice && m_pages->isAncestorOf(choice)
            && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            // Use the visible primary action, including all review and delivery
            // guards. Holding Enter must not confirm broader consent or resend.
            if (!key->isAutoRepeat() && m_submit->isVisible()) m_submit->click();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void QuestionCard::submit()
{
    if (!m_submit->isEnabled()) return;
    bool valid = false; const auto result = answers(&valid);
    if (!valid) return;
    // An option beyond this one request needs a second, explicit confirmation.
    const QString broader = m_question.value("approval_choices").toBool() ? broaderSelection() : QString();
    if (!broader.isEmpty() && m_drafts[m_key].confirm != broader) {
        m_drafts[m_key].confirm = broader; updateControls(); return;
    }
    // Submitting is the user's explicit choice of the shown values, seeds included.
    m_drafts[m_key].seeded.clear();
    emit answerRequested(m_session, m_id, result);
}

QString QuestionCard::broaderSelection() const
{
    const auto draft = m_drafts.value(m_key);
    for (const auto &value : m_question.value("questions").toArray()) {
        const auto question = value.toObject(); const auto chosen = draft.answers.value(question.value("id").toString()).options;
        for (const auto &option : question.value("options").toArray())
            if (option.toObject().value("scope") == "broader" && chosen.contains(option.toObject().value("id").toString()))
                return option.toObject().value("label").toString();
    }
    return {};
}

void QuestionCard::paintEvent(QPaintEvent *)
{
    QStyleOption option; option.initFrom(this); QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
}

void QuestionCard::setNativeUi(bool native) { m_terminal->setText(native ? tr("Open native UI") : tr("Open Terminal")); }

void QuestionCard::setContentScale(double scale)
{
    scale = ContentScale::clamp(scale);
    if (qFuzzyCompare(scale, m_scale)) return;
    m_scale = scale; setTheme(m_dark);
}

void QuestionCard::setTheme(bool dark)
{
    m_dark = dark;
    const auto colors = cardColors(dark);
    setStyleSheet(QString(R"(
        QWidget#questionCard { background:%1; border:1px solid %3; border-radius:10px; }
        QWidget#questionPage, QScrollArea { background:transparent; border:0; }
        QLabel { color:%2; border:0; }
        QLabel#questionHeading { color:%5; font-size:%9px; font-weight:600; }
        QLabel#questionAskedAt, QLabel#questionPendingCount, QLabel#questionProgress, QLabel#questionMode, QLabel#questionBody, QLabel#questionChoiceDescription { color:%7; font-size:%10px; }
        QLabel#questionPrompt { font-size:%9px; font-weight:600; padding-bottom:3px; }
        QLabel#questionChoiceTitle { font-size:%11px; }
        QTextBrowser#questionPrompt, QTextBrowser#questionBody { background:%1; border:0; }
        QTextBrowser#questionPrompt { font-size:%9px; font-weight:600; }
        QTextBrowser#questionBody, QTextBrowser#questionChoiceDescription { font-size:%10px; }
        QFrame#questionChoice QTextBrowser#questionChoiceDescription { background:%8; border:0; }
        QFrame#questionChoice[checked=true] QTextBrowser#questionChoiceDescription { background:%4; }
        QFrame#questionChoice { background:%8; border:1px solid %3; border-radius:7px; }
        QFrame#questionChoice[checked=true] { background:%4; border-color:%5; }
        QFrame#questionChoice:hover { border-color:%5; }
        QCheckBox, QRadioButton { border:0; background:transparent; }
        QCheckBox::indicator, QRadioButton::indicator { width:12px; height:12px; border:1px solid %7; background:%8; }
        QRadioButton::indicator { border-radius:7px; }
        QCheckBox::indicator { border-radius:3px; }
        QCheckBox::indicator:checked, QRadioButton::indicator:checked { background:%5; border:2px solid %8; }
        QLineEdit { background:%8; color:%2; border:1px solid %3; border-radius:7px; padding:%12px; font-size:%11px; }
        QLineEdit:focus { border-color:%5; }
        QTabBar { font-size:11px; }
        QTabBar::tab { background:transparent; color:%7; padding:7px 10px; border-bottom:2px solid transparent; }
        QTabBar::tab:selected { color:%5; border-bottom-color:%5; }
        QPushButton#submitQuestionAnswer, QPushButton#approveToolRequest { background:%5; color:%6; border:1px solid %5; padding:7px 12px; border-radius:7px; font-weight:600; }
        QPushButton#submitQuestionAnswer:disabled, QPushButton#approveToolRequest:disabled { background:%8; color:%7; border-color:%3; }
        QPushButton#questionOpenTerminal, QPushButton#questionAllowRetry, QPushButton#questionPrevious, QPushButton#questionNext, QPushButton#denyToolRequest { padding:6px 9px; font-size:11px; color:%2; background:%8; border:1px solid %3; border-radius:6px; }
        QPushButton#questionNext { border-color:%5; color:%5; }
        QPushButton#questionPrevious:disabled, QPushButton#questionNext:disabled { color:%7; border-color:%3; }
        QToolButton#questionQueuePrevious, QToolButton#questionQueueNext { color:%2; background:%8; border:1px solid %3; border-radius:5px; padding:0; }
        QToolButton#questionQueuePrevious:hover, QToolButton#questionQueueNext:hover { border-color:%5; }
        QToolButton#questionQueuePrevious:disabled, QToolButton#questionQueueNext:disabled { color:%7; background:transparent; }
    )").arg(colors.surface.name(), colors.text.name(), colors.border.name(), colors.selected.name(),
             colors.accent.name(), colors.onAccent.name(), colors.muted.name(), colors.field.name())
        .arg(ContentScale::px(13, m_scale)).arg(ContentScale::px(11, m_scale)).arg(ContentScale::px(12, m_scale)).arg(ContentScale::px(7, m_scale)));
    style()->unpolish(this); style()->polish(this);
    for (auto *child : findChildren<QWidget *>()) { child->style()->unpolish(child); child->style()->polish(child); }
    for (auto *text : findChildren<MarkdownText *>()) renderMarkdown(text, text->markdownSource());
    update();
    updateControls();
}

void QuestionCard::setTerminalAvailable(bool available, const QString &reason)
{
    m_terminal->setEnabled(available);m_terminal->setToolTip(available?QString():reason);
}
