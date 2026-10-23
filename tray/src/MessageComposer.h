#pragma once

#include "HgsClient.h"
#include "ComposerDraftStore.h"
#include <QHash>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <QVariant>
#include <functional>

class QAction;
class QLabel;
class QMenu;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class ComposerToolbar;
class ToolbarChip;
class QUrl;
class QVBoxLayout;
class QHBoxLayout;
class QFrame;
class OpenCodeModelPicker;

// Drafts belong to a selected session, never to the visible row position.
// A failed or uncertain delivery keeps text and attachments for recovery.
class MessageComposer : public QWidget {
    Q_OBJECT
public:
    explicit MessageComposer(QWidget *parent = nullptr);
    ~MessageComposer() override;
    void setSessionKey(const QString &key);
    // Read a retained draft in the background without navigating to its session.
    void ensureDraftLoaded(const QString &key);
    void setAvailability(bool available, const QString &reason = {});
    // Session feedback uses the existing action row and never replaces a draft.
    void setSessionStatus(const QString &text, bool error = false, bool blockInput = false);
    // Independent of delivery/loading locks; keeps the existing draft selectable.
    void setRecording(bool recording);
    void setAttachmentAvailability(bool available, const QString &reason = {});
    // Editable path text on this exact session's computer, without reading or
    // uploading the file. An empty key withdraws the reference target.
    void setFileReferenceTarget(const QString &key, const QString &host, const QString &machine, const QString &context = {});
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
    // modelDefaultEffort: an empty effort is a choice, the model's own default
    // (OpenCode variants); the popup then offers "Default" first.
    void setModelSettings(const QString &model, const QString &effort, const QJsonArray &options,
                          bool enabled, const QString &reason, const QString &pendingModel = {},
                          const QString &pendingEffort = {}, const QString &applyWhen = {}, bool modelDefaultEffort = false);
    // OpenCode only. `supported`: the session's hgs has `account models` and `account favorite`
    // (its details report `model_options_truncated`); `online`: its computer answers.
    // `host` and `folder` are where the session's catalog is read: with the session they are
    // the picker's context. Given before setModelSettings, a new context starts afresh first.
    void setOpenCodeContext(bool supported, bool truncated, bool online, const QString &computer,
                            const QString &host = {}, const QString &folder = {});
    // Changes with every new picker context and every change of session, to none included,
    // so also on return to the same session and folder: an answer to a request made in
    // another context is not shown.
    quint64 openCodeGeneration() const { return m_openCodeGeneration; }
    // Answers for the shown session; any other session's answer is ignored.
    void setOpenCodeCatalog(const QString &sessionKey, const QJsonObject &catalog);
    void setOpenCodeCatalogFailed(const QString &sessionKey, const QString &reason);
    // A confirmed favorite change, or with `error` the state kept after a failed one.
    void setOpenCodeFavorite(const QString &sessionKey, const QString &value, bool favorite, const QString &error);
    // The provider's own warning about applying a change, shown before Apply.
    void setSettingsNotice(const QString &notice);
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
    bool canEditAttachments() const { return !m_recording && !m_sessionStatusBlocksInput && !m_key.isEmpty() && !isDraftLoading(m_key) && !isSending(m_key) && !isWaitingToSend(m_key); }
    bool canAttachFiles() const { return m_attachmentsAvailable && canEditAttachments(); }
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
    // Whether the session a saved draft belongs to can be shown now. Without it,
    // Saved drafts offers no Go to session.
    std::function<bool(const QString &draftKey)> canOpenDraftSession;
    // Presentation only; draft ownership and Go to eligibility stay independent.
    std::function<QMap<int, QVariant>(const QString &draftKey)> savedDraftSessionRoles;
    // Send and push now (message.push) in this field; off by default. Only the
    // main message field pushes: a subagent message bypasses the session's queue.
    void setPushEnabled(bool enabled);

signals:
    void sessionRefreshRequested(const QString &sessionKey);
    void interruptRequested(const QString &sessionKey);
    void settingsTerminalRequested(const QString &sessionKey);
    void settingsRequested(const QString &sessionKey, const QString &model, const QString &effort);
    void openCodeCatalogRequested(const QString &sessionKey);
    void openCodeFavoriteRequested(const QString &sessionKey, const QString &value, bool favorite);
    void sendRequested(const QString &sessionKey, const QString &text, const QList<MessageAttachment> &attachments);
    // Right before sendRequested of a message sent with push, with its text.
    void pushRequested(const QString &sessionKey, const QString &text);
    // Push with an empty field and no attachments: send the existing queue now.
    void sendNowRequested(const QString &sessionKey);
    // hasDraft(sessionKey) changed.
    void draftChanged(const QString &sessionKey);
    // A revision-bound background load completed, even if presence was already known.
    void draftLoaded(const QString &sessionKey);
    // Go to session in Saved drafts, after the dialog has closed.
    void draftSessionRequested(const QString &draftKey);
    void controlsChanged();

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using Draft = ComposerDraft;
    bool saveDraft(const QString &key);
    void send();
    void push();
    void applyShortcuts();
    void restoreDraft();
    void notifyDraft(const QString &key);
    void updatePlaceholder();
    struct Suggestion { QString text; double at = 0; };
    QHash<QString, Suggestion> m_suggestions;
    QHash<QString, double> m_dismissedSuggestions;
    void attachFiles();
    void referenceFile();
    void attachFile(const QString &path);
    void updateControls();
    int boundedEditorHeight(int height) const;
    void updateEditorHeight();
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
    QString m_sessionStatus;
    bool m_sessionStatusError = false, m_sessionStatusBlocksInput = false;
    bool m_available = false, m_loading = false, m_dark = true, m_recording = false, m_attachmentsAvailable = true;
    QString m_attachmentsReason;
    QString m_referenceKey, m_referenceHost, m_referenceMachine, m_referenceContext;
    quint64 m_referenceRevision = 0;
    double m_scale = 1.0;
    double m_editorHeight = 76.0; // At 100%; temporary viewport limits do not overwrite it.
    int m_dragEditorHeight = 0;
    QPlainTextEdit *m_editor = nullptr;
    QWidget *m_resizeHandle;
    QPointer<QWidget> m_heightParent;
    QLabel *m_status;
    QFrame *m_settingsPopup;
    QComboBox *m_models, *m_efforts;
    QLabel *m_settingsHint;
    QLabel *m_modelLabel, *m_effortLabel;
    QPushButton *m_settingsTerminal;
    bool m_settingsTerminalAvailable = false;
    QPushButton *m_settings, *m_applySettings;
    QJsonArray m_modelOptions;
    bool m_modelDefaultEffort = false;
    QString m_model, m_effort, m_pendingModel, m_pendingEffort, m_settingsReason, m_applyWhen, m_popupKey, m_settingsNotice;
    bool m_settingsEnabled = false;
    // OpenCode's grouped picker in the settings popup; m_models stays the selected value.
    bool enterOpenCodeContext();
    void resetOpenCodeModels();
    void chooseOpenCodeModel(const QString &value);
    QJsonArray withConfirmedFavorites(QJsonArray entries) const;
    QJsonObject withConfirmedFavorites(QJsonObject catalog) const;
    OpenCodeModelPicker *m_openCodeModels;
    QStringList m_openCodeContext;                    // the session, computer and folder the picker shows
    QString m_openCodeHost, m_openCodeFolder;         // the shown session's, as last given
    quint64 m_openCodeGeneration = 0;                 // counts the picker's contexts
    QJsonArray m_openCodeShort;                       // that session's own list, as last given
    // Picker choices per session (at most 32 sessions, 16 choices each) with the computer
    // and folder they were made for, merged into the model options on every setModelSettings.
    struct OpenCodeChosen { QString host, folder; QJsonArray entries; };
    QHash<QString, OpenCodeChosen> m_openCodeChosen;
    QStringList m_openCodeChosenOrder;
    // Favorite changes confirmed for the shown session (at most 64), applied to its
    // lists and catalogs until the context changes, so an older list cannot undo them.
    QHash<QString, bool> m_openCodeFavorites;
    QString m_openCodeComputer, m_openCodeError, m_openCodeAvailability;
    bool m_openCodeSupported = false, m_openCodeTruncated = false, m_openCodeOnline = false;
    QPushButton *m_attach, *m_reference, *m_send, *m_retry;
    QMenu *m_fileMenu;
    QAction *m_attachFileAction, *m_referenceFileAction;
    QPushButton *m_refreshStatus;
    QPushButton *m_stop;
    ComposerToolbar *m_toolbar;
    ToolbarChip *m_attachmentsChip;
    QWidget *m_attachmentList, *m_input;
    QVBoxLayout *m_attachmentsLayout;
    QLabel *m_preview;
    QHash<int, QPixmap> m_previews;   // decoded once per list; a null pixmap marks a non-image
    QHBoxLayout *m_actions;
    bool m_narrow = false;
    // Composer-scope commands (Shortcuts.h) on the field.
    QAction *m_sendAction, *m_pushAction;
    bool m_pushEnabled = false, m_pushing = false;
};
