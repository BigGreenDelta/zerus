#include "speech/SpeechClient.h"
#include "speech/SpeechRecorder.h"
#include "speech/RoundVideo.h"
#include "speech/SpeechComposer.h"
#include "speech/SpeechSettings.h"
#include "ComposerToolbar.h"
#include "ComposerDraftStore.h"
#include <QApplication>
#include <QAudioDevice>
#include <QDir>
#include <QDialog>
#include <QComboBox>
#include <QClipboard>
#include <QCheckBox>
#include <QLineEdit>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QVBoxLayout>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <optional>
#ifdef Q_OS_WIN
#include "support/FakeCli.h"
#endif

namespace {
QAudioFormat format(int rate, int channels = 1) {
    QAudioFormat result; result.setSampleRate(rate); result.setChannelCount(channels); result.setSampleFormat(QAudioFormat::Int16); return result;
}
QByteArray tone(int rate, int channels, int frames, int frequency) {
    QByteArray result; result.resize(frames * channels * 2);
    for (int frame = 0; frame < frames; ++frame) {
        const qint16 sample = qint16(std::sin(frame * 2 * 3.14159265358979323846 * frequency / rate) * 16000);
        for (int channel = 0; channel < channels; ++channel) std::memcpy(result.data() + (frame * channels + channel) * 2, &sample, 2);
    }
    return result;
}
double rms(const QByteArray &pcm) {
    double sum = 0; const int skip = 100;
    for (qsizetype index = skip * 2; index < pcm.size() - skip * 2; index += 2) {
        const double value = qFromLittleEndian<qint16>(pcm.constData() + index); sum += value * value;
    }
    return std::sqrt(sum / (pcm.size()/2 - 2*skip));
}
QString fixture(const QString &directory) {
    QFile file(directory + "/hgs"); if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(R"PY(#!/usr/bin/python3
import json, pathlib, sys, time, uuid
root = pathlib.Path(__file__).parent
with (root / 'calls').open('a') as log:
    log.write(json.dumps(sys.argv[1:]) + '\n')
if '--help' in sys.argv:
    while (root / 'help-gate').exists(): time.sleep(.01)
    print('hgs ls --json' if '@legacy' in sys.argv or (root / 'legacy').exists() else 'hgs speech --json\nhgs voice <session> --json' + ('' if (root/'management-legacy').exists() else '\nSpeech profile management v1'))
else:
    payload = json.load(sys.stdin)
    (root / 'payload').write_text(json.dumps(payload))
    if payload['action'].startswith(('config_','profile_')):
        action=payload['action'];machine='cccccccc-cccc-4ccc-cccc-cccccccccccc' if '@other-host' in sys.argv else 'bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb'
        configured=json.loads((root/'profiles').read_text()) if (root/'profiles').exists() else [{'id':'russian','name':'Russian','available':True}]
        profiles=[dict({'revision':1,'adapter':'gigaam','model':'v3_e2e_rnnt','language':'ru','device':'cpu','credential':{'configured':False}},**p) for p in configured]
        state_path=root/('config-'+machine); state=json.loads(state_path.read_text()) if state_path.exists() else {'revision':1,'profiles':profiles}
        if action=='config_list':
            (root/'config-started').write_text('started')
            while (root/'config-gate').exists():time.sleep(.01)
            print(json.dumps(dict(schema=1,machine_id=machine,service=json.loads((root/'service').read_text()) if (root/'service').exists() else {'available':True},adapters=[{'id':'gigaam','models':['v3_e2e_rnnt','v3_e2e_ctc'],'devices':['cpu','cuda','mps']},{'id':'openai_http'}],**state)))
            sys.exit(0)
        ops_path=root/'operations';ops=json.loads(ops_path.read_text()) if ops_path.exists() else {}
        if action=='config_status':
            if payload['operation_id'] in ops and ops[payload['operation_id']]['state']=='running':
                ops[payload['operation_id']]['state']='completed';ops_path.write_text(json.dumps(ops))
            print(json.dumps(ops.get(payload['operation_id'],dict(schema=1,machine_id=machine,request_id=payload['operation_id'],state='failed',error_code='operation_not_found',error='Operation not found'))));sys.exit(0)
        with (root/'management-actions').open('a') as log:log.write(json.dumps(payload)+'\n')
        request_id=payload['request_id'];uuid.UUID(request_id)
        result=dict(schema=1,machine_id=machine,request_id=request_id,state='completed')
        if request_id in ops: print(json.dumps(ops[request_id]));sys.exit(0)
        if payload['expected_machine_id']!=machine:result.update(state='failed',error_code='wrong_machine',error='Computer changed')
        elif action!='profile_test' and (payload['expected_revision']!=state['revision'] or (root/'revision-conflict').exists()):result.update(state='failed',error_code='revision_conflict',error='Configuration changed')
        elif action=='profile_upsert':
            profile=payload['profile'];old=next((p for p in state['profiles'] if p['id']==profile['id']),{})
            credential=payload['credential'];configured=old.get('credential',{}).get('configured',False)
            if credential['mode']=='replace':assert credential['value'];configured=True
            elif credential['mode']=='remove':configured=False
            state['revision']+=1;profile=dict(profile,revision=state['revision'],credential={'configured':configured},available=True)
            state['profiles']=[p for p in state['profiles'] if p['id']!=profile['id']]+[profile]
            result.update(profile_id=profile['id'],profile_revision=profile['revision'],revision=state['revision'])
        elif action=='profile_remove':
            state['revision']+=1;state['profiles']=[p for p in state['profiles'] if p['id']!=payload['profile_id']];result.update(revision=state['revision'],profile_id=payload['profile_id'])
        elif action=='profile_test':result.update(profile_id=payload['profile_id'],profile_revision=payload['expected_profile_revision'])
        if action=='profile_test' and (root/'running-test').exists():result['state']='running'
        state_path.write_text(json.dumps(state));ops[request_id]=result;ops_path.write_text(json.dumps(ops))
        (root/'management-started').write_text('started')
        while (root/'management-gate').exists():time.sleep(.01)
        if (root/'lost-management').exists():
            print(payload.get('credential',{}).get('value','private fixture value'),file=sys.stderr);sys.exit(1)
        print(json.dumps(result))
    elif 'voice' in sys.argv:
        uuid.UUID(payload['request_id'])
        assert payload['expected_run_id']
        assert 'expected_conversation_id' in payload
        if payload['action'] == 'profiles':
            computers=json.loads((root/'voice-profiles').read_text()) if (root/'voice-profiles').exists() else [{'machine_id':'bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb','name':'Fixture','profiles':[{'id':'russian','name':'Russian','available':True}]}]
            fail=(root/'profile-failure').exists()
            (root/'profiles-started').write_text('started')
            (root/('profiles-started-'+payload['expected_conversation_id'])).write_text('started')
            while (root/'profiles-gate').exists() or (root/('profiles-gate-'+payload['expected_conversation_id'])).exists(): time.sleep(.01)
            if fail:
                print('Synthetic provider lookup failure',file=sys.stderr);sys.exit(1)
            print(json.dumps({'schema':1,'computers':computers,'features':[] if (root/'video-legacy').exists() else ['voice_video']}))
        else:
            with (root / 'voice-actions').open('a') as log:
                log.write(json.dumps(payload) + '\n')
            message_id=payload['request_id'] if payload['action']=='submit' else payload['message_id']
            if payload['action']=='submit' and (root/'lost-submit').exists():
                sys.exit(1)
            media={}
            if 'video' in payload:
                import base64
                media=dict(payload['video']);media.pop('format');media['bytes']=len(base64.b64decode(media.pop('data_base64')))
                media.update(request_id=message_id,index=1,name='video.mp4',mime='video/mp4')
            print(json.dumps({'schema':1,'message':dict({'id':message_id,'run_id':payload['expected_run_id'],'conversation_id':payload['expected_conversation_id'],'created_at':100,'revision':1,'state':'queued','audio':{'request_id':message_id,'index':0,'name':'voice.wav','mime':'audio/wav','bytes':3244,'duration_ms':100}},**({'video':media} if media else {}))}))
    elif payload['action'] == 'providers':
        profiles = json.loads((root / 'profiles').read_text()) if (root / 'profiles').exists() else [{'id':'russian','name':'Russian','available':True}]
        print(json.dumps({'schema':1,'available':True,'profiles':profiles}))
    elif payload['action'] == 'stats':
        print(json.dumps({'schema':1,'enabled':True,'retention_days':90,'totals':{'submitted':2,'completed':2,'audio_ms':12000}}))
    elif payload['action'] == 'submit':
        print(json.dumps({'schema':1,'job_id':payload['request_id'],'state':'completed'}))
    else:
        print(json.dumps({'schema':1,'job_id':payload.get('job_id',payload.get('request_id')),'state':'completed','text':'Recognized fixture'}))
)PY"); file.close();
#ifdef Q_OS_WIN
    if (!FakeCli::makeRunnable(file.fileName())) return {};
    return FakeCli::program(file.fileName());
#else
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); return file.fileName();
#endif
}
SpeechComposer::Target target(MessageComposer &composer,const QString &conversation="native-conversation") {
    return {composer.sessionKey(),"","fixture","native-run",conversation,{},true};
}
class FixtureRecorder : public SpeechRecorder {
public:
    bool startImmediately = true, cancelled = false, finishImmediately = true;
    int starts = 0, stops = 0;
    std::optional<Failure> failure;
    void start(const QByteArray &) override {
        ++starts;cancelled=false;
        if (failure) emit failed(*failure, QStringLiteral("Synthetic recording error mentioning permission"));
        else if (startImmediately) emit started();
    }
    void finish() { emit recorded(QByteArray(3200,'\0').toBase64()); }
    void stop() override { ++stops;if(finishImmediately)finish(); }
    void cancel() override { cancelled = true; }
};
class FixtureVideoRecorder : public RoundVideoRecorder {
public:
    FixtureVideoRecorder():RoundVideoRecorder(nullptr){}
    int starts=0,stops=0;bool cancelled=false,startImmediately=true;
    void start(const QByteArray &) override {++starts;cancelled=false;if(startImmediately)emit started();}
    void stop() override {++stops;emit completed(QByteArray(32000,'\0'),QByteArray("synthetic-video"),QJsonObject{{"format","mp4"},{"duration_ms",1000},{"width",480},{"height",480}});}
    void cancel() override {cancelled=true;}
};

}
// These retained-preview tests use the explicit Stop control. Physical clicks
// on Record and Send have separate coverage below.
static void recordOrPreview(ToolbarChip *voice) {
    if(voice->fullLabel().startsWith("Send (")) {
        auto *parent=voice->parentWidget();
        while(parent && !qobject_cast<MessageComposer *>(parent))parent=parent->parentWidget();
        Q_ASSERT(parent);parent->findChild<QPushButton *>("voiceStopPreview")->click();
    } else voice->click();
}

