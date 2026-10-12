#pragma once

#include "OvermindModel.h"
#include "TasksView.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QUrl>
#include <QWidget>

#include <functional>

class MarkdownText;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class TaskFlow;

// The open card of the Tasks tab, on the right of the board: what `show` says
// about the task (prompt, images, labels, priority, pin, relations and the
// journal rendered as Markdown), its edits (edit, label, pin, unpin, link) and
// its actions (send, review-send, rework-send, answer, move, done, delete,
// resolve-delivery). Every write goes through TasksView::perform and reports in
// the board's message bar. Delete, taking a task back, reopening, Not delivered
// and discarding a changed prompt ask first.
//
// The task is read again (`show`) whenever its board entry changes. An edited
// prompt or a typed answer is a draft of its task while the window lives: it
// survives other cards, changes made elsewhere (the prompt then says so) and a
// refused save; it goes once saved or discarded.
class TaskPanel : public QWidget {
    Q_OBJECT
public:
    TaskPanel(TasksView *view, OvermindModel *model, QWidget *parent = nullptr);

    // The card to show, by UUID; empty shows none.
    void setTask(const QString &project, const QString &uuid);
    QString task() const { return m_uuid; }
    // The board changed: reads the task again when its board entry changed.
    void refresh();
    // Recipients, a delivery in flight or the board changed what may be done.
    void updateActions();
    void setTheme(bool dark);
    // Opens a copy of an image; tests replace the desktop handler.
    std::function<bool(const QUrl &)> openUrl;

signals:
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Purpose { Send, Review, Pin };
    struct Draft {
        QString text;   // the edited prompt
        QString base;   // the prompt it was edited from
    };
    QJsonObject current() const;   // the board entry with the details of `show`
    QString ref() const;
    QString refOf(const QString &uuid) const;
    void load();
    void render();
    void renderNotes(const QJsonObject &task);
    void renderAnswer(const QJsonObject &task);
    void renderPrompt();
    void renderImages(const QJsonObject &task);
    void renderLabels(const QJsonObject &task);
    void renderPin(const QJsonObject &task);
    void renderRelations(const QJsonObject &task);
    void renderJournal(const QJsonObject &task);
    void updatePromptControls();
    // The prompt field follows its text: 4 to 14 lines.
    void fitPrompt();
    void fillSessions(QMenu *menu, Purpose purpose);
    void applyStyle();
    // A write about the open task; `session` names the receiving session.
    void write(const QString &command, QJsonObject request, const QString &session = {}, const QString &done = {},
               OvermindModel::Callback after = {});
    void commitTitle();
    void savePrompt();
    void discardPrompt();
    void sendAnswer();
    void resolveDelivery(bool delivered);
    void deleteTask();
    void addLabel();
    void addRelation();
    void fetchImage(const QString &sha256);
    void openImage(const QString &sha256, const QString &name);
    void setImage(QToolButton *button, const QString &sha256);
    void renderMarkdown(MarkdownText *text, const QString &markdown);

    TasksView *m_view;
    QPointer<OvermindModel> m_model;
    QString m_project, m_uuid;
    QJsonObject m_board;    // the task's board entry
    QJsonObject m_detail;   // the last `show` answer
    bool m_loaded = false, m_reading = false, m_again = false, m_gone = false;
    bool m_settingPrompt = false, m_dark = false;
    quint64 m_generation = 0;
    QString m_error;
    QHash<QString, Draft> m_drafts;     // per task
    QHash<QString, QString> m_answers;  // per task
    QHash<QString, QString> m_images;   // sha256 -> local path
    QHash<QString, QString> m_imageErrors;
    QSet<QString> m_fetching;
    QHash<QString, QPixmap> m_thumbnails;
    QSet<QString> m_savingPrompts;     // tasks whose prompt is being saved
    QJsonArray m_journal;               // as rendered
    QString m_journalTask;

    QLabel *m_ref, *m_stage, *m_meta, *m_status, *m_note;
    QToolButton *m_close;
    QLineEdit *m_title;
    QPushButton *m_send, *m_review, *m_move, *m_rework, *m_done;
    QScrollArea *m_scroll;
    QFrame *m_delivery;
    QLabel *m_deliveryText;
    QPushButton *m_delivered, *m_notDelivered;
    QFrame *m_answerBox;
    MarkdownText *m_question;
    QPlainTextEdit *m_answer;
    QPushButton *m_answerSend;
    QPlainTextEdit *m_prompt;
    QPushButton *m_promptSave, *m_promptDiscard;
    QLabel *m_promptNote;
    QWidget *m_imagesSection;
    TaskFlow *m_imageFlow;
    QComboBox *m_priority;
    QLabel *m_pinned;
    QPushButton *m_pin;
    QPushButton *m_unpin;
    QWidget *m_labelsBox;
    TaskFlow *m_labelFlow;
    QLineEdit *m_labelAdd;
    QWidget *m_relationsBox;
    QVBoxLayout *m_relations;
    QComboBox *m_relationKind;
    QLineEdit *m_relationTarget;
    QPushButton *m_relationAdd;
    QLabel *m_journalTitle;
    QVBoxLayout *m_journalLayout;
    QPushButton *m_delete;
};
