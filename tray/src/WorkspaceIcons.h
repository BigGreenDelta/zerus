#pragma once
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include "WorkspaceFocus.h"

// One small stroke family, drawn at native DPR instead of platform-dependent glyphs.
inline QIcon workspaceIcon(const QString &name, const QColor &color)
{
    // List rebuilds, details, polls and card delegates on every frame request the
    // same few icons, and building one renders six pixmaps. Paint each name and
    // color once. Never destroyed: pixmaps must not outlive QGuiApplication at exit.
    static auto *cache = new QHash<std::pair<QString, QRgb>, QIcon>;
    const std::pair<QString, QRgb> key{name, color.rgba()};
    if (const auto found = cache->constFind(key); found != cache->cend()) return *found;
    QIcon icon;
    const QIcon symbol = name == "github" ? QIcon(":/hgs/icons/github-mark.svg")
                         : name == "zerus" ? QIcon(":/hgs/icons/hgs-zerus-symbolic.svg") : QIcon();
    for (int size : {16, 20, 24, 32, 40, 48}) {
        QPixmap pm(size, size); pm.fill(Qt::transparent);
        QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.scale(size / 24., size / 24.);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        if (name == "github" || name == "zerus") {
            p.resetTransform(); p.drawPixmap(0, 0, symbol.pixmap(size, size));
            p.setCompositionMode(QPainter::CompositionMode_SourceIn); p.fillRect(pm.rect(), color);
        } else if (name == "codex") {
            QPainterPath petal;petal.moveTo(12,5);petal.cubicTo(20,2,23,12,16,15);petal.lineTo(10,12);petal.lineTo(10,7);
            for(int i=0;i<6;++i){p.save();p.translate(12,12);p.rotate(i*60);p.translate(-12,-12);p.drawPath(petal);p.restore();}
        } else if (name == "claude") {
            for(int i=0;i<8;++i){p.save();p.translate(12,12);p.rotate(i*45);p.drawLine(QPointF(0,3),QPointF(0,9));p.restore();}
        } else if (name == "kimi") {
            p.drawLine(6,4,6,20);p.drawLine(6,12,18,4);p.drawLine(6,12,18,20);
        } else if (name == "opencode") {
            p.drawLine(8,5,3,12);p.drawLine(3,12,8,19);
            p.drawLine(16,5,21,12);p.drawLine(21,12,16,19);p.drawLine(14,5,10,19);
        } else if (name == "dsh") {
            QPainterPath whale;whale.moveTo(3,10);whale.cubicTo(5,5,15,4,18,10);whale.lineTo(23,7);whale.lineTo(22,13);
            whale.lineTo(18,13);whale.cubicTo(15,22,3,19,3,10);p.drawPath(whale);p.drawPoint(8,10);
        } else if (name == "sessions" || name == "all") {
            p.drawRoundedRect(QRectF(4, 4, 16, 7), 2, 2); p.drawRoundedRect(QRectF(4, 14, 16, 6), 2, 2);
            p.drawPoint(QPointF(8, 7.5)); p.drawLine(QPointF(12, 7.5), QPointF(16, 7.5));
        } else if (name == "calendar") {
            p.drawRoundedRect(QRectF(4, 5, 16, 16), 2, 2); p.drawLine(4, 10, 20, 10);
            p.drawLine(8, 3, 8, 7); p.drawLine(16, 3, 16, 7);
            p.drawPoint(QPointF(8, 14)); p.drawPoint(QPointF(12, 14)); p.drawPoint(QPointF(16, 14));
        } else if (name == "chevron-left" || name == "chevron-right") {
            const bool right = name == "chevron-right";
            QPainterPath path; path.moveTo(right ? 9 : 15, 6); path.lineTo(right ? 15 : 9, 12); path.lineTo(right ? 9 : 15, 18); p.drawPath(path);
        } else if (name == "machines") {
            p.drawRoundedRect(QRectF(3, 4, 18, 12), 2, 2); p.drawLine(12, 16, 12, 20); p.drawLine(8, 20, 16, 20);
        } else if (name == "terminal") {
            p.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
            p.drawLine(7, 9, 10, 12); p.drawLine(10, 12, 7, 15); p.drawLine(13, 15, 17, 15);
        } else if (name == "folder" || name == "projects" || name == "group") {
            QPainterPath path; path.moveTo(3, 8); path.lineTo(3, 5); path.lineTo(10, 5); path.lineTo(12, 8); path.lineTo(21, 8);
            path.lineTo(21, 19); path.lineTo(3, 19); path.closeSubpath(); p.drawPath(path);
            if (name == "group") { p.drawLine(9, 13, 15, 13); p.drawLine(12, 10, 12, 16); }
        } else if (name == "folder-move") {
            QPainterPath folder; folder.moveTo(3, 8); folder.lineTo(3, 5); folder.lineTo(10, 5);
            folder.lineTo(12, 8); folder.lineTo(21, 8); folder.lineTo(21, 20); folder.lineTo(3, 20); folder.lineTo(3, 17);
            p.drawPath(folder); p.drawLine(2, 13, 13, 13); p.drawLine(9, 9, 13, 13); p.drawLine(9, 17, 13, 13);
        } else if (name == "pin" || name == "unpinned" || name == "pinned") {
            if (name == "unpinned") { p.translate(12, 12); p.rotate(-35); p.translate(-12, -12); }
            if (name == "pinned") p.setBrush(color);
            QPainterPath path; path.moveTo(8, 3); path.lineTo(16, 3); path.lineTo(15, 9); path.lineTo(19, 13);
            path.lineTo(5, 13); path.lineTo(9, 9); path.closeSubpath(); p.drawPath(path); p.drawLine(12, 13, 12, 21);
            if (name == "pinned") p.drawLine(8, 19, 16, 19);
        } else if (name == "git-branch") {
            p.drawLine(QPointF(6, 7.5), QPointF(6, 16.5));
            QPainterPath branch; branch.moveTo(18, 7.5); branch.lineTo(18, 10);
            branch.cubicTo(18, 15, 6, 11, 6, 16.5); p.drawPath(branch);
            p.drawEllipse(QPointF(6, 5), 2.5, 2.5);
            p.drawEllipse(QPointF(6, 19), 2.5, 2.5);
            p.drawEllipse(QPointF(18, 5), 2.5, 2.5);
        } else if (name == "git-diff") {
            QPainterPath page; page.moveTo(13, 3); page.lineTo(5, 3); page.lineTo(5, 21);
            page.lineTo(19, 21); page.lineTo(19, 9); page.closeSubpath(); p.drawPath(page);
            p.drawLine(13, 3, 13, 9); p.drawLine(13, 9, 19, 9);
            p.drawLine(8, 11, 12, 11); p.drawLine(10, 9, 10, 13); p.drawLine(8, 17, 14, 17);
        } else if (name == "git-push" || name == "git-pull") {
            QPainterPath tray; tray.moveTo(4, 15); tray.lineTo(4, 19);
            tray.quadTo(4, 21, 6, 21); tray.lineTo(18, 21); tray.quadTo(20, 21, 20, 19); tray.lineTo(20, 15); p.drawPath(tray);
            const bool push = name == "git-push";
            p.drawLine(12, 3, 12, 16);
            QPainterPath arrow; arrow.moveTo(7, push ? 8 : 11); arrow.lineTo(12, push ? 3 : 16); arrow.lineTo(17, push ? 8 : 11); p.drawPath(arrow);
        } else if (name == "git-synced" || name == "git-unknown") {
            QPainterPath cloud; cloud.moveTo(6, 19); cloud.cubicTo(0, 19, 0, 10, 6, 10);
            cloud.cubicTo(5, 2, 17, 1, 19, 9); cloud.cubicTo(25, 9, 25, 19, 19, 19);
            cloud.closeSubpath(); p.drawPath(cloud);
            if (name == "git-synced") {
                QPainterPath check; check.moveTo(8, 13); check.lineTo(11, 16); check.lineTo(16, 11); p.drawPath(check);
            } else {
                QPainterPath question; question.moveTo(10, 10); question.cubicTo(10, 7, 15, 7, 15, 10);
                question.cubicTo(15, 12, 12, 11, 12, 14); p.drawPath(question); p.drawPoint(QPointF(12, 17));
            }
        } else if (name == "accounts") {
            p.drawEllipse(QPointF(10, 7), 3.5, 3.5);
            QPainterPath path; path.moveTo(3, 20); path.cubicTo(3, 11, 17, 11, 17, 20); p.drawPath(path);
            p.drawEllipse(QPointF(18, 12), 2, 2); p.drawLine(18,14,18,20); p.drawLine(18,18,21,18);
        } else if (name == "settings") {
            p.drawEllipse(QPointF(12, 12), 6, 6); p.drawEllipse(QPointF(12, 12), 2.5, 2.5);
            for (int a = 0; a < 360; a += 45) { p.save(); p.translate(12, 12); p.rotate(a); p.drawLine(0, 7, 0, 9); p.restore(); }
        } else if (name == "context-warning") {
            p.drawEllipse(QRectF(3, 3, 18, 18)); p.drawLine(12, 7, 12, 12); p.drawPoint(QPointF(12, 16));
        } else if (name == "question") {
            // An optional question: the circle of context-warning with git-unknown's mark.
            p.drawEllipse(QRectF(3, 3, 18, 18));
            QPainterPath question; question.moveTo(10, 10); question.cubicTo(10, 7, 15, 7, 15, 10);
            question.cubicTo(15, 12, 12, 11, 12, 14); p.drawPath(question); p.drawPoint(QPointF(12, 17));
        } else if (name == "attention" || name == "mute") {
            QPainterPath path; path.moveTo(6, 10); path.cubicTo(6, 3, 18, 3, 18, 10); path.lineTo(18, 15); path.lineTo(20, 18); path.lineTo(4, 18); path.lineTo(6, 15); path.closeSubpath();
            p.drawPath(path); p.drawArc(QRectF(10, 18, 4, 4), 180*16, 180*16);
            if (name == "mute") p.drawLine(3, 3, 21, 21);
        } else if (name == "working" || name == "active") {
            QPainterPath path; path.moveTo(13, 3); path.lineTo(5, 13); path.lineTo(11, 13); path.lineTo(10, 21); path.lineTo(19, 10); path.lineTo(13, 10); path.closeSubpath(); p.drawPath(path);
        } else if (name == "paused" || name == "pause") {
            p.drawLine(8, 5, 8, 19); p.drawLine(16, 5, 16, 19);
        } else if (name == "play") {
            QPainterPath path; path.moveTo(8, 5); path.lineTo(19, 12); path.lineTo(8, 19); path.closeSubpath(); p.drawPath(path);
        } else if (name == "archived") {
            p.drawRect(QRectF(3, 4, 18, 4)); p.drawRect(QRectF(5, 8, 14, 12)); p.drawLine(10, 12, 14, 12);
        } else if (name == "trash") {
            p.drawLine(4, 6, 20, 6); p.drawLine(9, 3, 15, 3);
            p.drawLine(6, 6, 7, 21); p.drawLine(7, 21, 17, 21); p.drawLine(17, 21, 18, 6);
            p.drawLine(10, 10, 10, 17); p.drawLine(14, 10, 14, 17);
        } else if (name == "stop") {
            p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
        } else if (name == "read-all") {
            QPainterPath check; check.moveTo(3, 12); check.lineTo(7, 16); check.lineTo(16, 7); p.drawPath(check);
            QPainterPath second; second.moveTo(11, 15); second.lineTo(13, 17); second.lineTo(22, 8); p.drawPath(second);
        } else if (name == "add") { p.drawLine(12, 5, 12, 19); p.drawLine(5, 12, 19, 12); }
        else if (name == "refresh") {
            p.drawArc(QRectF(5, 5, 14, 14), 40*16, 290*16); p.drawLine(19, 4, 19, 9); p.drawLine(14, 9, 19, 9);
        } else if (name == "reset") {
            // Refresh turned back: counterclockwise, to the default value.
            p.drawArc(QRectF(5, 5, 14, 14), 140*16, -290*16); p.drawLine(5, 4, 5, 9); p.drawLine(5, 9, 10, 9);
        } else if (name == "clear") {
            // An eraser on its baseline: Clear session.
            p.save(); p.translate(12, 11); p.rotate(-45);
            p.drawRoundedRect(QRectF(-7, -4, 14, 8), 1.5, 1.5); p.drawLine(QPointF(-1, -4), QPointF(-1, 4)); p.restore();
            p.drawLine(10, 21, 20, 21);
        } else if (name == "connect" || name == "disconnect") {
            // Opposing chain links, separated for a terminal detach.
            QPainterPath left, right;
            left.moveTo(10, 7); left.lineTo(7, 7); left.cubicTo(1, 7, 1, 17, 7, 17); left.lineTo(10, 17);
            right.moveTo(14, 7); right.lineTo(17, 7); right.cubicTo(23, 7, 23, 17, 17, 17); right.lineTo(14, 17);
            p.drawPath(left); p.drawPath(right);
            if (name == "connect") p.drawLine(8, 12, 16, 12);
            else { p.drawLine(11, 3, 13, 6); p.drawLine(11, 18, 13, 21); }
        } else if (name == "video") {
            p.drawRoundedRect(QRectF(3,5,11,14),2,2);
            QPainterPath lens;lens.moveTo(14,9);lens.lineTo(21,5);lens.lineTo(21,19);lens.lineTo(14,15);p.drawPath(lens);
        } else if (name == "microphone") {
            p.drawRoundedRect(QRectF(9, 3, 6, 12), 3, 3);
            p.drawArc(QRectF(6, 8, 12, 11), 180 * 16, 180 * 16);
            p.drawLine(12, 19, 12, 22); p.drawLine(8, 22, 16, 22);
        } else if (name == "attachment") {
            QPainterPath path; path.moveTo(9, 16); path.lineTo(16, 9);
            path.cubicTo(19, 6, 15, 2, 12, 5); path.lineTo(5, 12);
            path.cubicTo(-1, 18, 7, 26, 13, 20); path.lineTo(20, 13);
            path.cubicTo(24, 9, 18, 3, 14, 7); path.lineTo(7, 14);
            p.drawPath(path);
        } else if (name == "inspector") {
            p.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2); p.drawLine(14, 4, 14, 20);
            p.drawLine(17, 8, 18, 8); p.drawLine(17, 12, 18, 12);
        } else if (name == "collapse-sessions" || name == "expand-sessions") {
            // The bar is the session list's edge; the arrow points where it goes.
            const int tip = name == "collapse-sessions" ? 9 : 19, tail = name == "collapse-sessions" ? 19 : 9;
            p.drawLine(4, 4, 4, 20); p.drawLine(tail, 12, tip, 12);
            p.drawLine(14, 7, tip, 12); p.drawLine(14, 17, tip, 12);
        } else if (name == "expand-row") {
            p.drawLine(5, 4, 19, 4); p.drawLine(5, 20, 19, 20);
            p.drawLine(12, 8, 12, 16);
            p.drawLine(8, 11, 12, 7); p.drawLine(12, 7, 16, 11);
            p.drawLine(8, 13, 12, 17); p.drawLine(12, 17, 16, 13);
        } else if (name == "compact-row") {
            p.drawLine(5, 4, 19, 4); p.drawLine(5, 20, 19, 20);
            p.drawLine(12, 5, 12, 10); p.drawLine(8, 7, 12, 11); p.drawLine(12, 11, 16, 7);
            p.drawLine(12, 14, 12, 19); p.drawLine(8, 17, 12, 13); p.drawLine(12, 13, 16, 17);
        } else if (name == "search") {
            p.drawEllipse(QPointF(10.5, 10.5), 6, 6); p.drawLine(QPointF(15, 15), QPointF(20, 20));
        } else if (name == "collapse-panel") {
            p.drawLine(20, 4, 20, 20); p.drawLine(5, 12, 15, 12);
            p.drawLine(10, 7, 15, 12); p.drawLine(10, 17, 15, 12);
        } else if (name == "stop") {
            p.setBrush(color);p.drawRoundedRect(QRectF(6,6,12,12),1,1);
        } else if (name == "close") {
            p.drawLine(6, 6, 18, 18); p.drawLine(6, 18, 18, 6);
        } else if (name == "copy") {
            p.drawRoundedRect(QRectF(8, 8, 12, 13), 2, 2); p.drawLine(4, 16, 4, 3); p.drawLine(4, 3, 16, 3);
        } else if (name == "drafts") {
            QPainterPath page; page.moveTo(11, 21); page.lineTo(4, 21); page.lineTo(4, 3);
            page.lineTo(13, 3); page.lineTo(18, 8); page.lineTo(18, 10); p.drawPath(page);
            p.drawLine(13, 3, 13, 8); p.drawLine(13, 8, 18, 8);
            p.drawLine(7, 11, 12, 11); p.drawLine(7, 15, 9, 15);
            QPainterPath pencil; pencil.moveTo(12, 17); pencil.lineTo(19, 10); pencil.lineTo(22, 13);
            pencil.lineTo(15, 20); pencil.lineTo(11, 21); pencil.closeSubpath(); p.drawPath(pencil);
            p.drawLine(17, 12, 20, 15);
        } else if (name == "external") {
            p.drawLine(12, 4, 20, 4); p.drawLine(20, 4, 20, 12); p.drawLine(20, 4, 10, 14);
            QPainterPath path; path.moveTo(8, 5); path.lineTo(4, 5); path.lineTo(4, 20); path.lineTo(19, 20); path.lineTo(19, 16); p.drawPath(path);
        } else if (name == "send" || name == "chevron-down" || name == "chevrons-down") {
            // Composer actions sit beside semibold text; match its weight.
            p.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (name == "send") { p.drawLine(12, 19, 12, 5); p.drawLine(6, 11, 12, 5); p.drawLine(18, 11, 12, 5); }
            else if (name == "chevrons-down") {
                p.drawLine(6, 6, 12, 12); p.drawLine(12, 12, 18, 6);
                p.drawLine(6, 12, 12, 18); p.drawLine(12, 18, 18, 12);
            }
            else { p.drawLine(6, 9, 12, 15); p.drawLine(12, 15, 18, 9); }
        } else if (name == "more") { p.drawPoint(5, 12); p.drawPoint(12, 12); p.drawPoint(19, 12); }
        p.end(); icon.addPixmap(pm);
    }
    cache->insert(key, icon);
    return icon;
}

inline void configureWorkspaceIconButton(QPushButton *button, const QString &name, const QString &caption, bool dark)
{
    WorkspaceFocus::install();
    button->setText({}); button->setToolTip(caption); button->setAccessibleName(caption);
    button->setFixedSize(34, 34); button->setIconSize(QSize(18, 18));
    // Override inherited padding/minimums so this stays square inside any page.
    button->setStyleSheet("QPushButton { padding:0; min-width:32px; max-width:32px; min-height:32px; max-height:32px; }");
    button->setIcon(workspaceIcon(name, QColor(dark ? "#a1adbb" : "#647386")));
}
