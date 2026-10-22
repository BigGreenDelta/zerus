#include "MessageComposer.h"
#include "OpenCodeEffort.h"
#include "OpenCodeModelPicker.h"
#include "OpenCodeModels.h"
#include "AttachmentViewer.h"
#include "ComposerToolbar.h"
#include "ContentScale.h"
#include "ExternalFile.h"
#include "LocalFile.h"
#include "MachinePlatform.h"
#include "Shortcuts.h"
#include "SessionCardDelegate.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include "StatusMessage.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QDragLeaveEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QToolTip>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <utility>

namespace {
constexpr qsizetype MaxTextBytes = HgsClient::MaximumMessageBytes;
constexpr qsizetype MaxAttachmentBytes = HgsClient::MaximumAttachmentBytes;
constexpr qsizetype MaxTotalBytes = HgsClient::MaximumAttachmentsBytes;
constexpr qsizetype MaxAttachments = HgsClient::MaximumAttachments;

// Decodes at most bounds; oversized or unreadable images give a null image.
QImage attachmentImage(const QByteArray &data, const QSize &bounds)
{
    QBuffer source; source.setData(data); source.open(QIODevice::ReadOnly);
    QImageReader reader(&source);
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 32000000) return {};
    reader.setScaledSize(size.scaled(bounds, Qt::KeepAspectRatio));
    return reader.read();
}

// Theme glyphs keep their own disabled color instead of Qt's generated gray.
static QIcon composerIcon(const QString &glyph, const QColor &normal, const QColor &disabled)
{
    QIcon icon = workspaceIcon(glyph, normal);
    const QIcon muted = workspaceIcon(glyph, disabled);
    for (const auto &size : muted.availableSizes()) icon.addPixmap(muted.pixmap(size), QIcon::Disabled);
    return icon;
}

// The model may be long, but the effort and disclosure chevron must remain
// readable when the inspector leaves only a narrow composer column.
class SettingsButton : public QPushButton {
public:
    static constexpr int Chevron = 12, ChevronGap = 4;
    explicit SettingsButton(QWidget *parent) : QPushButton(parent) {
        setMinimumWidth(78); setMaximumWidth(330);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    QSize minimumSizeHint() const override { return QSize(78, 34); }
    QSize sizeHint() const override { return QPushButton::sizeHint() + QSize(Chevron + ChevronGap, 0); }
protected:
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this); QStyleOptionButton option; initStyleOption(&option);
        const QString suffix = property("suffix").toString();
        const int available = qMax(0, width() - 22 - Chevron - ChevronGap);
        option.text = fontMetrics().elidedText(property("modelLabel").toString(), Qt::ElideRight,
            qMax(0, available - fontMetrics().horizontalAdvance(suffix))) + suffix;
        painter.drawControl(QStyle::CE_PushButtonBevel, option);
        const QRect contents = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
        QStyleOptionButton label = option; label.rect = contents.adjusted(0, 0, -(Chevron + ChevronGap), 0);
        painter.drawControl(QStyle::CE_PushButtonLabel, label);
        const QRect chevron(contents.right() - Chevron + 1, contents.center().y() - Chevron / 2, Chevron, Chevron);
        property("chevron").value<QIcon>().paint(&painter, chevron, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
    }
};

class ComposeEdit : public QPlainTextEdit {
public:
    explicit ComposeEdit(QWidget *parent) : QPlainTextEdit(parent) {}
    std::function<void()> submit;
    std::function<bool()> escape;
    std::function<QString()> suggestion;
    std::function<void(const QString &)> attachFile;
    std::function<void(const QImage &)> attachImage;
    // An input method is composing text; its keys are not commands.
    bool composing() const { return m_composing; }
protected:
    bool canInsertFromMimeData(const QMimeData *source) const override {
        return !isReadOnly() && (source->hasImage() || source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source));
    }
    void insertFromMimeData(const QMimeData *source) override {
        if (isReadOnly()) return;
        if (source->hasUrls()) {
            const auto urls = source->urls();
            bool allLocal = !urls.isEmpty();
            for (const auto &url : urls) allLocal = allLocal && url.isLocalFile();
            if (allLocal) { for (const auto &url : urls) attachFile(url.toLocalFile()); return; }
        }
        if (source->hasImage()) {
            const auto data = source->imageData();
            QImage image = qvariant_cast<QImage>(data);
            if (image.isNull() && data.canConvert<QPixmap>()) image = qvariant_cast<QPixmap>(data).toImage();
            if (!image.isNull()) { attachImage(image); return; }
        }
        if (source->hasText()) insertPlainText(source->text());
    }
    bool event(QEvent *event) override {
        if (event->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            event->accept(); return true;
        }
        // Tab takes the suggested next message into an empty field, as in Claude's
        // terminal; otherwise it keeps moving focus.
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Tab
            && !isReadOnly() && static_cast<QKeyEvent *>(event)->modifiers() == Qt::NoModifier && document()->isEmpty() && suggestion) {
            const QString text = suggestion();
            if (!text.isEmpty()) { insertPlainText(text); event->accept(); return true; }
        }
        return QPlainTextEdit::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (!m_composing && event->key() == Qt::Key_Escape) {
            // Like Escape in Terminal: stop a working turn, otherwise leave the field.
            if (!(escape && escape())) clearFocus();
            event->accept(); return;
        }
        if (!m_composing && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && !event->modifiers().testFlag(Qt::ShiftModifier)) {
            submit(); event->accept(); return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
    void dropEvent(QDropEvent *event) override {
        if (isReadOnly()) { event->ignore(); return; }
        const auto *mime = event->mimeData();
        if (mime->hasImage() || mime->hasUrls()) {
            // We handle attachment insertion ourselves, so finish the native
            // drag first. Otherwise Qt retains its drag-feedback cursor and
            // autoscroll timer; that cursor overrides the real caret's paint
            // position even after subsequent mouse/keyboard navigation.
            QDragLeaveEvent leave;
            QPlainTextEdit::dragLeaveEvent(&leave);
            // QTextControl otherwise selects its insertion range after a drop,
            // making the next insertion replace the newly added reference.
            setTextCursor(cursorForPosition(event->position().toPoint()));
            insertFromMimeData(mime);
            event->acceptProposedAction();
            return;
        }
        QPlainTextEdit::dropEvent(event);
    }
    void inputMethodEvent(QInputMethodEvent *event) override {
        m_composing = !event->preeditString().isEmpty();
        // A queued IME commit can arrive after capture/delivery made the field
        // read-only; QPlainTextEdit still accepts that commit unless guarded.
        if(isReadOnly()){event->ignore();return;}
        QPlainTextEdit::inputMethodEvent(event);
    }
private:
    bool m_composing = false;
};
}

