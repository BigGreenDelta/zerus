#pragma once
#include "MessageComposer.h"
#include "LocalFile.h"
#include <QApplication>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QVBoxLayout>
#include <functional>

// Catch file drags over child viewports as well as the panel's empty margins.
// The editor keeps its native drop caret and insertion-position handling.
class SessionFileDrop : public QObject {
public:
    SessionFileDrop(QWidget *panel,std::function<MessageComposer *()> composer,
                    std::function<void(MessageComposer *)> reveal,
                    std::function<QString()> terminalTarget = {},
                    std::function<void(const QStringList &)> terminalDrop = {})
        :QObject(panel),m_panel(panel),m_composer(std::move(composer)),m_reveal(std::move(reveal)),
          m_terminalTarget(std::move(terminalTarget)),m_terminalDrop(std::move(terminalDrop)) {
        panel->setAcceptDrops(true);
        m_overlay=new QFrame(panel);m_overlay->setObjectName("sessionFileDropOverlay");
        m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *layout=new QVBoxLayout(m_overlay);m_hint=new QLabel;
        m_hint->setAlignment(Qt::AlignCenter);m_hint->setWordWrap(true);layout->addWidget(m_hint);
        m_overlay->hide();setTheme(true);qApp->installEventFilter(this);
    }
    void setTheme(bool dark) {
        m_overlay->setStyleSheet(QString("QFrame#sessionFileDropOverlay{background:%1;border:2px dashed %2;border-radius:10px;} QLabel{background:transparent;border:0;color:%2;font-size:16px;padding:24px;}")
            .arg(dark?"rgba(24,45,39,230)":"rgba(237,249,243,235)",dark?"#8bdfc0":"#167357"));
    }
protected:
    bool eventFilter(QObject *watched,QEvent *event) override {
        if(watched==m_panel&&event->type()==QEvent::Resize)m_overlay->setGeometry(m_panel->rect());
        const auto type=event->type();
        if(type!=QEvent::DragEnter&&type!=QEvent::DragMove&&type!=QEvent::DragLeave&&type!=QEvent::Drop)return false;
        auto *widget=qobject_cast<QWidget *>(watched);
        if(!widget||(widget!=m_panel&&!m_panel->isAncestorOf(widget)))return false;
        if(type==QEvent::DragLeave){m_overlay->hide();m_target.clear();m_terminalKey.clear();return false;}
        auto *drop=static_cast<QDropEvent *>(event);const auto *mime=drop->mimeData();
        QStringList paths;
        for(const auto &url:mime->urls()) {
            if(!url.isLocalFile()||!LocalFile::readable(QFileInfo(url.toLocalFile()))){paths.clear();break;}
            paths<<url.toLocalFile();
        }
        const auto terminalKey=m_terminalTarget?m_terminalTarget():QString();
        if(!terminalKey.isEmpty() || !m_terminalKey.isEmpty()) {
            if(type==QEvent::DragEnter)m_terminalKey=terminalKey;
            if(paths.isEmpty() || terminalKey.isEmpty() || terminalKey!=m_terminalKey) {
                m_overlay->hide();drop->ignore();return true;
            }
            if(type==QEvent::Drop) {m_overlay->hide();m_terminalKey.clear();m_terminalDrop(paths);}
            else {m_hint->setText(tr("Drop files to insert their paths in Terminal"));m_overlay->setGeometry(m_panel->rect());m_overlay->show();m_overlay->raise();}
            drop->setDropAction(Qt::CopyAction);drop->accept();return true;
        }
        auto *composer=m_composer();
        if(paths.isEmpty()||!composer||!composer->canAttachFiles()){m_overlay->hide();return false;}
        if(type==QEvent::DragEnter){m_target=composer;m_key=composer->sessionKey();}
        if(m_target!=composer||m_key!=composer->sessionKey()){m_overlay->hide();drop->ignore();return true;}
        if(type==QEvent::Drop){
            m_overlay->hide();
            if(widget==composer->editor()->viewport())return false;
            m_reveal(composer);composer->attachDroppedFiles(paths);m_target.clear();
        } else {m_hint->setText(tr("Drop files to attach to this message"));m_overlay->setGeometry(m_panel->rect());m_overlay->show();m_overlay->raise();}
        if(widget==composer->editor()->viewport())return false;
        drop->setDropAction(Qt::CopyAction);drop->accept();return true;
    }
private:
    QWidget *m_panel;QFrame *m_overlay;QLabel *m_hint;QPointer<MessageComposer> m_target;QString m_key,m_terminalKey;
    std::function<MessageComposer *()> m_composer;
    std::function<void(MessageComposer *)> m_reveal;
    std::function<QString()> m_terminalTarget;
    std::function<void(const QStringList &)> m_terminalDrop;
};
