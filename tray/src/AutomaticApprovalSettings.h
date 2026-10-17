#pragma once
#include "HgsClient.h"
#include <QWidget>
class QCheckBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QPlainTextEdit;
class QPushButton;

class AutomaticApprovalSettings : public QWidget {
    Q_OBJECT
public:
    explicit AutomaticApprovalSettings(const QString &executable, QWidget *parent = nullptr);
    void refresh();
private:
    HgsClient m_client;
    QCheckBox *m_enabled;
    QLineEdit *m_session, *m_host;
    QTableWidget *m_table;
    QPlainTextEdit *m_detail;
    QLabel *m_status;
    QPushButton *m_newer, *m_older;
    quint64 m_listRequest = 0, m_detailRequest = 0;
    bool m_listBusy = false, m_refreshAgain = false;
    qint64 m_before = 0, m_after = 0, m_next = 0, m_previous = 0;
};
