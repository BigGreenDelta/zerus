#pragma once
#include "RecoverySettings.h"
#include "ActivityWidth.h"
#include "ContentScale.h"
#include "WorkspaceIcons.h"
#include "UpdatesWidget.h"
#include "RelaySettings.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QListWidget>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QUrl>
#include "ProcessSettings.h"

class SettingsPage : public QWidget {
public:
    std::function<void()> appearanceChanged;
    std::function<void()> contentScaleChanged;
    std::function<void()> activityWidthChanged;
    std::function<void(bool)> windowLayerChanged;
    std::function<void()> pollingChanged;
    explicit SettingsPage(const QString &executable,QWidget *parent=nullptr):QWidget(parent){
        setObjectName("settingsPage");auto *outer=new QVBoxLayout(this);outer->setContentsMargins(24,20,24,20);outer->setSpacing(20);
        auto *title=new QLabel(tr("Settings"));title->setObjectName("heading");outer->addWidget(title);
        auto *body=new QHBoxLayout;body->setSpacing(24);nav=new QListWidget;nav->setObjectName("settingsSections");nav->setFixedWidth(180);nav->addItems({tr("Appearance"),tr("Sessions"),tr("Automatic recovery"),tr("Processes"),tr("Updates"),tr("Mobile connection"),tr("About")});body->addWidget(nav);
        pages=new QStackedWidget;pages->setMinimumWidth(0);body->addWidget(pages,1);outer->addLayout(body,1);
        auto makePage=[&](const QString &heading){auto *content=new QWidget;content->setMaximumWidth(850);auto *layout=new QVBoxLayout(content);layout->setContentsMargins(0,0,12,0);layout->setSpacing(18);auto *h=new QLabel(heading);h->setObjectName("heading");layout->addWidget(h);addPage(content);return layout;};
        auto *appearance=makePage(tr("Appearance"));auto *theme=new QComboBox;theme->setObjectName("workspaceTheme");theme->addItem(tr("Follow system appearance"),"system");theme->addItem(tr("Dark"),"dark");theme->addItem(tr("Light"),"light");theme->setCurrentIndex(qMax(0,theme->findData(QSettings().value("workspace/theme","system"))));appearance->addWidget(new QLabel(tr("Theme")));appearance->addWidget(theme);
        appearance->addWidget(new QLabel(tr("Content scale")));
        auto *scaleRow=new QHBoxLayout;scaleRow->setSpacing(4);auto *scale=new QSlider(Qt::Horizontal);scale->setObjectName("workspaceContentScale");
        // Twentieths keep the slider on 5% steps for both dragging and the keyboard.
        scale->setRange(qRound(ContentScale::Minimum*20),qRound(ContentScale::Maximum*20));scale->setPageStep(5);
        const auto scaleFactor=ContentScale::factor();scale->setValue(qRound(scaleFactor*20));
        // Normalize preferences saved by older versions to the current bounds.
        QSettings().setValue("workspace/contentScale",scaleFactor);
        scale->setAccessibleName(tr("Content scale"));scale->setMaximumWidth(360);
        // A value keeps its widest text's width, so the reset button right after it stays put while dragging.
        auto *scaleValue=new QLabel;scaleValue->setObjectName("workspaceContentScaleValue");scaleValue->setMinimumWidth(scaleValue->fontMetrics().horizontalAdvance("200%")+2);
        scaleRow->addWidget(scale,1);scaleRow->addSpacing(8);scaleRow->addWidget(scaleValue);
        scaleRow->addWidget(resetButton(scale,qRound(ContentScale::Default*20),tr("Reset to %1%").arg(qRound(ContentScale::Default*100)),"resetContentScale"));
        scaleRow->addStretch();appearance->addLayout(scaleRow);
        auto *scaleHint=new QLabel(tr("Adjusts Activity, the message field and Terminal from 75% to 200%. The session list and the side panel keep their size."));scaleHint->setWordWrap(true);appearance->addWidget(scaleHint);
        // Rebuilding a long transcript on every slider step would stall dragging.
        auto *scaleApply=new QTimer(this);scaleApply->setSingleShot(true);scaleApply->setInterval(150);
        connect(scaleApply,&QTimer::timeout,this,[this]{if(contentScaleChanged)contentScaleChanged();});
        auto showScale=[scaleValue](int steps){scaleValue->setText(QStringLiteral("%1%").arg(steps*5));};showScale(scale->value());
        connect(scale,&QSlider::valueChanged,this,[scaleApply,showScale](int steps){QSettings().setValue("workspace/contentScale",steps/20.);showScale(steps);scaleApply->start();});
        appearance->addWidget(new QLabel(tr("Activity width")));
        auto *widthRow=new QHBoxLayout;widthRow->setSpacing(4);activityWidth=new QSlider(Qt::Horizontal);activityWidth->setObjectName("workspaceActivityWidth");
        activityWidth->setRange(ActivityWidth::Minimum,ActivityWidth::Maximum);activityWidth->setSingleStep(10);activityWidth->setPageStep(100);
        activityWidth->setAccessibleName(tr("Maximum Activity width"));activityWidth->setMaximumWidth(360);
        activityWidthValue=new QLabel;activityWidthValue->setObjectName("workspaceActivityWidthValue");activityWidthValue->setMinimumWidth(activityWidthValue->fontMetrics().horizontalAdvance("2400 px")+2);
        widthRow->addWidget(activityWidth,1);widthRow->addSpacing(8);widthRow->addWidget(activityWidthValue);
        widthRow->addWidget(resetButton(activityWidth,ActivityWidth::Default,tr("Reset to %1 px").arg(ActivityWidth::Default),"resetActivityWidth"));
        widthRow->addStretch();appearance->addLayout(widthRow);
        activityFullWidth=new QCheckBox(tr("Full-width Activity"));activityFullWidth->setObjectName("workspaceActivityFullWidth");appearance->addWidget(activityFullWidth);
        auto *widthHint=new QLabel(tr("In a wide window, Activity, questions and the message field stay centered at this width, which grows with content scale. Drag the column edges in Activity to adjust it, or double-click an edge to restore %1 px. Full-width Activity fills the whole pane.").arg(ActivityWidth::Default));widthHint->setWordWrap(true);appearance->addWidget(widthHint);
        syncActivityWidth();
        connect(activityWidth,&QSlider::valueChanged,this,[this](int width){ActivityWidth::setWidth(width);activityWidthValue->setText(tr("%1 px").arg(width));if(activityWidthChanged)activityWidthChanged();});
        connect(activityFullWidth,&QCheckBox::toggled,this,[this](bool full){ActivityWidth::setFullWidth(full);activityWidth->setEnabled(!full);updateResets();if(activityWidthChanged)activityWidthChanged();});
        onTop=new QCheckBox(tr("Keep Zerus above other windows"));onTop->setObjectName("workspaceAlwaysOnTop");onTop->setEnabled(false);appearance->addWidget(onTop);
        onTopHint=new QLabel;onTopHint->setObjectName("workspaceAlwaysOnTopHint");onTopHint->setWordWrap(true);appearance->addWidget(onTopHint);
        connect(onTop,&QCheckBox::toggled,this,[this](bool on){if(windowLayerChanged)windowLayerChanged(on);});
        auto *local=new QLabel(tr("Appearance and session list preferences apply to this Zerus."));local->setWordWrap(true);appearance->addWidget(local);appearance->addStretch();
        connect(theme,&QComboBox::currentIndexChanged,this,[this,theme]{QSettings().setValue("workspace/theme",theme->currentData());if(appearanceChanged)appearanceChanged();});
        auto *sessions=makePage(tr("Sessions"));
        for(const auto &option:QList<QStringList>{{"workspaceCompact","workspace/compact",tr("Compact session rows"),"false"},{"workspaceExpandSessionsOnHover","workspace/expandSessionsOnHover",tr("Expand the collapsed session list on hover"),"true"},{"workspaceHideEmptyProjects","workspace/hideEmptyProjects",tr("Hide projects without active sessions"),"true"},{"workspaceAnimateActivity","workspace/animateActivity",tr("Animate working sessions"),"true"}}){
            auto *check=new QCheckBox(option[2]);check->setObjectName(option[0]);check->setChecked(QSettings().value(option[1],option[3]=="true").toBool());sessions->addWidget(check);connect(check,&QCheckBox::toggled,this,[this,key=option[1]](bool on){QSettings().setValue(key,on);if(appearanceChanged)appearanceChanged();});
        }
        auto *completed=new QCheckBox(tr("Show completed subagents"));completed->setObjectName("workspaceShowCompletedSubagents");completed->setChecked(QSettings().value("workspace/showCompletedSubagents",false).toBool());sessions->addWidget(completed);
        auto *completedHint=new QLabel(tr("Include agents whose latest turn has finished in the session list and its counts. Tasks & agents always shows all reported agents. Agents with unknown status remain visible."));completedHint->setWordWrap(true);sessions->addWidget(completedHint);
        connect(completed,&QCheckBox::toggled,this,[this](bool on){QSettings().setValue("workspace/showCompletedSubagents",on);if(appearanceChanged)appearanceChanged();});
        auto *hint=new QLabel(tr("Closing Zerus keeps agents running. Projects and the arrangement of your workspace are saved on this device."));hint->setWordWrap(true);sessions->addWidget(hint);sessions->addStretch();
        sync=new RecoverySync(executable,this);recovery=new RecoverySettings(sync);recovery->setMaximumWidth(850);addPage(recovery);
        auto *processes=makePage(tr("Processes"));
        auto *enabled=new QCheckBox(tr("Enable Processes tab"));enabled->setObjectName("processesEnabled");enabled->setChecked(ProcessSettings::enabled());processes->addWidget(enabled);
        auto *timings=new QWidget;auto *form=new QFormLayout(timings);form->setContentsMargins(0,0,0,0);form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);timings->setEnabled(enabled->isChecked());processes->addWidget(timings);
        connect(enabled,&QCheckBox::toggled,this,[this,timings](bool on){QSettings().setValue("processes/enabled",on);timings->setEnabled(on);if(pollingChanged)pollingChanged();});
        auto timing=[&](const char *key,const QString &label,int value,double low,double high){
            auto *spin=new QDoubleSpinBox;spin->setObjectName(QLatin1String(key));spin->setRange(low,high);spin->setDecimals(1);spin->setSingleStep(0.5);spin->setSuffix(tr(" s"));spin->setValue(value/1000.);form->addRow(label,spin);
            connect(spin,&QDoubleSpinBox::valueChanged,this,[this,key](double seconds){QSettings().setValue(QStringLiteral("processes/")+QLatin1String(key),seconds);if(pollingChanged)pollingChanged();});
        };
        timing("activeSeconds",tr("Open Processes tab"),ProcessSettings::activeMs(),1,60);
        timing("backgroundSeconds",tr("Background process count"),ProcessSettings::backgroundMs(),5,600);
        timing("unknownSeconds",tr("Unconfirmed process output"),ProcessSettings::unknownMs(),1,600);
        timing("loadingSeconds",tr("Show loading indicator after"),ProcessSettings::loadingMs(),0,5);
        auto *processHint=new QLabel(tr("Off by default. When disabled, the tab is hidden and processes are not polled. Enable it to inspect running shells and background commands, read recorded output and stop selected processes. Only the selected session is inspected; opening Processes refreshes immediately. Output is read only while the tab is visible. Timings apply to this Zerus. Background refresh also controls session inspection while Terminal or Native UI is open; session overview and account limits refresh separately."));processHint->setWordWrap(true);processes->addWidget(processHint);processes->addStretch();
        auto *updates=makePage(tr("Updates"));updates->addWidget(new UpdatesWidget);updates->addStretch();
        auto *mobile=new RelaySettings::Panel;mobile->setMaximumWidth(850);addPage(mobile);
        auto *about=makePage(tr("About"));about->addWidget(new QLabel(tr("Zerus %1").arg(QCoreApplication::applicationVersion())));
        cliVersion=new QLabel(tr("hgs CLI version unavailable"));cliVersion->setObjectName("aboutHgsVersion");
        cliVersion->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);about->addWidget(cliVersion);
        versionClient=new HgsClient(executable,this);
        connect(versionClient,&HgsClient::versionReady,this,[this](const QString &version){cliVersion->setText(version.isEmpty()?tr("hgs CLI version unavailable"):tr("hgs CLI %1").arg(version));});
        auto *repository=new QPushButton(QStringLiteral("ufna/zerus"));repository->setObjectName("aboutRepository");repository->setProperty("glyph","github");
        repository->setIcon(workspaceIcon("github",palette().color(QPalette::WindowText)));repository->setIconSize(QSize(20,20));repository->setAutoDefault(false);
        repository->setCursor(Qt::PointingHandCursor);repository->setToolTip(QStringLiteral("https://github.com/ufna/zerus"));repository->setAccessibleName(tr("Open Zerus repository on GitHub"));
        connect(repository,&QPushButton::clicked,this,[]{QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/ufna/zerus")));});
        about->addWidget(repository,0,Qt::AlignLeft);about->addStretch();
        connect(nav,&QListWidget::currentRowChanged,pages,&QStackedWidget::setCurrentIndex);nav->setCurrentRow(0);
        connect(nav,&QListWidget::currentRowChanged,this,[this](int row){if(row==6)refreshCliVersion();});
    }
    void setPeers(const QStringList &peers){sync->setPeers(peers);}
    void openUpdates(){nav->setCurrentRow(4);}
    void openRecovery(){nav->setCurrentRow(2);sync->refresh();}
    void setWindowLayerState(bool on,bool enabled,const QString &hint){const QSignalBlocker block(onTop);onTop->setChecked(on);onTop->setEnabled(enabled);onTopHint->setText(hint);}
    void refresh(){sync->refresh();if(nav->currentRow()==6)refreshCliVersion();}
    // Activity's column edges and menu change the same preference.
    void syncActivityWidth(){
        const QSignalBlocker width(activityWidth),full(activityFullWidth);
        activityWidth->setValue(ActivityWidth::width());activityWidthValue->setText(tr("%1 px").arg(activityWidth->value()));
        activityFullWidth->setChecked(ActivityWidth::fullWidth());activityWidth->setEnabled(!activityFullWidth->isChecked());
        updateResets();
    }
private:
    void refreshCliVersion(){cliVersion->setText(tr("hgs CLI: checking…"));versionClient->requestVersion();}
    HgsClient *versionClient;QLabel *cliVersion;
    // An icon button restoring a slider's default, shown only after a change.
    // It keeps its space while hidden, so the rows below do not move. Focus
    // moves to the slider first: a focused button that hides itself would
    // pass focus onward.
    QPushButton *resetButton(QSlider *slider,int value,const QString &caption,const QString &name){
        auto *button=new QPushButton;button->setObjectName(name);button->setProperty("glyph","reset");button->setProperty("settingReset",true);
        button->setToolTip(caption);button->setAccessibleName(caption);button->setAutoDefault(false);
        button->setFixedSize(26,26);button->setIconSize(QSize(16,16));
        button->setStyleSheet("QPushButton { padding:0; min-width:24px; max-width:24px; min-height:24px; max-height:24px; }");
        button->setIcon(workspaceIcon("reset",palette().color(QPalette::WindowText)));
        auto policy=button->sizePolicy();policy.setRetainSizeWhenHidden(true);button->setSizePolicy(policy);button->hide();
        connect(button,&QPushButton::clicked,this,[button,slider,value]{if(button->hasFocus())slider->setFocus(Qt::OtherFocusReason);slider->setValue(value);});
        connect(slider,&QSlider::valueChanged,this,[this]{updateResets();});
        resets.append({button,slider,value});return button;
    }
    void updateResets(){for(const auto &reset:resets)reset.button->setVisible(reset.slider->isEnabled()&&reset.slider->value()!=reset.value);}
    struct Reset{QPushButton *button;QSlider *slider;int value;};QList<Reset> resets;
    void addPage(QWidget *page){if(page->objectName().isEmpty())page->setObjectName("settingsContent");auto *scroll=new QScrollArea;scroll->setFrameShape(QFrame::NoFrame);scroll->setWidgetResizable(true);scroll->setWidget(page);scroll->setMinimumWidth(0);pages->addWidget(scroll);}
    QListWidget *nav;QStackedWidget *pages;RecoverySync *sync;RecoverySettings *recovery;QCheckBox *onTop;QLabel *onTopHint;QSlider *activityWidth;QLabel *activityWidthValue;QCheckBox *activityFullWidth;
};
