#include "MessageComposer.h"
#include "AttachmentViewer.h"
#include "ComposerToolbar.h"
#include "ContentScale.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
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
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPlainTextEdit>
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
#include <functional>

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
    m_status = new QLabel; m_status->setObjectName("messageStatus"); m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText); m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_retry = new QPushButton(tr("Allow retry")); m_retry->setObjectName("allowMessageRetry"); m_retry->hide();
    m_retry->setToolTip(tr("Check the terminal before sending again to avoid a duplicate message"));
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
    connect(m_applySettings, &QPushButton::clicked, this, [this] {
        if (m_popupKey != m_key || !m_applySettings->isEnabled()) return;
        const QString model = m_models->currentData().toString(), effort = m_efforts->currentData().toString();
        m_settingsPopup->hide(); emit settingsRequested(m_key, model, effort);
    });
    actions->addWidget(m_attach); actions->addStretch(0); actions->addWidget(m_status, 1); actions->addWidget(m_retry); actions->addWidget(m_settings); actions->addWidget(m_stop); actions->addWidget(m_send);
    inputLayout->addLayout(actions);
    m_feedback = new QHBoxLayout; m_feedback->setContentsMargins(0, 0, 0, 0); m_feedback->setSpacing(8); inputLayout->addLayout(m_feedback);
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
    connect(m_attach, &QPushButton::clicked, this, &MessageComposer::attachFiles);
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
    m_draftStore.indexAsync(this, [this](const QHash<QString, QString> &index) {
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
    const bool narrow = width() < 640;
    if (narrow == m_narrow) return;
    m_narrow = narrow;
    if (narrow) {
        m_actions->setStretch(1, 1);
        m_actions->removeWidget(m_status);
        m_actions->removeWidget(m_retry);
        m_feedback->addWidget(m_status, 1); m_feedback->addWidget(m_retry);
    } else {
        m_actions->setStretch(1, m_status->isHidden() ? 1 : 0);
        m_feedback->removeWidget(m_status); m_feedback->removeWidget(m_retry);
        m_actions->insertWidget(2, m_status, 1);
        m_actions->insertWidget(3, m_retry);
    }
}

void MessageComposer::setSessionKey(const QString &key)
{
    if (key == m_key) return;
    ++m_navigationRevision;
    m_settingsPopup->hide();
    m_toolbar->closePopovers(); m_preview->hide();
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
        });
    }
    m_key = key; m_loading = true;
    restoreDraft();
    m_loading = false; rebuildAttachments(); updateControls(); updatePlaceholder();
}

