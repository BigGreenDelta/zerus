#pragma once

#include "HgsClient.h"
#include "ComposerDraftStore.h"
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QWidget>
#include <functional>

class QLabel;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class ComposerToolbar;
class ToolbarChip;
class QUrl;
class QVBoxLayout;
class QHBoxLayout;
class QFrame;

// Drafts belong to a selected session, never to the visible row position.
// A failed or uncertain delivery keeps text and attachments for recovery.
// Add as task (Alt+Enter) hands the draft to the owner instead of sending it;
// only a task that was added clears the draft.
class MessageComposer : public QWidget {
    Q_OBJECT
public:
    // Where Add as task puts the draft: this session's queue (Queue, pinned to
    // the session), the project pool (Queue, not pinned) or Backlog.
    enum class TaskDestination { Queue, Pool, Backlog };
    Q_ENUM(TaskDestination)

    explicit MessageComposer(QWidget *parent = nullptr);
    ~MessageComposer() override;
    void setSessionKey(const QString &key);
    void setAvailability(bool available, const QString &reason = {});
    // Independent of delivery/loading locks; keeps the existing draft selectable.
    void setRecording(bool recording);
    void setInterruptAvailability(bool working, bool enabled, const QString &reason = {});
    // Same as Stop; false when no turn can be interrupted now.
    bool requestInterrupt();
    // Fills an empty draft, for example with the prompt of an interrupted turn.
    bool offerDraft(const QString &key, const QString &text);
    // The agent's suggested next message: the empty field's placeholder, inserted
    // with Tab. An empty text withdraws it; one already answered is not shown again.
    void setSuggestion(const QString &key, const QString &text, double at);
    QString suggestion() const;
    void setTheme(bool dark);
    // Enlarges the message field; its actions keep the workspace size.
    void setContentScale(double scale);
    void setModelSettings(const QString &model, const QString &effort, const QJsonArray &options,
                          bool enabled, const QString &reason, const QString &pendingModel = {},
                          const QString &pendingEffort = {}, const QString &applyWhen = {});
    void setSettingsTerminalAvailable(bool available);
    // Only a successful callback permits transport; pending work locks this exact draft.
    void prepareSendingAsync(const QString &key, bool preserveDraft, std::function<void(bool)> done);
    bool isPreparingSending(const QString &key) const { return m_preparingKeys.contains(key); }
    bool isDraftLoading(const QString &key) const { return m_loadingKeys.contains(key); }
    // UI convenience: acceptance is asynchronous. Transport owners use prepareSendingAsync.
    bool setSending(const QString &key, bool preserveDraft = false);
    // A later send awaits startup/compaction. Keep its payload visible and
    // selectable, while locking edits. A restart restores an ordinary draft.
    void prepareWaitingToSendAsync(const QString &key, const QString &reason, std::function<void(bool)> done);
    bool setWaitingToSend(const QString &key, const QString &reason);
    bool isWaitingToSend(const QString &key) const { return m_waitingSends.contains(key); }
    bool draftMatches(const QString &key, const QString &text, const QList<MessageAttachment> &attachments) const;
    void deliveryFinished(const QString &key, bool ok, const QString &detail = {}, bool uncertain = false);
    void renameDraft(const QString &oldKey, const QString &newKey);
    void showSavedDrafts();
    bool isSending(const QString &key) const;
    // Text or attachments not yet sent; a message on its way does not count.
    bool hasDraft(const QString &key) const;
    bool addAttachment(const QString &name, const QString &mime, const QByteArray &data);
    bool canAttachFiles() const { return !m_recording && !m_key.isEmpty() && !isDraftLoading(m_key) && !isSending(m_key) && !isWaitingToSend(m_key) && !isAddingTask(m_key); }
    QString sessionKey() const { return m_key; }
    void attachDroppedFiles(const QStringList &paths);
    QPlainTextEdit *editor() const { return m_editor; }
    // Notices and attachments above the field; the window adds its own chips.
    ComposerToolbar *toolbar() const { return m_toolbar; }
    // A required question replaces the field and its actions; the toolbar stays.
    void setInputVisible(bool visible);
    bool isInputVisible() const;
    struct VoiceTarget { QString key; quint64 revision = 0, navigation = 0; };
    VoiceTarget voiceTarget() const { return {m_key, m_saveRevision.value(m_key), m_navigationRevision}; }
    // Append only to an unchanged, still selected editable draft. A successful
    // callback means the inserted text is durable; it never submits a message.
    bool insertVoiceText(const VoiceTarget &target, const QString &text, std::function<void(bool)> saved);
    // Opens a stored copy of an attachment; tests replace the desktop handler.
    std::function<bool(const QUrl &)> openUrl;

