#include "SpeechComposer.h"
#include "SpeechClient.h"
#include "SpeechRecorder.h"
#include "ComposerToolbar.h"
#include "AttachmentViewer.h"
#include <QAudioOutput>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtEndian>

namespace {
QByteArray wav(const QByteArray &pcm) {
    QByteArray result("RIFF",4);
    const auto u32=[&](quint32 value){char bytes[4];qToLittleEndian(value,bytes);result.append(bytes,4);};
    const auto u16=[&](quint16 value){char bytes[2];qToLittleEndian(value,bytes);result.append(bytes,2);};
    u32(pcm.size()+36);result.append("WAVEfmt ",8);u32(16);u16(1);u16(1);u32(16000);u32(32000);u16(2);u16(16);
    result.append("data",4);u32(pcm.size());result.append(pcm);return result;
}
QString duration(int seconds) { return QString("%1:%2").arg(seconds/60).arg(seconds%60,2,10,QLatin1Char('0')); }
}

SpeechComposer::SpeechComposer(const QString &executable, MessageComposer *composer, std::function<Target()> target,
                               SpeechRecorder *recorder, std::function<bool(const QUrl &)> openSettings)
    : QObject(composer), m_composer(composer), m_client(new SpeechClient(executable,this)),
      m_recorder(recorder ? recorder : new SpeechRecorder(this)), m_currentTarget(std::move(target)),
      m_openSettings(openSettings ? std::move(openSettings) : QDesktopServices::openUrl)
{
    m_recorder->setParent(this);m_composer->installEventFilter(this);
    m_voice=new ToolbarChip;m_voice->setObjectName("voiceRecord");m_voice->setIconName("microphone");
    m_cancel=new ToolbarChip;m_cancel->setObjectName("voiceCancel");m_cancel->setIconName("close");m_cancel->setLabels(tr("Cancel recording"),tr("Cancel"));
    m_result=new ToolbarChip;m_result->setObjectName("voiceResult");m_result->setIconName("microphone");
    composer->toolbar()->add(ComposerToolbar::Slot::Voice,m_voice);
    composer->toolbar()->add(ComposerToolbar::Slot::VoiceCancel,m_cancel);
    composer->toolbar()->add(ComposerToolbar::Slot::VoiceResult,m_result);
    m_preview=new QWidget(composer);m_preview->setObjectName("voicePreview");
    auto *row=new QHBoxLayout(m_preview);row->setContentsMargins(0,4,0,4);
    m_previewLabel=new QLabel;m_previewLabel->setTextFormat(Qt::PlainText);row->addWidget(m_previewLabel);
    m_play=new QPushButton(tr("Play"));m_play->setObjectName("voicePreviewPlay");row->addWidget(m_play);
    m_provider=new QComboBox;m_provider->setObjectName("voiceProvider");m_provider->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_provider->setMinimumContentsLength(12);row->addWidget(m_provider,1);
    m_discard=new QPushButton(tr("Discard"));m_discard->setObjectName("voiceDiscard");row->addWidget(m_discard);
    m_send=new QPushButton(tr("Send voice"));m_send->setObjectName("voiceSend");row->addWidget(m_send);
    qobject_cast<QVBoxLayout *>(composer->layout())->insertWidget(0,m_preview);
    for(auto *button:{m_play,m_discard,m_send})button->setAutoDefault(false);
    connect(m_voice,&QPushButton::clicked,this,[this]{if(m_state==State::Recording){m_state=State::Saving;update();m_recorder->stop();}else start();});
    connect(m_cancel,&QPushButton::clicked,this,&SpeechComposer::discard);
    connect(m_result,&QPushButton::clicked,this,&SpeechComposer::showResult);
    connect(m_send,&QPushButton::clicked,this,[this]{if(m_state==State::Uncertain)check();else send();});
    connect(m_discard,&QPushButton::clicked,this,&SpeechComposer::discard);
    connect(m_play,&QPushButton::clicked,this,&SpeechComposer::play);
    connect(m_provider,&QComboBox::currentIndexChanged,this,[this]{
        if(m_syncing)return;
        const auto profile=m_provider->currentData().toJsonObject();
        if(!profile.isEmpty()){QSettings s;s.setValue("speech/machine_id",profile.value("machine_id").toString());s.setValue("speech/profile",profile.value("profile_id").toString());}
        update();
    });
    connect(composer,&MessageComposer::controlsChanged,this,[this]{syncTarget();update();});
    connect(m_recorder,&SpeechRecorder::started,this,[this]{if(m_state!=State::Starting)return;m_state=State::Recording;m_seconds=0;update();});
    connect(m_recorder,&SpeechRecorder::elapsed,this,[this](int seconds){m_seconds=seconds;update();});
    connect(m_recorder,&SpeechRecorder::recorded,this,[this](const QByteArray &audio){if(m_state==State::Saving || m_state==State::Recording)recorded(audio);});
    connect(m_recorder,&SpeechRecorder::failed,this,[this](SpeechRecorder::Failure reason,const QString &error){
        if(m_state!=State::Starting && m_state!=State::Recording && m_state!=State::Saving)return;
        const bool denied=reason==SpeechRecorder::Failure::PermissionDenied;fail(error,denied?Notice::MicrophonePermission:Notice::General);
        if(denied){const auto generation=m_generation;QTimer::singleShot(0,this,[this,generation]{if(generation==m_generation && m_noticeKind==Notice::MicrophonePermission && m_composer->isVisible())showPermissionRecovery();});}
    });
    syncTarget();update();
}
QString SpeechComposer::storageKey() const {return "voice-message-draft\n"+m_loadedKey;}
QJsonObject SpeechComposer::identityPayload(const QString &action) const {
    return {{"action",action},{"request_id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"expected_run_id",m_target.run},{"expected_conversation_id",m_target.conversation}};
}
bool SpeechComposer::eventFilter(QObject *watched,QEvent *event) {
    if(watched==m_composer && event->type()==QEvent::Hide){stopPlayback();if(m_state==State::Checking || m_state==State::Starting || m_state==State::Recording)discard();}
    return QObject::eventFilter(watched,event);
}
void SpeechComposer::syncTarget() {
    if(m_syncing)return;
    const auto key=m_composer->sessionKey();if(key==m_loadedKey)return;
    ++m_generation;m_recorder->cancel();stopPlayback();m_loadedKey=key;m_target=m_currentTarget();
    m_pcm.clear();m_id.clear();m_notice.clear();m_noticeKind=Notice::General;m_state=State::Idle;m_loading=!key.isEmpty();
    m_syncing=true;m_provider->clear();m_syncing=false;
    if(key.isEmpty())return;
    const auto generation=m_generation;
    m_store.loadAsync(storageKey(),this,[this,generation](const ComposerDraft &draft){
        if(generation!=m_generation)return;m_loading=false;
        if(!draft.attachments.isEmpty()){
            const auto meta=draft.formState;
            m_target={m_loadedKey,meta.value("host").toString(),meta.value("session").toString(),meta.value("run").toString(),meta.value("conversation").toString(),{},false};
            const auto audio=draft.attachments.first().data;
            if(!audio.startsWith("RIFF") || audio.mid(8,8)!="WAVEfmt " || audio.mid(36,4)!="data" || audio.size()<3244){m_notice=tr("The saved voice recording is invalid.");update();return;}
            m_pcm=audio.mid(44);m_id=meta.value("id").toString();m_seconds=m_pcm.size()/32000;
            m_state=meta.value("submitted").toBool()?State::Uncertain:State::Preview;
            if(m_state==State::Uncertain)m_notice=tr("Submission was interrupted. Check this message; audio is never sent twice automatically.");
            loadProfiles([](bool){});
        }
        update();
    });
}
void SpeechComposer::loadProfiles(std::function<void(bool)> done) {
    const auto generation=m_generation;
    m_client->requestVoice(m_target.host,m_target.session,identityPayload("profiles"),[this,generation,done](const QJsonObject &reply,const QString &error){
        if(generation!=m_generation)return;
        if(!error.isEmpty()){m_notice=error;update();done(false);return;}
        const QSettings settings;const auto preferredMachine=settings.value("speech/machine_id").toString();const auto preferredProfile=settings.value("speech/profile").toString();
        const auto previous=m_provider->currentData().toJsonObject();int selected=-1,profileMatch=-1;
        m_syncing=true;m_provider->clear();
        for(const auto &computerValue:reply.value("computers").toArray()){
            const auto computer=computerValue.toObject();const auto machine=computer.value("machine_id").toString();
            for(const auto &value:computer.value("profiles").toArray()){
                const auto profile=value.toObject();if(!profile.value("available").toBool() || machine.isEmpty())continue;
                const auto id=profile.value("id").toString();if(id.isEmpty())continue;
                const QJsonObject data{{"machine_id",machine},{"profile_id",id}};
                m_provider->addItem(computer.value("name").toString()+" / "+profile.value("name").toString(id),QVariant::fromValue(data));
                const auto index=m_provider->count()-1;
                if(data==previous || (machine==preferredMachine && id==preferredProfile))selected=index;
                if(profileMatch<0 && preferredMachine.isEmpty() && id==preferredProfile)profileMatch=index;
            }
        }
        if(selected<0)selected=profileMatch;
        if(selected>=0)m_provider->setCurrentIndex(selected);
        else if(!preferredProfile.isEmpty()){
            m_provider->addItem(tr("Selected provider (unavailable)"),QVariant::fromValue(QJsonObject{}));
            m_provider->setCurrentIndex(m_provider->count()-1);
        }
        m_syncing=false;
        if(!m_provider->count())m_notice=tr("No speech provider is available from this conversation computer.");
        update();done(m_provider->count()>0);
    });
}
void SpeechComposer::start() {
    syncTarget();const auto target=m_currentTarget();
    if(m_loading || m_state!=State::Idle || !target.available || target.run.isEmpty() || target.conversation.isEmpty() || !m_composer->isInputVisible())return;
    m_target=target;m_notice.clear();m_noticeKind=Notice::General;m_state=State::Checking;const auto generation=++m_generation;update();
    loadProfiles([this,generation](bool okay){
        if(generation!=m_generation)return;
        if(!okay){m_state=State::Idle;update();return;}
        if(!m_currentTarget().sameConversation(m_target)){fail(tr("The conversation changed. Start recording again in the intended conversation."));return;}
        m_state=State::Starting;update();m_recorder->start(QSettings().value("speech/microphone").toByteArray());
    });
}
void SpeechComposer::recorded(const QByteArray &base64) {
    const auto decoded=QByteArray::fromBase64Encoding(base64,QByteArray::AbortOnBase64DecodingErrors);
    if(!decoded || decoded.decoded.size()<3200 || decoded.decoded.size()>SpeechPcmConverter::MaximumBytes){fail(tr("The voice recording is invalid."));return;}
    m_pcm=decoded.decoded;m_seconds=m_pcm.size()/32000;m_id=QUuid::createUuid().toString(QUuid::WithoutBraces);m_state=State::Saving;
    saveVoice([this](bool saved){m_state=State::Preview;if(!saved)m_notice=tr("Could not save this recording. Keep Zerus open and try Send voice again.");update();});
}
void SpeechComposer::saveVoice(std::function<void(bool)> done,bool requireGeneration) {
    ComposerDraft draft;draft.label=tr("Voice message (%1)").arg(m_target.session);
    draft.notice=tr("Open the original conversation to review or check this voice message.");
    draft.attachments.append({"voice.wav","audio/wav",wav(m_pcm),{}});
    draft.formState={{"kind","voice_message"},{"id",m_id},{"host",m_target.host},{"session",m_target.session},{"run",m_target.run},{"conversation",m_target.conversation},
        {"submitted",m_state==State::Submitting || m_state==State::Uncertain}};
    const auto generation=m_generation;
    m_store.saveDurableAsync(storageKey(),draft,this,[this,generation,done,requireGeneration](bool okay,const ComposerDraft &){if(!requireGeneration || generation==m_generation)done(okay);});
}
void SpeechComposer::send() {
    if(m_state!=State::Preview || m_pcm.isEmpty() || m_provider->currentData().toJsonObject().value("machine_id").toString().isEmpty())return;
    const auto current=m_currentTarget();
    if(!current.available || !current.sameConversation(m_target)){m_notice=tr("The conversation changed or cannot receive input. The recording is kept without sending.");update();return;}
    if(!current.warning.isEmpty()){
        const auto originalId=m_id;const auto originalGeneration=m_generation;const auto originalTarget=m_target;
        QMessageBox confirm(QMessageBox::Warning,tr("Continue with a cold cache?"),current.warning,QMessageBox::NoButton,m_composer);
        confirm.setObjectName("voiceColdCacheConfirm");
        auto *send=confirm.addButton(tr("Send with full context"),QMessageBox::AcceptRole);send->setObjectName("confirmVoiceSend");
        auto *cancel=confirm.addButton(QMessageBox::Cancel);confirm.setDefaultButton(cancel);confirm.setEscapeButton(cancel);confirm.exec();
        const auto after=m_currentTarget();
        if(confirm.clickedButton()!=send || m_generation!=originalGeneration || m_id!=originalId || m_state!=State::Preview
            || !m_target.sameConversation(originalTarget) || !after.available || !after.sameConversation(originalTarget))return;
    }
    stopPlayback();m_notice.clear();m_state=State::Submitting;update();
    const auto generation=m_generation;auto payload=identityPayload("submit");payload["request_id"]=m_id;payload["speech"]=m_provider->currentData().toJsonObject();
    const auto chosen=m_provider->currentData().toJsonObject();QSettings settings;
    settings.setValue("speech/machine_id",chosen.value("machine_id").toString());settings.setValue("speech/profile",chosen.value("profile_id").toString());
    payload["audio"]=QJsonObject{{"format","pcm_s16le"},{"sample_rate",16000},{"channels",1},{"data_base64",QString::fromLatin1(m_pcm.toBase64())}};
    const auto target=m_target;const auto key=storageKey();
    saveVoice([this,generation,payload,target,key](bool saved){
        if(!saved){if(generation==m_generation){m_state=State::Preview;m_notice=tr("Could not save the voice request. No audio was sent.");update();}return;}
        // Explicit Send owns this immutable target even if navigation changes
        // while the local checkpoint is being written.
        m_client->requestVoice(target.host,target.session,payload,[this,generation,target,key,payload](const QJsonObject &reply,const QString &error){
            if(generation==m_generation){result(reply,error);return;}
            const auto message=reply.value("message").toObject();
            if(error.isEmpty() && message.value("id")==payload.value("request_id") && message.value("run_id")==target.run && message.value("conversation_id")==target.conversation){
                emit messageChanged(target.key,message);m_store.saveDurableAsync(key,{},this,[](bool,const ComposerDraft&){});
            }
        });
    },false);
}
void SpeechComposer::check() {
    if(m_id.isEmpty() || m_state!=State::Uncertain)return;
    m_state=State::Submitting;update();auto payload=identityPayload("status");payload["message_id"]=m_id;const auto generation=m_generation;
    m_client->requestVoice(m_target.host,m_target.session,payload,[this,generation](const QJsonObject &reply,const QString &error){if(generation==m_generation)result(reply,error);});
}
void SpeechComposer::result(const QJsonObject &reply,const QString &error) {
    const auto message=reply.value("message").toObject();
    if(!error.isEmpty() || message.value("id").toString()!=m_id || message.value("run_id").toString()!=m_target.run || message.value("conversation_id").toString()!=m_target.conversation){
        m_state=State::Uncertain;m_notice=error.isEmpty()?tr("Voice submission is not confirmed. Check this message before trying another recording."):error;update();return;
    }
    emit messageChanged(m_target.key,message);
    const auto generation=m_generation;
    m_store.saveDurableAsync(storageKey(),{},this,[this,generation](bool saved,const ComposerDraft &){
        if(generation!=m_generation)return;
        if(!saved){m_state=State::Uncertain;m_notice=tr("The message was accepted, but its local receipt could not be saved. Check it again.");update();return;}
        m_pcm.clear();m_id.clear();m_state=State::Idle;m_notice.clear();update();
    });
}
void SpeechComposer::discard() {
    if(m_state==State::Submitting || m_state==State::Saving)return;
    if(m_state==State::Uncertain){
        auto payload=identityPayload("cancel");payload["message_id"]=m_id;
        const auto generation=m_generation;m_state=State::Submitting;update();
        m_client->requestVoice(m_target.host,m_target.session,payload,[this,generation](const QJsonObject &reply,const QString &error){if(generation==m_generation)result(reply,error);});
        return;
    }
    if(!m_pcm.isEmpty()){
        const auto originalId=m_id;const auto originalTarget=m_target;const auto originalGeneration=m_generation;
        QMessageBox dialog(QMessageBox::Warning,tr("Discard voice recording?"),
            tr("Delete the saved, unsent voice recording for %1?").arg(m_target.session),QMessageBox::NoButton,m_composer);
        dialog.setObjectName("voiceDiscardConfirm");dialog.setTextFormat(Qt::PlainText);
        auto *confirm=dialog.addButton(tr("Discard recording"),QMessageBox::DestructiveRole);confirm->setObjectName("confirmVoiceDiscard");
        auto *cancel=dialog.addButton(QMessageBox::Cancel);dialog.setDefaultButton(cancel);dialog.setEscapeButton(cancel);dialog.exec();
        if(dialog.clickedButton()!=confirm || m_generation!=originalGeneration || m_id!=originalId
            || !m_target.sameConversation(originalTarget) || m_state!=State::Preview)return;
        stopPlayback();const auto generation=m_generation;m_state=State::Saving;update();
        m_store.saveDurableAsync(storageKey(),{},this,[this,generation](bool saved,const ComposerDraft&){
            if(generation!=m_generation)return;
            if(!saved){m_state=State::Preview;m_notice=tr("Could not discard the saved recording. Try again.");update();return;}
            m_pcm.clear();m_id.clear();m_state=State::Idle;m_notice.clear();update();
        });return;
    }
    ++m_generation;m_recorder->cancel();stopPlayback();m_pcm.clear();m_id.clear();m_state=State::Idle;m_notice.clear();m_noticeKind=Notice::General;
    if(!m_loadedKey.isEmpty())m_store.saveDurableAsync(storageKey(),{},this,[](bool,const ComposerDraft&){});update();
}
void SpeechComposer::stopPlayback(){if(m_player)m_player->stop();}
void SpeechComposer::play() {
    if(m_pcm.isEmpty())return;
    if(m_player && m_player->playbackState()==QMediaPlayer::PlayingState){m_player->pause();return;}
    if(!m_player){m_player=new QMediaPlayer(this);m_output=new QAudioOutput(this);m_player->setAudioOutput(m_output);
        connect(m_player,&QMediaPlayer::playbackStateChanged,this,[this]{update();});
        connect(m_player,&QMediaPlayer::errorOccurred,this,[this](QMediaPlayer::Error,const QString &error){m_notice=tr("Could not play recording: %1").arg(error);update();});}
    const auto generation=m_generation;
    AttachmentFiles::prepareAsync({MessageAttachment{"voice.wav","audio/wav",wav(m_pcm),{}}},this,[this,generation](const QJsonArray &files,const QString &error){
        if(generation!=m_generation || m_pcm.isEmpty())return;
        if(!error.isEmpty() || files.isEmpty()){m_notice=error;update();return;}
        const auto url=QUrl::fromLocalFile(files.first().toObject().value("local_path").toString());if(m_player->source()!=url)m_player->setSource(url);m_player->play();
    });
}
void SpeechComposer::fail(const QString &error,Notice notice){m_state=State::Idle;m_notice=error;m_noticeKind=notice;update();}
void SpeechComposer::update() {
    const auto current=m_currentTarget();const bool recording=m_state==State::Recording;
    m_voice->setActive(true);m_voice->setEnabled(!m_loading && (recording || (m_state==State::Idle && current.available && !current.conversation.isEmpty() && m_composer->isInputVisible())));
    m_voice->setTone(recording?ChipTone::Danger:ChipTone::Quiet);m_voice->setIconName(recording?"stop":"microphone");
    m_voice->setLabels(recording?tr("Stop voice (%1)").arg(duration(m_seconds)):m_state==State::Checking?tr("Checking voice…"):m_state==State::Starting?tr("Opening microphone…"):tr("Voice"));
    m_voice->setDetail(tr("Record a voice message, review it, then choose Send voice. Your text draft stays separate."));
    m_cancel->setActive(m_state==State::Checking || m_state==State::Starting || recording);
    const bool preview=!m_pcm.isEmpty();m_preview->setVisible(preview);m_previewLabel->setText(tr("Voice %1").arg(duration(m_seconds)));
    m_play->setText(m_player && m_player->playbackState()==QMediaPlayer::PlayingState?tr("Pause"):tr("Play"));
    m_play->setEnabled(preview && m_state!=State::Submitting);m_discard->setEnabled(m_state==State::Preview || m_state==State::Uncertain);m_provider->setEnabled(m_state==State::Preview);
    m_discard->setText(m_state==State::Uncertain?tr("Cancel message"):tr("Discard"));
    m_send->setText(m_state==State::Uncertain?tr("Check message"):m_state==State::Submitting?tr("Sending voice…"):tr("Send voice"));
    m_send->setEnabled(m_state==State::Uncertain || (m_state==State::Preview && current.available && current.sameConversation(m_target)
        && !m_provider->currentData().toJsonObject().value("machine_id").toString().isEmpty()));
    m_result->setActive(!m_notice.isEmpty());m_result->setLabels(m_noticeKind==Notice::MicrophonePermission?tr("Microphone access"):tr("Voice notice"));m_result->setDetail(m_notice);
}
void SpeechComposer::showResult() {
    if(m_noticeKind==Notice::MicrophonePermission){showPermissionRecovery();return;}
    QDialog dialog(m_composer);dialog.setObjectName("voiceResultDialog");dialog.setWindowTitle(tr("Voice message"));
    auto *layout=new QVBoxLayout(&dialog);layout->setSizeConstraint(QLayout::SetFixedSize);
    auto *hint=new QLabel(m_notice);hint->setWordWrap(true);hint->setFixedWidth(420);layout->addWidget(hint);
    auto *close=new QPushButton(tr("Close"));layout->addWidget(close,0,Qt::AlignRight);connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);dialog.exec();
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