bool MessageComposer::insertVoiceText(const VoiceTarget &target, const QString &text, std::function<void(bool)> saved)
{
    if (target.key.isEmpty() || target.key != m_key || target.revision != m_saveRevision.value(m_key)
        || target.navigation != m_navigationRevision || !canAttachFiles() || isPreparingSending(m_key)
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
    if ((m_recording && key==m_key) || key.isEmpty() || isDraftLoading(key) || text.trimmed().isEmpty() || isWaitingToSend(key)) return false;
    auto &draft = m_drafts[key];
    // A draft the user started, or a message on its way, always wins.
    if (!draft.text.trimmed().isEmpty() || !draft.attachments.isEmpty() || draft.sending) return false;
    draft.text = text; draft.position = draft.anchor = int(text.size()); draft.notice.clear();
    if (key == m_key) { m_loading = true; restoreDraft(); m_loading = false; }
    saveDraft(key); notifyDraft(key); updateControls(); return true;
}

void MessageComposer::setAvailability(bool available, const QString &reason)
{
    if (available != m_available || reason != m_unavailableReason) QToolTip::hideText();
    m_available = available; m_unavailableReason = reason; updateControls();
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
    if (oldKey == newKey || !m_drafts.contains(oldKey) || isSending(oldKey) || isWaitingToSend(oldKey)
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
    emit sendRequested(m_key, draft.text, draft.attachments);
}

void MessageComposer::attachFiles()
{
    if (!canAttachFiles()) return;
    const QString key = m_key;
    const auto files = QFileDialog::getOpenFileNames(this, tr("Attach files"), {}, tr("All files (*)"));
    if (key != m_key) return;
    for (const auto &file : files) attachFile(file);
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
    if (!info.isFile() || info.size() > MaxAttachmentBytes) { showError(tr("Choose a file of at most 10 MiB.")); return; }
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
    auto &current = m_drafts[m_key]; if (!canAttachFiles() || index < 0 || index >= current.attachments.size()) return;
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
        if (!openUrl(QUrl::fromLocalFile(path))) showError(tr("Could not open the attachment with its default application."));
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
    const bool locked = m_recording || draft.sending || waiting || m_loadingKeys.contains(m_key);
    // Disabling a focused editor makes Qt focus another action (often Stop).
    // Keep its focus while locking the outgoing payload against edits.
    m_editor->setEnabled(!m_key.isEmpty());
    m_editor->setReadOnly(locked);
    const auto lockHint=m_recording?tr("Recording voice. Space / Esc to stop."):waiting?m_waitingSends.value(m_key):QString();
    m_editor->setToolTip(lockHint);
    m_editor->setAccessibleDescription(lockHint);
    m_attach->setEnabled(!m_key.isEmpty() && !locked);
    for (auto *button : m_attachmentList->findChildren<QPushButton *>("removeAttachment")) button->setEnabled(!locked);
    m_send->setEnabled(!m_key.isEmpty() && m_available && hasContent && !tooLong && !locked && !draft.uncertain);
    m_send->setText(draft.sending ? tr("Sending…") : waiting ? tr("Waiting to send…") : tr("Send"));
    m_send->setToolTip(waiting ? m_waitingSends.value(m_key) : tr("Send: Enter; new line: Shift+Enter"));
    m_retry->setVisible(draft.uncertain && !locked);
    m_settings->setEnabled(!m_key.isEmpty() && !locked);
    if (m_settingsPopup->isVisible()) updateSettingsApply();
    // Routine loading, saving and delivery feedback belongs to the controls.
    // Reserve this space for errors and cautions about blocked/deferred sends.
    QString status = !draft.storageError.isEmpty() ? draft.storageError : waiting ? m_waitingSends.value(m_key)
        : draft.error || draft.uncertain ? draft.notice : QString();
    if (status.isEmpty()) status = tooLong ? tr("Message exceeds 64 KiB.") : m_unavailableReason;
    m_status->setText(status); m_status->setToolTip(status);
    m_status->setVisible(!status.isEmpty());
    m_actions->setStretch(1, m_narrow || status.isEmpty() ? 1 : 0);
    setWorkspaceStyle(m_status, QString("font-size:11px;color:%1;").arg((!waiting && draft.error) || tooLong || !draft.storageError.isEmpty() ? (m_dark ? "#f4ab9b" : "#a13224") : (m_dark ? "#a2adbc" : "#627082")));
    emit controlsChanged();
}

void MessageComposer::showSavedDrafts()
{
    QDialog dialog(this); dialog.setObjectName("savedDraftsDialog"); dialog.setWindowTitle(tr("Saved drafts")); dialog.resize(720, 440);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(tr("Drafts stay on this desktop, including those from ended or unavailable sessions. Restoring never sends a message."));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto *list = new QListWidget; list->setObjectName("savedDraftList"); layout->addWidget(list, 1);
    const auto populate = [this, list](const QHash<QString, QString> &index) {
        QSet<QString> keys;
        for (auto it = index.cbegin(); it != index.cend(); ++it) keys.insert(it.key());
        for (auto it = m_drafts.cbegin(); it != m_drafts.cend(); ++it)
            if (!it.value().text.trimmed().isEmpty() || !it.value().attachments.isEmpty()) keys.insert(it.key());
        auto sorted = keys.values(); sorted.sort(); list->clear();
        for (const auto &key : sorted) {
            auto *item = new QListWidgetItem(index.value(key, QString(key).replace('\n', " / ")), list);
            item->setData(Qt::UserRole, key);
        }
        if (list->count()) list->setCurrentRow(0);
    };
    auto *preview = new QPlainTextEdit; preview->setObjectName("savedDraftPreview"); preview->setReadOnly(true); layout->addWidget(preview, 1);
    auto *detail = new QLabel; detail->setWordWrap(true); layout->addWidget(detail);
    auto *actions = new QHBoxLayout;
    auto *copy = new QPushButton(tr("Copy text")); copy->setObjectName("copySavedDraft");
    auto *restore = new QPushButton(tr("Restore in current session")); restore->setObjectName("restoreSavedDraft");
    auto *close = new QPushButton(tr("Close")); close->setDefault(true);
    actions->addWidget(copy); actions->addWidget(restore); actions->addStretch(); actions->addWidget(close); layout->addLayout(actions);
    const auto target = m_key;
    Draft selected;
    quint64 selection = 0;
    const auto presentSelection = [&] {
        if (selected.sending) {
            selected.sending = false; selected.uncertain = true;
            selected.notice = tr("This message may already have been sent. Check Activity or Terminal before sending again.");
        }
        preview->setPlainText(selected.text);
        detail->setText(selected.storageError.isEmpty() ? tr("%1 attachment(s). %2").arg(selected.attachments.size()).arg(selected.notice) : selected.storageError);
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
            if (confirm.exec() != QMessageBox::Ok || m_recording || m_key != target || isSending(target) || isWaitingToSend(target)) return;
        }
        selected.label.clear(); selected.formState = {};
        m_drafts[target] = selected; saveDraft(target);
        m_loading = true; restoreDraft(); m_loading = false; rebuildAttachments(); updateControls(); dialog.accept();
    });
    copy->setEnabled(false); restore->setEnabled(false);
    m_draftStore.indexAsync(&dialog, [&, populate](const QHash<QString, QString> &index) {
        populate(index); if (!list->count()) hint->setText(tr("No saved drafts on this desktop."));
    });
    dialog.exec();
}

