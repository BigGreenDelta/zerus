#include "DownloadCacheSettings.h"
#include "DownloadCache.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace { constexpr qint64 GiB = 1024LL * 1024 * 1024; }

DownloadCacheSettings::DownloadCacheSettings(QWidget *parent) : QWidget(parent)
{
    setObjectName("downloadCacheSettings"); setMaximumWidth(850);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 12, 0); layout->setSpacing(18);
    auto *title = new QLabel(tr("Files")); title->setObjectName("heading"); layout->addWidget(title);
    auto *description = new QLabel(tr("Download and open saves a copy here and opens it immediately. Download… lets you choose where to save. Existing files are preserved; repeated names get a numbered suffix."));
    description->setWordWrap(true); layout->addWidget(description);
    auto *form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    auto *path = new QLineEdit(QDir::toNativeSeparators(DownloadCache::directory())); path->setReadOnly(true); path->setObjectName("downloadCacheFolder");
    form->addRow(tr("Downloaded files"), path);
    m_mode = new QComboBox; m_mode->setObjectName("downloadCachePolicy");
    m_mode->addItems({tr("Never clear"), tr("Limit size")});
    m_mode->setMinimumWidth(160);
    m_mode->setCurrentIndex(DownloadCache::limitBytes() > 0 ? 1 : 0); form->addRow(tr("Keep copies"), m_mode);
    m_size = new QDoubleSpinBox; m_size->setObjectName("downloadCacheSize"); m_size->setRange(0.1, 1024);
    m_size->setDecimals(1); m_size->setSingleStep(0.5); m_size->setSuffix(tr(" GiB"));
    m_size->setButtonSymbols(QAbstractSpinBox::NoButtons); m_size->setMinimumWidth(160);
    m_size->setValue(DownloadCache::limitBytes() ? double(DownloadCache::limitBytes()) / GiB : 1.0);
    form->addRow(tr("Maximum size"), m_size); layout->addLayout(form);
    auto *hint = new QLabel(tr("Never clear is the default. With a limit, Zerus removes its oldest downloaded copies first. Files you add or edit are kept. Downloads in progress and copies downloaded in the last minute are protected, so usage can briefly exceed the limit. This setting applies to this computer."));
    hint->setWordWrap(true); layout->addWidget(hint);
    m_usage = new QLabel; m_usage->setObjectName("downloadCacheUsage"); m_usage->setWordWrap(true);
    m_usage->setTextFormat(Qt::PlainText); layout->addWidget(m_usage);
    m_apply = new QPushButton(tr("Apply")); m_apply->setObjectName("applyDownloadCachePolicy");
    layout->addWidget(m_apply, 0, Qt::AlignLeft); layout->addStretch();
    connect(m_mode, &QComboBox::currentIndexChanged, this, &DownloadCacheSettings::updateControls);
    connect(m_size, &QDoubleSpinBox::valueChanged, this, &DownloadCacheSettings::updateControls);
    connect(m_apply, &QPushButton::clicked, this, [this] {
        const auto selected = selectedLimit(), current = DownloadCache::limitBytes();
        if (selected > 0 && (!current || selected < current)) {
            QMessageBox confirm(QMessageBox::Question, tr("Limit downloaded files?"),
                tr("Zerus will automatically delete its oldest downloaded copies when they exceed %1. Files you added or edited will be kept.")
                    .arg(QLocale().formattedDataSize(selected)), QMessageBox::NoButton, this);
            confirm.setObjectName("confirmDownloadCacheLimit");
            auto *apply = confirm.addButton(tr("Apply limit"), QMessageBox::AcceptRole);
            auto *cancel = confirm.addButton(QMessageBox::Cancel); confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel);
            confirm.exec(); if (confirm.clickedButton() != apply) return;
        }
        DownloadCache::setLimitBytes(selected); updateControls(); refresh();
    });
    auto *timer = new QTimer(this); timer->setInterval(DownloadCache::OpeningGraceMs + 5000);
    connect(timer, &QTimer::timeout, this, [this] { if (DownloadCache::limitBytes()) refresh(); }); timer->start();
    updateControls(); refresh();
}

qint64 DownloadCacheSettings::selectedLimit() const
{
    return m_mode->currentIndex() == 0 ? 0 : qRound64(m_size->value() * GiB);
}

void DownloadCacheSettings::updateControls()
{
    m_size->setEnabled(m_mode->currentIndex() == 1);
    m_apply->setEnabled(selectedLimit() != DownloadCache::limitBytes());
}

void DownloadCacheSettings::refresh()
{
    if (m_busy) { m_refreshPending = true; return; }
    m_busy = true; m_usage->setText(tr("Checking downloaded files…"));
    const auto root = DownloadCache::directory(); const auto limit = DownloadCache::limitBytes();
    DownloadCache::async<DownloadCache::Usage>(this,
        [root, limit] { return limit ? DownloadCache::trim(root, limit) : DownloadCache::usage(root); },
        [this](const DownloadCache::Usage &result) {
            m_busy = false;
            m_usage->setText(result.error.isEmpty() ? tr("Downloaded copies: %1 · %2 files")
                .arg(QLocale().formattedDataSize(result.bytes)).arg(result.files) : result.error);
            if (m_refreshPending) { m_refreshPending = false; refresh(); }
        });
}
