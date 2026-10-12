#include <QtTest>
#include <QTemporaryDir>
#include "AttachmentViewer.h"

class TestAttachmentViewer : public QObject {
    Q_OBJECT
    QList<QUrl> m_opened;
private slots:
    void capture(const QUrl &url){m_opened.append(url);}
    void initTestCase(){QStandardPaths::setTestModeEnabled(true);}
    void init(){m_opened.clear();QDesktopServices::setUrlHandler("file",this,"capture");}
    void cleanup(){QDesktopServices::unsetUrlHandler("file");}
    void cachedImageHasThumbnailAndOpensInDefaultAppWithoutDialog() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);
        QImage image(320,200,QImage::Format_RGB32);image.fill(Qt::blue);
        QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
        const auto path=AttachmentFiles::store("../../screen.png",bytes);QVERIFY(!path.isEmpty());QVERIFY(path.endsWith("/screen.png"));
        QSignalSpy errors(&client,&HgsClient::attachmentFailed);
        const auto thumbnail=AttachmentLoader::thumbnail(path);QVERIFY(!thumbnail.isNull());QVERIFY(thumbnail.width()<=192);QVERIFY(thumbnail.height()<=128);
        bool done=false;
        loader.open("mac","session","conversation",{}, {},{{"name","screen.png"},{"mime","image/png"},{"local_path",path}},
            [&](const QString &opened,const QString &error){QCOMPARE(opened,path);QVERIFY(error.isEmpty());done=true;});
        QTRY_VERIFY(done);QCOMPARE(m_opened,QList<QUrl>{QUrl::fromLocalFile(path)});QCOMPARE(errors.size(),0);
        for(auto *widget:QApplication::topLevelWidgets())QVERIFY(widget->objectName()!="attachmentPreviewDialog");
        const auto html=AttachmentFiles::store("notes.html","<script>never execute</script>");
        QVERIFY(AttachmentLoader::thumbnail(html).isNull());QFile::remove(path);QFile::remove(html);
    }
    void longNamesKeepTheirLastExtension() {
        const QByteArray bytes("fixture bytes");
        // Cutting the end would expose an inner extension: the last one is kept.
        const QFileInfo command(AttachmentFiles::store(QString(176,'A')+".jarX.command",bytes));
        QVERIFY2(command.fileName().endsWith(".command") && command.fileName().size()==180,qPrintable(command.fileName()));
        const QFileInfo png(AttachmentFiles::store(QString(300,'B')+".png",bytes));
        QVERIFY(png.fileName().endsWith(".png"));QCOMPARE(png.fileName().size(),180);
        // Without a short last extension, the shortened name has no dot.
        const QFileInfo none(AttachmentFiles::store(QString(172,'C')+".command"+QString(50,'D'),bytes));
        QVERIFY2(!none.fileName().contains('.') && none.fileName().size()==180,qPrintable(none.fileName()));
        QCOMPARE(QFileInfo(AttachmentFiles::store("short.png",bytes)).fileName(),QString("short.png"));
        for(const auto &info:{command,png,none})QFile::remove(info.filePath());
        QFile::remove(QFileInfo(command.dir(),"short.png").filePath());
    }
    void missingFileAndUnrelatedResponseDoNotOpenAnotherAttachment() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);bool done=false;
        loader.open("mac","session","conversation",{}, {},{{"name","missing.png"},{"request_id","request-one"},{"index",0}},
            [&](const QString &path,const QString &error){QVERIFY(path.isEmpty());QVERIFY(!error.isEmpty());done=true;});
        client.attachmentReady(77,{{"name","other.txt"}},"unrelated");QVERIFY(m_opened.isEmpty());
        QTRY_VERIFY(done);QVERIFY(m_opened.isEmpty());
        QTemporaryFile file;QVERIFY(file.open());QVERIFY(AttachmentFiles::cached({{"local_path",file.fileName()}}).isEmpty());
    }
    void thumbnailAndOpenShareRequestButConversationsDoNot() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);int done=0;
        const QJsonObject file{{"name","image.png"},{"mime","image/png"},{"request_id","request-one"},{"index",0}};
        QString path;
        loader.fetch("mac","session","one",{}, {},file,[&](const QString &p,const QString &error){QVERIFY(error.isEmpty());path=p;++done;});
        loader.open("mac","session","one",{}, {},file,[&](const QString &p,const QString &error){QCOMPARE(p,path);QVERIFY(error.isEmpty());++done;});
        loader.fetch("mac","session","two",{}, {},file,[&](const QString &p,const QString &error){QVERIFY(p.isEmpty());QVERIFY(!error.isEmpty());++done;});
        client.attachmentReady(1,{{"name","image.png"}},"fixture bytes");
        QCOMPARE(done,2);QCOMPARE(m_opened.size(),1);
        client.attachmentFailed(2,"No such file in the other conversation");QCOMPARE(done,3);
        loader.fetch("mac","session","one",{}, {},file,[&](const QString &p,const QString &error){QCOMPARE(p,path);QVERIFY(error.isEmpty());++done;});
        QTRY_COMPARE(done,4);QCOMPARE(m_opened.size(),1);QFile::remove(path);
    }
};
QTEST_MAIN(TestAttachmentViewer)
#include "test_attachmentviewer.moc"
