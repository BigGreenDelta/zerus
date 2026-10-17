#pragma once
#include <QDialog>
#include "HgsClient.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QToolButton;

// Native folder selection locally; the CLI browser accesses a peer over SSH.
class DirectoryDialog : public QDialog {
    Q_OBJECT
public:
    static QString chooseDirectory(const QString &hgsPath, const QString &host,
                                   const QString &path, QWidget *parent = nullptr);
    DirectoryDialog(const QString &hgsPath, const QString &host, const QString &path,
                    QWidget *parent = nullptr);
    QString directory() const { return m_current; }
    void reject() override;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void navigate(const QString &path);
    void newFolder();
    void newFolderDialog();
    void cancelInlineFolder();
    void createFolder(const QString &path);
    void setCreating(bool creating);
    HgsClient m_client;
    QString m_host, m_current, m_parent;
    quint64 m_request = 0;
    quint64 m_createRequest = 0;
    bool m_creating = false;
    QString m_createdPath, m_creationError;
    QListWidgetItem *m_inlineItem = nullptr;
    QLineEdit *m_inlineName = nullptr;
    QLineEdit *m_path, *m_filter;
    QListWidget *m_list;
    QCheckBox *m_hidden;
    QLabel *m_status;
    QPushButton *m_choose, *m_newFolder, *m_up, *m_home, *m_go, *m_cancel;
    QToolButton *m_newFolderOptions;
};
