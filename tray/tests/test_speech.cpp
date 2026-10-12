#include "speech/SpeechClient.h"
#include "speech/SpeechRecorder.h"
#include "speech/SpeechComposer.h"
#include "speech/SpeechSettings.h"
#include "ComposerToolbar.h"
#include "ComposerDraftStore.h"
#include <QApplication>
#include <QAudioDevice>
#include <QDir>
#include <QDialog>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QMediaDevices>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
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
import json, pathlib, sys
root = pathlib.Path(__file__).parent
with (root / 'calls').open('a') as log:
    log.write(json.dumps(sys.argv[1:]) + '\n')
if '--help' in sys.argv:
    print('hgs ls --json' if '@legacy' in sys.argv or (root / 'legacy').exists() else 'hgs speech --json')
else:
    payload = json.load(sys.stdin)
    (root / 'payload').write_text(json.dumps(payload))
    if payload['action'] == 'providers':
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
class FixtureRecorder : public SpeechRecorder {
public:
    bool startImmediately = true, cancelled = false;
    int starts = 0;
    std::optional<Failure> failure;
    void start(const QByteArray &) override {
        ++starts;
        if (failure) emit failed(*failure, QStringLiteral("Synthetic recording error mentioning permission"));
        else if (startImmediately) emit started();
    }
    void stop() override { emit recorded(QByteArray(3200,'\0').toBase64()); }
    void cancel() override { cancelled = true; }
};
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
        QFile profiles(dir.path()+"/profiles"); QVERIFY(profiles.open(QIODevice::WriteOnly));
        profiles.write(R"JSON([{"id":"first","name":"First","available":true},{"id":"russian","name":"Russian","available":true},{"id":"offline","name":"Offline","available":false}])JSON"); profiles.close();
        QSettings().setValue("speech/host","speech-host"); QSettings().setValue("speech/profile","russian");
        SpeechSettings settings(fixture(dir.path())); settings.setPeers({"speech-host"}); settings.show(); settings.refresh();
        auto *provider=settings.findChild<QComboBox *>("speechProfile"); QVERIFY(provider);
        auto *refresh=settings.findChild<QPushButton *>("speechRefresh"); QVERIFY(refresh);
        QTRY_VERIFY(refresh->isEnabled()); QVERIFY(provider->isEnabled()); QCOMPARE(provider->count(),3);
        QCOMPARE(provider->currentData().toString(),"russian");
        QCOMPARE(provider->currentData().metaType(),QMetaType::fromType<QString>());
        QVERIFY(provider->model()->flags(provider->model()->index(1,0)).testFlag(Qt::ItemIsEnabled));
        QVERIFY(!provider->model()->flags(provider->model()->index(2,0)).testFlag(Qt::ItemIsEnabled));
        provider->setFocus(); QTest::keyClick(provider,Qt::Key_Home);
        QCOMPARE(provider->currentData().toString(),"first"); QCOMPARE(QSettings().value("speech/profile").toString(),"first");
        refresh->click(); QTRY_VERIFY(refresh->isEnabled()); QCOMPARE(provider->currentData().toString(),"first");
        QFile calls(dir.path()+"/calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
        for(const auto &line:calls.readAll().split('\n')) if(!line.isEmpty()) QCOMPARE(QJsonDocument::fromJson(line).array().at(0).toString(),"@speech-host");
    }
    void settingsCanRefreshAfterSpeechIsRestoredOnReceiver() {
        QTemporaryDir dir; const auto executable=fixture(dir.path());
        QFile legacy(dir.path()+"/legacy"); QVERIFY(legacy.open(QIODevice::WriteOnly)); legacy.close();
        QSettings().setValue("speech/host","speech-host"); QSettings().setValue("speech/profile","russian");
        SpeechSettings settings(executable); settings.show(); settings.refresh();
        auto *provider=settings.findChild<QComboBox *>("speechProfile");
        auto *status=settings.findChild<QLabel *>("speechSettingsStatus");
        auto *refresh=settings.findChild<QPushButton *>("speechRefresh");
        QTRY_VERIFY(refresh->isEnabled()); QVERIFY(!provider->isEnabled()); QVERIFY(status->text().contains("updated hgs"));
        QCOMPARE(QSettings().value("speech/profile").toString(),"russian"); QVERIFY(!QFile::exists(dir.path()+"/payload"));
        QVERIFY(legacy.remove()); refresh->click(); QTRY_VERIFY(refresh->isEnabled());
        QVERIFY(provider->isEnabled()); QCOMPARE(provider->count(),1); QCOMPARE(provider->currentData().toString(),"russian");
        QVERIFY(status->text().startsWith("Ready."));
    }
    void nativeSettingsOnExplicitSpeechHost() {
        const auto executable=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_HGS");
        const auto host=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_HOST");
        const auto expected=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_PROFILE");
        if(executable.isEmpty() || expected.isEmpty()) QSKIP("Requires an explicit installed hgs and profile; only reads providers/statistics.");
        QSettings().setValue("speech/host",host); QSettings().setValue("speech/profile",expected);
        SpeechSettings settings(executable); settings.setPeers({host}); settings.show(); settings.refresh();
        auto *provider=settings.findChild<QComboBox *>("speechProfile");
        auto *status=settings.findChild<QLabel *>("speechSettingsStatus");
        auto *refresh=settings.findChild<QPushButton *>("speechRefresh");
        QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),65000);
        QVERIFY2(provider->isEnabled(),qPrintable(status->text())); QCOMPARE(provider->currentData().toString(),expected);
        const auto index=provider->model()->index(provider->currentIndex(),0);
        QVERIFY(provider->model()->flags(index).testFlag(Qt::ItemIsEnabled));
        QVERIFY(provider->model()->flags(index).testFlag(Qt::ItemIsSelectable));
        provider->setFocus(); provider->showPopup(); QTest::keyClick(provider,Qt::Key_Return);
        QCOMPARE(QSettings().value("speech/profile").toString(),expected);
        QVERIFY(status->text().startsWith("Ready."));
    }
    void interruptedRequestRecoversWithoutSendingOrInserting() {
        QTemporaryDir dir; ComposerDraftStore store;
        ComposerDraft pending; pending.formState = {{"job_id","aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa"},{"host","speech-host"},{"target","original\nsession"}};
        QVERIFY(store.save("voice-pending\nmessageComposer",pending));
        MessageComposer composer; composer.setSessionKey("different\nconversation");
        QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey())); composer.editor()->setPlainText("Keep this draft");
        QSignalSpy sent(&composer, &MessageComposer::sendRequested);
        SpeechComposer speech(fixture(dir.path()), &composer, [] { return "different-identity"; });
        auto *voice = composer.findChild<ToolbarChip *>("voiceRecord"); QVERIFY(voice);
        QTRY_COMPARE(voice->fullLabel(), "Check voice result"); voice->click();
        QTRY_COMPARE(voice->fullLabel(), "Voice");
        QCOMPARE(composer.editor()->toPlainText(), "Keep this draft"); QCOMPARE(sent.count(),0);
        ComposerDraftStore::flushWrites();
        QCOMPARE(store.load("voice\naaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa").text,"Recognized fixture");
        QVERIFY(store.load("voice-pending\nmessageComposer").formState.isEmpty());
        auto *result = composer.findChild<ToolbarChip *>("voiceResult"); QVERIFY(result->isActive());
    }
    void dictationInsertsOnlyIntoAnUnchangedOriginalDraft_data() {
        QTest::addColumn<QString>("change");
        QTest::newRow("unchanged") << QString(); QTest::newRow("edit") << QString("edit");
        QTest::newRow("navigate") << QString("navigate"); QTest::newRow("identity") << QString("identity");
    }
    void dictationInsertsOnlyIntoAnUnchangedOriginalDraft() {
        QFETCH(QString,change); QTemporaryDir dir;
        MessageComposer composer; composer.setSessionKey("fixture\noriginal"); QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        composer.editor()->setPlainText("Keep"); composer.editor()->moveCursor(QTextCursor::End);
        QString identity="original-native-conversation"; auto *recorder=new FixtureRecorder;
        SpeechComposer speech(fixture(dir.path()),&composer,[&] {return identity;},recorder);
        QSignalSpy sent(&composer,&MessageComposer::sendRequested);
        auto *voice=composer.findChild<ToolbarChip *>("voiceRecord"); QTRY_VERIFY(voice->isEnabled()); voice->click();
        QTRY_VERIFY(voice->fullLabel().startsWith("Stop voice"));
        if(change=="edit") composer.editor()->insertPlainText(" typed");
        if(change=="navigate") { composer.setSessionKey("fixture\nother"); QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey())); composer.editor()->setPlainText("Other draft"); }
        if(change=="identity") identity="new-native-conversation";
        voice->click();
        QTRY_COMPARE(voice->fullLabel(),"Voice");
        if(change.isEmpty()) QTRY_COMPARE(composer.editor()->toPlainText(),"Keep Recognized fixture");
        else {
            QCOMPARE(composer.editor()->toPlainText(), change=="edit"?"Keep typed":change=="navigate"?"Other draft":"Keep");
            auto *result=composer.findChild<ToolbarChip *>("voiceResult"); QVERIFY(result->isActive());
            ComposerDraftStore::flushWrites(); bool recovered=false;
            ComposerDraftStore store; for(const auto &key:store.keys()) if(key.startsWith("voice\n")) recovered|=store.load(key).text=="Recognized fixture";
            QVERIFY(recovered);
        }
        QCOMPARE(sent.count(),0);
        // Completed submit acknowledgements have no transcript; status fetched it.
        QFile payload(dir.path()+"/payload"); QVERIFY(payload.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(payload.readAll()).object().value("action").toString(),"status");
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
        SpeechComposer speech(fixture(dir.path()),&composer,[] {return "permission-identity";},recorder,
            [&](const QUrl &url) { ++opened; openedUrl=url; return openSucceeds; });
        QSignalSpy sent(&composer,&MessageComposer::sendRequested);
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
        inspect.start(); voice->click(); QTRY_VERIFY(inspected);
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
        recorder->failure=SpeechRecorder::Failure::Recording; voice->click(); QTRY_COMPARE(recorder->starts,2);
        QVERIFY(!composer.findChild<QDialog *>("voicePermissionDialog"));
        bool generic=false;
        QTimer::singleShot(0,&composer,[&] {
            auto *dialog=composer.findChild<QDialog *>("voiceResultDialog");
            if(dialog) { generic=!dialog->findChild<QPushButton *>("voiceOpenMicrophoneSettings") && dialog->findChildren<QPlainTextEdit *>().isEmpty(); dialog->accept(); }
        });
        composer.findChild<ToolbarChip *>("voiceResult")->click(); QVERIFY(generic);
        QCOMPARE(composer.editor()->toPlainText(),"Keep this exact draft"); QCOMPARE(sent.count(),0);
        QFile payload(dir.path()+"/payload"); QVERIFY(payload.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(payload.readAll()).object().value("action").toString(),"providers");
    }
    void hidingComposerCancelsUnsubmittedCapture() {
        QFETCH(bool,started); QTemporaryDir dir;
        MessageComposer composer; composer.setSessionKey("fixture\noriginal"); QTRY_VERIFY(!composer.isDraftLoading(composer.sessionKey()));
        auto *recorder=new FixtureRecorder; recorder->startImmediately=started;
        SpeechComposer speech(fixture(dir.path()),&composer,[] {return "identity";},recorder);
        composer.show(); auto *voice=composer.findChild<ToolbarChip *>("voiceRecord"); QTRY_VERIFY(voice->isEnabled()); voice->click();
        QTRY_VERIFY(started ? voice->fullLabel().startsWith("Stop voice") : voice->fullLabel()=="Opening microphone…");
        composer.hide(); QVERIFY(recorder->cancelled); QCOMPARE(voice->fullLabel(),"Voice");
        // Late native callbacks from a cancelled capture cannot submit audio.
        recorder->stop(); QTest::qWait(100); ComposerDraftStore::flushWrites();
        QFile payload(dir.path()+"/payload"); QVERIFY(payload.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(payload.readAll()).object().value("action").toString(),"providers");
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
