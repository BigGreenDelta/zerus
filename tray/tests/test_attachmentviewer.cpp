#include <QtTest>
#include <QTemporaryDir>
#include <QSemaphore>
#include <QScopeGuard>
#include <QThread>
#include "AttachmentViewer.h"

class TestAttachmentViewer : public QObject {
    Q_OBJECT
    QList<QUrl> m_opened;
    QStringList m_revealed;
private slots:
    void capture(const QUrl &url){m_opened.append(url);}
    void initTestCase(){QStandardPaths::setTestModeEnabled(true);}
    void init(){m_opened.clear();m_revealed.clear();QDesktopServices::setUrlHandler("file",this,"capture");
        ExternalFile::setRevealHandler([this](const QString &path){m_revealed<<path;return true;});}
    void cleanup(){AttachmentFiles::setIoObserver({}); QVERIFY(AttachmentFiles::pool().waitForDone(5000)); QDesktopServices::unsetUrlHandler("file");
        ExternalFile::setRevealHandler({});}
    void programAttachmentIsShownInItsFolder() {
        // A conversation attachment that runs programs is revealed, never started.
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);
        const auto path=AttachmentFiles::store("setup.exe","MZ program fixture");QVERIFY(!path.isEmpty());
        auto cleanup=qScopeGuard([&]{QFile::remove(path);});
        QString notice;bool done=false;
        loader.open("mac","session","conversation",{}, {},{{"name","setup.exe"},{"local_path",path}},
            [&](const QString &opened,const QString &error,const QString &message){QCOMPARE(opened,path);QVERIFY(error.isEmpty());notice=message;done=true;});
        QTRY_VERIFY(done);QVERIFY(m_opened.isEmpty());QCOMPARE(m_revealed,QStringList{path});QVERIFY(notice.contains("setup.exe"));
    }
    void cachedImageHasThumbnailAndOpensInDefaultAppWithoutDialog() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);
        QImage image(320,200,QImage::Format_RGB32);image.fill(Qt::blue);
        QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
        const auto path=AttachmentFiles::store("../../screen.png",bytes);QVERIFY(!path.isEmpty());QVERIFY(path.endsWith("/screen.png"));
        QSignalSpy errors(&client,&HgsClient::attachmentFailed);
        const auto thumbnail=AttachmentLoader::thumbnail(path);QVERIFY(!thumbnail.isNull());QVERIFY(thumbnail.width()<=192);QVERIFY(thumbnail.height()<=128);
        bool done=false;
        loader.open("mac","session","conversation",{}, {},{{"name","screen.png"},{"mime","image/png"},{"local_path",path}},
            [&](const QString &opened,const QString &error,const QString &notice){QCOMPARE(opened,path);QVERIFY(error.isEmpty());QVERIFY(notice.isEmpty());done=true;});
        QTRY_VERIFY(done);QCOMPARE(m_opened,QList<QUrl>{QUrl::fromLocalFile(path)});QCOMPARE(errors.size(),0);
        for(auto *widget:QApplication::topLevelWidgets())QVERIFY(widget->objectName()!="attachmentPreviewDialog");
        const auto html=AttachmentFiles::store("notes.html","<script>never execute</script>");
        QVERIFY(AttachmentLoader::thumbnail(html).isNull());QFile::remove(path);QFile::remove(html);
    }
    void missingFileAndUnrelatedResponseDoNotOpenAnotherAttachment() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);bool done=false;
        loader.open("mac","session","conversation",{}, {},{{"name","missing.png"},{"request_id","request-one"},{"index",0}},
            [&](const QString &path,const QString &error,const QString &){QVERIFY(path.isEmpty());QVERIFY(!error.isEmpty());done=true;});
        client.attachmentReady(77,{{"name","other.txt"}},"unrelated");QVERIFY(m_opened.isEmpty());
        QTRY_VERIFY(done);QVERIFY(m_opened.isEmpty());
        QTemporaryFile file;QVERIFY(file.open());QVERIFY(AttachmentFiles::cached({{"local_path",file.fileName()}}).isEmpty());
    }
    void thumbnailAndOpenShareRequestButConversationsDoNot() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);int done=0,firstConversation=0;
        const QJsonObject file{{"name","image.png"},{"mime","image/png"},{"request_id","request-one"},{"index",0}};
        QString path;
        loader.fetch("mac","session","one",{}, {},file,[&](const QString &p,const QString &error){QVERIFY(error.isEmpty());path=p;++firstConversation;++done;});
        loader.open("mac","session","one",{}, {},file,[&](const QString &p,const QString &error,const QString &){QCOMPARE(p,path);QVERIFY(error.isEmpty());++firstConversation;++done;});
        loader.fetch("mac","session","two",{}, {},file,[&](const QString &p,const QString &error){QVERIFY(p.isEmpty());QVERIFY(!error.isEmpty());++done;});
        client.attachmentReady(1,{{"name","image.png"}},"fixture bytes");
        QTRY_COMPARE(firstConversation,2);QCOMPARE(m_opened.size(),1);
        client.attachmentFailed(2,"No such file in the other conversation");QCOMPARE(done,3);
        loader.fetch("mac","session","one",{}, {},file,[&](const QString &p,const QString &error){QCOMPARE(p,path);QVERIFY(error.isEmpty());++done;});
        QTRY_COMPARE(done,4);QCOMPARE(m_opened.size(),1);QFile::remove(path);
    }
    void snapshotsAndRecoveryPreserveMetadataAndBytes() {
        const QList<MessageAttachment> attachments{{"../../example.txt","text/plain","synthetic text","[File #1]"},
            {"image.png","image/png","synthetic image","[Image #2]"}};
        QJsonArray files;QString error;bool done=false;
        AttachmentFiles::prepareAsync(attachments,this,[&](const QJsonArray &result,const QString &failure){files=result;error=failure;done=true;});
        QTRY_VERIFY(done);QVERIFY(error.isEmpty());QCOMPARE(files.size(),2);
        for(int i=0;i<2;++i) {
            const auto file=files[i].toObject();QCOMPARE(file.value("name").toString(),attachments[i].name);
            QCOMPARE(file.value("mime").toString(),attachments[i].mime);QCOMPARE(file.value("reference").toString(),attachments[i].reference);
            QCOMPARE(file.value("bytes").toInteger(),attachments[i].data.size());QVERIFY(!AttachmentFiles::cached(file).isEmpty());
        }
        QList<MessageAttachment> restored;done=false;
        AttachmentFiles::readAsync(files,this,[&](const QList<MessageAttachment> &result,const QString &failure){restored=result;error=failure;done=true;});
        QTRY_VERIFY(done);QVERIFY(error.isEmpty());QCOMPARE(restored.size(),attachments.size());
        for(int i=0;i<restored.size();++i) {
            QCOMPARE(restored[i].name,attachments[i].name);QCOMPARE(restored[i].mime,attachments[i].mime);
            QCOMPARE(restored[i].reference,attachments[i].reference);QCOMPARE(restored[i].data,attachments[i].data);
            QFile::remove(files[i].toObject().value("local_path").toString());
        }
    }
    void blockedIoKeepsHeartbeatAndCallbacksOnOwnerThread() {
        struct Gate { QSemaphore entered,release; };auto gate=std::make_shared<Gate>();
        auto unblock=qScopeGuard([&]{gate->release.release(2);});
        AttachmentFiles::setIoObserver([gate]{gate->entered.release();gate->release.acquire();});
        bool prepared=false;bool read=false;bool heartbeat=false;QJsonArray files;
        AttachmentFiles::prepareAsync({{"example.txt","text/plain","heartbeat fixture","[File #1]"}},this,
            [&](const QJsonArray &result,const QString &error){QCOMPARE(QThread::currentThread(),thread());QVERIFY(error.isEmpty());files=result;prepared=true;});
        QTRY_VERIFY(gate->entered.available()==1);
        QTimer::singleShot(0,this,[&]{heartbeat=true;});QTRY_VERIFY(heartbeat);QVERIFY(!prepared);
        gate->entered.acquire();gate->release.release();QTRY_VERIFY(prepared);heartbeat=false;
        AttachmentFiles::readAsync(files,this,[&](const QList<MessageAttachment> &result,const QString &error){
            QCOMPARE(QThread::currentThread(),thread());QVERIFY(error.isEmpty());QCOMPARE(result.first().data,QByteArray("heartbeat fixture"));read=true;
        });
        QTRY_VERIFY(gate->entered.available()==1);
        QTimer::singleShot(0,this,[&]{heartbeat=true;});QTRY_VERIFY(heartbeat);QVERIFY(!read);
        gate->release.release();QTRY_VERIFY(read);QFile::remove(files.first().toObject().value("local_path").toString());
    }
    void destroyedOwnerDropsCallbacksAndPoolBoundsConcurrentWork() {
        struct Gate { QSemaphore entered,release; };auto gate=std::make_shared<Gate>();
        auto unblock=qScopeGuard([&]{gate->release.release(8);});
        AttachmentFiles::setIoObserver([gate]{gate->entered.release();gate->release.acquire();});
        auto *owner=new QObject;bool stale=false;int completed=0;
        AttachmentFiles::prepareAsync({{"owner.txt","text/plain","owner lifecycle fixture",{}}},owner,[&](const QJsonArray &,const QString &){stale=true;});
        QTRY_VERIFY(gate->entered.available()==1);delete owner;gate->entered.acquire();
        QStringList paths;auto cleanup=qScopeGuard([&]{for(const auto &path:paths)QFile::remove(path);});
        for(int i=0;i<5;++i)AttachmentFiles::prepareAsync({{"owner.txt","text/plain","owner lifecycle fixture",{}}},this,[&](const QJsonArray &files,const QString &error){QVERIFY(error.isEmpty());paths<<files.first().toObject().value("local_path").toString();++completed;});
        QTRY_VERIFY(gate->entered.available()==1);QCOMPARE(AttachmentFiles::pool().activeThreadCount(),2);
        int emptyCompleted=0;
        AttachmentFiles::prepareAsync({},this,[&](const QJsonArray &files,const QString &error){QVERIFY(files.isEmpty());QVERIFY(error.isEmpty());++emptyCompleted;});
        AttachmentFiles::readAsync({},this,[&](const QList<MessageAttachment> &files,const QString &error){QVERIFY(files.isEmpty());QVERIFY(error.isEmpty());++emptyCompleted;});
        bool heartbeat=false;QTimer::singleShot(0,this,[&]{heartbeat=true;});QTRY_VERIFY(heartbeat);QTRY_COMPARE(emptyCompleted,2);QCOMPARE(completed,0);
        gate->release.release(6);QTRY_COMPARE(completed,5);QVERIFY(!stale);
    }
    void identicalAttachmentKeepsAnOpenCachedCopy() {
        // Windows cannot replace a file that a viewer or thumbnail reader has open.
        const auto path=AttachmentFiles::store("shared.txt","shared fixture");QVERIFY(!path.isEmpty());
        auto cleanup=qScopeGuard([&]{QFile::remove(path);});
        {
            QFile reader(path);QVERIFY(reader.open(QIODevice::ReadOnly));
            QCOMPARE(AttachmentFiles::store("shared.txt","shared fixture"),path);
        }
        // A damaged copy of the same size is replaced rather than reused.
        {QFile damaged(path);QVERIFY(damaged.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(damaged.write("shared FIXTURE"),qint64(14));}
        QCOMPARE(AttachmentFiles::store("shared.txt","shared fixture"),path);
        QFile restored(path);QVERIFY(restored.open(QIODevice::ReadOnly));QCOMPARE(restored.readAll(),QByteArray("shared fixture"));
    }
    void cachedNamesStayPlainFiles() {
        // Qt and the Windows shell treat a .lnk name as a shortcut to another path,
        // and Windows drops trailing dots and spaces from names.
        // Windows 10 also reserves device names, and Linux and macOS limit names
        // to 255 bytes, from which QSaveFile may need 7 for a temporary suffix.
        const QRegularExpression device("^(con|prn|aux|nul|com.|lpt.) *(\\..*)?$",QRegularExpression::CaseInsensitiveOption);
        // U+20000 is a letter outside the BMP, so names keep it as a surrogate pair.
        const auto wide=QString::fromUcs4(U"\U00020000");QString wides;for(int i=0;i<70;++i)wides+=wide;
        const QStringList names{"folder.lnk","FOLDER.LNK","folder.lnk. ","..","notes.txt","CON.txt","nul","Com1 .log",
            QString::fromUtf8("LPT\xc2\xb9.txt"),QString(125,QChar(0x0436))+"a.lnk",QString(100,QChar(0x4e2d))+".png",
            wides+".txt",QString(190,'a')+".pdf",QString(179,'a')+wide+".txt",
            // Both caps cut just after a high surrogate here.
            QString(175,'a')+wide+"x.txt","a"+wides+".txt"};
        for(const auto &name:names) {
            const auto path=AttachmentFiles::store(name,"plain fixture "+name.toUtf8());QVERIFY2(!path.isEmpty(),qPrintable(name));
            auto cleanup=qScopeGuard([&]{QFile::remove(path);});
            const auto file=QFileInfo(path).fileName();
            QVERIFY2(!file.endsWith(".lnk",Qt::CaseInsensitive) && !file.endsWith('.') && !file.endsWith(' '),qPrintable(file));
            QVERIFY2(!device.match(file).hasMatch() && QFile::encodeName(file).size()<=248,qPrintable(file));
            QCOMPARE(QFile::decodeName(QFile::encodeName(file)),file);QVERIFY2(file.size()<=182,qPrintable(file));
            // Shortening keeps the extension that chooses the application.
            if(name.endsWith(".txt") || name.endsWith(".pdf") || name.endsWith(".png"))QCOMPARE(file.right(4),name.right(4));
            QCOMPARE(AttachmentFiles::cached({{"local_path",path}}),QFileInfo(path).canonicalFilePath());
        }
        QVERIFY(AttachmentFiles::store(QString(100,QChar(0x4e2d))+".png","long").endsWith(".png"));
        QVERIFY(AttachmentFiles::store("notes.txt","notes").endsWith("/notes.txt"));
        QFile::remove(AttachmentFiles::store("notes.txt","notes"));
        QFile::remove(AttachmentFiles::store(QString(100,QChar(0x4e2d))+".png","long"));
#ifdef Q_OS_WIN
        // A real shortcut to a folder stays an available attachment once cached.
        QTemporaryDir folder;QVERIFY(folder.isValid());const auto shortcut=folder.filePath("folder.lnk");
        QVERIFY(QFile::link(folder.path(),shortcut));
        QFile source(shortcut);QVERIFY(source.open(QIODevice::ReadOnly));const auto bytes=source.readAll();QVERIFY(!bytes.isEmpty());
        const auto path=AttachmentFiles::store("folder.lnk",bytes);QVERIFY(!path.isEmpty());
        auto cleanup=qScopeGuard([&]{QFile::remove(path);});
        QVERIFY(!AttachmentFiles::cached({{"local_path",path}}).isEmpty());
        QList<MessageAttachment> restored;QString error;bool done=false;
        AttachmentFiles::readAsync({QJsonObject{{"name","folder.lnk"},{"mime","application/octet-stream"},{"bytes",qint64(bytes.size())},{"local_path",path}}},this,
            [&](const QList<MessageAttachment> &result,const QString &failure){restored=result;error=failure;done=true;});
        QTRY_VERIFY(done);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(restored.size(),1);QCOMPARE(restored.first().data,bytes);
#endif
    }
    void cachedShortcutNeverWritesThroughToItsTarget() {
        // QFile::link makes a .lnk shortcut on Windows. Its cached copy is a plain
        // .lnk_ file; storing the bytes again never writes to the user's target.
        QTemporaryDir folder;QVERIFY(folder.isValid());const auto target=folder.filePath("document.txt");
        {QFile file(target);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.write("user document")>0);}
        const auto shortcut=folder.filePath("document.lnk");QVERIFY(QFile::link(target,shortcut));
        QFile source(shortcut);QVERIFY(source.open(QIODevice::ReadOnly));const auto bytes=source.readAll();QVERIFY(!bytes.isEmpty());
        const auto unchanged=[&]{QFile file(target);return file.open(QIODevice::ReadOnly) && file.readAll()=="user document";};
        const auto path=AttachmentFiles::store("document.lnk",bytes);QVERIFY(path.endsWith("/document.lnk_"));
        auto cleanup=qScopeGuard([&]{QFile::remove(path);});
        QCOMPARE(AttachmentFiles::store("document.lnk",bytes),path);QVERIFY(unchanged());
        {QFile damaged(path);QVERIFY(damaged.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(damaged.write(QByteArray(bytes.size(),'x')),qint64(bytes.size()));}
        QCOMPARE(AttachmentFiles::store("document.lnk",bytes),path);QVERIFY(unchanged());
        QFile restored(path);QVERIFY(restored.open(QIODevice::ReadOnly));QCOMPARE(restored.readAll(),bytes);
#ifndef Q_OS_WIN
        // A symbolic link planted in the cache is replaced as an entry, never followed.
        const auto planted=AttachmentFiles::store("planted.txt","planted bytes");QVERIFY(!planted.isEmpty());
        auto unplant=qScopeGuard([&]{QFile::remove(planted);});
        QVERIFY(QFile::remove(planted));QVERIFY(QFile::link(target,planted));
        QCOMPARE(AttachmentFiles::store("planted.txt","planted bytes"),planted);QVERIFY(unchanged());
        QVERIFY(!QFileInfo(planted).isSymLink());
#endif
    }
    void preparationCapturesCacheRootBeforeWorkerStarts() {
        struct Gate { QSemaphore entered,release; };auto gate=std::make_shared<Gate>();
        auto unblock=qScopeGuard([&]{gate->release.release();});
        const auto application=QCoreApplication::applicationName();auto restore=qScopeGuard([&]{QCoreApplication::setApplicationName(application);});
        const auto originalRoot=AttachmentFiles::root();
        AttachmentFiles::setIoObserver([gate]{gate->entered.release();gate->release.acquire();});
        QJsonArray files;bool done=false;
        AttachmentFiles::prepareAsync({{"captured.txt","text/plain","captured cache root",{}}},this,
            [&](const QJsonArray &result,const QString &error){QVERIFY(error.isEmpty());files=result;done=true;});
        QTRY_VERIFY(gate->entered.available()==1);QCoreApplication::setApplicationName(application+"-other");
        QVERIFY(AttachmentFiles::root()!=originalRoot);gate->release.release();QTRY_VERIFY(done);
        const auto path=files.first().toObject().value("local_path").toString();QVERIFY(path.startsWith(originalRoot+'/'));QFile::remove(path);
    }
    void invalidSnapshotsAndLimitViolationsCannotBecomeRetryPayloads() {
        const QByteArray data="integrity fixture";const auto path=AttachmentFiles::store("integrity.txt",data);QVERIFY(!path.isEmpty());
        const QJsonObject valid{{"name","integrity.txt"},{"mime","text/plain"},{"reference","[File #1]"},{"bytes",data.size()},{"local_path",path}};
        const auto rejected=[&](const QJsonArray &files) {
            bool done=false;QString error;QList<MessageAttachment> result;
            AttachmentFiles::readAsync(files,this,[&](const QList<MessageAttachment> &value,const QString &failure){result=value;error=failure;done=true;});
            QTRY_VERIFY(done);QVERIFY(result.isEmpty());QVERIFY(!error.isEmpty());
        };
        auto wrongSize=valid;wrongSize["bytes"]=data.size()+1;rejected({wrongSize});wrongSize.remove("bytes");rejected({wrongSize});wrongSize["bytes"]="17";rejected({wrongSize});rejected({42});
        QTemporaryFile outside;QVERIFY(outside.open());outside.write(data);outside.flush();auto escaped=valid;escaped["local_path"]=outside.fileName();rejected({escaped});
        auto linkPath=QFileInfo(path).dir().filePath("outside.txt");QVERIFY(QFile::link(outside.fileName(),linkPath));
        escaped["local_path"]=linkPath;rejected({escaped});QFile::remove(linkPath);
        QFile corrupted(path);QVERIFY(corrupted.open(QIODevice::WriteOnly));QCOMPARE(corrupted.write(QByteArray(data.size(),'x')),qint64(data.size()));corrupted.close();rejected({valid});
        QVERIFY(corrupted.open(QIODevice::WriteOnly));QVERIFY(corrupted.resize(HgsClient::MaximumAttachmentBytes+1));corrupted.close();rejected({valid});QFile::remove(path);
        QJsonArray tooMany;for(int i=0;i<HgsClient::MaximumAttachments+1;++i)tooMany.append(valid);rejected(tooMany);
        const auto rejectPreparation=[&](const QList<MessageAttachment> &attachments) {
            bool done=false;QString error;QJsonArray files;
            AttachmentFiles::prepareAsync(attachments,this,[&](const QJsonArray &result,const QString &failure){files=result;error=failure;done=true;});
            QTRY_VERIFY(done);QVERIFY(files.isEmpty());QVERIFY(!error.isEmpty());
        };
        rejectPreparation({{"empty.txt","text/plain",{}, {}}});
        rejectPreparation({{"large.txt","text/plain",QByteArray(HgsClient::MaximumAttachmentBytes+1,'x'), {}}});
        QList<MessageAttachment> tooManyAttachments;for(int i=0;i<HgsClient::MaximumAttachments+1;++i)tooManyAttachments.append({"example.txt","text/plain","x",{}});
        rejectPreparation(tooManyAttachments);
        const QByteArray maximum(HgsClient::MaximumAttachmentBytes,'m');
        rejectPreparation({{"one.txt","text/plain",maximum,{}},{"two.txt","text/plain",maximum,{}},{"three.txt","text/plain","x",{}}});
        const auto maxPath=AttachmentFiles::store("maximum.txt",maximum);QVERIFY(!maxPath.isEmpty());
        const auto smallPath=AttachmentFiles::store("small.txt","x");QVERIFY(!smallPath.isEmpty());
        rejected({QJsonObject{{"local_path",maxPath},{"bytes",maximum.size()}},QJsonObject{{"local_path",maxPath},{"bytes",maximum.size()}},QJsonObject{{"local_path",smallPath},{"bytes",1}}});
        QFile::remove(maxPath);QFile::remove(smallPath);
    }

};
QTEST_MAIN(TestAttachmentViewer)
#include "test_attachmentviewer.moc"