MessageComposer::MessageComposer(QWidget *parent) : QWidget(parent)
{
    setObjectName("messageComposer");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 4, 0, 0); layout->setSpacing(0);
    m_toolbar = new ComposerToolbar(this); layout->addWidget(m_toolbar);
    m_attachmentsChip = new ToolbarChip; m_attachmentsChip->setObjectName("attachmentsChip");
    m_attachmentsChip->setTone(ChipTone::Neutral); m_attachmentsChip->setIconName("attachment");
    m_attachmentList = new QWidget; m_attachmentList->setObjectName("attachmentsPopover");
    m_attachmentsLayout = new QVBoxLayout(m_attachmentList); m_attachmentsLayout->setContentsMargins(8, 8, 8, 8); m_attachmentsLayout->setSpacing(2);
    m_attachmentsChip->setPopoverContent(m_attachmentList);
    m_toolbar->add(ComposerToolbar::Slot::Attachments, m_attachmentsChip);
    m_attachmentsChip->popover()->installEventFilter(this);   // the preview closes with the list
    m_preview = new QLabel(this, Qt::ToolTip); m_preview->setObjectName("attachmentPreview"); m_preview->hide();
    openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    m_input = new QWidget(this); m_input->setObjectName("messageInputArea");
    auto *inputLayout = new QVBoxLayout(m_input); inputLayout->setContentsMargins(0, 0, 0, 0); inputLayout->setSpacing(7);
    layout->addWidget(m_input);
    auto *edit = new ComposeEdit(this); m_editor = edit; m_toolbar->setFocusFallback(edit); edit->setObjectName("messageInput");
    edit->setAccessibleName(tr("Message to selected agent")); edit->setPlaceholderText(tr("Message this agent…"));
    edit->setFixedHeight(76); edit->setTabChangesFocus(true);
    inputLayout->addWidget(edit);
    auto *actions = new QHBoxLayout; m_actions = actions; actions->setContentsMargins(0, 0, 0, 0); actions->setSpacing(8);
    m_attach = new QPushButton; m_attach->setObjectName("attachMessageFile");
    m_attach->setFixedSize(34, 34); m_attach->setIconSize(QSize(19, 19));
    m_attach->setToolTip(tr("Attach files, paste an image, or drop files onto this session"));
    m_attach->setAccessibleName(tr("Attach files to message"));
    m_reference = new QPushButton; m_reference->setObjectName("referenceMessageFile");
    m_reference->setFixedSize(34, 34); m_reference->setIconSize(QSize(19, 19));
    m_reference->setAccessibleName(tr("Reference an existing file"));
    m_fileMenu = new QMenu(m_attach);
    m_attachFileAction = m_fileMenu->addAction(tr("Attach files…"), this, &MessageComposer::attachFiles);
    m_referenceFileAction = m_fileMenu->addAction(tr("Reference an existing file…"), this, &MessageComposer::referenceFile);
    m_referenceFileAction->setObjectName("referenceExistingFileAction");
    m_status = new StatusMessage; m_status->setObjectName("messageStatus"); m_status->setFixedHeight(34);
    m_status->setTextFormat(Qt::PlainText); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_retry = new QPushButton(tr("Allow retry")); m_retry->setObjectName("allowMessageRetry"); m_retry->hide();
    m_retry->setToolTip(tr("Check the terminal before sending again to avoid a duplicate message"));
    m_refreshStatus = new QPushButton; m_refreshStatus->setObjectName("refreshSessionStatus"); m_refreshStatus->setFixedSize(34, 34);
    m_refreshStatus->setIconSize(QSize(18, 18)); m_refreshStatus->setToolTip(tr("Refresh this session"));
    m_refreshStatus->setAccessibleName(m_refreshStatus->toolTip()); m_refreshStatus->hide();
    connect(m_refreshStatus, &QPushButton::clicked, this, [this] { emit sessionRefreshRequested(m_key); });
    m_send = new QPushButton(tr("Send")); m_send->setObjectName("sendMessage");
    m_send->setIconSize(QSize(14, 14)); m_send->setLayoutDirection(Qt::RightToLeft); // Arrow after the label.
    m_send->setToolTip(tr("Send: Enter; new line: Shift+Enter"));
    m_stop = new QPushButton(tr("Stop")); m_stop->setObjectName("interruptAgent"); m_stop->hide();
    connect(m_stop,&QPushButton::clicked,this,[this]{emit interruptRequested(m_key);});
    m_settings = new SettingsButton(this); m_settings->setObjectName("sessionModelSettings"); m_settings->setFixedHeight(34);
    m_settings->setAccessibleName(tr("Model and reasoning effort for this session"));
    m_settingsPopup = new ChipPopover(this); m_settingsPopup->setObjectName("sessionSettingsPopup");
    auto *settingsLayout = new QVBoxLayout(m_settingsPopup); settingsLayout->setContentsMargins(14, 12, 14, 12); settingsLayout->setSpacing(8);
    auto *modelLabel = m_modelLabel = new QLabel(tr("Model"));
    m_models = new QComboBox; m_models->setObjectName("sessionModelChoice"); m_models->setAccessibleName(tr("Model"));
    m_models->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_models->setMinimumContentsLength(12);
    modelLabel->setBuddy(m_models); settingsLayout->addWidget(modelLabel); settingsLayout->addWidget(m_models);
    // OpenCode chooses in its grouped picker; m_models stays hidden there as the selected value.
    m_openCodeModels = new OpenCodeModelPicker; m_openCodeModels->setObjectName("sessionOpenCodeModels"); m_openCodeModels->hide();
    if (auto *list = m_openCodeModels->findChild<QWidget *>("openCodeModelList")) list->setMaximumHeight(320);
    settingsLayout->addWidget(m_openCodeModels);
    auto *effortLabel = m_effortLabel = new QLabel(tr("Reasoning effort"));
    m_efforts = new QComboBox; m_efforts->setObjectName("sessionEffortChoice"); m_efforts->setAccessibleName(tr("Reasoning effort"));
    effortLabel->setBuddy(m_efforts); settingsLayout->addWidget(effortLabel); settingsLayout->addWidget(m_efforts);
    m_settingsHint = new QLabel; m_settingsHint->setObjectName("sessionSettingsHint"); m_settingsHint->setWordWrap(true); m_settingsHint->setTextFormat(Qt::PlainText);
    settingsLayout->addWidget(m_settingsHint);
    auto *settingsActions = new QHBoxLayout; settingsActions->setSpacing(8); settingsActions->addStretch();
    auto *cancelSettings = new QPushButton(tr("Cancel")); cancelSettings->setObjectName("cancelSessionSettings");
    cancelSettings->setToolTip(tr("Close without applying changes"));
    m_applySettings = new QPushButton(tr("Apply")); m_applySettings->setObjectName("applySessionSettings");
    m_settingsTerminal = new QPushButton(tr("Open Terminal")); m_settingsTerminal->setObjectName("sessionSettingsTerminal"); m_settingsTerminal->hide();
    connect(m_settingsTerminal, &QPushButton::clicked, this, [this] {
        if (m_popupKey != m_key || !m_settingsTerminalAvailable) return;
        m_settingsPopup->hide(); emit settingsTerminalRequested(m_key);
    });
    settingsActions->addWidget(m_settingsTerminal);
    settingsActions->addWidget(cancelSettings); settingsActions->addWidget(m_applySettings); settingsLayout->addLayout(settingsActions);
    connect(cancelSettings, &QPushButton::clicked, m_settingsPopup, &QWidget::hide);
    connect(m_settings, &QPushButton::clicked, this, &MessageComposer::openSettings);
    connect(m_models, &QComboBox::currentIndexChanged, this, &MessageComposer::updateSettingsEfforts);
    connect(m_efforts, &QComboBox::currentIndexChanged, this, &MessageComposer::updateSettingsApply);
    connect(m_openCodeModels, &OpenCodeModelPicker::chosen, this, &MessageComposer::chooseOpenCodeModel);
    connect(m_openCodeModels, &OpenCodeModelPicker::catalogRequested, this, [this] {
        if (m_popupKey == m_key && m_modelDefaultEffort) emit openCodeCatalogRequested(m_key);
    });
    connect(m_openCodeModels, &OpenCodeModelPicker::favoriteRequested, this, [this](const QString &value, bool favorite) {
        // Only the confirmed answer changes the star; a failure is never sent again on its own.
        if (m_popupKey != m_key || !m_modelDefaultEffort || !m_openCodeSupported || !m_openCodeOnline) return;
        m_openCodeError.clear(); m_openCodeModels->setFavoritePending(value, true); updateSettingsApply();
        emit openCodeFavoriteRequested(m_key, value, favorite);
    });
    connect(m_applySettings, &QPushButton::clicked, this, [this] {
        if (m_popupKey != m_key || !m_applySettings->isEnabled()) return;
        const QString model = m_models->currentData().toString(), effort = m_efforts->currentData().toString();
        m_settingsPopup->hide(); emit settingsRequested(m_key, model, effort);
    });
    actions->addWidget(m_attach); actions->addWidget(m_reference); actions->addStretch(0); actions->addWidget(m_status, 1); actions->addWidget(m_refreshStatus); actions->addWidget(m_retry); actions->addWidget(m_settings); actions->addWidget(m_stop); actions->addWidget(m_send);
    inputLayout->addLayout(actions);
    connect(edit, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_loading || m_key.isEmpty()) return;
        if (m_drafts.value(m_key).sending || isWaitingToSend(m_key) || m_renamingKeys.contains(m_key)) {
            m_loading = true; restoreDraft(); m_loading = false; return;
        }
        // Programmatic insertion can supersede a pending restore; actual input
        // stays read-only until its exact draft has loaded.
        m_loadingKeys.remove(m_key);
        auto &draft = m_drafts[m_key]; draft.text = m_editor->toPlainText();
        draft.position = m_editor->textCursor().position(); draft.anchor = m_editor->textCursor().anchor();
        if (!draft.error && !draft.uncertain) draft.notice.clear();
        saveDraft(m_key);
        updateControls();
    });
    auto rememberCursor = [this] {
        if (m_loading || m_key.isEmpty() || m_drafts[m_key].sending || isWaitingToSend(m_key) || isDraftLoading(m_key)) return;
        auto &draft = m_drafts[m_key];
        if (draft.position == m_editor->textCursor().position() && draft.anchor == m_editor->textCursor().anchor()) return;
        draft.position = m_editor->textCursor().position(); draft.anchor = m_editor->textCursor().anchor();
        saveDraft(m_key); updateControls();
    };
    connect(edit, &QPlainTextEdit::cursorPositionChanged, this, rememberCursor);
    connect(edit, &QPlainTextEdit::selectionChanged, this, rememberCursor);
    edit->submit = [this]() { send(); };
    edit->escape = [this]() { return requestInterrupt(); };
    edit->suggestion = [this]() { return suggestion(); };
    edit->attachFile = [this](const QString &path) { attachFile(path); };
    edit->attachImage = [this](const QImage &image) {
        if (qint64(image.width()) * image.height() > 32000000) { showError(tr("Image is too large. Use an image below 32 megapixels.")); return; }
        QByteArray data; QBuffer buffer(&data); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) { showError(tr("Could not read the pasted image.")); return; }
        addAttachment(QStringLiteral("pasted-image.png"), QStringLiteral("image/png"), data);
    };
    // Composer-scope commands: Ctrl+Enter sends like Enter; setPushEnabled adds
    // Send and push now. Their chords follow ShortcutRegistry.
    m_sendAction = new QAction(Shortcuts::title(*Shortcuts::command("message.send")), edit); m_sendAction->setObjectName("shortcut:message.send");
    m_pushAction = new QAction(Shortcuts::title(*Shortcuts::command("message.push")), edit); m_pushAction->setObjectName("shortcut:message.push");
    for (auto *action : {m_sendAction, m_pushAction}) action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_sendAction, &QAction::triggered, this, [this, edit] { if (!edit->composing()) send(); });
    connect(m_pushAction, &QAction::triggered, this, [this, edit] { if (!edit->composing()) push(); });
    edit->addAction(m_sendAction);
    connect(&ShortcutRegistry::instance(), &ShortcutRegistry::changed, this, &MessageComposer::applyShortcuts);
    applyShortcuts();
    connect(m_attach, &QPushButton::clicked, this, [this] { if (!m_attach->menu()) attachFiles(); });
    connect(m_reference, &QPushButton::clicked, this, &MessageComposer::referenceFile);
    connect(m_send, &QPushButton::clicked, this, &MessageComposer::send);
    connect(m_retry, &QPushButton::clicked, this, [this]() {
        if (m_key.isEmpty()) return;
        auto &draft = m_drafts[m_key]; draft.uncertain = false; draft.notice.clear(); draft.error = false;
        saveDraft(m_key);
        updateControls(); m_editor->setFocus();
    });
    auto *retrySave = new QTimer(this); retrySave->setInterval(2000);
    connect(retrySave, &QTimer::timeout, this, [this] {
        const auto pending = m_failedSaves;
        for (const auto &key : pending) saveDraft(key);
        if (!pending.isEmpty()) updateControls();
    }); retrySave->start();
    m_draftStore.indexAsync(this, [this](const QHash<QString, ComposerDraftSummary> &index) {
        for (auto it = index.cbegin(); it != index.cend(); ++it) {
            if (!m_drafts.contains(it.key())) m_storedDrafts.insert(it.key());
            notifyDraft(it.key());
        }
    });
    setTheme(true); updateControls();
}

MessageComposer::~MessageComposer() = default;

void MessageComposer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool narrow = width() < 400;
    if (m_narrow == narrow) return;
    m_narrow = narrow;
    updateControls();
}

void MessageComposer::setSessionKey(const QString &key)
{
    if (key == m_key) return;
    ++m_navigationRevision;
    setFileReferenceTarget({}, {}, {});
    m_sessionStatus.clear(); m_sessionStatusError = false; m_sessionStatusBlocksInput = false;
    m_settingsPopup->hide();
    // Another session or none: the OpenCode picker's context ends here, also when the same one returns.
    m_openCodeContext.clear(); resetOpenCodeModels();
    m_toolbar->closePopovers(); m_preview->hide();
    ensureDraftLoaded(key);
    m_key = key; m_loading = true;
    restoreDraft();
    m_loading = false; rebuildAttachments(); updateControls(); updatePlaceholder();
}

void MessageComposer::ensureDraftLoaded(const QString &key)
{
    if (!key.isEmpty() && !m_drafts.contains(key)) {
        m_drafts.insert(key, {}); m_loadingKeys.insert(key);
        const auto revision = m_saveRevision.value(key);
        m_draftStore.loadAsync(key, this, [this, key, revision](const ComposerDraft &draft) {
            if (!m_loadingKeys.contains(key) || m_saveRevision.value(key) != revision) return;
            m_loadingKeys.remove(key); m_drafts[key] = draft; notifyDraft(key);
            if (m_key == key) {
                m_loading = true; restoreDraft(); m_loading = false;
                rebuildAttachments(); updateControls(); updatePlaceholder();
            }
            emit draftLoaded(key);
        });
    }
}

bool MessageComposer::insertVoiceText(const VoiceTarget &target, const QString &text, std::function<void(bool)> saved)
{
    if (target.key.isEmpty() || target.key != m_key || target.revision != m_saveRevision.value(m_key)
        || target.navigation != m_navigationRevision || !canEditAttachments() || isPreparingSending(m_key)
        || !isInputVisible() || m_editor->isReadOnly() || text.isEmpty()) return false;
    // Dictation appends at the captured caret without replacing a selection.
    auto cursor = m_editor->textCursor(); cursor.clearSelection();
    QString insertion = text;
    const QString existing = m_editor->toPlainText();
    if (cursor.position() > 0 && !existing[cursor.position()-1].isSpace()) insertion.prepend(' ');
    if (cursor.position() < existing.size() && !existing[cursor.position()].isSpace()) insertion.append(' ');
    cursor.insertText(insertion); m_editor->setTextCursor(cursor);
    m_draftStore.saveDurableAsync(m_key, m_drafts.value(m_key), this,
        [saved](bool ok, const ComposerDraft &) { saved(ok); });
    return true;
}

