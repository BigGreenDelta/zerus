#include "SpeechComposer.h"
#include "SpeechClient.h"
#include "SpeechRecorder.h"
#include "ComposerToolbar.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

SpeechComposer::SpeechComposer(const QString &executable, MessageComposer *composer, std::function<QString()> identity, SpeechRecorder *recorder,
                               std::function<bool(const QUrl &)> openSettings)
    : QObject(composer), m_composer(composer), m_client(new SpeechClient(executable, this)),
      m_recorder(recorder ? recorder : new SpeechRecorder(this)), m_poll(new QTimer(this)), m_identity(std::move(identity)),
      m_openSettings(openSettings ? std::move(openSettings) : QDesktopServices::openUrl)
{
    m_recorder->setParent(this);
    m_composer->installEventFilter(this);
    m_voice = new ToolbarChip; m_voice->setObjectName("voiceRecord"); m_voice->setIconName("microphone");
    m_cancel = new ToolbarChip; m_cancel->setObjectName("voiceCancel"); m_cancel->setIconName("close"); m_cancel->setLabels(tr("Cancel voice"), tr("Cancel"));
    m_result = new ToolbarChip; m_result->setObjectName("voiceResult"); m_result->setIconName("drafts"); m_result->setLabels(tr("Voice result"), tr("Result"));
    composer->toolbar()->add(ComposerToolbar::Slot::Voice, m_voice);
    composer->toolbar()->add(ComposerToolbar::Slot::VoiceCancel, m_cancel);
    composer->toolbar()->add(ComposerToolbar::Slot::VoiceResult, m_result);
    connect(m_voice, &QPushButton::clicked, this, [this] {
        if (m_state == State::Recording) { m_state = State::Submitting; update(); m_recorder->stop(); }
        else if (m_state == State::Uncertain) { m_deadline.restart(); poll(); }
        else if (m_state == State::Idle) start();
    });
    connect(m_cancel, &QPushButton::clicked, this, &SpeechComposer::cancel);
    connect(m_result, &QPushButton::clicked, this, &SpeechComposer::showResult);
    connect(composer, &MessageComposer::controlsChanged, this, &SpeechComposer::update);
    connect(m_recorder, &SpeechRecorder::started, this, [this] {
        if (m_state != State::Starting) return;
        m_state = State::Recording; m_seconds = 0; update();
    });
    connect(m_recorder, &SpeechRecorder::elapsed, this, [this](int seconds) { m_seconds = seconds; update(); });
    connect(m_recorder, &SpeechRecorder::recorded, this, [this](const QByteArray &base64) {
        if (m_state == State::Submitting || m_state == State::Recording) submitRecording(base64);
    });
    connect(m_recorder, &SpeechRecorder::failed, this, [this](SpeechRecorder::Failure reason, const QString &error) {
        if (m_state != State::Starting && m_state != State::Recording && m_state != State::Submitting) return;
        const bool denied = reason == SpeechRecorder::Failure::PermissionDenied;
        fail(error, denied ? Notice::MicrophonePermission : Notice::General);
        if (denied) {
            const auto generation = m_generation;
            QTimer::singleShot(0, this, [this, generation] {
                if (generation == m_generation && m_state == State::Idle && m_noticeKind == Notice::MicrophonePermission
                    && m_composer->isVisible()) showPermissionRecovery();
            });
        }
    });
    m_poll->setSingleShot(true); m_poll->setInterval(1200);
    connect(m_poll, &QTimer::timeout, this, &SpeechComposer::poll);
    m_pendingKey = "voice-pending\n" + composer->objectName();
    m_store.loadAsync(m_pendingKey, this, [this](const ComposerDraft &pending) {
        m_loadingPending = false;
        if (!pending.formState.value("job_id").toString().isEmpty()) {
            m_job = pending.formState.value("job_id").toString();
            m_host = pending.formState.value("host").toString();
            m_target.key = pending.formState.value("target").toString();
            m_state = State::Uncertain;
            m_notice = tr("An earlier voice request was interrupted. Check voice result to recover it from its original speech computer.");
        }
        update();
    });
    update();
}
bool SpeechComposer::eventFilter(QObject *watched, QEvent *event)
{
    // Closing/minimizing the workspace or navigating away must not leave an
    // invisible microphone recording, nor grant a stale permission callback.
    if (watched == m_composer && event->type() == QEvent::Hide
        && (m_state == State::Checking || m_state == State::Starting || m_state == State::Recording)) cancel();
    return QObject::eventFilter(watched, event);
}
void SpeechComposer::start()
{
    if (!m_composer->canAttachFiles() || !m_composer->isInputVisible() || m_composer->isPreparingSending(m_composer->sessionKey())) return;
    m_target = m_composer->voiceTarget(); m_originalIdentity = m_identity();
    if (m_originalIdentity.isEmpty()) { fail(tr("Wait for this conversation to finish loading before recording.")); return; }
    m_notice.clear(); m_noticeKind = Notice::General;
    const QSettings settings; m_host = settings.value("speech/host").toString(); m_profile = settings.value("speech/profile").toString();
    m_job.clear(); m_state = State::Checking; const auto generation = ++m_generation; update();
    m_client->request(m_host, {{"action", "providers"}}, [this, generation](const QJsonObject &reply, const QString &error) {
        if (generation != m_generation) return;
        if (!error.isEmpty()) { fail(error); return; }
        QJsonObject chosen;
        for (const auto &value : reply.value("profiles").toArray()) {
            const auto profile = value.toObject();
            if (profile.value("id").toString() == m_profile || (m_profile.isEmpty() && profile.value("available").toBool())) { chosen = profile; break; }
        }
        if (!chosen.value("available").toBool()) {
            fail(chosen.value("reason").toString(reply.value("reason").toString(tr("Choose an available provider in Settings > Voice input.")))); return;
        }
        m_profile = chosen.value("id").toString();
        if (m_profile.isEmpty()) { fail(tr("The speech computer returned an invalid provider.")); return; }
        QSettings settings;
        if (settings.value("speech/host").toString() == m_host && settings.value("speech/profile").toString().isEmpty())
            settings.setValue("speech/profile", m_profile);
        const auto current = m_composer->voiceTarget();
        if (m_identity() != m_originalIdentity || current.key != m_target.key || current.navigation != m_target.navigation) {
            fail(tr("The conversation changed before recording started. Start voice input again in the intended conversation.")); return;
        }
        // Include edits made while the provider probe was pending in the capture.
        m_target = current; m_state = State::Starting; update();
        m_recorder->start(QSettings().value("speech/microphone").toByteArray());
    });
}
void SpeechComposer::submitRecording(const QByteArray &base64)
{
    m_job = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_state = State::Submitting; m_deadline.start(); const auto generation = m_generation; update();
    const QJsonObject audio{{"format", "pcm_s16le"}, {"sample_rate", 16000}, {"channels", 1}, {"data_base64", QString::fromLatin1(base64)}};
    ComposerDraft pending; pending.formState = {{"job_id", m_job}, {"host", m_host}, {"target", m_target.key}};
    m_store.saveDurableAsync(m_pendingKey, pending, this, [this, generation, audio](bool saved, const ComposerDraft &) {
        if (generation != m_generation) return;
        if (!saved) { fail(tr("Could not save the voice request for recovery. No audio was sent.")); return; }
        m_client->request(m_host, {{"action", "submit"}, {"request_id", m_job}, {"profile_id", m_profile}, {"audio", audio}},
            [this, generation](const QJsonObject &reply, const QString &error) {
                if (generation != m_generation) return;
                // Acknowledgements are content-free even when the durable job
                // already completed. Only status is allowed to return text.
                if (error.isEmpty() && reply.value("job_id").toString() == m_job && reply.value("state") == "completed") poll();
                else result(reply, error);
            });
    });
}
void SpeechComposer::poll()
{
    if (m_job.isEmpty()) return;
    m_state = State::Recognizing; const auto generation = m_generation; update();
    m_client->request(m_host, {{"action", "status"}, {"job_id", m_job}},
        [this, generation](const QJsonObject &reply, const QString &error) { if (generation == m_generation) result(reply, error); });
}
void SpeechComposer::result(const QJsonObject &reply, const QString &error)
{
    if (!error.isEmpty()) { m_state = State::Uncertain; m_notice = error; update(); return; }
    if (reply.value("job_id").toString() != m_job) { m_state = State::Uncertain; m_notice = tr("Speech response identity did not match this recording."); update(); return; }
    const auto state = reply.value("state").toString();
    if (state == "completed") { completed(reply.value("text").toString()); return; }
    if (state == "failed" || state == "cancelled") { clearPending(); fail(reply.value("error").toString(tr("Voice recognition was cancelled or failed."))); return; }
    if ((state != "queued" && state != "running") || m_deadline.elapsed() > 10 * 60 * 1000) {
        m_state = State::Uncertain; m_notice = tr("The speech result is not confirmed. Check result to query this recording again."); update(); return;
    }
    m_state = State::Recognizing; update(); m_poll->start();
}
void SpeechComposer::completed(const QString &text)
{
    if (text.trimmed().isEmpty()) { clearPending(); fail(tr("No speech was recognized. Try recording again.")); return; }
    m_text = text; m_state = State::Saving;
    m_recoveryKey = "voice\n" + m_job;
    ComposerDraft recovery; recovery.text = text;
    recovery.label = tr("Voice input (%1) %2").arg(QDateTime::currentDateTime().toString(Qt::ISODate), QString(m_target.key).replace('\n', " / "));
    recovery.notice = tr("Recognized voice. Review before inserting or sending.");
    const auto generation = m_generation; update();
    m_store.saveDurableAsync(m_recoveryKey, recovery, this, [this, generation](bool saved, const ComposerDraft &) {
        if (generation != m_generation) return;
        m_state = State::Idle;
        if (!saved) { m_notice = tr("Could not save the voice result. Open Voice result and copy it before closing Zerus."); update(); return; }
        clearPending();
        m_notice = tr("Voice result saved. Open Voice result to insert or copy it, or find it in Saved drafts.");
        if (m_identity() == m_originalIdentity) {
            const auto recoveryKey = m_recoveryKey;
            if (m_composer->insertVoiceText(m_target, m_text, [this, generation, recoveryKey](bool durable) {
                if (!durable) return;
                m_store.saveDurableAsync(recoveryKey, {}, this, [](bool, const ComposerDraft &) {});
                if (generation == m_generation) { m_text.clear(); m_notice.clear(); update(); }
            })) { update(); return; }
        }
        update();
    });
}
void SpeechComposer::cancel()
{
    ++m_generation; m_poll->stop(); m_recorder->cancel();
    if (!m_job.isEmpty() && (m_state == State::Submitting || m_state == State::Recognizing || m_state == State::Uncertain))
        m_client->request(m_host, {{"action", "cancel"}, {"job_id", m_job}}, [](const QJsonObject &, const QString &) {});
    m_job.clear(); m_state = State::Idle; m_notice.clear(); update();
    clearPending();
}
void SpeechComposer::clearPending() { m_store.saveDurableAsync(m_pendingKey, {}, this, [](bool, const ComposerDraft &) {}); }
void SpeechComposer::fail(const QString &error, Notice notice) { m_state = State::Idle; m_notice = error; m_noticeKind = notice; update(); }
void SpeechComposer::update()
{
    const bool recording = m_state == State::Recording;
    const bool idle = m_state == State::Idle;
    m_voice->setActive(true);
    m_voice->setEnabled(!m_loadingPending && (recording || m_state == State::Uncertain || (idle && m_composer->canAttachFiles() && m_composer->isInputVisible()
        && !m_composer->isPreparingSending(m_composer->sessionKey()))));
    m_voice->setTone(recording ? ChipTone::Danger : m_state == State::Uncertain ? ChipTone::Warning : ChipTone::Quiet);
    m_voice->setIconName(recording ? "stop" : "microphone");
    const auto label = recording ? tr("Stop voice (%1:%2)").arg(m_seconds / 60).arg(m_seconds % 60, 2, 10, QLatin1Char('0'))
        : idle ? tr("Voice") : m_state == State::Checking ? tr("Checking voice…") : m_state == State::Starting ? tr("Opening microphone…")
        : m_state == State::Uncertain ? tr("Check voice result") : m_state == State::Saving ? tr("Saving voice…") : tr("Recognizing…");
    m_voice->setLabels(label, recording ? tr("Stop (%1s)").arg(m_seconds) : QString());
    m_voice->setDetail(m_notice.isEmpty() ? tr("Record up to two minutes. Recognition adds an editable draft and never sends it.") : m_notice);
    m_cancel->setActive(!idle && m_state != State::Saving);
    m_result->setActive(!m_text.isEmpty() || !m_notice.isEmpty());
    m_result->setLabels(m_noticeKind == Notice::MicrophonePermission ? tr("Microphone access") : m_text.isEmpty() ? tr("Voice notice") : tr("Voice result"),
                        m_noticeKind == Notice::MicrophonePermission ? tr("Microphone") : m_text.isEmpty() ? tr("Notice") : tr("Result"));
    m_result->setDetail(m_notice);
}
void SpeechComposer::showResult()
{
    if (m_noticeKind == Notice::MicrophonePermission) { showPermissionRecovery(); return; }
    QDialog dialog(m_composer); dialog.setObjectName("voiceResultDialog"); dialog.setWindowTitle(tr("Voice input"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(m_notice); hint->setWordWrap(true); layout->addWidget(hint);
    if (m_text.isEmpty()) {
        hint->setFixedWidth(420); layout->setSizeConstraint(QLayout::SetFixedSize);
        auto *close = new QPushButton(tr("Close")); layout->addWidget(close,0,Qt::AlignRight);
        connect(close,&QPushButton::clicked,&dialog,&QDialog::accept); dialog.exec(); return;
    }
    dialog.resize(600, 350);
    auto *text = new QPlainTextEdit(m_text); text->setReadOnly(true); layout->addWidget(text, 1);
    auto *row = new QHBoxLayout; layout->addLayout(row);
    auto *insert = new QPushButton(tr("Insert into current draft")); insert->setObjectName("voiceInsert");
    auto *copy = new QPushButton(tr("Copy")); auto *close = new QPushButton(tr("Close"));
    row->addWidget(insert); row->addWidget(copy); row->addStretch(); row->addWidget(close);
    insert->setEnabled(!m_text.isEmpty() && m_composer->canAttachFiles() && m_composer->isInputVisible()); copy->setEnabled(!m_text.isEmpty());
    const auto target = m_composer->voiceTarget(); const auto identity = m_identity(); const auto recognized = m_text; const auto recovery = m_recoveryKey;
    connect(insert, &QPushButton::clicked, &dialog, [&, target, identity, recognized, recovery] {
        if (m_identity() != identity || !m_composer->insertVoiceText(target, recognized, [this, recovery](bool saved) {
            if (saved) m_store.saveDurableAsync(recovery, {}, this, [](bool, const ComposerDraft &) {});
        })) { hint->setText(tr("The current draft changed. Close and reopen Voice result to review the destination.")); return; }
        m_text.clear(); m_notice.clear(); update(); dialog.accept();
    });
    connect(copy, &QPushButton::clicked, &dialog, [recognized] { QApplication::clipboard()->setText(recognized); });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    dialog.exec();
}

QUrl SpeechComposer::microphoneSettingsUrl()
{
#ifdef Q_OS_MACOS
    return QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"));
#elif defined(Q_OS_WIN)
    return QUrl(QStringLiteral("ms-settings:privacy-microphone"));
#else
    return {};
#endif
}
void SpeechComposer::showPermissionRecovery()
{
    QDialog dialog(m_composer); dialog.setObjectName("voicePermissionDialog"); dialog.setWindowTitle(tr("Microphone access"));
    auto *layout = new QVBoxLayout(&dialog); layout->setSpacing(16); layout->setSizeConstraint(QLayout::SetFixedSize);
#ifdef Q_OS_MACOS
    const auto instructions = tr("Allow Zerus to use the microphone in System Settings > Privacy & Security > Microphone. Then return to Zerus and press Voice again.");
#elif defined(Q_OS_WIN)
    const auto instructions = tr("Open Settings > Privacy & security > Microphone. Enable Microphone access and Let desktop apps access your microphone. Then return to Zerus and press Voice again.");
#else
    const auto instructions = tr("Open your desktop's Settings and check microphone permissions for Zerus. Then return to Zerus and press Voice again.");
#endif
    auto *hint = new QLabel(instructions); hint->setObjectName("voicePermissionHint"); hint->setWordWrap(true); hint->setFixedWidth(420); layout->addWidget(hint);
    auto *row = new QHBoxLayout; layout->addLayout(row);
    const auto settings = microphoneSettingsUrl();
    if (!settings.isEmpty()) {
        auto *open = new QPushButton(tr("Open microphone settings")); open->setObjectName("voiceOpenMicrophoneSettings"); row->addWidget(open);
        connect(open,&QPushButton::clicked,&dialog,[this,&dialog,hint,instructions,settings] {
            if (m_openSettings(settings)) dialog.accept();
            else hint->setText(tr("Could not open system settings automatically.\n\n%1").arg(instructions));
        });
    }
    row->addStretch(); auto *close = new QPushButton(tr("Close")); close->setObjectName("voicePermissionClose"); row->addWidget(close);
    connect(close,&QPushButton::clicked,&dialog,&QDialog::accept); dialog.exec();
}
