#include "RoundVideo.h"
#include "SpeechRecorder.h"
#include <QAudioBuffer>
#include <climits>
#include <QAudioDevice>
#include <QAudioDecoder>
#include <QAudioInput>
#include <QBuffer>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QDeadlineTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QMediaPlayer>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRunnable>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#if QT_CONFIG(permissions)
#include <QPermissions>
#endif

namespace { QList<QPointer<QMediaPlayer>> messagePlayers; }
void pauseMessagePlayback(QMediaPlayer *except){
    messagePlayers.removeIf([](const auto &player){return player.isNull();});
    for(const auto &player:messagePlayers)if(player && player!=except)player->pause();
}
void registerMessagePlayer(QMediaPlayer *player){
    messagePlayers.removeIf([](const auto &item){return item.isNull();});messagePlayers.append(player);
    QObject::connect(player,&QMediaPlayer::playbackStateChanged,player,[player](QMediaPlayer::PlaybackState state){
        if(state==QMediaPlayer::PlayingState)pauseMessagePlayback(player);
    });
}

RoundVideoView::RoundVideoView(QWidget *parent):QWidget(parent),m_sink(new QVideoSink(this)) {
    setFixedSize(200,200);setAccessibleName(tr("Video message"));m_pool.setMaxThreadCount(1);
    connect(m_sink,&QVideoSink::videoFrameChanged,this,[this](const QVideoFrame &frame){
        if(m_busy || !isVisible() || !frame.isValid())return;m_busy=true;
        const QPointer<RoundVideoView> guard(this);
        m_pool.start(QRunnable::create([guard,frame]{
            const auto image=frame.toImage().scaled(480,480,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            if(guard)QMetaObject::invokeMethod(guard,[guard,image]{if(guard){guard->m_busy=false;guard->setImage(image);}},Qt::QueuedConnection);
        }));
    });
}
RoundVideoView::~RoundVideoView(){m_pool.waitForDone();}
void RoundVideoView::paintEvent(QPaintEvent *) {
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);QPainterPath clip;clip.addEllipse(rect());p.setClipPath(clip);p.fillRect(rect(),QColor("#26343c"));
    if(m_image.isNull())return;
    const auto side=qMin(m_image.width(),m_image.height());const QRect crop((m_image.width()-side)/2,(m_image.height()-side)/2,side,side);
    if(m_mirrored){p.translate(width(),0);p.scale(-1,1);}p.drawImage(rect(),m_image,crop);
}
RoundVideoRecorder::RoundVideoRecorder(RoundVideoView *view,QObject *parent):QObject(parent),m_view(view){}
RoundVideoRecorder::~RoundVideoRecorder(){cancel();}
void RoundVideoRecorder::releaseDevices(){
    if(m_timer){delete m_timer;m_timer=nullptr;}
    if(m_recorder){m_recorder->disconnect(this);m_recorder->stop();}
    if(m_camera)m_camera->stop();
    delete m_session;m_session=nullptr;delete m_recorder;m_recorder=nullptr;delete m_camera;m_camera=nullptr;delete m_audio;m_audio=nullptr;
    m_started=false;
}
void RoundVideoRecorder::cancel(){++m_generation;if(m_cancelled)m_cancelled->store(true);releaseDevices();m_directory.reset();m_finishing=false;}
void RoundVideoRecorder::start(const QByteArray &microphone){
    cancel();const auto generation=m_generation;
#if QT_CONFIG(permissions)
    // System consent ends the gesture. A grant requires a fresh press, never a late capture.
    const auto check=[this](auto permission){
        const auto status=qApp->checkPermission(permission);
        if(status==Qt::PermissionStatus::Undetermined){qApp->requestPermission(permission,this,[](const QPermission &){});emit failed(tr("Allow camera and microphone access, then hold Video again."));return false;}
        if(status!=Qt::PermissionStatus::Granted){emit failed(tr("Allow camera and microphone access in system settings, then hold Video again."));return false;}return true;
    };
    if(!check(QCameraPermission{}) || !check(QMicrophonePermission{}))return;
#endif
    auto camera=QMediaDevices::defaultVideoInput();
    for(const auto &candidate:QMediaDevices::videoInputs())if(candidate.position()==QCameraDevice::FrontFace){camera=candidate;break;}
    auto audio=QMediaDevices::defaultAudioInput();
    if(!microphone.isEmpty()){audio={};for(const auto &candidate:QMediaDevices::audioInputs())if(candidate.id()==microphone)audio=candidate;}
    if(camera.isNull() || audio.isNull()){emit failed(tr("A camera and microphone are required to record video."));return;}
    m_directory=std::make_shared<QTemporaryDir>();
    if(!m_directory->isValid()){emit failed(tr("Private recording storage is unavailable."));return;}
    m_cancelled=std::make_shared<std::atomic_bool>(false);m_session=new QMediaCaptureSession(this);m_camera=new QCamera(camera,this);m_audio=new QAudioInput(audio,this);m_recorder=new QMediaRecorder(this);
    QCameraFormat cameraFormat;int distance=INT_MAX;
    for(const auto &candidate:camera.videoFormats()){
        const auto size=candidate.resolution();if(size.width()<16 || size.height()<16 || size.width()>1920 || size.height()>1920 || candidate.maxFrameRate()<15)continue;
        const auto score=qAbs(size.width()*size.height()-640*480);
        if(score<distance){distance=score;cameraFormat=candidate;}
    }
    if(cameraFormat.isNull()){cancel();emit failed(tr("The camera has no supported recording format (up to 1920 pixels)."));return;}
    m_camera->setCameraFormat(cameraFormat);
    m_session->setCamera(m_camera);m_session->setAudioInput(m_audio);m_session->setRecorder(m_recorder);m_session->setVideoSink(m_view->sink());m_view->setMirrored(camera.position()==QCameraDevice::FrontFace);
    QMediaFormat format(QMediaFormat::MPEG4);format.setVideoCodec(QMediaFormat::VideoCodec::H264);format.setAudioCodec(QMediaFormat::AudioCodec::AAC);m_recorder->setMediaFormat(format);
    m_recorder->setEncodingMode(QMediaRecorder::AverageBitRateEncoding);m_recorder->setVideoResolution(cameraFormat.resolution());m_recorder->setVideoFrameRate(24);m_recorder->setVideoBitRate(700000);m_recorder->setAudioBitRate(64000);m_recorder->setAudioChannelCount(1);
    m_recorder->setOutputLocation(QUrl::fromLocalFile(m_directory->filePath("video.mp4")));
    connect(m_camera,&QCamera::errorOccurred,this,[this,generation](QCamera::Error,const QString &error){if(generation!=m_generation)return;cancel();emit failed(error);},Qt::QueuedConnection);
    connect(m_recorder,&QMediaRecorder::errorOccurred,this,[this,generation](QMediaRecorder::Error,const QString &error){if(generation!=m_generation)return;cancel();emit failed(error);},Qt::QueuedConnection);
    connect(m_recorder,&QMediaRecorder::recorderStateChanged,this,[this,generation](QMediaRecorder::RecorderState state){
        if(generation!=m_generation)return;
        if(state==QMediaRecorder::RecordingState && !m_started){m_started=true;emit started();}
        else if(state==QMediaRecorder::StoppedState && m_started && !m_finishing){m_finishing=true;QTimer::singleShot(0,this,[this,generation]{if(generation==m_generation)finish();});}
    });
    m_timer=new QTimer(this);m_timer->setInterval(200);
    connect(m_timer,&QTimer::timeout,this,[this,deadline=QDeadlineTimer(15000)]{if(!m_recorder)return;
        if(!m_started && deadline.hasExpired()){cancel();emit failed(tr("The camera did not start. Try recording again."));return;}emit elapsed(int(m_recorder->duration()/1000));
        if(m_recorder->duration()>=59000 || QFileInfo(m_directory->filePath("video.mp4")).size()>=MaximumBytes-131072)stop();});
    m_timer->start();m_camera->start();m_recorder->record();
}
void RoundVideoRecorder::stop(){if(m_finishing)return;if(m_recorder && m_started)m_recorder->stop();else cancel();}
void RoundVideoRecorder::finish(){
    if(!m_recorder || !m_directory)return;m_finishing=true;
    const int duration=int(m_recorder->duration());const auto size=m_recorder->videoResolution();const auto poster=m_view->image();const auto directory=m_directory;const auto cancelled=m_cancelled;
    releaseDevices();m_directory.reset();emit preparing();const QPointer<RoundVideoRecorder> guard(this);
    auto *worker=QThread::create([guard,directory,cancelled,duration,size,poster]{
        QString error;QByteArray pcm,mp4,thumbnail;
        const auto path=directory->filePath("video.mp4");QFile file(path);
        if(duration<100 || duration>60000 || size.width()<16 || size.width()>1920 || size.height()<16 || size.height()>1920 || !file.open(QIODevice::ReadOnly) || file.size()>MaximumBytes)error=tr("The video recording is invalid or exceeds 10 MiB.");
        else {
            pcm=extractAudio(path,cancelled,error);
            if(pcm.size()<3200 || pcm.size()>1920000 || qAbs(pcm.size()/32-duration)>1000)error=tr("The video audio could not be prepared.");
            if(error.isEmpty())mp4=file.read(MaximumBytes+1);
            if(!poster.isNull())for(const auto quality:{70,50,30}){thumbnail.clear();QBuffer buffer(&thumbnail);buffer.open(QIODevice::WriteOnly);poster.scaled(160,160,Qt::KeepAspectRatio,Qt::SmoothTransformation).save(&buffer,"JPEG",quality);if(thumbnail.size()<=8192)break;}
            if(thumbnail.size()>8192)thumbnail.clear();
        }
        if(guard && !cancelled->load())QMetaObject::invokeMethod(guard,[guard,cancelled,pcm,mp4,error,duration,size,thumbnail]{
            if(!guard || cancelled->load())return;guard->m_finishing=false;
            if(!error.isEmpty()){emit guard->failed(error);return;}
            QJsonObject metadata{{"format","mp4"},{"duration_ms",duration},{"width",size.width()},{"height",size.height()}};
            if(!thumbnail.isEmpty())metadata["poster_base64"]=QString::fromLatin1(thumbnail.toBase64());
            emit guard->completed(pcm,mp4,metadata);
        },Qt::QueuedConnection);
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);worker->start();
}

QByteArray RoundVideoRecorder::extractAudio(const QString &path,const std::shared_ptr<std::atomic_bool> &cancelled,QString &error){
    QByteArray pcm;
    QAudioDecoder decoder;QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);deadline.setInterval(30000);
    std::unique_ptr<SpeechPcmConverter> converter;
    QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{error=tr("Preparing video audio timed out.");loop.quit();});
    QObject::connect(&decoder,QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error),&loop,[&]{error=decoder.errorString();loop.quit();});
    QObject::connect(&decoder,&QAudioDecoder::finished,&loop,&QEventLoop::quit);
    QObject::connect(&decoder,&QAudioDecoder::bufferReady,&loop,[&]{
        if(cancelled->load()){loop.quit();return;}const auto buffer=decoder.read();if(!buffer.isValid())return;
        if(!converter)converter=std::make_unique<SpeechPcmConverter>(buffer.format());
        if(!converter->valid() || converter->full()){error=tr("Unsupported video audio.");loop.quit();return;}
        converter->append(QByteArray(buffer.constData<char>(),buffer.byteCount()));
    });
    decoder.setSource(QUrl::fromLocalFile(path));deadline.start();decoder.start();loop.exec();decoder.stop();
    if(converter)pcm=converter->finish();
    return pcm;
}
