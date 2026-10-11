#pragma once
#include "UpdateController.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

class UpdatesWidget : public QWidget {
public:
    explicit UpdatesWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        auto *controller = UpdateController::instance();
        setMaximumWidth(680);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(24);
        auto label = [](const QString &text = QString()) {
            auto *value = new QLabel(text);
            value->setWordWrap(true);
            value->setTextFormat(Qt::PlainText);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            value->setMinimumWidth(0);
            return value;
        };
        auto compact = [](QWidget *control) {
            control->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            control->setAttribute(Qt::WA_LayoutUsesWidgetRect);
        };

        auto *status = new QFrame;
        status->setObjectName("settingsCard");
        auto *statusLayout = new QVBoxLayout(status);
        statusLayout->setContentsMargins(18, 16, 18, 16);
        statusLayout->setSpacing(12);
        auto *title = label();
        title->setObjectName("heading");
        statusLayout->addWidget(title);

        auto *versions = new QFormLayout;
        versions->setContentsMargins(0, 0, 0, 0);
        versions->setHorizontalSpacing(20);
        versions->setVerticalSpacing(6);
        versions->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto *version = label(QCoreApplication::applicationVersion());
        auto *released = label();
        auto addVersion = [&](const QString &caption, QLabel *value) {
            auto *name = label(caption);
            name->setObjectName("muted");
            versions->addRow(name, value);
        };
        addVersion(tr("Installed"), version);
        addVersion(tr("Latest release"), released);
        statusLayout->addLayout(versions);
        auto *comparison = label();
        comparison->setObjectName("updatesComparison");
        statusLayout->addWidget(comparison);
        auto *checked = label();
        checked->setObjectName("muted");
        statusLayout->addWidget(checked);
        auto *cache = label(tr("Showing the last verified catalog."));
        cache->setObjectName("muted");
        statusLayout->addWidget(cache);
        auto *error = label();
        statusLayout->addWidget(error);
        auto *release = new QPushButton(tr("View release"));
        release->setObjectName("updatesRelease");
        compact(release);
        statusLayout->addWidget(release, 0, Qt::AlignLeft);
        auto *download = new QPushButton;
        download->setObjectName("updatesDownload");
        compact(download);
        statusLayout->addWidget(download, 0, Qt::AlignLeft);
        layout->addWidget(status);

        auto *preferences = new QWidget;
        auto *preferencesLayout = new QVBoxLayout(preferences);
        preferencesLayout->setContentsMargins(0, 0, 0, 0);
        preferencesLayout->setSpacing(12);
        auto *preferencesTitle = label(tr("Update checks"));
        preferencesTitle->setObjectName("heading");
        preferencesLayout->addWidget(preferencesTitle);
        auto *automatic = new QCheckBox(tr("Check for updates automatically"));
        automatic->setObjectName("updatesAutomatic");
        compact(automatic);
        preferencesLayout->addWidget(automatic, 0, Qt::AlignLeft);
        auto *visibility = label(tr("Available updates appear in the tray and workspace, with a system notification."));
        visibility->setObjectName("muted");
        preferencesLayout->addWidget(visibility);

        auto *channel = new QComboBox;
        channel->setObjectName("updatesChannel");
        channel->setAccessibleName(tr("Release channel"));
        channel->addItem(tr("Stable"), "stable");
        channel->addItem(tr("Nightly"), "nightly");
        channel->addItem(tr("Dev"), "dev");
        compact(channel);
        auto *check = new QPushButton(tr("Check now"));
        check->setObjectName("updatesCheck");
        compact(check);
        auto *channelControls = new QWidget;
        auto *channelLayout = new QHBoxLayout(channelControls);
        channelLayout->setContentsMargins(0, 0, 0, 0);
        channelLayout->setSpacing(12);
        channelLayout->addWidget(channel);
        channelLayout->addWidget(check);
        channelLayout->addStretch();
        auto *channelForm = new QFormLayout;
        channelForm->setContentsMargins(0, 0, 0, 0);
        channelForm->setHorizontalSpacing(16);
        channelForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
        auto *channelLabel = label(tr("Release channel"));
        channelLabel->setBuddy(channel);
        channelForm->addRow(channelLabel, channelControls);
        preferencesLayout->addLayout(channelForm);
        layout->addWidget(preferences);