void MessageComposer::restoreDraft()
{
    const auto draft = m_drafts.value(m_key);
    m_editor->setPlainText(draft.sending ? QString() : draft.text);
    auto cursor = m_editor->textCursor();
    const int length = m_editor->toPlainText().size();
    cursor.setPosition(qBound(0, draft.anchor, length));
    cursor.setPosition(qBound(0, draft.position, length), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
}

void MessageComposer::setInterruptAvailability(bool working, bool enabled, const QString &reason)
{
    m_stop->setVisible(working); m_stop->setEnabled(enabled);
    m_stop->setToolTip(reason.isEmpty()?tr("Interrupt the current turn, like Escape in Terminal. The session stays open; queued messages follow the agent's native behavior."):reason);
}

void MessageComposer::setSuggestion(const QString &key, const QString &text, double at)
{
    if (key.isEmpty()) return;
    if (text.isEmpty()) { m_suggestions.remove(key); m_dismissedSuggestions.remove(key); }
    else if (m_dismissedSuggestions.value(key, -1) != at) m_suggestions.insert(key, {text, at});
    if (key == m_key) updatePlaceholder();
}

QString MessageComposer::suggestion() const { return isWaitingToSend(m_key) ? QString() : m_suggestions.value(m_key).text; }

void MessageComposer::updatePlaceholder()
{
    const QString text = suggestion();
    m_editor->setPlaceholderText(text.isEmpty() ? tr("Message this agent…") : text);
}

bool MessageComposer::requestInterrupt()
{
    if (m_key.isEmpty() || m_stop->isHidden() || !m_stop->isEnabled()) return false;
    emit interruptRequested(m_key); return true;
}

bool MessageComposer::offerDraft(const QString &key, const QString &text)
{
    if (((m_recording || m_sessionStatusBlocksInput) && key==m_key) || key.isEmpty() || isDraftLoading(key) || text.trimmed().isEmpty() || isWaitingToSend(key)) return false;
    auto &draft = m_drafts[key];
    // A draft the user started, or a message on its way, always wins.
    if (!draft.text.trimmed().isEmpty() || !draft.attachments.isEmpty() || draft.sending) return false;
    draft.text = text; draft.position = draft.anchor = int(text.size()); draft.notice.clear();
    if (key == m_key) { m_loading = true; restoreDraft(); m_loading = false; }
    saveDraft(key); notifyDraft(key); updateControls(); return true;
}

void MessageComposer::setAttachmentAvailability(bool available, const QString &reason)
{
    m_attachmentsAvailable=available;m_attachmentsReason=reason;updateControls();
}

void MessageComposer::setAvailability(bool available, const QString &reason)
{
    if (available != m_available || reason != m_unavailableReason) QToolTip::hideText();
    m_available = available; m_unavailableReason = reason; updateControls();
}

void MessageComposer::setSessionStatus(const QString &text, bool error, bool blockInput)
{
    blockInput = blockInput && !text.isEmpty();
    if (m_sessionStatus == text && m_sessionStatusError == error && m_sessionStatusBlocksInput == blockInput) return;
    m_sessionStatus = text; m_sessionStatusError = error; m_sessionStatusBlocksInput = blockInput;
    updateControls();
}

bool MessageComposer::isSending(const QString &key) const { return m_drafts.value(key).sending || m_preparingKeys.contains(key); }

bool MessageComposer::hasDraft(const QString &key) const
{
    if (!m_drafts.contains(key)) {
        return m_storedDrafts.contains(key);
    }
    if (m_loadingKeys.contains(key)) return m_storedDrafts.contains(key) || (m_renamingKeys.contains(key)
        && (!m_drafts.value(key).text.trimmed().isEmpty() || !m_drafts.value(key).attachments.isEmpty()));
    const auto draft = m_drafts.value(key);
    return !draft.sending && (!draft.text.trimmed().isEmpty() || !draft.attachments.isEmpty());
}

void MessageComposer::notifyDraft(const QString &key)
{
    if (key.isEmpty() || hasDraft(key) == m_unsent.contains(key)) return;
    if (hasDraft(key)) m_unsent.insert(key); else m_unsent.remove(key);
    emit draftChanged(key);
}

bool MessageComposer::draftMatches(const QString &key, const QString &text, const QList<MessageAttachment> &attachments) const
{
    const auto draft=m_drafts.value(key);
    if(draft.text!=text || draft.attachments.size()!=attachments.size())return false;
    for(int i=0;i<attachments.size();++i)if(draft.attachments[i].name!=attachments[i].name
        || draft.attachments[i].mime!=attachments[i].mime || draft.attachments[i].data!=attachments[i].data
        || draft.attachments[i].reference!=attachments[i].reference)return false;
    return true;
}

bool MessageComposer::saveDraft(const QString &key)
{
    if (key.isEmpty() || m_loadingKeys.contains(key) || m_preparingKeys.contains(key) || m_preparingWaiting.contains(key)) return false;
    const auto &draft = m_preservedDrafts.contains(key) ? m_preservedDrafts[key] : m_drafts[key];
    const auto revision = ++m_saveRevision[key];
    m_savingKeys.insert(key);
    m_draftStore.saveAsync(key, draft, this, [this, key, revision](bool saved, const ComposerDraft &written) {
        if (m_saveRevision.value(key) != revision || !m_drafts.contains(key)) return;
        auto &current = m_preservedDrafts.contains(key) ? m_preservedDrafts[key] : m_drafts[key];
        current.storageError = written.storageError; current.attachmentHashes = written.attachmentHashes;
        if (m_preservedDrafts.contains(key)) m_drafts[key].storageError = written.storageError;
        m_savingKeys.remove(key);
        if (saved) m_failedSaves.remove(key); else m_failedSaves.insert(key);
        if (key == m_key) updateControls();
    });
    return true;
}

bool MessageComposer::setWaitingToSend(const QString &key, const QString &reason)
{
    if (key.isEmpty()) return false;
    if (reason.isEmpty()) { if (m_waitingSends.remove(key)) ++m_saveRevision[key]; m_preparingWaiting.remove(key); updateControls(); return true; }
    if ((m_recording && key==m_key) || isSending(key) || isWaitingToSend(key) || isDraftLoading(key) || m_drafts.value(key).uncertain
        || !m_drafts.value(key).storageError.isEmpty()) return false;
    prepareWaitingToSendAsync(key, reason, [](bool) {}); return true;
}

void MessageComposer::prepareWaitingToSendAsync(const QString &key, const QString &reason, std::function<void(bool)> done)
{
    if ((m_recording && key==m_key) || key.isEmpty() || reason.isEmpty() || isSending(key) || m_preparingWaiting.contains(key)
        || isDraftLoading(key) || m_drafts.value(key).uncertain) { done(false); return; }
    m_waitingSends.insert(key, reason); m_preparingWaiting.insert(key);
    const auto revision = ++m_saveRevision[key]; m_savingKeys.insert(key);
    if (key == m_key) { m_settingsPopup->hide(); updateControls(); updatePlaceholder(); }
    m_draftStore.saveDurableAsync(key, m_drafts.value(key), this, [this, key, revision, done = std::move(done)](bool okay, const ComposerDraft &written) {
        if (m_saveRevision.value(key) != revision || !m_preparingWaiting.contains(key)) { done(false); return; }
        m_preparingWaiting.remove(key); m_savingKeys.remove(key); m_drafts[key].storageError = written.storageError;
        if (!okay) { m_waitingSends.remove(key); m_failedSaves.insert(key); }
        if (key == m_key) { updateControls(); updatePlaceholder(); }
        done(okay);
    });
}

bool MessageComposer::setSending(const QString &key, bool preserveDraft)
{
    if ((m_recording && key==m_key) || key.isEmpty() || isSending(key) || m_preparingWaiting.contains(key) || isDraftLoading(key)
        || (m_drafts.value(key).uncertain && !preserveDraft)) return false;
    prepareSendingAsync(key, preserveDraft, [](bool) {}); return true;
}

void MessageComposer::prepareSendingAsync(const QString &key, bool preserveDraft, std::function<void(bool)> done)
{
    if ((m_recording && key==m_key) || key.isEmpty() || isSending(key) || m_preparingWaiting.contains(key) || isDraftLoading(key) || (m_drafts.value(key).uncertain && !preserveDraft)) { done(false); return; }
    m_waitingSends.remove(key); m_preparingKeys.insert(key);
    if (preserveDraft) m_preservedDrafts.insert(key, m_drafts.value(key));
    auto &draft = m_drafts[key]; draft.sending = true; draft.error = false; draft.notice = tr("Saving before sending…");
    // Preserve a newer draft on Activity retry, but also durably record that a
    // delivery may have begun. Recovery never repeats the older message.
    auto snapshot = m_preservedDrafts.value(key, draft); snapshot.sending = true;
    const auto revision = ++m_saveRevision[key]; m_savingKeys.insert(key);
    if (key == m_key) updateControls();
    m_draftStore.saveDurableAsync(key, snapshot, this, [this, key, revision, done = std::move(done)](bool okay, const ComposerDraft &written) {
        if (m_saveRevision.value(key) != revision || !m_preparingKeys.contains(key)) { done(false); return; }
        m_preparingKeys.remove(key); m_savingKeys.remove(key);
        auto &draft = m_drafts[key]; draft.storageError = written.storageError;
        if (!okay) {
            draft.sending = false; draft.notice.clear(); m_preservedDrafts.remove(key); m_failedSaves.insert(key);
            if (key == m_key) updateControls(); done(false); return;
        }
        m_failedSaves.remove(key); draft.notice = tr("Sending…");
        if (key == m_key) {
            if (m_send->hasFocus()) m_editor->setFocus(Qt::OtherFocusReason);
            m_loading = true; m_editor->clear(); m_loading = false;
            rebuildAttachments(); updateControls();
        }
        notifyDraft(key); done(true);
    });
}

void MessageComposer::deliveryFinished(const QString &key, bool ok, const QString &detail, bool uncertain)
{
    m_waitingSends.remove(key); m_preparingKeys.remove(key); m_preparingWaiting.remove(key);
    if(m_preservedDrafts.contains(key)) {
        m_drafts[key]=m_preservedDrafts.take(key);
        saveDraft(key);
        if(key==m_key){m_loading=true;restoreDraft();m_loading=false;rebuildAttachments();updateControls();}
        notifyDraft(key); return;
    }
    auto &draft = m_drafts[key]; const bool wasSending = draft.sending;
    draft.sending = false; draft.error = !ok; draft.uncertain = uncertain;
    draft.notice = ok ? tr("Submitted to terminal") : detail;
    if (ok) { draft.text.clear(); draft.attachments.clear(); draft.attachmentHashes.clear(); draft.nextAttachmentNumber = 1; }
    saveDraft(key);
    notifyDraft(key);
    if (key != m_key) return;
    if (ok || wasSending) { m_loading = true; restoreDraft(); m_loading = false; }
    rebuildAttachments(); updateControls();
}

void MessageComposer::renameDraft(const QString &oldKey, const QString &newKey)
{
    if (oldKey == newKey || !m_drafts.contains(oldKey) || !m_drafts.value(oldKey).storageError.isEmpty() || isSending(oldKey) || isWaitingToSend(oldKey)
        || isDraftLoading(oldKey) || isSending(newKey) || isWaitingToSend(newKey) || m_renamingKeys.contains(newKey)) return;
    const auto destination = m_drafts.value(newKey);
    if (m_drafts.contains(newKey) && (!destination.text.isEmpty() || !destination.attachments.isEmpty() || !destination.storageError.isEmpty())) {
        if (m_key == oldKey) { m_key.clear(); setSessionKey(newKey); showError(tr("Another draft already exists here. Recover the previous one from Saved drafts.")); }
        return;
    }
    const auto moved = m_preservedDrafts.value(oldKey, m_drafts.value(oldKey));
    m_drafts.insert(newKey, moved); m_loadingKeys.insert(oldKey); m_loadingKeys.insert(newKey);
    m_renamingKeys.insert(oldKey); m_renamingKeys.insert(newKey);
    const auto revision = ++m_saveRevision[oldKey], destinationRevision = ++m_saveRevision[newKey];
    if (m_key == oldKey || m_key == newKey) updateControls();
    m_draftStore.renameAsync(oldKey, newKey, moved, this, [this, oldKey, newKey, revision, destinationRevision](bool okay, const ComposerDraft &written) {
        if (m_saveRevision.value(oldKey) != revision || m_saveRevision.value(newKey) != destinationRevision) return;
        m_renamingKeys.remove(oldKey); m_renamingKeys.remove(newKey); m_loadingKeys.remove(oldKey); m_loadingKeys.remove(newKey);
        if (!okay) {
            m_drafts[oldKey].storageError = written.storageError;
            m_drafts.remove(newKey);
            if (m_key == newKey) { m_key.clear(); setSessionKey(newKey); }
            else if (m_key == oldKey) updateControls();
            return;
        }
        m_storedDrafts.remove(oldKey); m_storedDrafts.insert(newKey);
        m_drafts.remove(oldKey); m_preservedDrafts.remove(oldKey); m_failedSaves.remove(oldKey); m_savingKeys.remove(oldKey);
        m_drafts[newKey] = written;
        notifyDraft(oldKey); notifyDraft(newKey);
        if (m_key == oldKey) { m_key.clear(); setSessionKey(newKey); }
        else if (m_key == newKey) { m_loading = true; restoreDraft(); m_loading = false; rebuildAttachments(); updateControls(); }
    });
}

void MessageComposer::send()
{
    if (m_key.isEmpty()) return;
    if (!m_send->isEnabled()) {
        const auto draft = m_drafts.value(m_key);
        if (!draft.sending && (!draft.text.trimmed().isEmpty() || !draft.attachments.isEmpty()))
            QToolTip::showText(m_editor->mapToGlobal(QPoint(10, m_editor->height())), m_status->text().toHtmlEscaped(), m_editor, {}, 5000);
        return;
    }
    QToolTip::hideText();
    const auto draft = m_drafts.value(m_key);
    // Sending answers the suggestion; the same one never returns.
    if (m_suggestions.contains(m_key)) {
        m_dismissedSuggestions.insert(m_key, m_suggestions.take(m_key).at); updatePlaceholder();
    }
    if (std::exchange(m_pushing, false)) emit pushRequested(m_key, draft.text);
    emit sendRequested(m_key, draft.text, draft.attachments);
}

// Send and push now: the typed message goes the ordinary way and the window
// pushes it once the agent queues it. With nothing typed (a message on its way
// leaves the field empty), the window sends the existing queue now.
void MessageComposer::push()
{
    if (m_key.isEmpty()) return;
    const auto draft = m_drafts.value(m_key);
    if (draft.sending || (draft.text.trimmed().isEmpty() && draft.attachments.isEmpty())) { emit sendNowRequested(m_key); return; }
    m_pushing = true; send(); m_pushing = false;
}

void MessageComposer::setPushEnabled(bool enabled)
{
    if (m_pushEnabled == enabled) return;
    m_pushEnabled = enabled;
    if (enabled) m_editor->addAction(m_pushAction); else m_editor->removeAction(m_pushAction);
    applyShortcuts();
}

void MessageComposer::applyShortcuts()
{
    const auto &registry = ShortcutRegistry::instance();
    m_sendAction->setShortcuts(registry.keys("message.send"));
    m_pushAction->setShortcuts(m_pushEnabled ? registry.keys("message.push") : QList<QKeySequence>());
}

void MessageComposer::attachFiles()
{
    if (!canAttachFiles()) return;
    const QString key = m_key;
    const auto files = QFileDialog::getOpenFileNames(this, tr("Attach files"), {}, tr("All files (*)"));
    if (key != m_key) return;
    for (const auto &file : files) attachFile(file);
}

void MessageComposer::setFileReferenceTarget(const QString &key, const QString &host, const QString &machine, const QString &context)
{
    const QString bound = key == m_key ? key : QString();
    if (m_referenceKey == bound && m_referenceHost == host && m_referenceMachine == machine && m_referenceContext == context) return;
    m_referenceKey = bound; m_referenceHost = host; m_referenceMachine = machine; m_referenceContext = context;
    ++m_referenceRevision;
    updateControls();
}

void MessageComposer::referenceFile()
{
    if (m_referenceKey.isEmpty() || m_referenceKey != m_key || !canEditAttachments() || isPreparingSending(m_key)) return;
    const auto target = voiceTarget();
    const auto revision = m_referenceRevision;
    const QPointer<MessageComposer> owner(this);
    const auto current = [owner, target, revision] {
        if (!owner) return false;
        const auto now = owner->voiceTarget();
        return now.key == target.key && now.navigation == target.navigation && now.revision == target.revision
            && revision == owner->m_referenceRevision && owner->m_referenceKey == owner->m_key
            && owner->canEditAttachments() && !owner->isPreparingSending(owner->m_key);
    };
    const bool local = m_referenceHost.isEmpty();
    const auto machine = local ? tr("this computer") : m_referenceMachine.isEmpty() ? m_referenceHost : m_referenceMachine;
    const QPointer<QDialog> dialog = new QDialog(this); dialog->setObjectName("fileReferenceDialog");
    dialog->setWindowTitle(tr("Reference file — %1").arg(machine));
    auto *layout = new QVBoxLayout(dialog);
    auto *hint = new QLabel(local
        ? tr("Insert a path to an existing file on this computer. Only the path is added; the file is not read or uploaded.")
        : tr("Enter the path of an existing file on %1 (%2), not on this computer. Only the path is added; its existence is not checked here.").arg(machine, m_referenceHost));
    hint->setObjectName("fileReferenceHint"); hint->setTextFormat(Qt::PlainText); hint->setWordWrap(true); layout->addWidget(hint);
    auto *row = new QHBoxLayout;
    const QPointer<QLineEdit> path = new QLineEdit; path->setObjectName("fileReferencePath"); path->setAccessibleName(tr("File path on %1").arg(machine));
    path->setPlaceholderText(local ? tr("Absolute file path") : tr("/path/to/file, ~/file or C:\\path\\file")); row->addWidget(path, 1);
    if (local) {
        auto *browse = new QPushButton(tr("Browse…")); browse->setObjectName("browseReferenceFile"); row->addWidget(browse);
        connect(browse, &QPushButton::clicked, dialog, [owner, dialog, path] {
            if (!owner || !dialog) return;
            const QPointer<QFileDialog> picker = new QFileDialog(dialog, tr("Reference an existing file"));
            picker->setObjectName("fileReferencePicker"); picker->setFileMode(QFileDialog::ExistingFile);
            picker->setNameFilter(tr("All files (*)"));
            const auto outcome = picker->exec();
            const auto selected = picker && outcome == QDialog::Accepted ? picker->selectedFiles().value(0) : QString();
            delete picker.data();
            if (owner && dialog && path && !selected.isEmpty()) path->setText(selected);
        });
    }
    layout->addLayout(row);
    auto *error = new QLabel; error->setObjectName("fileReferenceError"); error->setWordWrap(true); error->setTextFormat(Qt::PlainText); layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *insert = buttons->addButton(tr("Insert reference"), QDialogButtonBox::AcceptRole); insert->setObjectName("insertFileReference");
    layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QString reference;
    connect(buttons, &QDialogButtonBox::accepted, dialog, [&, owner, dialog] {
        if (!dialog) return;
        if (!current()) {
            dialog->reject(); return;
        }
        QString value = path->text();
        if (value.isEmpty() || value.toUtf8().size() > 32768
            || std::any_of(value.cbegin(), value.cend(), [](QChar c) { return c.category() == QChar::Other_Control; })) {
            error->setText(tr("Enter a file path without line breaks or control characters.")); return;
        }
        if (local) {
            if (value.startsWith("~/")) value = QDir::homePath() + value.mid(1);
            const QFileInfo info(value);
            if (!info.isAbsolute() || !info.isFile()) { error->setText(tr("Choose an existing file on this computer.")); return; }
            value = info.absoluteFilePath();
        } else if (!((value.startsWith('/') && !value.startsWith("//")) || value.startsWith("~/")
            || MachinePlatform::absolute(value, MachinePlatform::Style::Windows))) {
            error->setText(tr("Enter an absolute path or ~/ path on %1.").arg(machine)); return;
        }
        // JSON quoting preserves spaces, literal quotes and Windows separators.
        // These are draft instructions, never a shell command or an upload.
        const auto quoted = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        reference = tr("Use the existing file at %1.").arg(QString::fromUtf8(quoted.mid(1, quoted.size() - 2)));
        const auto text = owner->m_editor->toPlainText();
        const auto cursor = owner->m_editor->textCursor();
        const auto begin = cursor.selectionStart(), end = cursor.selectionEnd();
        if (begin > 0 && !text[begin - 1].isSpace()) reference.prepend(' ');
        if (end < text.size() && !text[end].isSpace()) reference.append(' ');
        auto result = text;
        result.replace(begin, end - begin, reference);
        if (result.toUtf8().size() > MaxTextBytes) {
            error->setText(tr("Message exceeds 64 KiB.")); return;
        }
        dialog->accept();
    });
    dialog->resize(520, dialog->sizeHint().height()); path->setFocus();
    const auto outcome = dialog->exec();
    delete dialog.data();
    if (outcome != QDialog::Accepted || !current()) return;
    auto cursor = owner->m_editor->textCursor(); cursor.insertText(reference); owner->m_editor->setTextCursor(cursor);
    owner->m_editor->setFocus(Qt::OtherFocusReason);
}

void MessageComposer::attachDroppedFiles(const QStringList &paths)
{
    if(!canAttachFiles())return;
    auto cursor=m_editor->textCursor();cursor.clearSelection();m_editor->setTextCursor(cursor);
    for(const auto &path:paths)attachFile(path);
    m_editor->setFocus(Qt::OtherFocusReason);
}

void MessageComposer::attachFile(const QString &path)
{
    QFile file(path); const QFileInfo info(file);
    if (!LocalFile::readable(info) || LocalFile::size(info) > MaxAttachmentBytes) { showError(tr("Choose a file of at most 10 MiB.")); return; }
    if (!file.open(QIODevice::ReadOnly)) { showError(tr("Cannot read %1: %2").arg(info.fileName(), file.errorString())); return; }
    const QByteArray bytes = file.read(MaxAttachmentBytes + 1);
    addAttachment(info.fileName(), QMimeDatabase().mimeTypeForData(bytes).name(), bytes);
}

bool MessageComposer::addAttachment(const QString &name, const QString &mime, const QByteArray &data)
{
    if (!canAttachFiles()) return false;
    auto &draft = m_drafts[m_key]; qsizetype total = data.size();
    for (const auto &attachment : draft.attachments) total += attachment.data.size();
    if (draft.attachments.size() >= MaxAttachments) { showError(tr("Attach up to 8 files per message.")); return false; }
    if (data.isEmpty()) { showError(tr("The attachment is empty.")); return false; }
    if (data.size() > MaxAttachmentBytes || total > MaxTotalBytes) { showError(tr("Attachments: 10 MiB per file, 20 MiB per message.")); return false; }
    QString reference;
    do {
        reference = QString("[%1 #%2]").arg(mime.startsWith("image/") ? "Image" : "File").arg(draft.nextAttachmentNumber++);
    } while (draft.text.contains(reference));
    draft.attachments.append(MessageAttachment{name, mime, data, reference});
    draft.attachmentHashes.clear();
    auto cursor = m_editor->textCursor();
    const auto text = m_editor->toPlainText();
    const bool leading = cursor.selectionStart() > 0 && !text[cursor.selectionStart() - 1].isSpace();
    const bool trailing = cursor.selectionEnd() == text.size() || !text[cursor.selectionEnd()].isSpace();
    cursor.insertText((leading ? " " : "") + reference + (trailing ? " " : ""));
    m_editor->setTextCursor(cursor);
    if (!draft.uncertain) { draft.notice.clear(); draft.error = false; }
    saveDraft(m_key); rebuildAttachments(); updateControls(); m_attachmentsChip->flash(); return true;
}

void MessageComposer::showError(const QString &text)
{
    if (m_key.isEmpty()) return;
    auto &draft = m_drafts[m_key]; draft.notice = text; draft.error = true; updateControls();
}

void MessageComposer::rebuildAttachments()
{
    m_preview->hide(); m_previews.clear();
    // Detach before deleting: a row's remove button may be the caller.
    while (auto *item = m_attachmentsLayout->takeAt(0)) {
        if (auto *row = item->widget()) { row->hide(); row->setParent(nullptr); row->deleteLater(); }
        delete item;
    }
    const auto draft = m_drafts.value(m_key);
    const int count = draft.sending ? 0 : int(draft.attachments.size());
    QStringList names;
    for (int i = 0; i < count; ++i) {
        const auto &attachment = draft.attachments[i];
        names << attachment.reference + ' ' + attachment.name;
        auto *row = new QWidget; row->setObjectName("attachmentRow"); row->setProperty("attachmentIndex", i);
        row->setAttribute(Qt::WA_StyledBackground); row->setAttribute(Qt::WA_Hover); row->setFocusPolicy(Qt::TabFocus);
        row->setCursor(Qt::PointingHandCursor); row->setToolTip(tr("Open %1").arg(attachment.name)); row->installEventFilter(this);
        auto *line = new QHBoxLayout(row); line->setContentsMargins(6, 4, 4, 4); line->setSpacing(8);
        auto *image = new QLabel; image->setFixedSize(30, 22); image->setAlignment(Qt::AlignCenter);
        const QImage thumbnail = attachmentImage(attachment.data, QSize(60, 44));
        image->setPixmap(thumbnail.isNull() ? style()->standardIcon(QStyle::SP_FileIcon).pixmap(18, 18)
            : QPixmap::fromImage(thumbnail).scaled(30, 22, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        auto *text = new QLabel; text->setTextFormat(Qt::PlainText);
        text->setText(attachment.reference + ' ' + text->fontMetrics().elidedText(attachment.name, Qt::ElideMiddle, 180)
            + '\n' + tr("%1 KiB").arg(qMax<qsizetype>(1, (attachment.data.size() + 1023) / 1024)));
        auto *remove = new QPushButton(QStringLiteral("×")); remove->setObjectName("removeAttachment"); remove->setFixedSize(24, 24);
        remove->setAccessibleName(tr("Remove attachment %1").arg(attachment.name));
        connect(remove, &QPushButton::clicked, this, [this, i] { removeAttachment(i); });
        line->addWidget(image); line->addWidget(text, 1); line->addWidget(remove);
        m_attachmentsLayout->addWidget(row); row->show();   // now, not on the next pass: focus and size need it
    }
    m_attachmentsChip->setLabels(tr("%1 attached").arg(count), QString::number(count));
    m_attachmentsChip->setDetail(names.join('\n'));
    m_attachmentsChip->setActive(count > 0);   // closes the popover with the last one
    if (m_attachmentsChip->popover()->isVisible()) m_attachmentsChip->popover()->reposition();
}

void MessageComposer::removeAttachment(int index)
{
    auto &current = m_drafts[m_key]; if (!canEditAttachments() || index < 0 || index >= current.attachments.size()) return;
    QWidget *focus = QApplication::focusWidget();
    const bool keepFocus = focus && focus->objectName() == "removeAttachment" && m_attachmentList->isAncestorOf(focus);
    const auto reference = current.attachments.takeAt(index).reference;
    current.attachmentHashes.clear();
    if (!reference.isEmpty()) {
        auto edit = m_editor->textCursor(); edit.beginEditBlock();
        auto match = m_editor->document()->find(reference);
        while (!match.isNull()) {
            match.removeSelectedText();
            match = m_editor->document()->find(reference, match);
        }
        edit.endEditBlock();
    }
    saveDraft(m_key); rebuildAttachments(); updateControls();
    // Stay in the list: the row that took this place, else the one before it.
    const auto removes = m_attachmentList->findChildren<QPushButton *>("removeAttachment");
    if (keepFocus && !removes.isEmpty()) removes[qMin(index, int(removes.size()) - 1)]->setFocus(Qt::TabFocusReason);
}

void MessageComposer::openAttachment(int index)
{
    const auto draft = m_drafts.value(m_key); if (draft.sending || index < 0 || index >= draft.attachments.size()) return;
    const auto &attachment = draft.attachments[index];
    const QString key = m_key;
    m_attachmentsChip->closePopover();
    AttachmentFiles::prepareAsync({attachment}, this, [this, key](const QJsonArray &files, const QString &error) {
        if (m_key != key) return;
        if (!error.isEmpty() || files.isEmpty()) { showError(error.isEmpty() ? tr("Could not open the attachment.") : error); return; }
        const auto path = files.first().toObject().value("local_path").toString();
        switch (ExternalFile::open(path, ExternalFile::Origin::ThisComputer, openUrl)) {
        case ExternalFile::Outcome::Opened: return;
        // The status line carries cautions, including an open that was not run.
        case ExternalFile::Outcome::Revealed: showError(ExternalFile::revealedNotice(path)); return;
        case ExternalFile::Outcome::RevealFailed: showError(ExternalFile::revealFailedNotice(path)); return;
        case ExternalFile::Outcome::OpenFailed: showError(tr("Could not open the attachment with its default application.")); return;
        }
    });
}

void MessageComposer::showAttachmentPreview(QWidget *row, int index)
{
    const auto draft = m_drafts.value(m_key); if (index < 0 || index >= draft.attachments.size()) return;
    if (!m_previews.contains(index)) m_previews.insert(index, QPixmap::fromImage(attachmentImage(draft.attachments[index].data, QSize(320, 240))));
    const QPixmap image = m_previews.value(index);
    if (image.isNull()) { m_preview->hide(); return; }
    m_preview->setPixmap(image); m_preview->adjustSize();
    const QRect screen = row->screen()->availableGeometry();
    QPoint position = row->mapToGlobal(QPoint(row->width() + 10, 0));
    if (position.x() + m_preview->width() > screen.right()) position.setX(row->mapToGlobal(QPoint(0, 0)).x() - m_preview->width() - 10);
    position.setY(qBound(screen.top() + 8, position.y(), screen.bottom() - m_preview->height() - 8));
    m_preview->move(position); m_preview->show();
}

bool MessageComposer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_attachmentsChip->popover() && event->type() == QEvent::Hide) m_preview->hide();
    auto *row = qobject_cast<QWidget *>(watched);
    if (row && row->objectName() == "attachmentRow") {
        const int index = row->property("attachmentIndex").toInt();
        switch (event->type()) {
        case QEvent::Enter: showAttachmentPreview(row, index); break;
        case QEvent::Leave: m_preview->hide(); break;
        case QEvent::MouseButtonPress: return true;   // take the grab so the release comes here
        case QEvent::MouseButtonRelease:
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) { openAttachment(index); return true; }
            break;
        case QEvent::KeyPress: {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) { openAttachment(index); return true; }
            break;
        }
        default: break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MessageComposer::setInputVisible(bool visible)
{
    if (!visible) m_settingsPopup->hide();
    m_input->setVisible(visible);
}

bool MessageComposer::isInputVisible() const { return m_input->isVisibleTo(this); }

void MessageComposer::setRecording(bool recording)
{
    if(m_recording==recording)return;
    m_recording=recording;updateControls();
}

void MessageComposer::updateControls()
{
    notifyDraft(m_key);   // typing, attachments and retries end here
    const auto draft = m_drafts.value(m_key);
    const bool hasContent = !draft.text.trimmed().isEmpty() || !draft.attachments.isEmpty();
    const bool tooLong = draft.text.toUtf8().size() > MaxTextBytes;
    const bool waiting = isWaitingToSend(m_key);
    const bool operationLocked = m_recording || draft.sending || waiting || m_loadingKeys.contains(m_key);
    const bool locked = operationLocked || m_sessionStatusBlocksInput;
    // Disabling a focused editor makes Qt focus another action (often Stop).
    // Keep its focus while locking the outgoing payload against edits.
    m_editor->setEnabled(!m_key.isEmpty());
    m_editor->setReadOnly(locked);
    if (m_editor->property("sessionBlocked").toBool() != m_sessionStatusBlocksInput) {
        m_editor->setProperty("sessionBlocked", m_sessionStatusBlocksInput);
        m_editor->style()->unpolish(m_editor); m_editor->style()->polish(m_editor); m_editor->update();
    }
    const auto lockHint=m_recording?tr("Recording voice. Space / Esc to stop."):waiting?m_waitingSends.value(m_key)
        :m_sessionStatusBlocksInput?m_sessionStatus:QString();
    m_editor->setToolTip(lockHint);
    m_editor->setAccessibleDescription(lockHint);
    const bool referenceTarget = !m_referenceKey.isEmpty() && m_referenceKey == m_key;
    const bool fileMenu = m_narrow && referenceTarget;
    // Keep Files, model settings, Stop and Send within a narrow column.
    // The existing Stop icon and accessible name retain the same action.
    m_stop->setText(fileMenu ? QString() : tr("Stop"));
    m_stop->setAccessibleName(tr("Stop current turn"));
    m_stop->setMinimumWidth(fileMenu ? 34 : 0); m_stop->setMaximumWidth(fileMenu ? 34 : QWIDGETSIZE_MAX);
    m_attach->setMenu(fileMenu ? m_fileMenu : nullptr);
    m_attach->setEnabled(!m_key.isEmpty() && !locked && (m_attachmentsAvailable || fileMenu));
    m_attach->setAccessibleName(fileMenu ? tr("Files: attach or reference an existing file") : tr("Attach files to message"));
    m_attachFileAction->setEnabled(canAttachFiles());
    m_referenceFileAction->setEnabled(referenceTarget && canEditAttachments() && !isPreparingSending(m_key));
    m_reference->setVisible(referenceTarget && !fileMenu);
    m_reference->setEnabled(!m_key.isEmpty() && !locked && !isPreparingSending(m_key));
    m_reference->setToolTip(m_referenceHost.isEmpty()
        ? tr("Reference an existing file on this computer without uploading it")
        : tr("Reference an existing file on %1 without uploading it").arg(m_referenceMachine.isEmpty() ? m_referenceHost : m_referenceMachine));
    m_attach->setToolTip(fileMenu ? tr("Files: attach copies or reference an existing file on the agent's computer")
        : m_attachmentsAvailable ? tr("Attach files, paste an image, or drop files onto this session") : m_attachmentsReason);
    for (auto *button : m_attachmentList->findChildren<QPushButton *>("removeAttachment")) button->setEnabled(!locked);
    m_send->setEnabled(!m_key.isEmpty() && m_available && hasContent && (draft.attachments.isEmpty()||m_attachmentsAvailable) && !tooLong && !locked && !draft.uncertain);
    m_send->setText(draft.sending ? tr("Sending…") : waiting ? tr("Waiting to send…") : tr("Send"));
    m_send->setToolTip(waiting ? m_waitingSends.value(m_key) : tr("Send: Enter; new line: Shift+Enter"));
    m_retry->setVisible(draft.uncertain && !operationLocked);
    m_refreshStatus->setVisible(!m_sessionStatus.isEmpty() && m_sessionStatusError);
    m_refreshStatus->setEnabled(!operationLocked);
    m_settings->setEnabled(!m_key.isEmpty() && !operationLocked);
    if (m_settingsPopup->isVisible()) updateSettingsApply();
    // Routine loading, saving and delivery feedback belongs to the controls.
    // Reserve this space for errors and cautions about blocked/deferred sends.
    QString status = !draft.storageError.isEmpty() ? draft.storageError : waiting ? m_waitingSends.value(m_key)
        : draft.error || draft.uncertain ? draft.notice : QString();
    const bool ownError = (!waiting && draft.error) || tooLong || !draft.storageError.isEmpty();
    if (status.isEmpty() && !draft.attachments.isEmpty() && !m_attachmentsAvailable) status=m_attachmentsReason;
    if (status.isEmpty()) status = tooLong ? tr("Message exceeds 64 KiB.") : !m_sessionStatus.isEmpty() ? m_sessionStatus : m_unavailableReason;
    const auto detail = !m_sessionStatus.isEmpty() && m_sessionStatus != status ? status + '\n' + m_sessionStatus : status;
    m_status->setText(status); m_status->setToolTip(Qt::convertFromPlainText(detail)); m_status->setAccessibleName(status);
    m_status->setAccessibleDescription(detail);
    m_refreshStatus->setToolTip(Qt::convertFromPlainText(tr("Refresh this session") + '\n' + m_sessionStatus));
    m_refreshStatus->setAccessibleDescription(m_sessionStatus);
    m_status->setVisible(!status.isEmpty());
    // On a narrow column, keep recovery beside Attach without adding a row.
    m_retry->setAccessibleName(tr("Allow retry"));
    m_retry->setText(m_narrow ? QString() : tr("Allow retry"));
    m_retry->setMinimumWidth(m_narrow ? 34 : 0); m_retry->setMaximumWidth(m_narrow ? 34 : QWIDGETSIZE_MAX);
    const bool showSettings = !m_narrow || status.isEmpty();
    const auto focus = QApplication::focusWidget();
    const bool returnFocus = !showSettings && (m_settings->hasFocus() || (focus && m_settingsPopup->isAncestorOf(focus)));
    m_settings->setVisible(showSettings);
    if (!showSettings) m_settingsPopup->hide();
    // Do not transfer an unfinished settings interaction to the Stop action.
    if (returnFocus) m_editor->setFocus(Qt::OtherFocusReason);
    m_actions->setStretch(2, status.isEmpty() ? 1 : 0);
    setWorkspaceStyle(m_status, QString("font-size:11px;color:%1;").arg(ownError || (status == m_sessionStatus && m_sessionStatusError)
        ? (m_dark ? "#f4ab9b" : "#a13224") : waiting ? (m_dark ? "#efc978" : "#92641b") : (m_dark ? "#a2adbc" : "#627082")));
    emit controlsChanged();
}

void MessageComposer::showSavedDrafts()
{
    QDialog dialog(this); dialog.setObjectName("savedDraftsDialog"); dialog.setWindowTitle(tr("Saved drafts")); dialog.resize(720, 640);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(20, 20, 20, 20); layout->setSpacing(12);
    auto *hint = new QLabel(tr("Drafts stay on this desktop, including those from ended or unavailable sessions. Restoring never sends a message."));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *list = new WorkspaceList; list->setObjectName("savedDraftList"); layout->addWidget(list, 2);
    list->setProperty("hgsDark",m_dark);list->setProperty("compact",true);list->setProperty("selectionIsInspection",true);
    list->setItemDelegate(new SessionDelegate(list));list->setMouseTracking(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const auto populate = [this, list](const QHash<QString, ComposerDraftSummary> &index) {
        auto summaries = index;
        // Unsaved edits here are newer than their stored copy.
        for (auto it = m_drafts.cbegin(); it != m_drafts.cend(); ++it) {
            if (it.value().text.trimmed().isEmpty() && it.value().attachments.isEmpty()) continue;
            auto &summary = summaries[it.key()];
            summary.preview.clear(); summary.attachments = it.value().attachments.size();
            for (const auto &line : it.value().text.split('\n')) if (!line.trimmed().isEmpty()) { summary.preview = line.simplified().left(200); break; }
        }
        auto keys = summaries.keys();
        // Most recently edited first; a draft not yet saved is the newest.
        std::sort(keys.begin(), keys.end(), [&summaries = std::as_const(summaries)](const QString &a, const QString &b) {
            const auto left = summaries[a].modified, right = summaries[b].modified;
            if (left.isValid() != right.isValid()) return !left.isValid();
            return left != right ? left > right : a < b;
        });
        list->clear();
        for (const auto &key : keys) {
            const auto summary = summaries.value(key);
            const auto owner = ComposerDraftStore::owner(key);
            QStringList title, detail;
            if (owner.kind == ComposerDraftOwner::Kind::Unknown) {
                title << (summary.label.isEmpty() ? QString(key).replace('\n', " / ") : summary.label);
            } else {
                title << owner.session << (owner.machine.isEmpty() ? tr("this computer") : owner.machine);
                if (owner.kind == ComposerDraftOwner::Kind::Subagent) title << tr("subagent");
                else if (owner.kind == ComposerDraftOwner::Kind::Question) title << tr("question answer");
                else if (owner.kind == ComposerDraftOwner::Kind::Voice) title << tr("voice message");
                if (!owner.archive.isEmpty()) title << tr("archived");
            }
            if (summary.modified.isValid()) detail << QLocale().toString(summary.modified, QLocale::ShortFormat);
            if (!summary.preview.isEmpty()) detail << summary.preview;
            if (summary.attachments) detail << tr("%n attachment(s)", nullptr, summary.attachments);
            const auto text = title.join(QStringLiteral(" / ")) + (detail.isEmpty() ? QString() : '\n' + detail.join('\n'));
            auto *item = new QListWidgetItem(text, list);
            QString caption=owner.session.section('/',2);
            if(caption.isEmpty())caption=owner.session.section('/',1,1);
            if(caption.isEmpty())caption=title.value(0);
            item->setData(SessionRoles::Title,caption);
            item->setData(SessionRoles::Agent,owner.kind==ComposerDraftOwner::Kind::Unknown?QString():owner.session.section('/',0,0));
            const auto machine=owner.machine.isEmpty()?tr("this computer"):owner.machine;
            item->setData(SessionRoles::Host,machine);item->setData(SessionRoles::MachineName,machine);
            item->setData(SessionRoles::MachineColor,MachineAppearance::color(machine));
            item->setData(SessionRoles::Meta,owner.session);
            if(savedDraftSessionRoles){const auto roles=savedDraftSessionRoles(key);for(auto it=roles.cbegin();it!=roles.cend();++it)item->setData(it.key(),it.value());}
            QStringList context;
            if(owner.kind==ComposerDraftOwner::Kind::Subagent){context<<tr("Subagent");item->setData(SessionRoles::Title,item->data(SessionRoles::Title).toString()+" / "+owner.subagent);}
            else if(owner.kind==ComposerDraftOwner::Kind::Question)context<<tr("Question answer");
            else if(owner.kind==ComposerDraftOwner::Kind::Voice)context<<tr("Voice message");
            if(!owner.archive.isEmpty())context<<tr("Archived");
            if(summary.attachments&&!summary.preview.isEmpty())context<<tr("%n attachment(s)",nullptr,summary.attachments);
            if(!context.isEmpty()){const auto meta=item->data(SessionRoles::Meta).toString();if(!meta.isEmpty())context.prepend(meta);item->setData(SessionRoles::Meta,context.join(" / "));}
            item->setData(SessionRoles::Status,tr("Draft"));item->setData(SessionRoles::Draft,true);
            item->setData(SessionRoles::Detail,summary.preview.isEmpty()&&summary.attachments?tr("%n attachment(s)",nullptr,summary.attachments):summary.preview);
            if(summary.modified.isValid())item->setData(SessionRoles::ModifiedAt,QLocale().toString(summary.modified,QLocale::ShortFormat));
            item->setData(Qt::UserRole,key);item->setData(Qt::AccessibleTextRole,text);item->setToolTip({});
        }
        if (list->count()) list->setCurrentRow(0);
    };
    auto *preview = new QPlainTextEdit; preview->setObjectName("savedDraftPreview"); preview->setReadOnly(true); layout->addWidget(preview, 1);
    auto *detail = new QLabel; detail->setWordWrap(true); layout->addWidget(detail);
    auto *actions = new QHBoxLayout;
    auto *copy = new QPushButton(tr("Copy text")); copy->setObjectName("copySavedDraft");
    auto *restore = new QPushButton(tr("Restore in current session")); restore->setObjectName("restoreSavedDraft");
    auto *open = new QPushButton(tr("Go to session")); open->setObjectName("openSavedDraftSession");
    auto *close = new QPushButton(tr("Close")); close->setDefault(true);
    actions->addWidget(copy); actions->addWidget(restore); actions->addWidget(open); actions->addStretch(); actions->addWidget(close);
    layout->addLayout(actions);
    const auto target = m_key;
    QString requested;
    Draft selected;
    quint64 selection = 0;
    const auto presentSelection = [&] {
        if (selected.sending) {
            selected.sending = false; selected.uncertain = true;
            selected.notice = tr("This message may already have been sent. Check Activity or Terminal before sending again.");
        }
        preview->setPlainText(selected.text);
        QStringList details{tr("%n attachment(s)", nullptr, selected.attachments.size())};
        if (!selected.notice.isEmpty()) details << selected.notice;
        detail->setText(selected.storageError.isEmpty() ? details.join(" / ") : selected.storageError);
        if(selected.storageError.isEmpty() && selected.formState.value("kind")=="voice_message")
            detail->setText(tr("Open the original conversation (%1) to review or check this voice message.").arg(selected.formState.value("session").toString()));
        copy->setEnabled(!selected.text.isEmpty());
        restore->setEnabled(!m_recording && !target.isEmpty() && !isSending(target) && !isWaitingToSend(target)
            && !isDraftLoading(target) && selected.storageError.isEmpty() && list->currentItem()
            && selected.formState.value("kind") != "voice_message");
    };
    connect(list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
        const auto generation = ++selection;
        const auto key = item ? item->data(Qt::UserRole).toString() : QString();
        const bool reachable = item && ComposerDraftStore::owner(key).kind != ComposerDraftOwner::Kind::Unknown
            && canOpenDraftSession && canOpenDraftSession(key);
        open->setEnabled(reachable);
        open->setToolTip(reachable || !item ? QString() : tr("This session is not in the session list now."));
        if (!item) selected = {};
        else if (m_preservedDrafts.contains(key)) selected = m_preservedDrafts.value(key);
        else if (m_drafts.contains(key) && !isDraftLoading(key)) selected = m_drafts.value(key);
        else {
            selected = {}; preview->clear(); detail->setText(tr("Loading draft…")); copy->setEnabled(false); restore->setEnabled(false);
            m_draftStore.loadAsync(key, &dialog, [&, generation](const ComposerDraft &loaded) {
                if (generation != selection) return;
                selected = loaded; presentSelection();
            }); return;
        }
        presentSelection();
    });

    connect(copy, &QPushButton::clicked, &dialog, [&] { QApplication::clipboard()->setText(selected.text); });
    const auto openSession = [&] {
        if (!list->currentItem()) return;
        const auto key = list->currentItem()->data(Qt::UserRole).toString();
        if (ComposerDraftStore::owner(key).kind == ComposerDraftOwner::Kind::Unknown
            || !canOpenDraftSession || !canOpenDraftSession(key)) return;
        requested = key; dialog.accept();
    };
    connect(open, &QPushButton::clicked, &dialog, openSession);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, openSession);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(restore, &QPushButton::clicked, &dialog, [&] {
        if(selected.formState.value("kind")=="voice_message")return;
        if (m_recording || m_key != target || target.isEmpty() || isSending(target) || isWaitingToSend(target) || isDraftLoading(target)) return;
        const auto current = m_drafts.value(target);
        if ((!current.text.isEmpty() || !current.attachments.isEmpty())
            && list->currentItem()->data(Qt::UserRole).toString() != target) {
            QMessageBox confirm(QMessageBox::Warning, tr("Replace current draft?"),
                tr("Replace the unsent draft in %1 with the selected saved draft?").arg(QString(target).replace('\n', " / ")),
                QMessageBox::Ok | QMessageBox::Cancel, &dialog);
            confirm.setObjectName("replaceDraftConfirm"); confirm.setTextFormat(Qt::PlainText); confirm.setDefaultButton(QMessageBox::Cancel);
            confirm.button(QMessageBox::Ok)->setText(tr("Replace draft")); confirm.setEscapeButton(QMessageBox::Cancel);
            if (confirm.exec() != QMessageBox::Ok || m_recording || m_key != target || isSending(target) || isWaitingToSend(target)) return;
        }
        selected.label.clear(); selected.formState = {};
        m_drafts[target] = selected; saveDraft(target);
        m_loading = true; restoreDraft(); m_loading = false; rebuildAttachments(); updateControls(); dialog.accept();
    });
    copy->setEnabled(false); restore->setEnabled(false); open->setEnabled(false);
    m_draftStore.indexAsync(&dialog, [&, populate](const QHash<QString, ComposerDraftSummary> &index) {
        populate(index); if (!list->count()) hint->setText(tr("No saved drafts on this desktop."));
    });
    dialog.exec();
    if (!requested.isEmpty()) emit draftSessionRequested(requested);
}

void MessageComposer::setModelSettings(const QString &model, const QString &effort, const QJsonArray &options,
                                     bool enabled, const QString &reason, const QString &pendingModel,
                                     const QString &pendingEffort, const QString &applyWhen, bool modelDefaultEffort)
{
    m_model = model; m_effort = effort; m_modelOptions = options; m_settingsEnabled = enabled; m_modelDefaultEffort = modelDefaultEffort;
    m_settingsReason = reason; m_pendingModel = pendingModel; m_pendingEffort = pendingEffort; m_applyWhen = applyWhen;
    if (modelDefaultEffort) {
        // Another context starts the picker afresh before its list and current model are set.
        enterOpenCodeContext();
        m_openCodeShort = options;
        // Picker choices stay offered with their variants, also when this list lacks them.
        const auto chosen = m_openCodeChosen.value(m_key).entries;
        for (const auto &entry : chosen) {
            const auto id = entry.toObject().value("id").toString();
            if (std::none_of(m_modelOptions.cbegin(), m_modelOptions.cend(), [&](const QJsonValue &option) { return option.toObject().value("id").toString() == id; }))
                m_modelOptions.append(entry);
        }
        m_openCodeModels->setShortList(withConfirmedFavorites(options), m_openCodeTruncated);
        // An open popup keeps the model chosen in it; otherwise the session's own.
        m_openCodeModels->setCurrent(m_settingsPopup->isVisible() && m_popupKey == m_key ? m_models->currentData().toString()
                                                                                        : pendingModel.isEmpty() ? model : pendingModel);
    } else if (!m_openCodeContext.isEmpty()) { m_openCodeContext.clear(); resetOpenCodeModels(); }
    updateSettingsButton();
    if (m_settingsPopup->isVisible()) updateSettingsApply();
}

void MessageComposer::updateSettingsButton()
{
    const bool pending = !m_pendingModel.isEmpty() || !m_pendingEffort.isEmpty();
    const QString model = m_model.isEmpty() ? tr("Model") : m_model;
    const QString suffix = (m_effort.isEmpty() ? QString() : "  " + m_effort) + (pending ? "  ◷" : QString());
    m_settings->setProperty("modelLabel", model); m_settings->setProperty("suffix", suffix);
    m_settings->setText(model + suffix); m_settings->setAccessibleName(tr("Model and reasoning effort: %1").arg(model + suffix));
    QString hint = tr("Current: %1%2").arg(model, m_effort.isEmpty() ? QString() : "  " + m_effort);
    if (pending) hint += '\n' + tr("Pending: %1%2").arg(m_pendingModel.isEmpty() ? model : m_pendingModel,
        m_pendingEffort.isEmpty() ? QString() : "  " + m_pendingEffort);
    if (!m_settingsReason.isEmpty()) hint += '\n' + m_settingsReason;
    m_settings->setToolTip(hint); m_settings->updateGeometry(); m_settings->update();
}

void MessageComposer::openSettings()
{
    if (m_key.isEmpty()) return;
    m_popupKey = m_key;
    const QString model = m_pendingModel.isEmpty() ? m_model : m_pendingModel;
    const QSignalBlocker blocker(m_models); m_models->clear();
    for (const auto &value : m_modelOptions) {
        const auto option = value.toObject(); const auto id = option.value("id").toString();
        if (!id.isEmpty() && m_models->findData(id) < 0) m_models->addItem(option.value("label").toString(id), id);
    }
    if (m_models->findData(model) < 0 && !model.isEmpty()) m_models->insertItem(0, model, model);
    m_models->setCurrentIndex(m_models->findData(model));
    if (m_modelDefaultEffort) m_openCodeModels->setCurrent(model);
    updateSettingsEfforts(); updateSettingsApply();
    const QRect available = screen()->availableGeometry();
    m_settingsPopup->setFixedWidth(qMin(m_modelDefaultEffort ? 420 : 360, available.width() - 24)); m_settingsPopup->adjustSize();
    QPoint position = m_settings->mapToGlobal(QPoint(m_settings->width() - m_settingsPopup->width(), -m_settingsPopup->height() - 7));
    position.setX(qBound(available.left() + 8, position.x(), available.right() - m_settingsPopup->width() - 8));
    position.setY(qBound(available.top() + 8, position.y(), available.bottom() - m_settingsPopup->height() - 8));
    m_settingsPopup->move(position); m_settingsPopup->show();
    (m_settingsTerminal->isVisible() ? static_cast<QWidget *>(m_settingsTerminal)
        : m_modelDefaultEffort ? static_cast<QWidget *>(m_openCodeModels) : m_models)->setFocus(Qt::PopupFocusReason);
}

void MessageComposer::updateSettingsEfforts()
{
    const bool pending = !m_pendingModel.isEmpty() || !m_pendingEffort.isEmpty();
    // The model default is an explicit choice there, so an empty current
    // choice is kept like any other.
    const QString preferred = m_efforts->isVisible() && (m_modelDefaultEffort || !m_efforts->currentData().toString().isEmpty())
        ? m_efforts->currentData().toString() : m_modelDefaultEffort ? (pending ? m_pendingEffort : m_effort)
        : m_pendingEffort.isEmpty() ? m_effort : m_pendingEffort;
    const QSignalBlocker blocker(m_efforts); m_efforts->clear();
    if (m_modelDefaultEffort) m_efforts->addItem(tr("Default"), QString());
    for (const auto &value : m_modelOptions) {
        const auto option = value.toObject();
        if (option.value("id").toString() != m_models->currentData().toString()) continue;
        const auto labels = option.value("effort_labels").toArray();
        const auto efforts = option.value("effort_options").toArray();
        for (qsizetype index = 0; index < efforts.size(); ++index) {
            const QString id = efforts[index].toString();
            if (id.isEmpty() || (m_modelDefaultEffort && (!OpenCodeEffort::valid(id) || m_efforts->findData(id) >= 0))) continue;
            auto label = m_modelDefaultEffort ? labels.at(index).toString().trimmed() : QString();
            if (label.size() > 80 || std::any_of(label.cbegin(), label.cend(), [](QChar c) { return c.category() == QChar::Other_Control; })) label.clear();
            m_efforts->addItem(label.isEmpty() ? id : label, id);
        }
        break;
    }
    // As the model list keeps an unknown current model, the session's current
    // variant stays offered for its own model: Apply without edits keeps it.
    const QString sessionModel = m_pendingModel.isEmpty() ? m_model : m_pendingModel;
    const QString sessionEffort = pending ? m_pendingEffort : m_effort;
    if (m_modelDefaultEffort && OpenCodeEffort::valid(sessionEffort) && m_models->currentData().toString() == sessionModel
        && m_efforts->findData(sessionEffort) < 0) m_efforts->addItem(sessionEffort, sessionEffort);
    if (m_efforts->count() == 0) m_efforts->addItem(tr("Provider default"), QString());
    const int selected = m_efforts->findData(preferred);
    m_efforts->setCurrentIndex(selected >= 0 ? selected : 0); updateSettingsApply();
}

void MessageComposer::setSettingsNotice(const QString &notice)
{
    if (m_settingsNotice == notice) return;
    m_settingsNotice = notice;
    if (m_settingsPopup->isVisible()) updateSettingsApply();
}

void MessageComposer::setSettingsTerminalAvailable(bool available)
{
    m_settingsTerminalAvailable = available;
    if (m_settingsPopup->isVisible()) updateSettingsApply();
}

void MessageComposer::updateSettingsApply()
{
    const bool native = m_settingsTerminalAvailable && !m_settingsEnabled;
    m_settingsTerminal->setVisible(native);
    for (auto *widget : QList<QWidget *>{m_efforts,m_modelLabel,m_effortLabel,m_applySettings}) widget->setVisible(!native);
    // OpenCode chooses in its picker; m_models stays the selected value.
    m_models->setVisible(!native && !m_modelDefaultEffort); m_openCodeModels->setVisible(!native && m_modelDefaultEffort);
    bool offered = false;
    for (const auto &value : m_modelOptions) offered |= value.toObject().value("id").toString() == m_models->currentData().toString();
    const bool enabled = m_settingsEnabled && m_popupKey == m_key && !isSending(m_key);
    m_models->setEnabled(enabled && m_models->count() > 1); m_openCodeModels->setEnabled(enabled);
    m_efforts->setEnabled(enabled && offered && (m_modelDefaultEffort ? m_efforts->count() > 1 : !m_efforts->currentData().toString().isEmpty()));
    const QString intendedModel = m_pendingModel.isEmpty() ? m_model : m_pendingModel;
    const bool pending = !m_pendingModel.isEmpty() || !m_pendingEffort.isEmpty();
    const QString intendedEffort = pending ? m_pendingEffort : m_effort;
    const bool changed = m_models->currentData().toString() != intendedModel || m_efforts->currentData().toString() != intendedEffort;
    m_applySettings->setEnabled(enabled && offered && (changed || (pending && m_applyWhen == "now")));
    m_applySettings->setText(m_applyWhen == "resume" ? tr("Save for resume") : m_applyWhen == "ready" ? tr("Save change") : tr("Apply"));
    QString hint = m_settingsReason;
    if (hint.isEmpty()) hint = !m_settingsEnabled ? tr("Model settings are not available for this session.")
        : m_applyWhen == "resume" ? tr("Saved for the next resume. The session stays stopped.")
        : m_applyWhen == "ready" ? tr("Applies when the agent is ready. Messages sent while it is working use the current model.")
        : tr("Applies to this session only.");
    if (m_settingsEnabled && !m_settingsNotice.isEmpty()) hint += '\n' + m_settingsNotice;
    if (m_modelDefaultEffort && !m_openCodeError.isEmpty()) hint += '\n' + m_openCodeError;
    if (pending)
        hint = tr("Pending: %1%2\nCurrent: %3%4\n%5").arg(intendedModel, intendedEffort.isEmpty() ? QString() : "  " + intendedEffort,
            m_model, m_effort.isEmpty() ? QString() : "  " + m_effort, hint);
    m_settingsHint->setText(hint);
}

void MessageComposer::setOpenCodeContext(bool supported, bool truncated, bool online, const QString &computer,
                                         const QString &host, const QString &folder)
{
    m_openCodeSupported = supported; m_openCodeOnline = online; m_openCodeComputer = computer;
    m_openCodeHost = host; m_openCodeFolder = folder;
    // Another session's context starts in setModelSettings, with its list. The shown session
    // somewhere else starts afresh here, with its own list and current model.
    const bool shown = !m_openCodeContext.isEmpty() && m_openCodeContext.first() == m_key;
    if (shown && enterOpenCodeContext()) {
        m_modelOptions = m_openCodeShort; m_openCodeTruncated = truncated;
        m_openCodeModels->setShortList(withConfirmedFavorites(m_openCodeShort), truncated);
        m_openCodeModels->setCurrent(m_pendingModel.isEmpty() ? m_model : m_pendingModel);
    } else if (truncated != m_openCodeTruncated) {
        m_openCodeTruncated = truncated;
        if (shown) m_openCodeModels->setShortList(withConfirmedFavorites(m_openCodeShort), truncated);
    }
    // Details refresh often: the stars are redrawn only when this changes.
    const auto availability = QString::number(supported) + QString::number(online) + computer;
    if (availability == m_openCodeAvailability) return;
    m_openCodeAvailability = availability;
    m_openCodeModels->setAllModelsAvailable(supported, OpenCodeModels::updateText(computer));
    m_openCodeModels->setFavoritesAvailable(supported && online, supported ? tr("%1 is offline.").arg(computer) : OpenCodeModels::updateText(computer));
}

void MessageComposer::setOpenCodeCatalog(const QString &sessionKey, const QJsonObject &catalog)
{
    if (sessionKey == m_key && sessionKey == m_openCodeContext.value(0)) m_openCodeModels->setCatalog(withConfirmedFavorites(catalog));
}

void MessageComposer::setOpenCodeCatalogFailed(const QString &sessionKey, const QString &reason)
{
    if (sessionKey == m_key && sessionKey == m_openCodeContext.value(0)) m_openCodeModels->setCatalogFailed(reason);
}

void MessageComposer::setOpenCodeFavorite(const QString &sessionKey, const QString &value, bool favorite, const QString &error)
{
    // Another session's answer is ignored; the owner passes only answers made in this context.
    if (sessionKey != m_key || sessionKey != m_openCodeContext.value(0)) return;
    if (error.isEmpty()) {
        // Lists and catalogs read before the change keep it: details refresh before they report it.
        if (!m_openCodeFavorites.contains(value) && m_openCodeFavorites.size() >= 64) m_openCodeFavorites.erase(m_openCodeFavorites.begin());
        m_openCodeFavorites.insert(value, favorite);
    } else m_openCodeError = error;
    m_openCodeModels->setFavoritePending(value, false); m_openCodeModels->setFavorite(value, favorite);
    if (m_settingsPopup->isVisible()) updateSettingsApply();
}

// The picker's context is the session with the computer and folder its catalog is read
// from. A new one, a return to the same session included, starts afresh before its list
// and current model are set; this session's choices made elsewhere are dropped.
bool MessageComposer::enterOpenCodeContext()
{
    const QStringList context{m_key, m_openCodeHost, m_openCodeFolder};
    if (context == m_openCodeContext) return false;
    // An open popup of the same session holds choices made for its old place.
    if (m_openCodeContext.value(0) == m_key && m_popupKey == m_key) m_settingsPopup->hide();
    const auto chosen = m_openCodeChosen.value(m_key);
    if (chosen.host != m_openCodeHost || chosen.folder != m_openCodeFolder) { m_openCodeChosen.remove(m_key); m_openCodeChosenOrder.removeAll(m_key); }
    m_openCodeContext = context; resetOpenCodeModels();
    return true;
}

// Every new context starts the picker afresh: its catalog, a loading request, All models,
// pending stars, confirmed favorites and failure text belong to the context before.
void MessageComposer::resetOpenCodeModels()
{
    m_openCodeModels->setCatalog({}); m_openCodeFavorites.clear(); m_openCodeError.clear(); m_openCodeAvailability.clear();
    ++m_openCodeGeneration;
}

// The picker's choice: kept for this session in this context, since its own list may lack
// it, and selected in m_models, whose effort and Apply logic is unchanged. A model chosen
// again takes its newest description, also in the combo.
void MessageComposer::chooseOpenCodeModel(const QString &value)
{
    if (m_popupKey != m_key || !m_modelDefaultEffort || m_openCodeContext.value(0) != m_key) return;
    const auto model = m_openCodeModels->model(value); if (!model) return;
    const QJsonObject entry{{"id", value}, {"value", value}, {"label", model->label},
        {"effort_options", QJsonArray::fromStringList(model->efforts)}, {"effort_labels", QJsonArray::fromStringList(model->effortLabels)}};
    const auto put = [&](QJsonArray &entries) {
        for (qsizetype index = 0; index < entries.size(); ++index)
            if (entries.at(index).toObject().value("id").toString() == value) { entries.replace(index, entry); return; }
        entries.append(entry);
    };
    auto &chosen = m_openCodeChosen[m_key];
    chosen.host = m_openCodeContext.value(1); chosen.folder = m_openCodeContext.value(2);
    put(chosen.entries); while (chosen.entries.size() > 16) chosen.entries.removeAt(0);
    m_openCodeChosenOrder.removeAll(m_key); m_openCodeChosenOrder.append(m_key);
    while (m_openCodeChosenOrder.size() > 32) m_openCodeChosen.remove(m_openCodeChosenOrder.takeFirst());
    put(m_modelOptions);
    const auto label = model->label.isEmpty() ? value : model->label;
    if (const int listed = m_models->findData(value); listed < 0) m_models->addItem(label, value); else m_models->setItemText(listed, label);
    const int index = m_models->findData(value);
    if (m_models->currentIndex() == index) updateSettingsEfforts(); else m_models->setCurrentIndex(index);
}

// A confirmed favorite change in entries hgs listed (value, else id): the mark is set,
// or removed as hgs leaves it out; every other field stays.
QJsonArray MessageComposer::withConfirmedFavorites(QJsonArray entries) const
{
    if (m_openCodeFavorites.isEmpty()) return entries;
    for (qsizetype index = 0; index < entries.size(); ++index) {
        auto entry = entries.at(index).toObject();
        const auto value = entry.value("value").toString(entry.value("id").toString());
        if (!m_openCodeFavorites.contains(value)) continue;
        if (m_openCodeFavorites.value(value)) entry.insert("favorite", true); else entry.remove("favorite");
        entries.replace(index, entry);
    }
    return entries;
}

// The same in a catalog: both model lists and its favorites, where a new favorite is
// listed once, as hgs lists it ({value, available}).
QJsonObject MessageComposer::withConfirmedFavorites(QJsonObject catalog) const
{
    if (m_openCodeFavorites.isEmpty()) return catalog;
    for (const auto *list : {"model_options", "models"})
        if (catalog.contains(list)) catalog.insert(list, withConfirmedFavorites(catalog.value(list).toArray()));
    if (!catalog.contains("favorites")) return catalog;
    QJsonArray favorites; QSet<QString> listed;
    for (const auto &entry : catalog.value("favorites").toArray()) {
        const auto value = entry.toObject().value("value").toString();
        if (m_openCodeFavorites.contains(value) && !m_openCodeFavorites.value(value)) continue;
        listed.insert(value); favorites.append(entry);
    }
    const auto models = catalog.value("models").toArray();
    for (auto it = m_openCodeFavorites.cbegin(); it != m_openCodeFavorites.cend(); ++it) {
        if (!it.value() || listed.contains(it.key())) continue;
        const bool available = std::any_of(models.cbegin(), models.cend(), [&](const QJsonValue &model) {
            const auto entry = model.toObject(); return entry.value("value").toString(entry.value("id").toString()) == it.key(); });
        favorites.append(QJsonObject{{"value", it.key()}, {"available", available}});
    }
    catalog.insert("favorites", favorites);
    return catalog;
}

void MessageComposer::setTheme(bool dark)
{
    m_dark = dark;
    updateSettingsButton();
    m_toolbar->setTheme(dark);
    m_attach->setIcon(workspaceIcon("attachment", dark ? QColor("#c5cfdb") : QColor("#536477")));
    m_reference->setIcon(workspaceIcon("connect", dark ? QColor("#c5cfdb") : QColor("#536477")));
    m_refreshStatus->setIcon(composerIcon("refresh", QColor(dark ? "#efc978" : "#92641b"), QColor(dark ? "#85909e" : "#6d7784")));
    m_retry->setIcon(composerIcon("refresh", QColor(dark ? "#efc978" : "#92641b"), QColor(dark ? "#85909e" : "#6d7784")));
    m_stop->setIcon(workspaceIcon("stop",dark ? QColor("#ff9ca8") : QColor("#b52d48")));
    // Match the stylesheet's enabled and disabled Send text colors below.
    m_send->setIcon(composerIcon("send", QColor(dark ? "#10231b" : "#ffffff"), QColor(dark ? "#85909e" : "#6d7784")));
    const QColor chevron(dark ? "#a2adbc" : "#627082");
    m_settings->setProperty("chevron", QVariant::fromValue(composerIcon("chevron-down", chevron, chevron))); m_settings->update();
    setStyleSheet(QString(R"(
        QPlainTextEdit#messageInput { background:%1; color:%2; border:1px solid %3; border-radius:9px; padding:%8px; font-size:%9px; selection-background-color:%4; }
        QPlainTextEdit#messageInput:focus { border-color:%5; }
        QPlainTextEdit#messageInput[sessionBlocked="true"] { color:%7; border-color:%3; }
        QWidget#messageComposer { background:transparent; border:0; }
        QWidget#attachmentRow { border-radius:6px; }
        QWidget#attachmentRow:hover, QWidget#attachmentRow:focus { background:%4; }
        QWidget#attachmentsPopover QLabel { font-size:11px; border:0; background:transparent; }
        QPushButton#removeAttachment { padding:0; border:0; background:transparent; font-size:15px; color:%7; }
        QPushButton#removeAttachment:hover { color:%2; }
        QLabel#attachmentPreview { background:%1; border:1px solid %3; border-radius:6px; padding:4px; }
        QPushButton#sendMessage { background:%5; color:%6; border:1px solid %5; padding:8px 15px; border-radius:7px; font-weight:600; }
        QPushButton#sendMessage:disabled { background:%1; color:%7; border-color:%3; }
        QPushButton#sessionModelSettings { padding:0 10px; font-size:11px; min-height:0; border:1px solid transparent; border-radius:7px; color:%2; background:transparent; }
        QPushButton#sessionModelSettings:hover { background:%1; border-color:%3; }
        QPushButton#sessionModelSettings:focus[keyboardFocus="true"] { border-color:%5; }
        QFrame#sessionSettingsPopup { background:%1; color:%2; border:1px solid %3; border-radius:9px; }
        QFrame#sessionSettingsPopup QLabel { color:%2; border:0; }
        QLabel#sessionSettingsHint { color:%7; font-size:11px; }
        QFrame#sessionSettingsPopup QComboBox { background:%1; color:%2; border:1px solid %3; border-radius:6px; padding:7px 26px 7px 9px; }
        QFrame#sessionSettingsPopup QComboBox::drop-down { border:0; width:24px; }
        QFrame#sessionSettingsPopup QComboBox::down-arrow { image:url(:/hgs/chevron-down.svg); width:14px; height:14px; }
        QFrame#sessionSettingsPopup QComboBox QAbstractItemView { background:%1; color:%2; selection-background-color:%4; }
        QPushButton#applySessionSettings { padding:7px 12px; background:%5; color:%6; border:1px solid %5; border-radius:6px; }
        QPushButton#applySessionSettings:disabled { background:%1; color:%7; border-color:%3; }
        QPushButton#cancelSessionSettings { padding:7px 12px; background:transparent; color:%2; border:1px solid %3; border-radius:6px; }
        QPushButton#cancelSessionSettings:hover { background:%4; }
        QPushButton#cancelSessionSettings:focus[keyboardFocus="true"] { border-color:%5; }
        QPushButton#attachMessageFile { padding:0; border:1px solid %3; border-radius:7px; background:transparent; }
        QPushButton#attachMessageFile:hover { background:%1; border-color:%5; }
        QPushButton#attachMessageFile:focus[keyboardFocus="true"] { border-color:%5; }
        QPushButton#refreshSessionStatus { padding:0; border:0; background:transparent; }
        QPushButton#allowMessageRetry { padding:7px 10px; font-size:11px; }
    )").arg(dark ? "#171d24" : "#f8fafb", dark ? "#e8edf4" : "#1a2733", dark ? "#3d4855" : "#dce2e8",
             dark ? "#365b4d" : "#c7eadd", dark ? "#8bdfc0" : "#167357", dark ? "#10231b" : "#ffffff", dark ? "#85909e" : "#6d7784")
        .arg(ContentScale::px(9, m_scale)).arg(ContentScale::px(13, m_scale)) + workspaceScrollbars(dark));
    updateControls();
}

void MessageComposer::setContentScale(double scale)
{
    scale = ContentScale::clamp(scale);
    if (qFuzzyCompare(scale, m_scale)) return;
    m_scale = scale;
    // Keep the same number of visible lines as at the native size.
    m_editor->setFixedHeight(ContentScale::px(76, scale));
    setTheme(m_dark);
}
