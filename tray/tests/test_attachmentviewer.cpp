#include <QtTest>
#include <QTemporaryDir>
#include <memory>
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
    // Windows drops trailing dots and spaces from a name: "A….jar." would open as a jar.
    void trailingDotsAndSpacesAreNoExtension() {
        const auto dotted=AttachmentFiles::fitName(QString(175,'A')+".jar"+QString(20,'X')+".");
        QVERIFY2(!dotted.contains('.') && dotted.size()==180,qPrintable(dotted));
        const auto spaced=AttachmentFiles::fitName(QString(174,'A')+".jar"+QString(20,'X')+". ");
        QVERIFY2(!spaced.contains('.') && spaced.size()==180,qPrintable(spaced));
        const auto letter=AttachmentFiles::fitName(QString(200,'A')+".c");
        QVERIFY2(letter.endsWith(QString(178,'A')+".c") && letter.size()==180,qPrintable(letter));
    }
    // An agent names the images it attaches to a task, and a review or rework
    // passes them on to the Activity card: a picture opens from a copy named for
    // its real format, and bytes that are no picture never reach the desktop.
    void agentNamedImagesOpenOnlyAsPictures() {
        HgsClient client("/nonexistent/hgs");AttachmentLoader loader(&client);
        const auto encode=[](const char *format,const QColor &color) {
            QImage image(16,8,QImage::Format_RGB32);image.fill(color);
            QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,format);return bytes;
        };
        struct Opened { bool done=false; QString path,error; };
        const auto open=[&](const QJsonObject &file,const std::function<void()> &deliver={}) {
            auto result=std::make_shared<Opened>();
            loader.open("mac","session","conversation",{},{},file,[result](const QString &path,const QString &error) {
                result->done=true;result->path=path;result->error=error;
            });
            if(deliver)deliver();
            if(!QTest::qWaitFor([result]{return result->done;},5000))result->error=QStringLiteral("timed out");
            return *result;
        };
        const auto png=encode("PNG",Qt::red),jpeg=encode("JPEG",Qt::green);QVERIFY(!png.isEmpty() && !jpeg.isEmpty());
        QStringList made;

        const auto bat=AttachmentFiles::store("x.bat",png);QVERIFY(bat.endsWith("/x.bat"));made<<bat;
        auto opened=open({{"name","x.bat"},{"mime","image/png"},{"local_path",bat}});
        QVERIFY2(opened.done && opened.error.isEmpty(),qPrintable(opened.error));
        QVERIFY2(opened.path.endsWith("/x.png"),qPrintable(opened.path));made<<opened.path;
        QCOMPARE(m_opened,QList<QUrl>{QUrl::fromLocalFile(opened.path)});
        QFile copy(opened.path);QVERIFY(copy.open(QIODevice::ReadOnly));QCOMPARE(copy.readAll(),png);

        const auto terminal=AttachmentFiles::store("shot.terminal",jpeg);made<<terminal;m_opened.clear();
        opened=open({{"name","shot.terminal"},{"mime","image/jpeg"},{"local_path",terminal}});
        QVERIFY2(opened.path.endsWith("/shot.jpg"),qPrintable(opened.path));made<<opened.path;
        QCOMPARE(m_opened,QList<QUrl>{QUrl::fromLocalFile(opened.path)});

        // Fetched over the CLI under the agent's name, with any case of the type.
        m_opened.clear();
        opened=open({{"name","x.jar"},{"mime","IMAGE/PNG"},{"request_id","request-one"},{"index",0}},
                    [&]{client.attachmentReady(1,{{"name","x.jar"}},png);});
        QVERIFY2(opened.path.endsWith("/x.png") && opened.error.isEmpty(),qPrintable(opened.path+opened.error));
        QCOMPARE(m_opened,QList<QUrl>{QUrl::fromLocalFile(opened.path)});made<<QFileInfo(opened.path).dir().filePath("x.jar");

        // A script that claims to be a picture, or an SVG document, is not opened.
        m_opened.clear();
        const auto script=AttachmentFiles::store("x.bat","@echo off\r\ncalc.exe\r\n");made<<script;
        opened=open({{"name","x.bat"},{"mime","image/png"},{"local_path",script}});
        QVERIFY(opened.done);QVERIFY(opened.path.isEmpty());
        QVERIFY2(opened.error.contains("not a picture"),qPrintable(opened.error));
        const auto svg=AttachmentFiles::store("x.svg","<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'><rect width='8' height='8'/></svg>");made<<svg;
        opened=open({{"name","x.svg"},{"mime","image/svg+xml"},{"local_path",svg}});
        QVERIFY(opened.done);QVERIFY(opened.path.isEmpty());QVERIFY(!opened.error.isEmpty());
        QVERIFY(m_opened.isEmpty());
        QVERIFY(!QFileInfo::exists(QFileInfo(script).dir().filePath("x.png")));

        // Any other attachment opens as it was saved.
        const auto notes=AttachmentFiles::store("notes.txt","plain text");made<<notes;
        opened=open({{"name","notes.txt"},{"mime","text/plain"},{"local_path",notes}});
        QCOMPARE(opened.path,notes);QCOMPARE(m_opened,QList<QUrl>{QUrl::fromLocalFile(notes)});
        for(const auto &path:made)QFile::remove(path);
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
        QImage image(4,4,QImage::Format_RGB32);image.fill(Qt::blue);
        QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(image.save(&buffer,"PNG"));
        client.attachmentReady(1,{{"name","image.png"}},png);
        QCOMPARE(done,2);QCOMPARE(m_opened.size(),1);
        client.attachmentFailed(2,"No such file in the other conversation");QCOMPARE(done,3);
        loader.fetch("mac","session","one",{}, {},file,[&](const QString &p,const QString &error){QCOMPARE(p,path);QVERIFY(error.isEmpty());++done;});
        QTRY_COMPARE(done,4);QCOMPARE(m_opened.size(),1);QFile::remove(path);
    }
};
QTEST_MAIN(TestAttachmentViewer)
#include "test_attachmentviewer.moc"