class TestSpeech : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("zerus-speech-tests"); QCoreApplication::setApplicationName("speech");
        QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_home.path());
    }
    void init() { ComposerDraftStore::flushWrites(); QDir(ComposerDraftStore::directory()).removeRecursively(); QSettings().clear(); }
    void streamingConversionIsIndependentOfChunks() {
        const auto input = tone(48000, 2, 48000, 997);
        SpeechPcmConverter full(format(48000, 2)), chunked(format(48000, 2));
        full.append(input);
        for (qsizetype offset = 0; offset < input.size(); offset += 337) chunked.append(input.mid(offset, 337));
        const auto output = full.finish(); QCOMPARE(chunked.finish(), output); QCOMPARE(output.size(), 32000);
        QVERIFY(rms(output) > 10000); QVERIFY(rms(output) < 12000);
    }
    void downsamplingRejectsAboveNyquist() {
        SpeechPcmConverter low(format(48000)), high(format(48000));
        low.append(tone(48000, 1, 48000, 1000)); high.append(tone(48000, 1, 48000, 13000));
        QVERIFY(rms(high.finish()) < rms(low.finish()) * .01);
    }
    void captureSizeIsBounded() {
        SpeechPcmConverter converter(format(16000)); QByteArray silence(32000, '\0');
        for (int second = 0; second < 125; ++second) converter.append(silence);
        QVERIFY(converter.full()); QCOMPARE(converter.finish().size(), SpeechPcmConverter::MaximumBytes);
    }
    void missingMacPermissionBackendFailsBeforeRequest() {
#ifdef Q_OS_MACOS
        // This fixture intentionally has no permission-plugin import or usage
        // description. The guard must fail before Qt can request consent/audio.
        QVERIFY(!SpeechRecorder::microphonePermissionBackendAvailable());
        SpeechRecorder recorder; QSignalSpy failed(&recorder, &SpeechRecorder::failed);
        QSignalSpy started(&recorder, &SpeechRecorder::started);
        recorder.start({});
        QCOMPARE(failed.count(), 1); QCOMPARE(started.count(), 0);
        QCOMPARE(qvariant_cast<SpeechRecorder::Failure>(failed[0][0]), SpeechRecorder::Failure::Setup);
        QVERIFY(failed[0][1].toString().contains("Update or reinstall Zerus"));
#else
        QVERIFY(SpeechRecorder::microphonePermissionBackendAvailable());
#endif
    }
    void oldHostNeverReceivesSpeechCommand() {
        QTemporaryDir dir; SpeechClient client(fixture(dir.path())); bool finished = false; QString error;
        client.request("legacy", {{"action","providers"}}, [&](const QJsonObject &, const QString &failure) { error = failure; finished = true; });
        QTRY_VERIFY(finished); QVERIFY(error.contains("updated hgs"));
        QFile calls(dir.path()+"/calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("[\"@legacy\", \"--help\"]\n"));
        QVERIFY(!QFile::exists(dir.path()+"/payload"));
    }
    void selectedHostAndAudioUseStdin() {
        QTemporaryDir dir; SpeechClient client(fixture(dir.path())); bool finished = false; QString error;
        const QJsonObject payload{{"action","submit"},{"request_id","aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"},{"audio",QJsonObject{{"data_base64","AAAA"}}}};
        client.request("speech-host", payload, [&](const QJsonObject &reply, const QString &failure) { error=failure; finished=true; QCOMPARE(reply.value("state").toString(),"completed"); });
        QTRY_VERIFY(finished); QVERIFY(error.isEmpty());
        QFile calls(dir.path()+"/calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("[\"@speech-host\", \"--help\"]\n[\"@speech-host\", \"speech\", \"--json\"]\n"));
        QFile sent(dir.path()+"/payload"); QVERIFY(sent.open(QIODevice::ReadOnly)); QCOMPARE(QJsonDocument::fromJson(sent.readAll()).object(), payload);
    }
    void settingsRestoreAndPersistTheSelectedRemoteProvider() {
        QTemporaryDir dir;
        QFile profiles(dir.path()+"/profiles");QVERIFY(profiles.open(QIODevice::WriteOnly));
        profiles.write(R"JSON([{"id":"first","name":"First","available":true},{"id":"russian","name":"Russian","available":true},{"id":"offline","name":"Offline","available":false}])JSON");profiles.close();
        QSettings().setValue("speech/host","speech-host");QSettings().setValue("speech/profile","russian");QSettings().setValue("speech/machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");
        SpeechSettings settings(fixture(dir.path()));settings.setPeers({"speech-host","other-host"});settings.show();settings.refresh();
        auto *list=settings.findChild<QListWidget *>("speechProfiles");auto *refresh=settings.findChild<QPushButton *>("speechRefresh");
        QTRY_VERIFY(refresh->isEnabled());QCOMPARE(list->count(),3);QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString(),"russian");
        list->setCurrentRow(0);QCOMPARE(QSettings().value("speech/profile").toString(),"russian");
        settings.findChild<QPushButton *>("speechUseDefault")->click();QCOMPARE(QSettings().value("speech/profile").toString(),"first");
        list->setCurrentRow(2);QVERIFY(settings.findChild<QPushButton *>("speechEdit")->isEnabled());
        const auto owner=QSettings().value("speech/machine_id").toString();auto *host=settings.findChild<QComboBox *>("speechComputer");host->setCurrentIndex(host->findData("other-host"));
        QTRY_VERIFY(refresh->isEnabled());QCOMPARE(QSettings().value("speech/profile").toString(),"first");QCOMPARE(QSettings().value("speech/machine_id").toString(),owner);
    }
    void settingsCanRefreshAfterSpeechIsRestoredOnReceiver() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());QFile legacy(dir.path()+"/management-legacy");QVERIFY(legacy.open(QIODevice::WriteOnly));legacy.close();
        QSettings().setValue("speech/host","speech-host");QSettings().setValue("speech/profile","russian");
        SpeechSettings settings(executable);settings.show();settings.refresh();
        auto *status=settings.findChild<QLabel *>("speechSettingsStatus");auto *refresh=settings.findChild<QPushButton *>("speechRefresh");
        QTRY_VERIFY(refresh->isEnabled());QVERIFY(!settings.findChild<QPushButton *>("speechAdd")->isEnabled());QVERIFY(status->text().contains("Update hgs"));
        QCOMPARE(QSettings().value("speech/profile").toString(),"russian");QVERIFY(!QFile::exists(dir.path()+"/management-actions"));
        QVERIFY(legacy.remove());refresh->click();QTRY_VERIFY(refresh->isEnabled());QVERIFY(settings.findChild<QPushButton *>("speechAdd")->isEnabled());
        QCOMPARE(settings.findChild<QListWidget *>("speechProfiles")->count(),1);
    }
    void nativeSettingsOnExplicitSpeechHost() {
        const auto executable=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_HGS");const auto host=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_HOST");
        const auto expected=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_PROFILE");
        if(executable.isEmpty() || expected.isEmpty())QSKIP("Requires an explicit installed hgs and profile; only reads configuration/statistics.");
        QSettings().setValue("speech/host",host);QSettings().setValue("speech/profile",expected);
        SpeechSettings settings(executable);settings.setPeers({host});settings.show();settings.refresh();
        auto *refresh=settings.findChild<QPushButton *>("speechRefresh");QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),65000);
        QVERIFY2(settings.findChild<QPushButton *>("speechAdd")->isEnabled(),qPrintable(settings.findChild<QLabel *>("speechSettingsStatus")->text()));
        auto *list=settings.findChild<QListWidget *>("speechProfiles");bool found=false;
        for(int row=0;row<list->count();++row){const auto profile=list->item(row)->data(Qt::UserRole).toJsonObject();if(profile.value("id").toString()==expected){QVERIFY(profile.value("available").toBool());found=true;}}
        QVERIFY(found);
    }
    void providerDocumentationOpensOnlyOnClickAndReportsFailure() {
        QTemporaryDir dir;QList<QUrl> opened;bool success=false;
        SpeechSettings settings(fixture(dir.path()),nullptr,[&](const QUrl &url){opened.append(url);return success;});settings.setPeers({"speech-host"});
        auto *host=settings.findChild<QComboBox *>("speechComputer");const QSignalBlocker block(host);host->setCurrentIndex(host->findData("speech-host"));
        auto *open=settings.findChild<QPushButton *>("speechDocumentation");auto *failure=settings.findChild<QLabel *>("speechDocumentationError");QVERIFY(open->isEnabled());QVERIFY(opened.isEmpty());QVERIFY(failure->isHidden());
        open->click();QCOMPARE(opened.size(),1);QCOMPARE(opened[0],QUrl("https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"));QVERIFY(!failure->isHidden());QVERIFY(failure->text().contains(opened[0].toString()));
        success=true;open->click();QCOMPARE(opened.size(),2);QVERIFY(failure->isHidden());QVERIFY(!QFile::exists(dir.path()+"/calls"));QVERIFY(!QFile::exists(dir.path()+"/management-actions"));
    }
    void editorDocumentationTracksTypeAndPreservesUnsavedFields_data() {
        QTest::addColumn<bool>("openSucceeds");QTest::newRow("opened")<<true;QTest::newRow("browser-unavailable")<<false;
    }
    void editorDocumentationTracksTypeAndPreservesUnsavedFields() {
        QFETCH(bool,openSucceeds);QTemporaryDir dir;QList<QUrl> opened;
        QSettings().setValue("speech/manage_host","speech-host");SpeechSettings settings(fixture(dir.path()),nullptr,[&](const QUrl &url){opened.append(url);return openSucceeds;});settings.setPeers({"speech-host","other-host"});settings.refresh();
        auto *add=settings.findChild<QPushButton *>("speechAdd");QTRY_VERIFY(add->isEnabled());bool checked=false;
        QTimer::singleShot(0,&settings,[&]{
            auto *editor=settings.findChild<QDialog *>("speechProfileEditor");QVERIFY(editor);QVERIFY(editor->findChild<QLabel *>("speechEditComputer")->text().contains("speech-host"));
            auto *type=editor->findChild<QComboBox *>("speechEditType");auto *documentation=editor->findChild<QPushButton *>("speechEditDocumentation");auto *failure=editor->findChild<QLabel *>("speechEditDocumentationError");
            QVERIFY(opened.isEmpty());type->setCurrentIndex(type->findData("gigaam"));QVERIFY(opened.isEmpty());documentation->click();
            QCOMPARE(opened.size(),1);QCOMPARE(opened[0],QUrl("https://github.com/ufna/zerus/blob/main/docs/speech-setup.md#local-gigaam"));
            const auto localLabel=documentation->text();type->setCurrentIndex(type->findData("openai_http"));QCOMPARE(opened.size(),1);QVERIFY(documentation->text()!=localLabel);QVERIFY(failure->isHidden());
            editor->findChild<QLineEdit *>("speechEditName")->setText("Unsaved remote provider");editor->findChild<QLineEdit *>("speechEditUrl")->setText("https://speech.example.net/private-api");editor->findChild<QComboBox *>("speechEditModel")->setEditText("private-model");
            auto *credential=editor->findChild<QComboBox *>("speechEditCredential");credential->setCurrentIndex(credential->findData("replace"));auto *key=editor->findChild<QLineEdit *>("speechEditKey");key->setText("synthetic-unsaved-key");
            QList<QPair<QLineEdit *,QString>> fields;for(auto *field:editor->findChildren<QLineEdit *>())fields.append({field,field->text()});
            auto *host=settings.findChild<QComboBox *>("speechComputer");const QSignalBlocker block(host);host->setCurrentIndex(host->findData("other-host"));
            auto *save=editor->findChild<QPushButton *>("speechSaveProfile");QVERIFY(save->isEnabled());documentation->click();
            QCOMPARE(opened.size(),2);QCOMPARE(opened[1],QUrl("https://github.com/ufna/zerus/blob/main/docs/speech-setup.md#openai-compatible-server"));
            for(const auto &field:fields)QCOMPARE(field.first->text(),field.second);
            QCOMPARE(credential->currentData().toString(),"replace");QCOMPARE(type->currentData().toString(),"openai_http");QVERIFY(save->isEnabled());QCOMPARE(failure->isHidden(),openSucceeds);
            if(!openSucceeds)QVERIFY(failure->text().contains(opened[1].toString()));checked=true;editor->reject();
        });add->click();QVERIFY(checked);QVERIFY(!QFile::exists(dir.path()+"/management-actions"));QVERIFY(QSettings().value("speech/providerOperation").toByteArray().isEmpty());
    }
    void managementRequiresItsOwnCapabilityBeforeSendingAKey() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());QFile legacy(dir.path()+"/management-legacy");QVERIFY(legacy.open(QIODevice::WriteOnly));legacy.close();
        SpeechClient client(executable);bool complete=false;QJsonObject reply;QString error;
        client.request("speech-host",{{"action","profile_upsert"},{"credential",QJsonObject{{"mode","replace"},{"value","synthetic-private-key"}}}},
            [&](const QJsonObject &result,const QString &failure){reply=result;error=failure;complete=true;});
        QTRY_VERIFY(complete);QVERIFY(error.contains("Update hgs"));QVERIFY(reply.contains("client_dispatch_started"));QVERIFY(!reply.value("client_dispatch_started").toBool());
        QVERIFY(!QFile::exists(dir.path()+"/payload"));QVERIFY(!QFile::exists(dir.path()+"/management-actions"));
    }
    void settingsCreateRemoteProfileWithoutRememberingItsKey() {
        QTemporaryDir dir;SpeechSettings settings(fixture(dir.path()));settings.refresh();
        auto *add=settings.findChild<QPushButton *>("speechAdd");QTRY_VERIFY(add->isEnabled());bool exercised=false;
        QTimer::singleShot(0,&settings,[&]{
            auto *dialog=settings.findChild<QDialog *>("speechProfileEditor");QVERIFY(dialog);
            auto *type=dialog->findChild<QComboBox *>("speechEditType");type->setCurrentIndex(type->findData("openai_http"));
            dialog->findChild<QLineEdit *>("speechEditName")->setText("Remote fixture");dialog->findChild<QLineEdit *>("speechEditId")->setText("remote-fixture");
            dialog->findChild<QComboBox *>("speechEditModel")->setEditText("gigaam");dialog->findChild<QLineEdit *>("speechEditUrl")->setText("https://speech.example.net/v1");
            auto *mode=dialog->findChild<QComboBox *>("speechEditCredential");mode->setCurrentIndex(mode->findData("replace"));
            auto *key=dialog->findChild<QLineEdit *>("speechEditKey");key->setText("synthetic-private-key");
            QCOMPARE(key->echoMode(),QLineEdit::Password);auto *save=dialog->findChild<QPushButton *>("speechSaveProfile");QVERIFY(save->isEnabled());save->click();QVERIFY(key->text().isEmpty());exercised=true;
        });
        add->click();QVERIFY(exercised);auto *list=settings.findChild<QListWidget *>("speechProfiles");QTRY_COMPARE(list->count(),2);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toJsonObject().value("id").toString(),"remote-fixture");
        QVERIFY(QSettings().value("speech/profile").toString().isEmpty());QSettings saved;saved.sync();QFile ini(saved.fileName());QVERIFY(ini.open(QIODevice::ReadOnly));QVERIFY(!ini.readAll().contains("synthetic-private-key"));
        QFile log(dir.path()+"/management-actions");QVERIFY(log.open(QIODevice::ReadOnly));const auto request=QJsonDocument::fromJson(log.readLine()).object();
        QCOMPARE(request.value("expected_machine_id").toString(),"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");QCOMPARE(request.value("expected_revision").toInteger(),1);
        QCOMPARE(request.value("profile").toObject().value("base_url").toString(),"https://speech.example.net/v1");QVERIFY(!request.value("profile").toObject().contains("device"));
        QCOMPARE(request.value("credential").toObject().value("value").toString(),"synthetic-private-key");QVERIFY(log.readAll().isEmpty());
        settings.findChild<QPushButton *>("speechUseDefault")->click();QCOMPARE(QSettings().value("speech/profile").toString(),"remote-fixture");QCOMPARE(QSettings().value("speech/profile_revision").toLongLong(),2);
    }
    void settingsRemoteCredentialModesAndHttpConsent_data() {
        QTest::addColumn<QString>("mode");QTest::newRow("keep")<<QString("keep");QTest::newRow("remove")<<QString("remove");
    }
    void settingsRemoteCredentialModesAndHttpConsent() {
        QFETCH(QString,mode);QTemporaryDir dir;QFile profiles(dir.path()+"/profiles");QVERIFY(profiles.open(QIODevice::WriteOnly));
        profiles.write("[{\"id\":\"remote\",\"name\":\"Remote\",\"available\":true,\"adapter\":\"openai_http\",\"model\":\"gigaam\",\"base_url\":\"https://speech.example.net/v1\",\"credential\":{\"configured\":true}}]");profiles.close();
        SpeechSettings settings(fixture(dir.path()));settings.refresh();auto *edit=settings.findChild<QPushButton *>("speechEdit");QTRY_VERIFY(edit->isEnabled());bool exercised=false;
        QTimer::singleShot(0,&settings,[&]{
            auto *dialog=settings.findChild<QDialog *>("speechProfileEditor");QVERIFY(dialog);auto *key=dialog->findChild<QLineEdit *>("speechEditKey");QVERIFY(key->text().isEmpty());QVERIFY(!key->isEnabled());
            auto *credential=dialog->findChild<QComboBox *>("speechEditCredential");QVERIFY(credential->currentText().contains("stored"));credential->setCurrentIndex(credential->findData(mode));
            auto *url=dialog->findChild<QLineEdit *>("speechEditUrl");auto *save=dialog->findChild<QPushButton *>("speechSaveProfile");
            url->setText("http://127.0.0.1:9000/v1");QVERIFY(!save->isEnabled());dialog->findChild<QCheckBox *>("speechEditInsecureHttp")->setChecked(true);QVERIFY(save->isEnabled());
            url->setText("https://user:secret@example.net/v1");QVERIFY(!save->isEnabled());url->setText("https://speech.example.net/v1?key=secret");QVERIFY(!save->isEnabled());
            url->setText("https://speech.example.net/v1");save->click();exercised=true;
        });edit->click();QVERIFY(exercised);
        QFile log(dir.path()+"/management-actions");QVERIFY(log.open(QIODevice::ReadOnly));const auto request=QJsonDocument::fromJson(log.readLine()).object();
        QCOMPARE(request.value("credential").toObject().value("mode").toString(),mode);QVERIFY(!request.value("credential").toObject().contains("value"));
    }
    void settingsPinnedSaveSurvivesBrowsingAnotherComputer() {
        QTemporaryDir dir;QFile gate(dir.path()+"/management-gate");QVERIFY(gate.open(QIODevice::WriteOnly));gate.close();
        SpeechSettings settings(fixture(dir.path()));settings.setPeers({"other-host"});settings.refresh();auto *edit=settings.findChild<QPushButton *>("speechEdit");QTRY_VERIFY(edit->isEnabled());bool checked=false;
        QTimer::singleShot(0,&settings,[&]{
            auto *dialog=settings.findChild<QDialog *>("speechProfileEditor");QVERIFY(dialog);dialog->findChild<QLineEdit *>("speechEditName")->setText("Only original computer");dialog->findChild<QPushButton *>("speechSaveProfile")->click();
            QTRY_VERIFY(QFile::exists(dir.path()+"/management-started"));auto *host=settings.findChild<QComboBox *>("speechComputer");host->setCurrentIndex(host->findData("other-host"));
            QTRY_VERIFY(settings.findChild<QPushButton *>("speechRefresh")->isEnabled());QCOMPARE(settings.findChild<QListWidget *>("speechProfiles")->currentItem()->data(Qt::UserRole).toJsonObject().value("name").toString(),"Russian");
            QVERIFY(gate.remove());checked=true;
        });edit->click();QVERIFY(checked);
        QCOMPARE(settings.findChild<QComboBox *>("speechComputer")->currentData().toString(),"other-host");
        QCOMPARE(settings.findChild<QListWidget *>("speechProfiles")->currentItem()->data(Qt::UserRole).toJsonObject().value("name").toString(),"Russian");
        QFile log(dir.path()+"/management-actions");QVERIFY(log.open(QIODevice::ReadOnly));const auto request=QJsonDocument::fromJson(log.readLine()).object();
        QCOMPARE(request.value("expected_machine_id").toString(),"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");QVERIFY(log.readLine().isEmpty());
    }
    void settingsConflictKeepsTheFormAndNeverRetries() {
        QTemporaryDir dir;QFile conflict(dir.path()+"/revision-conflict");QVERIFY(conflict.open(QIODevice::WriteOnly));conflict.close();
        SpeechSettings settings(fixture(dir.path()));settings.refresh();auto *edit=settings.findChild<QPushButton *>("speechEdit");QTRY_VERIFY(edit->isEnabled());bool retained=false;
        QTimer::singleShot(0,&settings,[&]{
            auto *dialog=settings.findChild<QDialog *>("speechProfileEditor");QVERIFY(dialog);auto *name=dialog->findChild<QLineEdit *>("speechEditName");name->setText("My unsaved edit");
            dialog->findChild<QPushButton *>("speechSaveProfile")->click();auto *status=dialog->findChild<QLabel *>("speechEditStatus");QTRY_VERIFY(status->text().contains("configuration changed"));
            QCOMPARE(name->text(),"My unsaved edit");QVERIFY(!dialog->findChild<QPushButton *>("speechSaveProfile")->isEnabled());retained=true;dialog->reject();
        });
        edit->click();QVERIFY(retained);QVERIFY(QSettings().value("speech/providerOperation").toByteArray().isEmpty());
        QFile log(dir.path()+"/management-actions");QVERIFY(log.open(QIODevice::ReadOnly));QVERIFY(!log.readLine().isEmpty());QVERIFY(log.readLine().isEmpty());
    }
    void settingsLostSaveUsesTheSameReceiptAfterRestart() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());QFile lost(dir.path()+"/lost-management");QVERIFY(lost.open(QIODevice::WriteOnly));lost.close();QString requestId;
        {
            SpeechSettings settings(executable);settings.refresh();auto *edit=settings.findChild<QPushButton *>("speechEdit");QTRY_VERIFY(edit->isEnabled());
            QTimer::singleShot(0,&settings,[&]{
                auto *dialog=settings.findChild<QDialog *>("speechProfileEditor");QVERIFY(dialog);dialog->findChild<QLineEdit *>("speechEditName")->setText("Saved despite lost response");
                dialog->findChild<QPushButton *>("speechSaveProfile")->click();auto *check=dialog->findChild<QPushButton *>("speechEditCheckOperation");QTRY_VERIFY(check->isEnabled());
                QVERIFY(dialog->findChild<QLabel *>("speechEditStatus")->text().contains("not confirmed"));QVERIFY(!dialog->findChild<QPushButton *>("speechSaveProfile")->isEnabled());
                requestId=QJsonDocument::fromJson(QSettings().value("speech/providerOperation").toByteArray()).object().value("request_id").toString();QVERIFY(!requestId.isEmpty());dialog->reject();
            });edit->click();
        }
        SpeechSettings restored(executable);restored.refresh();auto *check=restored.findChild<QPushButton *>("speechCheckOperation");QVERIFY(!check->isHidden());check->click();
        QTRY_VERIFY(QSettings().value("speech/providerOperation").toByteArray().isEmpty());QTRY_VERIFY(restored.findChild<QPushButton *>("speechEdit")->isEnabled());
        QCOMPARE(restored.findChild<QListWidget *>("speechProfiles")->currentItem()->data(Qt::UserRole).toJsonObject().value("name").toString(),"Saved despite lost response");
        QFile log(dir.path()+"/management-actions");QVERIFY(log.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(log.readLine()).object().value("request_id").toString(),requestId);QVERIFY(log.readLine().isEmpty());
    }
    void settingsUnknownReceiptRemainsPendingWithoutReplay() {
        QTemporaryDir dir;const QJsonObject pending{{"host",""},{"computer","This computer"},{"machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"},{"request_id","aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"},{"action","profile_upsert"}};
        QSettings().setValue("speech/providerOperation",QJsonDocument(pending).toJson(QJsonDocument::Compact));
        SpeechSettings settings(fixture(dir.path()));settings.refresh();auto *check=settings.findChild<QPushButton *>("speechCheckOperation");check->click();QTRY_VERIFY(check->isEnabled());
        QCOMPARE(QJsonDocument::fromJson(QSettings().value("speech/providerOperation").toByteArray()).object(),pending);
        QVERIFY(!settings.findChild<QPushButton *>("speechAdd")->isEnabled());QVERIFY(!QFile::exists(dir.path()+"/management-actions"));
    }
    void managementTransportNeverEchoesAReplacementKey() {
        QTemporaryDir dir;QFile lost(dir.path()+"/lost-management");QVERIFY(lost.open(QIODevice::WriteOnly));lost.close();
        SpeechClient client(fixture(dir.path()));bool finished=false;QString error;
        client.request("",{{"action","profile_upsert"},{"request_id","aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"},{"expected_machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"},{"expected_revision",1},
            {"profile",QJsonObject{{"id","remote"},{"name","Remote"},{"adapter","openai_http"},{"model","gigaam"},{"language","ru"},{"base_url","https://speech.example.net/v1"},{"allow_insecure_http",false}}},
            {"credential",QJsonObject{{"mode","replace"},{"value","synthetic-private-key"}}}},[&](const QJsonObject &reply,const QString &failure){QVERIFY(!reply.contains("client_dispatch_started"));error=failure;finished=true;});
        QTRY_VERIFY(finished);QVERIFY(!error.isEmpty());QVERIFY(!error.contains("synthetic-private-key"));
        QFile calls(dir.path()+"/calls");QVERIFY(calls.open(QIODevice::ReadOnly));QVERIFY(!calls.readAll().contains("synthetic-private-key"));
    }
    void settingsRemoveConfirmsAndPreservesUnavailableDefault() {
        QTemporaryDir dir;QSettings().setValue("speech/machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");QSettings().setValue("speech/profile","russian");
        SpeechSettings settings(fixture(dir.path()));settings.refresh();auto *remove=settings.findChild<QPushButton *>("speechRemove");QTRY_VERIFY(remove->isEnabled());
        QTimer::singleShot(0,&settings,[&]{auto *dialog=settings.findChild<QMessageBox *>("speechRemoveConfirm");QVERIFY(dialog);QVERIFY(dialog->text().contains("Russian"));dialog->reject();});remove->click();
        QVERIFY(!QFile::exists(dir.path()+"/management-actions"));
        QTimer::singleShot(0,&settings,[&]{settings.findChild<QPushButton *>("speechConfirmRemove")->click();});remove->click();
        QTRY_COMPARE(settings.findChild<QListWidget *>("speechProfiles")->count(),0);QCOMPARE(QSettings().value("speech/profile").toString(),"russian");
        QCOMPARE(QSettings().value("speech/machine_id").toString(),"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");QVERIFY(settings.findChild<QLabel *>("speechDefault")->text().contains("unavailable"));
    }
    void settingsTestPinsSavedProfileRevision() {
        QTemporaryDir dir;SpeechSettings settings(fixture(dir.path()));settings.refresh();auto *test=settings.findChild<QPushButton *>("speechTest");QTRY_VERIFY(test->isEnabled());test->click();
        QTRY_VERIFY(settings.findChild<QLabel *>("speechOperationResult")->text().contains("test on"));
        QFile log(dir.path()+"/management-actions");QVERIFY(log.open(QIODevice::ReadOnly));const auto request=QJsonDocument::fromJson(log.readLine()).object();
        QCOMPARE(request.value("action").toString(),"profile_test");QCOMPARE(request.value("profile_id").toString(),"russian");QCOMPARE(request.value("expected_profile_revision").toInteger(),1);
        QVERIFY(!request.contains("credential"));QVERIFY(!request.contains("audio"));QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void runningProviderTestPollsItsReceiptWithoutAnotherMutation() {
        QTemporaryDir dir;QFile running(dir.path()+"/running-test");QVERIFY(running.open(QIODevice::WriteOnly));running.close();
        SpeechSettings settings(fixture(dir.path()));settings.refresh();auto *test=settings.findChild<QPushButton *>("speechTest");QTRY_VERIFY(test->isEnabled());test->click();
        QTRY_VERIFY(!QSettings().value("speech/providerOperation").toByteArray().isEmpty());
        QTRY_VERIFY(settings.findChild<QLabel *>("speechOperationResult")->text().contains("completed successfully"));
        QVERIFY(QSettings().value("speech/providerOperation").toByteArray().isEmpty());
        QFile actions(dir.path()+"/management-actions");QVERIFY(actions.open(QIODevice::ReadOnly));const auto request=QJsonDocument::fromJson(actions.readLine()).object();QVERIFY(actions.readLine().isEmpty());
        QFile payload(dir.path()+"/payload");QVERIFY(payload.open(QIODevice::ReadOnly));
        QFile calls(dir.path()+"/calls");QVERIFY(calls.open(QIODevice::ReadOnly));QVERIFY(calls.readAll().count("speech")>=2);
        QFile operations(dir.path()+"/operations");QVERIFY(operations.open(QIODevice::ReadOnly));const auto receipts=QJsonDocument::fromJson(operations.readAll()).object();
        QCOMPARE(receipts.size(),1);QCOMPARE(receipts.value(request.value("request_id").toString()).toObject().value("state").toString(),"completed");
    }
    void newRecordingUsesCurrentRevisionOfExplicitDefault() {
        QTemporaryDir dir;QFile profiles(dir.path()+"/voice-profiles");QVERIFY(profiles.open(QIODevice::WriteOnly));
        profiles.write(R"JSON([{"machine_id":"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb","name":"Fixture","profiles":[{"id":"russian","name":"Russian","available":true,"profile_revision":2}]}])JSON");profiles.close();
        QSettings().setValue("speech/machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");QSettings().setValue("speech/profile","russian");QSettings().setValue("speech/profile_revision",1);
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());ComposerDraftStore::flushWrites();
        QCOMPARE(ComposerDraftStore().load("voice-message-draft\nfixture\noriginal").formState.value("speech").toObject().value("profile_revision").toInteger(),2);
    }
    void refreshedProfileRevisionRequiresExplicitPreviewReselection_data() {
        QTest::addColumn<int>("initialRevision");QTest::newRow("legacy-to-managed")<<0;QTest::newRow("changed-managed")<<1;
    }
    void refreshedProfileRevisionRequiresExplicitPreviewReselection() {
        QFETCH(int,initialRevision);QTemporaryDir dir;QFile profiles(dir.path()+"/voice-profiles");QVERIFY(profiles.open(QIODevice::WriteOnly));
        QJsonObject originalProfile{{"id","russian"},{"name","Russian"},{"available",true}};if(initialRevision)originalProfile["profile_revision"]=initialRevision;
        profiles.write(QJsonDocument(QJsonArray{QJsonObject{{"machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"},{"name","Fixture"},{"profiles",QJsonArray{originalProfile}}}}).toJson());profiles.close();
        const auto executable=fixture(dir.path());QString id;QByteArray audio;
        {
            MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
            auto *recorder=new FixtureRecorder;SpeechComposer speech(executable,&composer,[&]{return target(composer);},recorder);
            auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);recordOrPreview(voice);QTRY_VERIFY(composer.findChild<QPushButton *>("voiceSend")->isEnabled());
            ComposerDraftStore::flushWrites();const auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");QCOMPARE(draft.formState.value("speech").toObject().value("profile_revision").toInteger(),initialRevision);
            id=draft.formState.value("id").toString();audio=draft.attachments[0].data;
        }
        QVERIFY(profiles.open(QIODevice::WriteOnly|QIODevice::Truncate));profiles.write(R"JSON([{"machine_id":"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb","name":"Fixture","profiles":[{"id":"russian","name":"Russian","available":true,"profile_revision":2}]}])JSON");profiles.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        SpeechComposer speech(executable,&composer,[&]{return target(composer);});auto *provider=composer.findChild<QComboBox *>("voiceProvider");auto *send=composer.findChild<QPushButton *>("voiceSend");
        QTRY_COMPARE(provider->currentText(),"Selected provider (unavailable)");QVERIFY(!send->isEnabled());ComposerDraftStore::flushWrites();
        QCOMPARE(ComposerDraftStore().load("voice-message-draft\nfixture\noriginal").formState.value("speech").toObject().value("profile_revision").toInteger(),initialRevision);
        provider->setCurrentIndex(0);QVERIFY(send->isEnabled());ComposerDraftStore::flushWrites();const auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QCOMPARE(draft.formState.value("id").toString(),id);QCOMPARE(draft.attachments[0].data,audio);QCOMPARE(draft.formState.value("speech").toObject().value("profile_revision").toInteger(),2);
        send->click();QTRY_VERIFY(QFile::exists(dir.path()+"/voice-actions"));QFile log(dir.path()+"/voice-actions");QVERIFY(log.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(log.readLine()).object().value("speech").toObject().value("profile_revision").toInteger(),2);
    }
    void malformedProviderRevisionCannotBeSent_data() {
        QTest::addColumn<QJsonValue>("revision");QTest::newRow("zero")<<QJsonValue(0);QTest::newRow("negative")<<QJsonValue(-1);
        QTest::newRow("fraction")<<QJsonValue(1.5);QTest::newRow("unsafe-integer")<<QJsonValue(9007199254740992.);QTest::newRow("string")<<QJsonValue("1");
    }
    void malformedProviderRevisionCannotBeSent() {
        QFETCH(QJsonValue,revision);QTemporaryDir dir;QFile profiles(dir.path()+"/voice-profiles");QVERIFY(profiles.open(QIODevice::WriteOnly));
        const QJsonObject profile{{"id","russian"},{"name","Russian"},{"available",true},{"profile_revision",revision}};
        profiles.write(QJsonDocument(QJsonArray{QJsonObject{{"machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"},{"name","Fixture"},{"profiles",QJsonArray{profile}}}}).toJson());profiles.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);recordOrPreview(voice);
        QTRY_VERIFY(composer.findChild<QPushButton *>("voiceRefreshProviders")->isEnabled());QVERIFY(!composer.findChild<QPushButton *>("voiceSend")->isEnabled());
        QCOMPARE(composer.findChild<QComboBox *>("voiceProvider")->count(),0);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void recordingWaitsForExplicitSendAndPreservesText() {
        QTemporaryDir dir; MessageComposer composer;composer.setSessionKey("fixture\noriginal");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.editor()->setPlainText("Keep this text");
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        QSignalSpy messages(&speech,&SpeechComposer::messageChanged);QSignalSpy textSends(&composer,&MessageComposer::sendRequested);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);
        QTRY_VERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));QCOMPARE(composer.editor()->toPlainText(),"Keep this text");
        ComposerDraftStore::flushWrites();auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QCOMPARE(draft.attachments.size(),1);QCOMPARE(draft.attachments[0].data.size(),3244);QVERIFY(draft.attachments[0].data.startsWith("RIFF"));QVERIFY(!draft.formState.value("submitted").toBool());
        const auto id=draft.formState.value("id").toString();QVERIFY(!id.isEmpty());
        composer.editor()->insertPlainText(" edited");const auto text=composer.editor()->toPlainText();send->click();
        QTRY_COMPARE(messages.count(),1);QTRY_VERIFY(voice->isEnabled());QCOMPARE(composer.editor()->toPlainText(),text);QCOMPARE(textSends.count(),0);
        QFile actions(dir.path()+"/voice-actions");QVERIFY(actions.open(QIODevice::ReadOnly));
        const auto submit=QJsonDocument::fromJson(actions.readLine()).object();QCOMPARE(submit.value("request_id").toString(),id);
        QCOMPARE(submit.value("action").toString(),"submit");QCOMPARE(submit.value("speech").toObject().value("machine_id").toString(),"bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");
        QVERIFY(actions.readLine().isEmpty());
    }
    void unavailableTargetExplainsDisabledVoice() {
        // A conversation that can never take voice (a local Windows session, spec D16)
        // names the reason on the disabled button; one that can keeps the usual hint.
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\noriginal");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        const QString windows="Voice messages cannot reach this fixture computer.";
        bool available=false;QString reason=windows;auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{
            auto value=target(composer);value.available=available;value.reason=reason;return value;
        },recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");
        QTRY_COMPARE(voice->toolTip(),windows);QVERIFY(!voice->isEnabled());QCOMPARE(voice->accessibleDescription(),windows);
        QCOMPARE(voice->fullLabel(),QString("Voice"));recordOrPreview(voice);QCOMPARE(recorder->starts,0);
        available=true;reason.clear();composer.setAvailability(true,{});
        QTRY_VERIFY(voice->isEnabled());QVERIFY(voice->toolTip().startsWith("Click to start"));
        // Unavailable for a passing reason (offline, archived): the usual hint stays.
        available=false;composer.setAvailability(true,{});
        QVERIFY(!voice->isEnabled());QVERIFY(voice->toolTip().startsWith("Click to start"));
    }
    void nativeVideoAudioOnExplicitSyntheticFile() {
        const auto path=qEnvironmentVariable("ZERUS_TEST_VIDEO_FILE");if(path.isEmpty())QSKIP("Set ZERUS_TEST_VIDEO_FILE to an explicitly generated synthetic MP4.");
        QString error;const auto pcm=RoundVideoRecorder::extractAudio(path,std::make_shared<std::atomic_bool>(false),error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QVERIFY(pcm.size()>=62000 && pcm.size()<=66000);QVERIFY(rms(pcm)>100);
    }
    void videoClicksPersistTwoMediaAndSendOnePinnedRequest() {
        QTemporaryDir dir;QSettings().setValue("speech/video_mode",true);
        MessageComposer composer;composer.setSessionKey("fixture\nvideo");composer.show();auto *video=new FixtureVideoRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},new FixtureRecorder,{},video);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());QCOMPARE(voice->fullLabel(),"Video");
        QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(video->starts,1);QCOMPARE(video->stops,0);
        QTRY_COMPARE(composer.findChild<QComboBox *>("voiceProvider")->count(),1);
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));QTest::mouseClick(voice,Qt::LeftButton);
        QTRY_VERIFY(QFile::exists(dir.path()+"/voice-actions"));QFile actions(dir.path()+"/voice-actions");QVERIFY(actions.open(QIODevice::ReadOnly));
        const auto lines=actions.readAll().trimmed().split('\n');QCOMPARE(lines.size(),1);
        const auto payload=QJsonDocument::fromJson(lines[0]).object();QCOMPARE(payload.value("expected_conversation_id").toString(),"native-conversation");
        QCOMPARE(QByteArray::fromBase64(payload.value("video").toObject().value("data_base64").toString().toLatin1()),QByteArray("synthetic-video"));
        QCOMPARE(QByteArray::fromBase64(payload.value("audio").toObject().value("data_base64").toString().toLatin1()).size(),32000);
        QCOMPARE(video->stops,1);QTRY_VERIFY(voice->isEnabled());
        ComposerDraftStore store;QVERIFY(store.load("voice-message-draft\n"+composer.sessionKey()).attachments.isEmpty());
    }
    void legacyComputerKeepsPlayableVideoPreviewWithoutDispatch() {
        QTemporaryDir dir;QSettings().setValue("speech/video_mode",true);QFile old(dir.path()+"/video-legacy");QVERIFY(old.open(QIODevice::WriteOnly));old.close();
        MessageComposer composer;composer.setSessionKey("fixture\nlegacy-video");composer.show();auto *video=new FixtureVideoRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},new FixtureRecorder,{},video);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());
        QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(video->starts,1);QTRY_COMPARE(composer.findChild<QComboBox *>("voiceProvider")->count(),1);
        QTest::mouseClick(voice,Qt::LeftButton);QTRY_VERIFY(composer.findChild<QWidget *>("voicePreview")->isVisible());
        QVERIFY(!composer.findChild<QPushButton *>("voiceSend")->isEnabled());QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        ComposerDraftStore::flushWrites();ComposerDraftStore store;const auto draft=store.load("voice-message-draft\n"+composer.sessionKey());
        QCOMPARE(draft.attachments.size(),2);QCOMPARE(draft.attachments[1].data,QByteArray("synthetic-video"));
        QCOMPARE(draft.formState.value("video").toObject().value("duration_ms").toInt(),1000);
    }
    void adjacentModeIconSwitchesWithoutCaptureAndPersists() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());
        for(bool restored:{false,true}){
            MessageComposer composer;composer.setSessionKey("fixture\nmode");composer.resize(640,300);composer.show();
            auto *audio=new FixtureRecorder;auto *video=new FixtureVideoRecorder;
            SpeechComposer speech(executable,&composer,[&]{return target(composer);},audio,{},video);
            auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");auto *mode=composer.findChild<ToolbarChip *>("voiceMode");
            QVERIFY(mode);QTRY_VERIFY(voice->isEnabled());QVERIFY(mode->isEnabled());
            QCOMPARE(voice->fullLabel(),restored?QString("Video"):QString("Voice"));
            QCOMPARE(mode->accessibleName(),restored?QString("Switch to voice recording"):QString("Switch to video recording"));
            QCOMPARE(mode->toolTip(),mode->accessibleName());QVERIFY(mode->text().isEmpty());QVERIFY(!mode->icon().isNull());
            // The switch stays beside Record, including when the toolbar compacts.
            for(int width:{640,320}){
                composer.resize(width,300);composer.toolbar()->fit();QCoreApplication::processEvents();
                const QRect recordBounds(voice->mapTo(composer.toolbar(),QPoint()),voice->size());
                const QRect modeBounds(mode->mapTo(composer.toolbar(),QPoint()),mode->size());
                QVERIFY(mode->isVisible());QVERIFY(modeBounds.left()>recordBounds.right());
                QVERIFY(modeBounds.left()-recordBounds.right()<=7);QVERIFY(composer.toolbar()->rect().contains(modeBounds));
            }
            QTest::mouseClick(mode,Qt::LeftButton);
            QCOMPARE(voice->fullLabel(),restored?QString("Voice"):QString("Video"));
            QCOMPARE(QSettings().value("speech/video_mode").toBool(),!restored);
            QCOMPARE(audio->starts,0);QCOMPARE(video->starts,0);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        }
    }
    void clickStartsAndSecondClickSendsExactlyOnce() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\nclick");composer.show();
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());
        QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(recorder->starts,1);QCOMPARE(recorder->stops,0);
        QCOMPARE(voice->fullLabel(),"Send (0:00)");QVERIFY(!QSettings().value("speech/video_mode",false).toBool());
        QTRY_COMPARE(composer.findChild<QComboBox *>("voiceProvider")->count(),1);
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));QCOMPARE(recorder->stops,0);
        QTest::mouseClick(voice,Qt::LeftButton);
        QTRY_VERIFY(QFile::exists(dir.path()+"/voice-actions"));QFile calls(dir.path()+"/voice-actions");QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto lines=calls.readAll().trimmed().split('\n');QCOMPARE(lines.size(),1);
        QCOMPARE(QJsonDocument::fromJson(lines[0]).object().value("action").toString(),"submit");
        QTRY_VERIFY(voice->isEnabled());QCOMPARE(recorder->stops,1);
    }
    void previewPreferencePersistsOnThisDevice() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());
        { SpeechSettings settings(executable);auto *option=settings.findChild<QCheckBox *>("speechPreviewBeforeSend");
          QVERIFY(option);QVERIFY(!option->isChecked());option->click();QVERIFY(QSettings().value("speech/preview_before_send").toBool()); }
        SpeechSettings restored(executable);QVERIFY(restored.findChild<QCheckBox *>("speechPreviewBeforeSend")->isChecked());
        QVERIFY(!QSettings().contains("speech/profile"));QVERIFY(!QSettings().contains("speech/video_mode"));
    }
    void secondClickUsesCapturedPreviewPreference_data() {
        QTest::addColumn<bool>("videoMode");QTest::addColumn<bool>("preview");
        QTest::newRow("audio-preview")<<false<<true;QTest::newRow("video-preview")<<true<<true;
        QTest::newRow("audio-send")<<false<<false;QTest::newRow("video-send")<<true<<false;
    }
    void secondClickUsesCapturedPreviewPreference() {
        QFETCH(bool,videoMode);QFETCH(bool,preview);QTemporaryDir dir;
        QSettings().setValue("speech/video_mode",videoMode);QSettings().setValue("speech/preview_before_send",preview);
        MessageComposer composer;composer.setSessionKey("fixture\npreview-option");composer.show();
        auto *audio=new FixtureRecorder;auto *video=new FixtureVideoRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},audio,{},video);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());
        QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(videoMode?video->starts:audio->starts,1);
        QTRY_COMPARE(composer.findChild<QComboBox *>("voiceProvider")->count(),1);
        // Preferences changed elsewhere cannot turn an in-flight review into an upload, or vice versa.
        QSettings().setValue("speech/preview_before_send",!preview);
        QCOMPARE(voice->fullLabel(),preview?QString("Stop (0:00)"):QString("Send (0:00)"));
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));QTest::mouseClick(voice,Qt::LeftButton);
        if(preview){
            auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());
            QVERIFY(composer.findChild<QWidget *>("voicePreview")->isVisible());QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
            ComposerDraftStore::flushWrites();const auto saved=ComposerDraftStore().load("voice-message-draft\n"+composer.sessionKey());
            QCOMPARE(saved.attachments.size(),videoMode?2:1);QVERIFY(!saved.formState.value("submitted").toBool());
            const auto id=saved.formState.value("id").toString();QVERIFY(!id.isEmpty());send->click();
            QTRY_VERIFY(QFile::exists(dir.path()+"/voice-actions"));QFile file(dir.path()+"/voice-actions");QVERIFY(file.open(QIODevice::ReadOnly));
            const auto calls=file.readAll().trimmed().split('\n');QCOMPARE(calls.size(),1);
            QCOMPARE(QJsonDocument::fromJson(calls[0]).object().value("request_id").toString(),id);
        }else{QTRY_VERIFY(QFile::exists(dir.path()+"/voice-actions"));}
        QTRY_VERIFY(voice->isEnabled());QCOMPARE(videoMode?video->stops:audio->stops,1);
    }
    void modeSwitchIsDisabledForOpeningCaptureAndPreview_data() {
        QTest::addColumn<bool>("videoMode");QTest::newRow("audio")<<false;QTest::newRow("video")<<true;
    }
    void modeSwitchIsDisabledForOpeningCaptureAndPreview() {
        QFETCH(bool,videoMode);QSettings().setValue("speech/video_mode",videoMode);
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\nmode-lock");composer.show();
        auto *audio=new FixtureRecorder;audio->startImmediately=false;auto *video=new FixtureVideoRecorder;video->startImmediately=false;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},audio,{},video);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");auto *mode=composer.findChild<ToolbarChip *>("voiceMode");QTRY_VERIFY(voice->isEnabled());
        QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(videoMode?video->starts:audio->starts,1);QVERIFY(!mode->isEnabled());
        QVERIFY(!voice->isEnabled());QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(videoMode?video->starts:audio->starts,1);
        mode->click();QCOMPARE(QSettings().value("speech/video_mode").toBool(),videoMode);
        if(videoMode)emit video->started();else emit audio->started();
        QTRY_VERIFY(voice->isEnabled());mode->click();QCOMPARE(QSettings().value("speech/video_mode").toBool(),videoMode);
        composer.findChild<QPushButton *>("voiceStopPreview")->click();
        QTRY_VERIFY(composer.findChild<QPushButton *>("voiceSend")->isEnabled());QVERIFY(!mode->isEnabled());
        mode->click();QCOMPARE(QSettings().value("speech/video_mode").toBool(),videoMode);
        QCOMPARE(videoMode?video->stops:audio->stops,1);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void cancelledSendClickKeepsCaptureUntilExplicitCancel() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\ncancel");composer.show();
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());
        QTest::mouseClick(voice,Qt::LeftButton);QCOMPARE(recorder->starts,1);
        QTest::mousePress(voice,Qt::LeftButton);
        QTest::mouseRelease(voice,Qt::LeftButton,Qt::NoModifier,QPoint(10,composer.height()+400));
        QVERIFY(!recorder->cancelled);QCOMPARE(recorder->stops,0);QVERIFY(voice->fullLabel().startsWith("Send ("));
        composer.findChild<ToolbarChip *>("voiceCancel")->click();
        QVERIFY(recorder->cancelled);QCOMPARE(recorder->stops,0);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        QVERIFY(!composer.findChild<QWidget *>("voicePreview")->isVisible());
    }
    void captureStartsBeforeDiscovery_data() {
        QTest::addColumn<QString>("gate");
        QTest::newRow("capability-probe")<<QString("help-gate");
        QTest::newRow("provider-discovery")<<QString("profiles-gate");
    }
    void captureStartsBeforeDiscovery() {
        QFETCH(QString,gate);QTemporaryDir dir;QFile blocked(dir.path()+"/"+gate);QVERIFY(blocked.open(QIODevice::WriteOnly));blocked.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        composer.editor()->setPlainText("Keep text while discovery waits");auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");auto *send=composer.findChild<QPushButton *>("voiceSend");
        QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);
        // Synchronous local start is required even while either subprocess is gated.
        QCOMPARE(recorder->starts,1);QVERIFY(voice->fullLabel().startsWith("Send ("));QVERIFY(composer.editor()->isReadOnly());
        if(gate=="profiles-gate")QTRY_VERIFY(QFile::exists(dir.path()+"/profiles-started"));
        voice->click();QTRY_VERIFY(composer.findChild<QPushButton *>("voiceDiscard")->isEnabled());QVERIFY(!send->isEnabled());
        ComposerDraftStore::flushWrites();const auto before=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QCOMPARE(before.attachments.size(),1);QVERIFY(!before.formState.value("id").toString().isEmpty());
        QVERIFY(blocked.remove());QTRY_VERIFY(send->isEnabled());ComposerDraftStore::flushWrites();
        const auto after=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QCOMPARE(after.formState.value("id"),before.formState.value("id"));QCOMPARE(after.attachments[0].data,before.attachments[0].data);
        QCOMPARE(recorder->starts,1);QCOMPARE(recorder->stops,1);QVERIFY(!composer.editor()->isReadOnly());
        QCOMPARE(composer.editor()->toPlainText(),"Keep text while discovery waits");QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void failedDiscoveryCanRefreshTheSamePreview() {
        QTemporaryDir dir;QFile failure(dir.path()+"/profile-failure");QVERIFY(failure.open(QIODevice::WriteOnly));failure.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");auto *send=composer.findChild<QPushButton *>("voiceSend");
        auto *refresh=composer.findChild<QPushButton *>("voiceRefreshProviders");auto *provider=composer.findChild<QComboBox *>("voiceProvider");
        QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QCOMPARE(recorder->starts,1);
        QTRY_VERIFY(provider->toolTip().contains("Synthetic provider lookup failure"));
        QVERIFY(voice->fullLabel().startsWith("Send ("));QVERIFY(composer.editor()->isReadOnly());
        recordOrPreview(voice);QTRY_VERIFY(refresh->isEnabled());QVERIFY(!send->isEnabled());
        ComposerDraftStore::flushWrites();const auto before=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");QCOMPARE(before.attachments.size(),1);
        QVERIFY(failure.remove());refresh->click();QVERIFY(!send->isEnabled());QTRY_VERIFY(send->isEnabled());
        ComposerDraftStore::flushWrites();const auto after=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QCOMPARE(after.formState.value("id"),before.formState.value("id"));QCOMPARE(after.attachments[0].data,before.attachments[0].data);
        QCOMPARE(recorder->starts,1);QVERIFY(provider->toolTip().isEmpty());QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void unavailableSelectionSurvivesPreviewRestore() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());
        const QJsonObject original{{"machine_id","aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"},{"profile_id","selected"}};
        QSettings().setValue("speech/machine_id",original.value("machine_id").toString());QSettings().setValue("speech/profile","selected");
        QString id;QByteArray audio;
        {
            MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
            auto *recorder=new FixtureRecorder;SpeechComposer speech(executable,&composer,[&]{return target(composer);},recorder);
            auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QCOMPARE(recorder->starts,1);recordOrPreview(voice);
            auto *provider=composer.findChild<QComboBox *>("voiceProvider");QTRY_COMPARE(provider->currentText(),"Selected provider (unavailable)");
            QVERIFY(!composer.findChild<QPushButton *>("voiceSend")->isEnabled());
            ComposerDraftStore::flushWrites();const auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
            QCOMPARE(draft.formState.value("speech").toObject(),original);id=draft.formState.value("id").toString();audio=draft.attachments[0].data;
        }
        QSettings().setValue("speech/machine_id","bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb");QSettings().setValue("speech/profile","russian");
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(executable,&composer,[&]{return target(composer);},recorder);
        auto *provider=composer.findChild<QComboBox *>("voiceProvider");auto *send=composer.findChild<QPushButton *>("voiceSend");
        QTRY_COMPARE(provider->currentText(),"Selected provider (unavailable)");QVERIFY(!send->isEnabled());QCOMPARE(recorder->starts,0);
        provider->setCurrentIndex(0);QVERIFY(send->isEnabled());QVERIFY(provider->toolTip().isEmpty());
        ComposerDraftStore::flushWrites();const auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QCOMPARE(draft.formState.value("id").toString(),id);QCOMPARE(draft.attachments[0].data,audio);
        QCOMPARE(draft.formState.value("speech").toObject(),provider->currentData().toJsonObject());QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void delayedDiscoveryCannotValidateChangedConversation() {
        QTemporaryDir dir;QFile gate(dir.path()+"/profiles-gate");QVERIFY(gate.open(QIODevice::WriteOnly));gate.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        QString conversation="native-conversation";auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer,conversation);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QCOMPARE(recorder->starts,1);
        QTRY_VERIFY(QFile::exists(dir.path()+"/profiles-started"));conversation="replacement-conversation";QVERIFY(gate.remove());
        auto *provider=composer.findChild<QComboBox *>("voiceProvider");QTRY_VERIFY(provider->toolTip().contains("conversation changed"));
        QVERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);QTRY_VERIFY(composer.findChild<QPushButton *>("voiceDiscard")->isEnabled());
        QVERIFY(!composer.findChild<QPushButton *>("voiceSend")->isEnabled());ComposerDraftStore::flushWrites();
        const auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");QCOMPARE(draft.attachments.size(),1);
        QCOMPARE(draft.formState.value("conversation").toString(),"native-conversation");QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void oldDiscoveryCannotChangeANewCaptureAfterNavigation() {
        QTemporaryDir dir;QFile gate(dir.path()+"/profiles-gate-native-conversation");QVERIFY(gate.open(QIODevice::WriteOnly));gate.close();
        QFile failure(dir.path()+"/profile-failure");QVERIFY(failure.open(QIODevice::WriteOnly));failure.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        QString conversation="native-conversation";auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer,conversation);},recorder);
        QSignalSpy finished(speech.findChild<SpeechClient *>(),&SpeechClient::requestFinished);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QCOMPARE(recorder->starts,1);
        QTRY_VERIFY(QFile::exists(dir.path()+"/profiles-started-native-conversation"));QVERIFY(failure.remove());
        conversation="second-conversation";composer.setSessionKey("fixture\nsecond");QVERIFY(recorder->cancelled);QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));QVERIFY(!composer.editor()->isReadOnly());
        QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QCOMPARE(recorder->starts,2);recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());
        QCOMPARE(finished.count(),1);QVERIFY(gate.remove());QTRY_COMPARE(finished.count(),2);QVERIFY(send->isEnabled());
        QVERIFY(composer.findChild<QComboBox *>("voiceProvider")->toolTip().isEmpty());ComposerDraftStore::flushWrites();
        QVERIFY(ComposerDraftStore().load("voice-message-draft\nfixture\noriginal").attachments.isEmpty());
        const auto draft=ComposerDraftStore().load("voice-message-draft\nfixture\nsecond");QCOMPARE(draft.attachments.size(),1);
        QCOMPARE(draft.formState.value("conversation").toString(),"second-conversation");QCOMPARE(recorder->starts,2);
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void delayedDiscoveryCannotResurrectDiscardedPreview() {
        QTemporaryDir dir;QFile gate(dir.path()+"/profiles-gate");QVERIFY(gate.open(QIODevice::WriteOnly));gate.close();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        QSignalSpy finished(speech.findChild<SpeechClient *>(),&SpeechClient::requestFinished);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QCOMPARE(recorder->starts,1);
        QTRY_VERIFY(QFile::exists(dir.path()+"/profiles-started"));recordOrPreview(voice);auto *discard=composer.findChild<QPushButton *>("voiceDiscard");QTRY_VERIFY(discard->isEnabled());
        QTimer::singleShot(0,&composer,[&]{if(auto *button=composer.findChild<QPushButton *>("confirmVoiceDiscard"))button->click();});
        discard->click();QTRY_VERIFY(voice->isEnabled());QVERIFY(gate.remove());QTRY_COMPARE(finished.count(),1);ComposerDraftStore::flushWrites();
        QVERIFY(ComposerDraftStore().load("voice-message-draft\nfixture\noriginal").attachments.isEmpty());QCOMPARE(recorder->starts,1);
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void recordingKeysStopOnceAndKeepTheTypedDraft_data() {
        QTest::addColumn<int>("key");QTest::addColumn<QString>("focus");
        for(const auto key:{Qt::Key_Space,Qt::Key_Escape})for(const auto &focus:{"editor","history","cancel"})
            QTest::newRow(qPrintable(QString::number(key)+"-"+focus))<<int(key)<<QString(focus);
    }
    void recordingKeysStopOnceAndKeepTheTypedDraft() {
        QFETCH(int,key);QFETCH(QString,focus);QTemporaryDir dir;QWidget window;auto *layout=new QVBoxLayout(&window);
        QPlainTextEdit history;history.setReadOnly(true);history.setPlainText("Readable history");layout->addWidget(&history);
        MessageComposer composer;layout->addWidget(&composer);composer.setSessionKey("fixture\nkeys");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.setAvailability(true);composer.setInterruptAvailability(true,true);
        composer.editor()->setPlainText("Preserve selected text");auto cursor=composer.editor()->textCursor();cursor.setPosition(4);cursor.setPosition(12,QTextCursor::KeepAnchor);composer.editor()->setTextCursor(cursor);
        auto *recorder=new FixtureRecorder;recorder->finishImmediately=false;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        QShortcut escape(QKeySequence(Qt::Key_Escape),&window);connect(&escape,&QShortcut::activated,&composer,[&]{composer.requestInterrupt();});
        QSignalSpy interrupts(&composer,&MessageComposer::interruptRequested),sent(&composer,&MessageComposer::sendRequested);
        window.resize(700,480);window.show();window.activateWindow();composer.editor()->setFocus();QTRY_VERIFY(composer.editor()->hasFocus());
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_COMPARE(recorder->starts,1);
        QVERIFY(composer.editor()->isReadOnly());QVERIFY(composer.editor()->hasFocus());
        QCOMPARE(composer.editor()->textCursor().anchor(),cursor.anchor());QCOMPARE(composer.editor()->textCursor().position(),cursor.position());
        QVERIFY(composer.findChild<QLabel *>("voiceRecordingHint")->isVisible());
        QVERIFY(!composer.findChild<QPushButton *>("sendMessage")->isEnabled());QVERIFY(!composer.canAttachFiles());
        QTest::keyClicks(composer.editor(),"blocked");QTest::keyClick(composer.editor(),Qt::Key_Return);
        QInputMethodEvent commit;commit.setCommitString("IME insertion");QApplication::sendEvent(composer.editor(),&commit);
        // Offscreen has an isolated clipboard, so this never changes a desktop's
        // real clipboard during optional native permission qualification.
        if(QGuiApplication::platformName()=="offscreen"){
            QApplication::clipboard()->setText("Blocked paste");composer.editor()->paste();QApplication::clipboard()->clear();
        }
        QCOMPARE(composer.editor()->toPlainText(),"Preserve selected text");QCOMPARE(sent.count(),0);
        QWidget *focused=focus=="history"?static_cast<QWidget *>(&history):focus=="cancel"?static_cast<QWidget *>(composer.findChild<ToolbarChip *>("voiceCancel")):composer.editor();
        focused->setFocus();QTRY_VERIFY(focused->hasFocus());
        QTest::keyPress(focused,Qt::Key(key));QCOMPARE(recorder->stops,1);QVERIFY(!recorder->cancelled);QCOMPARE(interrupts.count(),0);
        QVERIFY(!composer.editor()->isReadOnly());
        auto *send=composer.findChild<QPushButton *>("voiceSend");recorder->finish();QTRY_VERIFY(send->isEnabled());
        // A held key remains owned through saving, preview and focus changes.
        auto *play=composer.findChild<QPushButton *>("voicePreviewPlay");play->setFocus();QSignalSpy played(play,&QPushButton::clicked);
        for(int i=0;i<3;++i){QKeyEvent press(QEvent::KeyPress,key,Qt::NoModifier,{},true);QApplication::sendEvent(play,&press);
            QKeyEvent release(QEvent::KeyRelease,key,Qt::NoModifier,{},true);QApplication::sendEvent(play,&release);}
        QTest::keyRelease(play,Qt::Key(key));QCOMPARE(played.count(),0);QCOMPARE(recorder->stops,1);QCOMPARE(interrupts.count(),0);
        QCOMPARE(composer.editor()->toPlainText(),"Preserve selected text");QCOMPARE(composer.editor()->textCursor().anchor(),cursor.anchor());QCOMPARE(composer.editor()->textCursor().position(),cursor.position());
        QCOMPARE(sent.count(),0);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        ComposerDraftStore::flushWrites();QCOMPARE(ComposerDraftStore().load("voice-message-draft\nfixture\nkeys").attachments.size(),1);
        // Once the physical release is drained, ordinary Escape works again.
        composer.editor()->setFocus();QTest::keyClick(composer.editor(),Qt::Key_Escape);QCOMPARE(interrupts.count(),1);
    }
    void recordingLockGuardsAttachmentsAndKeepsExistingLocks() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\nlocks");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.setAvailability(true);
        QVERIFY(composer.addAttachment("fixture.txt","text/plain","existing"));
        const auto original=composer.editor()->toPlainText();auto *remove=composer.findChild<QPushButton *>("removeAttachment");QVERIFY(remove);
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        composer.show();auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_COMPARE(recorder->starts,1);
        QVERIFY(!remove->isEnabled());remove->click();QVERIFY(!composer.addAttachment("new.txt","text/plain","blocked"));
        QFile file(dir.filePath("dropped.txt"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("blocked");file.close();composer.attachDroppedFiles({file.fileName()});
        QMimeData mime;mime.setUrls({QUrl::fromLocalFile(file.fileName())});
        QDragEnterEvent enter(QPoint(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(composer.editor()->viewport(),&enter);
        QDropEvent drop(QPointF(20,20),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(composer.editor()->viewport(),&drop);
        bool prepared=true;composer.prepareSendingAsync(composer.sessionKey(),false,[&](bool okay){prepared=okay;});QVERIFY(!prepared);
        QVERIFY(!composer.setWaitingToSend(composer.sessionKey(),"Blocked while recording"));QCOMPARE(composer.editor()->toPlainText(),original);
        emit recorder->failed(SpeechRecorder::Failure::Recording,"Synthetic capture failure");QVERIFY(!composer.editor()->isReadOnly());QVERIFY(remove->isEnabled());
        QVERIFY(composer.setWaitingToSend(composer.sessionKey(),"Existing waiting lock"));
        recordOrPreview(voice);QTRY_COMPARE(recorder->starts,2);recorder->finish();
        QTRY_VERIFY(composer.findChild<QPushButton *>("voiceSend")->isEnabled());QVERIFY(composer.editor()->isReadOnly());QVERIFY(!remove->isEnabled());
        QVERIFY(composer.setWaitingToSend(composer.sessionKey(),{}));QVERIFY(!composer.editor()->isReadOnly());QVERIFY(remove->isEnabled());
    }
    void aFreshKeyRecoversAfterLostRecordingRelease() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\nlost-release");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        composer.editor()->setPlainText("Draft");composer.editor()->moveCursor(QTextCursor::End);
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        composer.show();auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_COMPARE(recorder->starts,1);
        QTest::keyPress(composer.editor(),Qt::Key_Space);QTRY_VERIFY(composer.findChild<QPushButton *>("voiceSend")->isEnabled());
        // Release occurred outside this application; a new non-repeat press is
        // ordinary editing, not a second recording operation or a swallowed key.
        QTest::keyClick(composer.editor(),Qt::Key_Space);QCOMPARE(composer.editor()->toPlainText(),"Draft ");QCOMPARE(recorder->stops,1);
    }
    void pendingOpenAndNavigationReleaseOnlyRecordingLock() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\npending");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;recorder->startImmediately=false;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);composer.show();
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_COMPARE(recorder->starts,1);
        QVERIFY(composer.editor()->isReadOnly());QTest::keyClick(composer.editor(),Qt::Key_Escape);QVERIFY(recorder->cancelled);QVERIFY(!composer.editor()->isReadOnly());
        emit recorder->started();QCOMPARE(voice->fullLabel(),"Voice");QVERIFY(!composer.editor()->isReadOnly());
        recorder->startImmediately=true;recordOrPreview(voice);QTRY_COMPARE(recorder->starts,2);QVERIFY(composer.editor()->isReadOnly());
        composer.setSessionKey("fixture\nother");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));QVERIFY(!composer.editor()->isReadOnly());
        QVERIFY(recorder->cancelled);QCOMPARE(recorder->stops,0);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void recordingKeysLeaveOtherWindowsAndDialogsAlone() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\nwindow-scope");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        composer.show();auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_COMPARE(recorder->starts,1);
        QPushButton other("Other window");other.show();QSignalSpy clicked(&other,&QPushButton::clicked);QTest::keyClick(&other,Qt::Key_Space);QCOMPARE(clicked.count(),1);
        QDialog dialog(&composer);dialog.setModal(true);dialog.show();QTest::keyClick(&dialog,Qt::Key_Escape);QVERIFY(!dialog.isVisible());
        QCOMPARE(recorder->stops,0);QVERIFY(!recorder->cancelled);QVERIFY(composer.editor()->isReadOnly());
        composer.findChild<ToolbarChip *>("voiceCancel")->click();QVERIFY(recorder->cancelled);QVERIFY(!composer.editor()->isReadOnly());
        QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
    }
    void interruptedVoiceChecksSameIdentityWithoutReplay() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\noriginal");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.editor()->setPlainText("Keep draft");
        ComposerDraft pending;QByteArray wav(3244,'x');wav.replace(0,4,"RIFF");wav.replace(8,8,"WAVEfmt ");wav.replace(36,4,"data");
        pending.attachments.append({"voice.wav","audio/wav",wav,{}});
        pending.formState={{"id","aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"},{"host",""},{"session","fixture"},{"run","native-run"},{"conversation","native-conversation"},{"submitted",true}};
        QVERIFY(ComposerDraftStore().save("voice-message-draft\nfixture\noriginal",pending));
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);});
        QSignalSpy messages(&speech,&SpeechComposer::messageChanged);auto *send=composer.findChild<QPushButton *>("voiceSend");
        QTRY_COMPARE(send->text(),"Check message");QVERIFY(send->isEnabled());QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));send->click();QTRY_COMPARE(messages.count(),1);
        QFile actions(dir.path()+"/voice-actions");QVERIFY(actions.open(QIODevice::ReadOnly));const auto status=QJsonDocument::fromJson(actions.readLine()).object();
        QCOMPARE(status.value("action").toString(),"status");QCOMPARE(status.value("message_id").toString(),"aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");QVERIFY(actions.readLine().isEmpty());
        QCOMPARE(composer.editor()->toPlainText(),"Keep draft");
    }
    void explicitSendKeepsItsTargetDuringNavigation() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        QString conversation="native-conversation";auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer,conversation);},recorder);QSignalSpy messages(&speech,&SpeechComposer::messageChanged);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_VERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());send->click();
        conversation="other-conversation";composer.setSessionKey("fixture\nother");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.editor()->setPlainText("Other draft");
        QTRY_COMPARE(messages.count(),1);QCOMPARE(messages[0][0].toString(),"fixture\noriginal");
        QCOMPARE(messages[0][1].toJsonObject().value("conversation_id").toString(),"native-conversation");QCOMPARE(composer.editor()->toPlainText(),"Other draft");
        QFile actions(dir.path()+"/voice-actions");QVERIFY(actions.open(QIODevice::ReadOnly));const auto sent=QJsonDocument::fromJson(actions.readLine()).object();
        QCOMPARE(sent.value("expected_conversation_id").toString(),"native-conversation");QVERIFY(actions.readLine().isEmpty());
    }
    void changedConversationCannotSendRetainedRecording() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        QString conversation="native-conversation";auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer,conversation);},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_VERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());conversation="replacement-conversation";send->click();
        QTest::qWait(100);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        ComposerDraftStore::flushWrites();QVERIFY(!ComposerDraftStore().load("voice-message-draft\nfixture\noriginal").attachments.isEmpty());
    }
    void coldCacheConfirmationCannotAuthorizeAnotherConversation() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        QString conversation="native-conversation";auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&]{auto result=target(composer,conversation);result.warning="Synthetic full-context warning";return result;},recorder);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_VERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());
        bool confirmed=false;
        QTimer::singleShot(0,&composer,[&]{
            auto *dialog=composer.findChild<QMessageBox *>("voiceColdCacheConfirm");if(!dialog)return;
            conversation="other-conversation";composer.setSessionKey("fixture\nother");
            confirmed=true;dialog->findChild<QPushButton *>("confirmVoiceSend")->click();
        });
        send->click();QVERIFY(confirmed);QTest::qWait(100);QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        ComposerDraftStore::flushWrites();const auto saved=ComposerDraftStore().load("voice-message-draft\nfixture\noriginal");
        QVERIFY(!saved.attachments.isEmpty());QVERIFY(!saved.formState.value("submitted").toBool());
        QVERIFY(ComposerDraftStore().load("voice-message-draft\nfixture\nother").attachments.isEmpty());
    }
    void unsentVoiceRestoresSeparatelyFromTypedDraft() {
        QTemporaryDir dir;const auto executable=fixture(dir.path());
        {
            MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
            auto *recorder=new FixtureRecorder;SpeechComposer speech(executable,&composer,[&]{return target(composer);},recorder);
            auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_VERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);
            QTRY_VERIFY(composer.findChild<QPushButton *>("voiceSend")->isEnabled());
        }
        ComposerDraftStore::flushWrites();
        MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));composer.editor()->setPlainText("Keep typed draft");
        SpeechComposer speech(executable,&composer,[&]{return target(composer);});
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());QCOMPARE(send->text(),"Send voice");
        QCOMPARE(composer.editor()->toPlainText(),"Keep typed draft");QVERIFY(!QFile::exists(dir.path()+"/voice-actions"));
        bool guarded=false;QTimer timer;timer.setInterval(10);
        connect(&timer,&QTimer::timeout,&composer,[&]{
            auto *list=composer.findChild<QListWidget *>("savedDraftList");if(!list || !list->count())return;
            for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString().startsWith("voice-message-draft\n")){
                list->setCurrentRow(i);auto *restore=composer.findChild<QPushButton *>("restoreSavedDraft");
                if(!restore || restore->isEnabled())return;
                bool loaded=false;for(auto *label:list->window()->findChildren<QLabel *>())loaded|=label->text().contains("Open the original conversation");
                if(!loaded)return;
                guarded=true;timer.stop();qobject_cast<QDialog *>(list->window())->accept();return;
            }
        });
        QTimer::singleShot(3000,&composer,[&]{if(auto *list=composer.findChild<QListWidget *>("savedDraftList"))qobject_cast<QDialog *>(list->window())->reject();});
        timer.start();composer.showSavedDrafts();QVERIFY(guarded);
        QTimer::singleShot(0,&composer,[&]{if(auto *dialog=composer.findChild<QMessageBox *>("voiceDiscardConfirm"))dialog->reject();});
        composer.findChild<QPushButton *>("voiceDiscard")->click();QVERIFY(send->isEnabled());
        QTimer::singleShot(0,&composer,[&]{if(auto *button=composer.findChild<QPushButton *>("confirmVoiceDiscard"))button->click();});
        composer.findChild<QPushButton *>("voiceDiscard")->click();QTRY_VERIFY(composer.findChild<ToolbarChip *>("voiceRecord")->isEnabled());
        ComposerDraftStore::flushWrites();QVERIFY(ComposerDraftStore().load("voice-message-draft\nfixture\noriginal").attachments.isEmpty());
        QCOMPARE(composer.editor()->toPlainText(),"Keep typed draft");
    }
    void ambiguousSubmissionChecksWithoutResendingAudio() {
        QTemporaryDir dir;MessageComposer composer;composer.setSessionKey("fixture\noriginal");QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder;SpeechComposer speech(fixture(dir.path()),&composer,[&]{return target(composer);},recorder);
        QFile lost(dir.path()+"/lost-submit");QVERIFY(lost.open(QIODevice::WriteOnly));lost.close();
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord");QTRY_VERIFY(voice->isEnabled());recordOrPreview(voice);QTRY_VERIFY(voice->fullLabel().startsWith("Send ("));recordOrPreview(voice);
        auto *send=composer.findChild<QPushButton *>("voiceSend");QTRY_VERIFY(send->isEnabled());send->click();QTRY_COMPARE(send->text(),"Check message");send->click();QTRY_VERIFY(voice->isEnabled());
        QFile actions(dir.path()+"/voice-actions");QVERIFY(actions.open(QIODevice::ReadOnly));const auto submit=QJsonDocument::fromJson(actions.readLine()).object();const auto status=QJsonDocument::fromJson(actions.readLine()).object();
        QCOMPARE(submit.value("action").toString(),"submit");QCOMPARE(status.value("action").toString(),"status");QCOMPARE(submit.value("request_id"),status.value("message_id"));QVERIFY(actions.readLine().isEmpty());
    }
    void hidingComposerCancelsUnsubmittedCapture_data() {
        QTest::addColumn<bool>("started"); QTest::newRow("permission-pending")<<false; QTest::newRow("recording")<<true;
    }
    void deniedPermissionImmediatelyOffersFocusedRecovery_data() {
        QTest::addColumn<bool>("openSucceeds");
        QTest::newRow("settings-opened")<<true; QTest::newRow("manual-fallback")<<false;
    }
    void deniedPermissionImmediatelyOffersFocusedRecovery() {
        QFETCH(bool,openSucceeds); QTemporaryDir dir;
        MessageComposer composer; composer.setAttribute(Qt::WA_ShowWithoutActivating);
        composer.setWindowFlag(Qt::WindowDoesNotAcceptFocus); composer.setSessionKey("fixture\npermission");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey())); composer.editor()->setPlainText("Keep this exact draft");
        auto *recorder=new FixtureRecorder; recorder->failure=SpeechRecorder::Failure::PermissionDenied;
        int opened=0; QUrl openedUrl;
        SpeechComposer speech(fixture(dir.path()),&composer,[&] {return target(composer);},recorder,
            [&](const QUrl &url) { ++opened; openedUrl=url; return openSucceeds; });
        QSignalSpy sent(&composer,&MessageComposer::sendRequested);
        QSignalSpy finished(speech.findChild<SpeechClient *>(),&SpeechClient::requestFinished);
        composer.show(); auto *voice=composer.findChild<ToolbarChip *>("voiceRecord"); QTRY_VERIFY(voice->isEnabled());
        const bool supportsSettings=!SpeechComposer::microphoneSettingsUrl().isEmpty();
        bool inspected=false, compact=false, noTranscript=false, instructions=false, buttonCorrect=false, fallback=false;
        QTimer inspect; inspect.setInterval(20);
        connect(&inspect,&QTimer::timeout,&composer,[&] {
            auto *dialog=composer.findChild<QDialog *>("voicePermissionDialog"); if(!dialog || !dialog->isVisible()) return;
            inspect.stop(); inspected=true; compact=dialog->height()<260;
            noTranscript=dialog->findChildren<QPlainTextEdit *>().isEmpty() && !dialog->findChild<QPushButton *>("voiceInsert");
            auto *hint=dialog->findChild<QLabel *>("voicePermissionHint"); instructions=hint && hint->text().contains("press Voice again");
            auto *open=dialog->findChild<QPushButton *>("voiceOpenMicrophoneSettings"); buttonCorrect=bool(open)==supportsSettings;
            const auto screenshot=qEnvironmentVariable("ZERUS_SPEECH_PERMISSION_SCREENSHOT");
            if(!screenshot.isEmpty() && openSucceeds) dialog->grab().save(screenshot);
            if(open) { open->click(); if(!openSucceeds) fallback=hint->text().startsWith("Could not open system settings automatically."); }
            if(!open || !openSucceeds) dialog->findChild<QPushButton *>("voicePermissionClose")->click();
        });
        inspect.start(); recordOrPreview(voice); QTRY_VERIFY(inspected);
        QVERIFY(compact); QVERIFY(noTranscript); QVERIFY(instructions); QVERIFY(buttonCorrect);
        QCOMPARE(opened,supportsSettings?1:0);
        if(supportsSettings) { QCOMPARE(openedUrl,SpeechComposer::microphoneSettingsUrl()); if(!openSucceeds) QVERIFY(fallback); }
        QTest::qWait(100); QCOMPARE(recorder->starts,1); QCOMPARE(voice->fullLabel(),"Voice");
        QCOMPARE(composer.editor()->toPlainText(),"Keep this exact draft"); QCOMPARE(sent.count(),0);
        QVERIFY(ComposerDraftStore().load("voice-pending\nmessageComposer").formState.isEmpty());
        // Reopening the notice never records or changes the draft.
        inspected=false; inspect.start(); composer.findChild<ToolbarChip *>("voiceResult")->click(); QTRY_VERIFY(inspected);
        QCOMPARE(recorder->starts,1);
        // A later device failure mentioning permission is still a generic error.
        recorder->failure=SpeechRecorder::Failure::Recording; recordOrPreview(voice); QTRY_COMPARE(recorder->starts,2);
        QVERIFY(!composer.findChild<QDialog *>("voicePermissionDialog"));
        bool generic=false;
        QTimer::singleShot(0,&composer,[&] {
            auto *dialog=composer.findChild<QDialog *>("voiceResultDialog");
            if(dialog) { generic=!dialog->findChild<QPushButton *>("voiceOpenMicrophoneSettings") && dialog->findChildren<QPlainTextEdit *>().isEmpty(); dialog->accept(); }
        });
        composer.findChild<ToolbarChip *>("voiceResult")->click(); QVERIFY(generic);
        QCOMPARE(composer.editor()->toPlainText(),"Keep this exact draft"); QCOMPARE(sent.count(),0);
        QTRY_COMPARE(finished.count(),2);
        QFile payload(dir.path()+"/payload"); QVERIFY(payload.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(payload.readAll()).object().value("action").toString(),"profiles");
    }
    void hidingComposerCancelsUnsubmittedCapture() {
        QFETCH(bool,started); QTemporaryDir dir;
        MessageComposer composer; composer.setSessionKey("fixture\noriginal"); QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder; recorder->startImmediately=started;
        SpeechComposer speech(fixture(dir.path()),&composer,[&] {return target(composer);},recorder);
        QSignalSpy finished(speech.findChild<SpeechClient *>(),&SpeechClient::requestFinished);
        composer.show(); auto *voice=composer.findChild<ToolbarChip *>("voiceRecord"); QTRY_VERIFY(voice->isEnabled()); recordOrPreview(voice);
        QTRY_VERIFY(started ? voice->fullLabel().startsWith("Send (") : voice->fullLabel()=="Opening recorder…");
        composer.hide(); if(started){QCOMPARE(recorder->stops,1);QTRY_VERIFY(composer.findChild<QWidget *>("voicePreview")->isVisibleTo(&composer));}else QVERIFY(recorder->cancelled); QTRY_COMPARE(voice->fullLabel(),"Voice");
        // Late native callbacks from a cancelled capture cannot submit audio.
        recorder->stop(); QTRY_COMPARE(finished.count(),1); ComposerDraftStore::flushWrites();
        QFile payload(dir.path()+"/payload"); QVERIFY(payload.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(payload.readAll()).object().value("action").toString(),"profiles");
        QVERIFY(ComposerDraftStore().load("voice-pending\nmessageComposer").formState.isEmpty());
    }
    void nativeCaptureOnExplicitFixtureDevice() {
        const auto requested = qgetenv("ZERUS_SPEECH_CAPTURE_DEVICE");
        if (requested.isEmpty()) QSKIP("Requires an explicitly named synthetic audio input; never opens the person's microphone.");
        QByteArray id;
        for (const auto &device : QMediaDevices::audioInputs()) if (device.id() == requested || device.description().toUtf8() == requested) id = device.id();
        QVERIFY2(!id.isEmpty(), "Synthetic capture device unavailable");
        SpeechRecorder recorder; QSignalSpy started(&recorder, &SpeechRecorder::started), failed(&recorder, &SpeechRecorder::failed), recorded(&recorder,&SpeechRecorder::recorded);
        recorder.start(id); QTRY_VERIFY_WITH_TIMEOUT(started.count() || failed.count(),5000);
        QCOMPARE(failed.count(),0); QCOMPARE(started.count(),1); QTest::qWait(1100); recorder.stop();
        QTRY_COMPARE_WITH_TIMEOUT(recorded.count(),1,5000); QCOMPARE(failed.count(),0);
        const auto pcm=QByteArray::fromBase64(recorded[0][0].toByteArray()); QVERIFY(pcm.size()>16000); QVERIFY(pcm.size()<160000);
    }
private:
    QTemporaryDir m_home;
};
QTEST_MAIN(TestSpeech)
#include "test_speech.moc"
