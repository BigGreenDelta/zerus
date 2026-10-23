#pragma once

#include <QCursor>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QWidget>
#include <QImage>
#include <memory>
#include <optional>

namespace ActivityWidth { class ColumnLayout; }
class QAction;
class QMenu;
class QPushButton;
class QLabel;
class QTextBrowser;
class QUrl;
class QShowEvent;
class QHideEvent;
class QDialog;
class QMediaPlayer;
class QAudioOutput;
struct ActivityRenderResult;

// Chronological, read-only journal presentation. Agent input lives in the
// composer's own widget; viewing activity never writes to a running session.
class ActivityView : public QWidget {
    Q_OBJECT
public:
    explicit ActivityView(QWidget *parent = nullptr);
    void setSessionKey(const QString &key);
    // A confirmed archive restore changes the list key, not the conversation.
    // Keep its document, selection, scroll position and expanded messages.
    void continueSession(const QString &oldKey, const QString &newKey);
    void setActivity(const QJsonObject &details, const QJsonArray &events,
                     const QString &fallbackPrompt = {}, bool tracked = true);
    void setTimeline(const QJsonObject &details, const QJsonArray &events,
                     const QJsonArray &localMessages, const QString &fallbackPrompt = {}, bool tracked = true);
    // The owner reconciles these local acknowledgements with arriving hooks.
    // Items: id, text, submitted_at, status, attachments (file references), error.
    void setLocalMessages(const QJsonArray &messages);
    void setTheme(bool dark);
    // Enlarges the transcript and queued input, not the overlay controls.
    void setContentScale(double scale);
    // Centers the transcript and queued input in a column of at most this
    // many pixels. The scroll bar stays at the pane edge; 0 fills the pane.
    void setColumnWidth(int width);
    int columnWidth() const { return m_column; }
    // The current queue.sendNow chord, appended to the Send now tooltip.
    void setQueueSendNowShortcut(const QString &chord);
    void showSearchResult(const QJsonObject &event, const QString &query);
    void clearSearchResult();
    QWidget *compactionIndicator() const { return m_compaction; }
    QPushButton *jumpButton() const { return m_latest; }
    // The owner can place this icon in its toolbar instead of over the journal.
    void useCompactJumpButton();
    QTextBrowser *browser() const { return m_browser; }
    bool replyVisible(const QString &replyId) const;
    // The document as copied: chips and list markers spelled out.
    QString plainText() const;
    void setAttachmentPreview(const QString &key, const QImage &image);
    void playVoice(const QString &key, const QString &path, const QString &error = {});
    // Journal HTML generations. Tests verify that unchanged polls skip them.
    int renderPasses() const { return m_renderPasses; }
    bool renderPending() const { return m_renderDirty || m_renderInFlight || m_renderScheduled; }

public slots:
    void jumpToLatest();

signals:
    // Files are routed to the session owner, which knows their machine and cwd.
    void fileReferenceActivated(const QString &href);
    void externalLinkActivated(const QUrl &url);
    void attachmentActivated(const QJsonObject &file);
    void attachmentPreviewRequested(const QString &key, const QJsonObject &file);
    void voicePlaybackRequested(const QString &key, const QJsonObject &file);
    void queueSendNowRequested(const QString &queueId);
    void messageActionRequested(const QString &id, const QString &action);
    void processRequested(const QString &id);
    // Column edges and the context menu ask the owner, which stores the
    // preference and applies it to every column.
    void columnWidthRequested(int width);
    void columnResetRequested();
    void fullWidthRequested(bool full);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void render(bool contentUpdate);
    void startRender();
    void clearDocument();
    void commitRender(const std::shared_ptr<ActivityRenderResult> &result);
    void updateJumpButton();
    void positionJumpButton();
    void activateLink(const QUrl &url);
    void copyMarkdown(const QString &key);
    QString copyButton(const QString &state, const QString &key) const;
    QString copyKeyAt(const QPoint &position) const;
    bool nearBottom() const;
    void scheduleFollow();
    void layoutColumn();
    int scrollBarReserve() const;
    void dragColumn(int distance);
    void addColumnActions(QMenu *menu);
    void showColumnMenu(const QPoint &position);
    void updateColumnActions();
    void activateVoice(const QString &key,std::optional<qreal> fraction = {});
    QString voiceKeyAt(const QPoint &position) const;
    QString transcriptKeyAt(const QPoint &position) const;
    QRect voiceRect(const QString &key) const;
    void updateVoiceImage(const QString &key);

    QTextBrowser *m_browser;
    QWidget *m_queue;
    QLabel *m_queueText;
    QPushButton *m_queueSend;
    QString m_queueShortcut;
    QPushButton *m_latest = nullptr;
    QWidget *m_compaction;
    QString m_sessionKey, m_conversation, m_html, m_fallbackPrompt;
    QJsonObject m_details, m_activityInput;
    QJsonObject m_searchResult;
    QString m_searchQuery;
    QJsonArray m_events, m_localMessages;
    QHash<QString, bool> m_expanded;
    QHash<QString, QString> m_fileLinks;
    QHash<QString, QString> m_deliveryKeys, m_attachmentBindings;
    QHash<QString, QJsonObject> m_attachmentLinks;
    QHash<QString, QJsonObject> m_voiceLinks;
    QDialog *m_videoDialog = nullptr;
    QMediaPlayer *m_voicePlayer = nullptr;
    QAudioOutput *m_voiceOutput = nullptr;
    QString m_voiceKey, m_voicePlaying, m_voiceLoading, m_voiceError;
    QString m_voicePressed;
    QString m_transcriptPressed;
    QHash<QString,int> m_voicePositions;
    qint64 m_voicePosition = 0, m_voicePendingSeek = -1;
    bool m_voiceStartAfterLoad = false;
    QHash<QString, QJsonObject> m_previewFiles;
    QHash<QString, QImage> m_previews;
    QHash<QString, QPair<QString, QString>> m_messageActions;
    QHash<QString, QString> m_copyTexts;   // copy button key -> the reply's Markdown source
    QString m_copyPressed;
    QCursor m_copyCursor;
    bool m_copyHover = false;
    QSet<QString> m_knownEvents, m_toggleKeys, m_processLinks;
    bool m_dark = false, m_tracked = true, m_initial = true;
    bool m_compactJump = false;
    bool m_rendering = false, m_followLatest = true, m_followScheduled = false;
    quint64 m_renderGeneration = 0, m_contentRevision = 0;
    bool m_renderDirty = false, m_renderInFlight = false, m_renderScheduled = false, m_pendingContentUpdate = false;
    int m_renderWorkers = 0;
    int m_unseen = 0, m_renderPasses = 0;
    double m_scale = 1.0;
    int m_column = 0;               // 0: the transcript fills the pane
    int m_dragOrigin = 0;           // column width when an edge was pressed
    QList<QWidget *> m_edges;
    ActivityWidth::ColumnLayout *m_queueColumn = nullptr;
    QAction *m_fullWidth = nullptr, *m_resetWidth = nullptr;
};