        auto *installation = new QWidget;
        auto *installationLayout = new QVBoxLayout(installation);
        installationLayout->setContentsMargins(0, 0, 0, 0);
        installationLayout->setSpacing(12);
        auto *detailsToggle = new QPushButton(tr("Show installation details"));
        detailsToggle->setObjectName("updatesDetailsToggle");
        detailsToggle->setCheckable(true);
        compact(detailsToggle);
        installationLayout->addWidget(detailsToggle, 0, Qt::AlignLeft);
        auto *details = new QWidget;
        details->setObjectName("updatesInstallationDetails");
        auto *detailsLayout = new QVBoxLayout(details);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(12);
        auto *ownership = label();
        detailsLayout->addWidget(ownership);
        auto *command = label();
        command->setObjectName("updatesCommand");
        detailsLayout->addWidget(command);
        auto *copy = new QPushButton(tr("Copy command"));
        copy->setObjectName("updatesCopyCommand");
        compact(copy);
        detailsLayout->addWidget(copy, 0, Qt::AlignLeft);
        auto *instructions = label();
        instructions->setObjectName("muted");
        detailsLayout->addWidget(instructions);
        auto *source = new QPushButton(tr("Installation instructions"));
        compact(source);
        detailsLayout->addWidget(source, 0, Qt::AlignLeft);
        details->hide();
        installationLayout->addWidget(details);
        layout->addWidget(installation);

        connect(detailsToggle, &QPushButton::toggled, this, [details, detailsToggle](bool expanded) {
            details->setVisible(expanded);
            detailsToggle->setText(expanded ? tr("Hide installation details") : tr("Show installation details"));
        });
        connect(source, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl("https://github.com/ufna/zerus#installation"));
        });
        connect(automatic, &QCheckBox::toggled, controller, &UpdateController::setAutomaticChecks);
        connect(channel, &QComboBox::currentIndexChanged, this, [controller, channel] {
            if (controller->channel() != channel->currentData().toString())
                controller->setChannel(channel->currentData().toString());
        });
        connect(check, &QPushButton::clicked, controller, &UpdateController::check);
        connect(release, &QPushButton::clicked, this, [controller] {
            if (controller->feed().valid) QDesktopServices::openUrl(QUrl(controller->feed().releaseUrl));
        });
        connect(download, &QPushButton::clicked, this, [controller] {
            const auto feed=controller->feed();
            if(feed.valid && controller->channel()=="dev")
                QDesktopServices::openUrl(QUrl(controller->installation().arch ? feed.packageUrl : feed.sourceUrl));
        });
        connect(copy, &QPushButton::clicked, this, [controller] {
            const auto value = UpdateController::updateCommand(controller->installation(), controller->channel());
            if (!value.isEmpty()) QApplication::clipboard()->setText(value);
        });

        auto refresh = [=] {
            const auto install = controller->installation();
            QString ownershipText;
            if (install.kind == "package")
                ownershipText = tr("Package: %1\nInstalled package: %2").arg(install.package, install.version.isEmpty() ? tr("unconfirmed") : install.version);
            else if (install.kind == "source")
                ownershipText = tr("Local build in a Git checkout; update and rebuild deliberately.");
            else if (install.kind == "checking")
                ownershipText = tr("Checking installation ownership…");
            else
                ownershipText = tr("Local / manually installed build; package ownership unconfirmed.");
            if (install.arch) ownershipText += tr("\nSystem: Arch-based Linux");
            ownership->setText(ownershipText);

            const auto feed = controller->feed();
            release->setText(controller->channel()=="dev" ? tr("View build") : tr("View release"));
            download->setText(install.arch ? tr("Download dev package") : tr("Download dev source"));
            download->setVisible(feed.valid && controller->channel()=="dev");
            title->setText(controller->busy() ? tr("Checking for updates…") : controller->updateAvailable() ? tr("Update available") : feed.valid ? tr("Release status") : tr("Updates have not been checked"));
            released->setText(feed.valid ? feed.version : tr("Not checked"));
            comparison->setText(controller->comparison());
            comparison->setVisible(!controller->busy() && !comparison->text().isEmpty());
            checked->setText(tr("Last successful check: %1").arg(controller->lastChecked()));
            cache->setVisible(controller->cached());
            error->setText(controller->checkError());
            error->setVisible(!error->text().isEmpty());
            { QSignalBlocker blocker(automatic); automatic->setChecked(controller->automaticChecks()); }
            { QSignalBlocker blocker(channel); channel->setCurrentIndex(channel->findData(controller->channel())); }
            check->setEnabled(!controller->busy());
            release->setEnabled(feed.valid);

            const auto value = UpdateController::updateCommand(install, controller->channel());
            const bool optional = install.kind != "package";
            command->setText((optional ? tr("Optional AUR installation: ") : tr("Full system update: ")) + value);
            command->setVisible(!value.isEmpty());
            copy->setVisible(!value.isEmpty());
            copy->setAccessibleName(optional ? tr("Copy optional installation command") : tr("Copy update command"));
            instructions->setText(optional ? tr("This installs an AUR package; the current local build remains separate. Review the full system upgrade and your executable path.") : tr("Run this through your AUR helper and review the full system upgrade before confirming."));
            if (value.isEmpty() && install.arch)
                instructions->setText(tr("No supported update command is available for this installation. See the installation instructions."));
            instructions->setVisible(!value.isEmpty() || install.arch);
        };
        connect(controller, &UpdateController::changed, this, refresh);
        refresh();
    }
};
