#pragma once

#include "ComposerDraftStore.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QWidget>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>

class MarkdownText;
class QAbstractButton;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QToolButton;
struct MarkdownLink;

// A native form for a specific provider question request. The owner supplies
// identity-checked transport; this widget never sends terminal keystrokes.
class QuestionCard : public QWidget {
    Q_OBJECT
public:
    explicit QuestionCard(QWidget *parent = nullptr);
    ~QuestionCard() override;
    void setQuestion(const QString &sessionKey, const QJsonObject &question, int pendingQuestions = 0);
    void setQueueNavigation(int index, int count);
    void setAvailability(bool available, const QString &reason = {});
    void setTheme(bool dark);
    // Enlarges question text and answers; actions keep the workspace size.
    void setContentScale(double scale);
    void setNativeUi(bool native);
    // Only a successful callback permits transport; receipts can supersede pending work.
    void prepareSendingAsync(const QString &sessionKey, const QString &questionId, std::function<void(bool)> done);
    bool isDraftLoading() const { return m_loadingKeys.contains(m_key); }
    // UI convenience: acceptance is asynchronous. Transport owners use prepareSendingAsync.
    bool setSending(const QString &sessionKey, const QString &questionId, bool sending = true);
    void setError(const QString &sessionKey, const QString &questionId, const QString &detail, bool uncertain = false);
    void setAnswered(const QString &sessionKey, const QString &questionId);
    void setSubmitted(const QString &sessionKey, const QString &questionId);
    bool hasSubmittedAnswer(const QString &sessionKey, const QJsonObject &question);
    // Read-only, untrimmed editor state for consent across a nested event loop.
    QJsonObject submissionSnapshot(const QString &sessionKey, const QString &questionId) const;
    // Tests only: the review delay before an approval can be granted (3 s), so
    // suites need not wait it out. The application keeps the default.
    void setReviewDelayForTests(int ms) { m_reviewMs = ms; }

signals:
    void answerRequested(const QString &sessionKey, const QString &questionId, const QJsonArray &answers);
    void draftRestored();
    void openTerminalRequested();
    void queueNavigationRequested(int direction);
    // Links in the question's Markdown, after an explicit click.
    void externalLinkActivated(const QUrl &url);
    void fileReferenceActivated(const QString &href);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    struct Answer { QSet<QString> options; QString text; bool other = false; };
    struct Draft {
        QHash<QString, Answer> answers;
        QString notice, confirm, storageError, label;
        int page = 0;
        bool sending = false, submitted = false, answered = false, error = false, uncertain = false;
    };
    struct Form {
        QString id;
        bool multi = false, allowOther = false, required = true;
        QHash<QString, QAbstractButton *> options;
        QAbstractButton *other = nullptr;
        QLineEdit *text = nullptr;
    };
    void rebuild();
    void capture();
    bool saveDraft(const QString &key, bool asynchronous = true, std::function<void(bool)> done = {});
    Draft restoredDraft(const ComposerDraft &saved) const;
    void ensureDraft(const QString &key, const QString &sessionKey);
    void updateControls();
    void scheduleSizing();
    void sizeToContent();
    void submit();
    QJsonArray answers(bool *valid = nullptr) const;
    // Label of a selected approval option that reaches beyond this request.
    QString broaderSelection() const;
    QString callbackKey(const QString &sessionKey, const QString &questionId) const;
    static QString identity(const QString &sessionKey, const QString &questionId, const QString &hash = {});
    static QString draftIdentity(const QString &sessionKey, const QJsonObject &question);
    bool restoreSubmittedAnswer(const QString &key, const QJsonObject &question);
    MarkdownText *markdownText(const QString &name, const QString &source);
    void renderMarkdown(MarkdownText *text, const QString &source);
    MarkdownLink link(const QString &href);
    void activateLink(const QUrl &url);

    QHash<QString, Draft> m_drafts;
    ComposerDraftStore m_draftStore;
    QHash<QString, quint64> m_saveRevision;
    QSet<QString> m_failedSaves;
    QSet<QString> m_loadingKeys;
    QHash<QString, QString> m_latestKeys, m_sendingKeys;
    QHash<QString, QString> m_fileLinks;   // hgs-file: key -> the agent's reference
    QString m_session, m_id, m_key, m_unavailableReason;
    QJsonObject m_question;
    QList<Form> m_forms;
    QLabel *m_heading, *m_progress, *m_status, *m_askedAt, *m_pendingCount;
    QTabBar *m_tabs;
    QStackedWidget *m_pages;
    QPushButton *m_terminal, *m_retry, *m_submit, *m_skip;
    QPushButton *m_previous, *m_next;
    QToolButton *m_queuePrevious, *m_queueNext;
    QPushButton *m_approve, *m_deny;
    QTimer m_reviewTimer;
    QElapsedTimer m_reviewClock;
    int m_reviewMs = 3000, m_reviewSeconds = 3;
    // Whole seconds of review left, as the countdown shows them.
    int reviewSecondsLeft(qint64 elapsed) const { return int((qMax<qint64>(0, m_reviewMs - elapsed) + 999) / 1000); }
    int m_queueIndex = -1, m_queueCount = 0;
    bool m_available = true, m_loading = false, m_dark = true, m_sizingPending = false;
    double m_scale = 1.0;
};
