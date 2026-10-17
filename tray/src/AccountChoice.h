#pragma once
#include "AccountUsage.h"
#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>

// Account and agent rows as New session and Add account show them: a large
// colored provider logo beside a bold name and one line of limit meters or text.
namespace AccountChoice {
enum Role { ProviderRole=Qt::UserRole+1, ProfileRole, UsageTextRole, UsageColorRole, MembersRole };
class Delegate final : public QStyledItemDelegate {
public:
    explicit Delegate(QWidget *parent):QStyledItemDelegate(parent),m_limits(new AccountUsage::Button(parent)) {
        m_limits->setObjectName("launchAccountLimitsRenderer");m_limits->hide();
        m_limits->setShowProvider(false);m_limits->setCompact(true);m_limits->setFocusPolicy(Qt::NoFocus);
        m_limits->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &) const override {
        return {260,16+option.fontMetrics.lineSpacing()*2};
    }
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        auto background=option;initStyleOption(&background,index);background.text.clear();background.icon={};
        // The selection marks the current row. A native focus frame (Windows 11) would
        // outline only the base item's text area, beside the logo and inside the row.
        background.state&=~QStyle::State_HasFocus;
        const auto *widget=option.widget;auto *style=widget?widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&background,p,widget);
        p->save();p->setClipRect(option.rect);
        const bool selected=option.state&QStyle::State_Selected,enabled=option.state&QStyle::State_Enabled;
        const bool dark=widget&&widget->property("hgsDark").toBool();
        const QColor muted(dark?"#a1adbb":"#647386"),color=enabled?QColor(dark?"#e8edf4":"#1a2733"):muted;
        auto rect=option.rect.adjusted(10,8,-10,-8);
        const int logo=option.fontMetrics.lineSpacing()*2;
        index.data(Qt::DecorationRole).value<QIcon>().paint(p,QRect(rect.x(),rect.y(),logo,logo),Qt::AlignCenter,enabled?QIcon::Normal:QIcon::Disabled);
        rect.adjust(logo+12,0,0,0);
        auto font=option.font;font.setWeight(QFont::DemiBold);p->setFont(font);p->setPen(color);
        p->drawText(QRect(rect.x(),rect.y(),rect.width(),option.fontMetrics.lineSpacing()),Qt::AlignVCenter,
            QFontMetrics(font).elidedText(index.data().toString(),Qt::ElideRight,rect.width()));
        const QRect limits(rect.x(),rect.y()+option.fontMetrics.lineSpacing(),rect.width(),option.fontMetrics.lineSpacing());
        const auto data=AccountUsage::launchMeterData(index.data(ProfileRole).toJsonObject()["usage"].toObject());
        if(!data["windows"].toArray().isEmpty()){
            // Render the actual status-footer widget without its provider badge.
            // It remains hidden and never receives input or fetches account data.
            m_limits->setTheme(dark);m_limits->setFont(option.font);m_limits->setIndicator(AccountUsage::indicator());m_limits->setData(data);
            p->setClipRect(limits,Qt::IntersectClip);
            m_limits->paintContents(*p,QPoint(limits.x()-4,limits.y()+(limits.height()-m_limits->height())/2));
        }else{
            const auto usageColor=index.data(UsageColorRole).value<QColor>();
            p->setFont(option.font);p->setPen(usageColor.isValid()&&enabled?usageColor:selected&&enabled?color:muted);
            p->drawText(limits,Qt::AlignVCenter,option.fontMetrics.elidedText(index.data(UsageTextRole).toString(),Qt::ElideRight,limits.width()));
        }
        p->restore();
    }
private:
    AccountUsage::Button *m_limits;
};
}