    // Add as task and its menu; a subagent's composer shows none.
    void setTaskActionsVisible(bool visible);
    // Whether a task can be added at all (a task board is reachable)...
    void setTaskAvailability(bool available, const QString &reason = {});
    // ...and to this session's queue (Overmind knows the session's computer).
    void setTaskQueueAvailability(bool available, const QString &reason = {});
    bool isAddingTask(const QString &key) const { return m_addingTasks.contains(key); }
    // The owner's answer to addTaskRequested, shown under the field: a task
    // that was added clears the draft, anything else keeps it.
    void taskAdded(const QString &key, bool ok, const QString &detail);

signals:
    // Add as task: the draft stays, locked, until taskAdded answers.
    void addTaskRequested(const QString &sessionKey, const QString &text, const QList<MessageAttachment> &attachments,
                          MessageComposer::TaskDestination destination);
    void interruptRequested(const QString &sessionKey);
    void settingsTerminalRequested(const QString &sessionKey);
    void settingsRequested(const QString &sessionKey, const QString &model, const QString &effort);
    void sendRequested(const QString &sessionKey, const QString &text, const QList<MessageAttachment> &attachments);
    // hasDraft(sessionKey) changed.
    void draftChanged(const QString &sessionKey);
    void controlsChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using Draft = ComposerDraft;
    bool saveDraft(const QString &key);
    void send();
    void addTask(TaskDestination destination);
    // A button's reason to stay disabled, under the field where Enter would act.
    void explainDisabled(const QString &reason);
    void restoreDraft();
    void notifyDraft(const QString &key);
    void updatePlaceholder();
    struct Suggestion { QString text; double at = 0; };
    QHash<QString, Suggestion> m_suggestions;
    QHash<QString, double> m_dismissedSuggestions;
    void attachFiles();
    void attachFile(const QString &path);
    void updateControls();
    void rebuildAttachments();
    void removeAttachment(int index);
    void openAttachment(int index);
    void showAttachmentPreview(QWidget *row, int index);
    void showError(const QString &text);
    void openSettings();
    void updateSettingsButton();
    void updateSettingsEfforts();
    void updateSettingsApply();

    QHash<QString, Draft> m_drafts;
    QHash<QString, Draft> m_preservedDrafts;
    QHash<QString, QString> m_waitingSends;
    QSet<QString> m_unsent;   // keys whose last reported hasDraft() was true
    QSet<QString> m_loadingKeys;
    QSet<QString> m_preparingKeys;
    QSet<QString> m_preparingWaiting;
    QSet<QString> m_renamingKeys;
    QSet<QString> m_storedDrafts;
    ComposerDraftStore m_draftStore;
    QHash<QString, quint64> m_saveRevision;
    quint64 m_navigationRevision = 0;
    QSet<QString> m_savingKeys;
    QSet<QString> m_failedSaves;
    QString m_key, m_unavailableReason;
    bool m_available = false, m_loading = false, m_dark = true, m_recording = false;
    double m_scale = 1.0;
    QPlainTextEdit *m_editor;
    QLabel *m_status;
    QFrame *m_settingsPopup;
    QComboBox *m_models, *m_efforts;
    QLabel *m_settingsHint;
    QLabel *m_modelLabel, *m_effortLabel;
    QPushButton *m_settingsTerminal;
    bool m_settingsTerminalAvailable = false;
    QPushButton *m_settings, *m_applySettings;
    QJsonArray m_modelOptions;
    QString m_model, m_effort, m_pendingModel, m_pendingEffort, m_settingsReason, m_applyWhen, m_popupKey;
    bool m_settingsEnabled = false;
    QPushButton *m_attach, *m_send, *m_retry;
    QPushButton *m_stop;
    QWidget *m_addTaskGroup;
    QPushButton *m_addTask, *m_addTaskMenu;
    QSet<QString> m_addingTasks;   // keys whose draft is being added as a task
    QHash<QString, QString> m_taskOutcomes;   // the last added task's notice per key; shown while it is the draft's notice
    bool m_taskActions = true, m_taskAvailable = true, m_taskQueueAvailable = true;
    QString m_taskReason, m_taskQueueReason;
    ComposerToolbar *m_toolbar;
    ToolbarChip *m_attachmentsChip;
    QWidget *m_attachmentList, *m_input;
    QVBoxLayout *m_attachmentsLayout;
    QLabel *m_preview;
    QHash<int, QPixmap> m_previews;   // decoded once per list; a null pixmap marks a non-image
    QHBoxLayout *m_actions;
    QHBoxLayout *m_feedback;
    bool m_narrow = false;
};