void MessageComposer::setModelSettings(const QString &model, const QString &effort, const QJsonArray &options,
                                     bool enabled, const QString &reason, const QString &pendingModel,
                                     const QString &pendingEffort, const QString &applyWhen)
{
    m_model = model; m_effort = effort; m_modelOptions = options; m_settingsEnabled = enabled;
    m_settingsReason = reason; m_pendingModel = pendingModel; m_pendingEffort = pendingEffort; m_applyWhen = applyWhen;
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
    updateSettingsEfforts(); updateSettingsApply();
    const QRect available = screen()->availableGeometry();
    m_settingsPopup->setFixedWidth(qMin(360, available.width() - 24)); m_settingsPopup->adjustSize();
    QPoint position = m_settings->mapToGlobal(QPoint(m_settings->width() - m_settingsPopup->width(), -m_settingsPopup->height() - 7));
    position.setX(qBound(available.left() + 8, position.x(), available.right() - m_settingsPopup->width() - 8));
    position.setY(qBound(available.top() + 8, position.y(), available.bottom() - m_settingsPopup->height() - 8));
    m_settingsPopup->move(position); m_settingsPopup->show();
    (m_settingsTerminal->isVisible() ? static_cast<QWidget *>(m_settingsTerminal) : m_models)->setFocus(Qt::PopupFocusReason);
}

void MessageComposer::updateSettingsEfforts()
{
    const QString preferred = m_efforts->isVisible() && !m_efforts->currentData().toString().isEmpty()
        ? m_efforts->currentData().toString() : m_pendingEffort.isEmpty() ? m_effort : m_pendingEffort;
    const QSignalBlocker blocker(m_efforts); m_efforts->clear();
    for (const auto &value : m_modelOptions) {
        const auto option = value.toObject();
        if (option.value("id").toString() != m_models->currentData().toString()) continue;
        for (const auto &effort : option.value("effort_options").toArray()) {
            const QString id = effort.toString();
            if (!id.isEmpty()) m_efforts->addItem(id, id);
        }
        break;
    }
    if (m_efforts->count() == 0) m_efforts->addItem(tr("Provider default"), QString());
    const int selected = m_efforts->findData(preferred);
    m_efforts->setCurrentIndex(selected >= 0 ? selected : 0); updateSettingsApply();
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
    for (auto *widget : QList<QWidget *>{m_models,m_efforts,m_modelLabel,m_effortLabel,m_applySettings}) widget->setVisible(!native);
    bool offered = false;
    for (const auto &value : m_modelOptions) offered |= value.toObject().value("id").toString() == m_models->currentData().toString();
    const bool enabled = m_settingsEnabled && m_popupKey == m_key && !isSending(m_key);
    m_models->setEnabled(enabled && m_models->count() > 1);
    m_efforts->setEnabled(enabled && offered && !m_efforts->currentData().toString().isEmpty());
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
    if (pending)
        hint = tr("Pending: %1%2\nCurrent: %3%4\n%5").arg(intendedModel, intendedEffort.isEmpty() ? QString() : "  " + intendedEffort,
            m_model, m_effort.isEmpty() ? QString() : "  " + m_effort, hint);
    m_settingsHint->setText(hint);
}

void MessageComposer::setTheme(bool dark)
{
    m_dark = dark;
    updateSettingsButton();
    m_toolbar->setTheme(dark);
    m_attach->setIcon(workspaceIcon("attachment", dark ? QColor("#c5cfdb") : QColor("#536477")));
    m_stop->setIcon(workspaceIcon("stop",dark ? QColor("#ff9ca8") : QColor("#b52d48")));
    // Match the stylesheet's enabled and disabled Send text colors below.
    m_send->setIcon(composerIcon("send", QColor(dark ? "#10231b" : "#ffffff"), QColor(dark ? "#85909e" : "#6d7784")));
    const QColor chevron(dark ? "#a2adbc" : "#627082");
    m_settings->setProperty("chevron", QVariant::fromValue(composerIcon("chevron-down", chevron, chevron))); m_settings->update();
    setStyleSheet(QString(R"(
        QPlainTextEdit#messageInput { background:%1; color:%2; border:1px solid %3; border-radius:9px; padding:%8px; font-size:%9px; selection-background-color:%4; }
        QPlainTextEdit#messageInput:focus { border-color:%5; }
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
