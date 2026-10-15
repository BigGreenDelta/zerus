#pragma once
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

class DownloadCacheSettings : public QWidget {
    Q_OBJECT
public:
    explicit DownloadCacheSettings(QWidget *parent = nullptr);
    void refresh();
private:
    void updateControls();
    qint64 selectedLimit() const;
    QComboBox *m_mode;
    QDoubleSpinBox *m_size;
    QLabel *m_usage;
    QPushButton *m_apply;
    bool m_busy = false;
    bool m_refreshPending = false;
};
