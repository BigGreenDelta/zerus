#include "TaskPanel.h"

#include "AttachmentViewer.h"
#include "HgsClient.h"
#include "MarkdownText.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"

#include <QComboBox>
#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <utility>

// Wraps its items into rows, for chips and buttons in a narrow panel.
class TaskFlow : public QLayout {
public:
    explicit TaskFlow(QWidget *parent = nullptr, int spacing = 6) : QLayout(parent), m_gap(spacing) { setContentsMargins(0, 0, 0, 0); }
    ~TaskFlow() override
    {
        while (auto *item = takeAt(0)) delete item;
    }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override { return index >= 0 && index < m_items.size() ? m_items.takeAt(index) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), true); }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, false);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size;
        for (auto *item : m_items) size = size.expandedTo(item->minimumSize());
        const auto margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }
    // Takes every widget out at once; they are deleted later, as they may be
    // the sender of the signal being handled.
    void clear()
    {
        while (auto *item = takeAt(0)) {
            if (auto *widget = item->widget()) {
                widget->hide();
                widget->setParent(nullptr);
                widget->deleteLater();
            }
            delete item;
        }
        invalidate();
    }

private:
    int arrange(const QRect &rect, bool measure) const
    {
        const auto margins = contentsMargins();
        const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
        int x = area.x(), y = area.y(), line = 0;
        for (auto *item : m_items) {
            if (item->isEmpty()) continue;
            const QSize hint = item->sizeHint();
            if (x > area.x() && x + hint.width() > area.right() + 1) {
                x = area.x();
                y += line + m_gap;
                line = 0;
            }
            if (!measure) item->setGeometry(QRect(QPoint(x, y), QSize(qMin(hint.width(), area.width()), hint.height())));
            x += hint.width() + m_gap;
            line = qMax(line, hint.height());
        }
        return y + line - rect.y() + margins.bottom();
    }
    QList<QLayoutItem *> m_items;
    int m_gap;
};

namespace {

// The CLI's MAX_PROMPT, the same limit as a message (`hgs send`).
constexpr qsizetype MaxPromptBytes = HgsClient::MaximumMessageBytes;
// The CLI's image limits (MAX_ATTACHMENTS, MAX_ATTACHMENT, MAX_ATTACHMENTS_TOTAL).
constexpr int MaxImages = HgsClient::MaximumAttachments;
constexpr qsizetype MaxImageBytes = HgsClient::MaximumAttachmentBytes, MaxImagesBytes = HgsClient::MaximumAttachmentsBytes;

// At most `limit` bytes of a regular file. A device or FIFO named by a path
// (one pasted from the clipboard, say) is refused: it is never read without end.
bool readFile(const QString &path, qsizetype limit, QByteArray *bytes, QString *error)
{
    if (!QFileInfo(path).isFile()) {
        *error = TaskPanel::tr("it is not a regular file");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    *bytes = file.read(limit + 1);
    if (bytes->size() > limit) {
        *error = TaskPanel::tr("it is larger than 10 MiB");
        return false;
    }
    return true;
}

// Labels the workflow sets; a person does not remove them.
const QStringList ReservedLabels{QStringLiteral("reviewed"), QStringLiteral("review-failed"), QStringLiteral("epic"), QStringLiteral("epic-ready")};

QLabel *heading(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("taskPanelHeading"));
    return label;
}

QWidget *retired(QWidget *widget)
{
    widget->hide();
    widget->setParent(nullptr);
    widget->deleteLater();
    return widget;
}

void clearLayout(QLayout *layout)
{
    while (auto *item = layout->takeAt(0)) {
        if (auto *widget = item->widget()) retired(widget);
        else if (auto *inner = item->layout()) clearLayout(inner);
        delete item;
    }
}

QString nameOf(const QJsonValue &session) { return session.toObject().value(QStringLiteral("name")).toString(); }

bool sameSession(const QJsonValue &session, const OvermindModel::Session &other)
{
    const auto object = session.toObject();
    return !other.name.isEmpty() && object.value(QStringLiteral("name")).toString() == other.name
        && object.value(QStringLiteral("machine")).toString() == other.machine;
}

// Whether a person may send a task from `stage`: a claim rule, or, in a
// workflow that names no rules, a ready stage. The CLI checks the rest.
bool claimable(const OvermindModel::Board &board, const QString &stage)
{
    const auto rules = board.workflow.value(QStringLiteral("rules"));
    if (!rules.isArray() || rules.toArray().isEmpty()) return board.role(stage) == QLatin1String("ready");
    for (const auto &value : rules.toArray()) {
        const auto rule = value.toObject();
        if (rule.value(QStringLiteral("action")).toString() == QLatin1String("claim") && rule.value(QStringLiteral("from")).toArray().contains(stage)
            && rule.value(QStringLiteral("actors")).toArray().contains(QStringLiteral("human")))
            return true;
    }
    return false;
}

// The text of the latest journal entry with the marker.
QString latest(const QJsonObject &task, const QString &marker)
{
    const auto journal = task.value(QStringLiteral("journal")).toArray();
    for (int i = int(journal.size()); i-- > 0;) {
        const auto entry = journal[i].toObject();
        if (entry.value(QStringLiteral("marker")).toString() == marker) return entry.value(QStringLiteral("text")).toString();
    }
    return {};
}

// The question the task waits with, as `hgs overmind decisions` reads it: the
// latest [needs-human] entry, only when its event is the task's last
// transition (head). An older one belongs to an earlier stay in Your Call.
QString question(const QJsonObject &task)
{
    const QString head = task.value(QStringLiteral("head")).toString();
    const auto journal = task.value(QStringLiteral("journal")).toArray();
    for (int i = int(journal.size()); i-- > 0;) {
        const auto entry = journal[i].toObject();
        if (entry.value(QStringLiteral("marker")).toString() != QLatin1String("[needs-human]")) continue;
        const bool current = !head.isEmpty() && entry.value(QStringLiteral("event")).toString() == head;
        return current ? entry.value(QStringLiteral("text")).toString() : QString();
    }
    return {};
}

QString author(const QJsonObject &actor)
{
    const QString kind = actor.value(QStringLiteral("kind")).toString(), session = actor.value(QStringLiteral("session")).toString();
    if (kind == QLatin1String("human")) {
        const QString device = actor.value(QStringLiteral("device")).toString();
        return device.isEmpty() ? TaskPanel::tr("a person") : TaskPanel::tr("a person on %1").arg(device);
    }
    if (!session.isEmpty()) return session;
    if (kind == QLatin1String("autopilot")) return TaskPanel::tr("autopilot");
    return kind;
}

} // namespace

TaskPanel::TaskPanel(TasksView *view, OvermindModel *model, QWidget *parent)
    : QWidget(parent), m_view(view), m_model(model)
{
    setObjectName(QStringLiteral("taskPanel"));
    setAttribute(Qt::WA_StyledBackground);
    setMinimumWidth(300);
    setAccessibleName(tr("Task"));
    openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 8, 12);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    m_ref = new QLabel;
    m_ref->setObjectName(QStringLiteral("taskPanelRef"));
    m_ref->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(m_ref);
    m_stage = new QLabel;
    m_stage->setObjectName(QStringLiteral("taskPanelStage"));
    header->addWidget(m_stage);
    header->addStretch();
    m_close = new QToolButton;
    m_close->setObjectName(QStringLiteral("taskPanelClose"));
    m_close->setToolTip(tr("Close the card"));
    m_close->setAccessibleName(tr("Close the card"));
    m_close->setAutoRaise(true);
    m_close->setFixedSize(26, 26);
    m_close->setIconSize(QSize(14, 14));
    header->addWidget(m_close);
    layout->addLayout(header);

    m_title = new QLineEdit;
    m_title->setObjectName(QStringLiteral("taskTitle"));
    m_title->setAccessibleName(tr("Title"));
    m_title->setToolTip(tr("Enter saves the title"));
    m_title->setMaxLength(200);
    layout->addWidget(m_title);
    m_meta = new QLabel;
    m_meta->setObjectName(QStringLiteral("taskPanelMeta"));
    m_meta->setWordWrap(true);
    m_meta->setTextFormat(Qt::PlainText);
    layout->addWidget(m_meta);

    auto *actions = new QWidget;
    actions->setObjectName(QStringLiteral("taskActions"));
    auto *flow = new TaskFlow(actions, 6);
    // A button with a menu shows a ▾ after its text instead of the style's arrow.
    const auto menuButton = [flow](const QString &name, const QString &text, const QString &tip) {
        auto *button = new QPushButton(text + QStringLiteral("  ▾"));
        button->setObjectName(name);
        button->setToolTip(tip);
        button->setAutoDefault(false);
        button->setMenu(new QMenu(button));
        flow->addWidget(button);
        return button;
    };
    const auto pushButton = [flow](const QString &name, const QString &text, const QString &tip) {
        auto *button = new QPushButton(text);
        button->setObjectName(name);
        button->setToolTip(tip);
        button->setAutoDefault(false);
        flow->addWidget(button);
        return button;
    };
    m_send = menuButton(QStringLiteral("taskSend"), tr("Send to session"), tr("Claim the task for a session and type it there"));
    m_review = menuButton(QStringLiteral("taskReviewSend"), tr("Send for review"), tr("Ask another session to review the work"));
    m_rework = pushButton(QStringLiteral("taskRework"), tr("Send rework"), tr("Send the review's findings back to the session that did the work"));
    m_done = pushButton(QStringLiteral("taskDone"), tr("Done"), tr("Sign the task off: it moves to Done"));
    m_move = menuButton(QStringLiteral("taskMove"), tr("Move to"), tr("Move the task to another stage"));
    layout->addWidget(actions);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("taskPanelScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("taskPanelContent"));
    auto *body = new QVBoxLayout(content);
    body->setContentsMargins(0, 4, 6, 0);
    body->setSpacing(8);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("taskPanelStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    body->addWidget(m_status);

    m_delivery = new QFrame;
    m_delivery->setObjectName(QStringLiteral("taskDelivery"));
    auto *delivery = new QVBoxLayout(m_delivery);
    delivery->setContentsMargins(10, 8, 10, 8);
    delivery->setSpacing(6);
    m_deliveryText = new QLabel;
    m_deliveryText->setObjectName(QStringLiteral("taskDeliveryText"));
    m_deliveryText->setWordWrap(true);
    m_deliveryText->setTextFormat(Qt::PlainText);
    delivery->addWidget(m_deliveryText);
    auto *resolve = new QHBoxLayout;
    m_delivered = new QPushButton(tr("It was delivered"));
    m_delivered->setObjectName(QStringLiteral("taskDelivered"));
    m_delivered->setAutoDefault(false);
    m_notDelivered = new QPushButton(tr("Not delivered"));
    m_notDelivered->setObjectName(QStringLiteral("taskNotDelivered"));
    m_notDelivered->setAutoDefault(false);
    resolve->addWidget(m_delivered);
    resolve->addWidget(m_notDelivered);
    resolve->addStretch();
    delivery->addLayout(resolve);
    body->addWidget(m_delivery);

    m_note = new QLabel;
    m_note->setObjectName(QStringLiteral("taskNote"));
    m_note->setWordWrap(true);
    m_note->setTextFormat(Qt::PlainText);
    body->addWidget(m_note);

    m_answerBox = new QFrame;
    m_answerBox->setObjectName(QStringLiteral("taskAnswerBox"));
    auto *answer = new QVBoxLayout(m_answerBox);
    answer->setContentsMargins(10, 8, 10, 8);
    answer->setSpacing(6);
    auto *asks = new QLabel(tr("Your Call: the agent asks"));
    asks->setObjectName(QStringLiteral("taskAnswerTitle"));
    answer->addWidget(asks);
    m_question = new MarkdownText;
    m_question->setObjectName(QStringLiteral("taskQuestion"));
    answer->addWidget(m_question);
    m_answer = new QPlainTextEdit;
    m_answer->setObjectName(QStringLiteral("taskAnswer"));
    m_answer->setAccessibleName(tr("Your answer"));
    m_answer->setPlaceholderText(tr("Your answer"));
    m_answer->setTabChangesFocus(true);
    m_answer->setFixedHeight(84);
    answer->addWidget(m_answer);
    auto *answerRow = new QHBoxLayout;
    answerRow->addStretch();
    m_answerSend = new QPushButton(tr("Answer"));
    m_answerSend->setObjectName(QStringLiteral("taskAnswerSend"));
    m_answerSend->setToolTip(tr("Record the answer and type it into the session that asked"));
    m_answerSend->setAutoDefault(false);
    answerRow->addWidget(m_answerSend);
    answer->addLayout(answerRow);
    body->addWidget(m_answerBox);

    auto *promptHead = new QHBoxLayout;
    promptHead->setSpacing(6);
    promptHead->addWidget(heading(tr("Prompt")));
    promptHead->addStretch();
    m_promptDiscard = new QPushButton(tr("Discard"));
    m_promptDiscard->setObjectName(QStringLiteral("taskPromptDiscard"));
    m_promptDiscard->setToolTip(tr("Discard your changes to the prompt"));
    m_promptDiscard->setAutoDefault(false);
    promptHead->addWidget(m_promptDiscard);
    m_promptSave = new QPushButton(tr("Save"));
    m_promptSave->setObjectName(QStringLiteral("taskPromptSave"));
    m_promptSave->setToolTip(tr("Save the prompt; a session that already has the task is not told"));
    m_promptSave->setAutoDefault(false);
    promptHead->addWidget(m_promptSave);
    body->addLayout(promptHead);
    m_prompt = new QPlainTextEdit;
    m_prompt->setObjectName(QStringLiteral("taskPrompt"));
    m_prompt->setAccessibleName(tr("Prompt"));
    m_prompt->setTabChangesFocus(true);
    m_prompt->installEventFilter(this);
    body->addWidget(m_prompt);
    auto *noteRow = new QHBoxLayout;
    noteRow->setSpacing(6);
    m_promptNote = new QLabel;
    m_promptNote->setObjectName(QStringLiteral("taskPromptNote"));
    m_promptNote->setWordWrap(true);
    m_promptNote->setTextFormat(Qt::PlainText);
    noteRow->addWidget(m_promptNote, 1);
    m_promptShowCurrent = new QPushButton(tr("Show the current version"));
    m_promptShowCurrent->setObjectName(QStringLiteral("taskPromptShowCurrent"));
    m_promptShowCurrent->setToolTip(tr("Show the prompt as it is saved now; your draft stays"));
    m_promptShowCurrent->setCheckable(true);
    m_promptShowCurrent->setAutoDefault(false);
    noteRow->addWidget(m_promptShowCurrent, 0, Qt::AlignTop);
    body->addLayout(noteRow);
    m_promptCurrent = new QPlainTextEdit;
    m_promptCurrent->setObjectName(QStringLiteral("taskPromptCurrent"));
    m_promptCurrent->setAccessibleName(tr("The prompt as saved now"));
    m_promptCurrent->setReadOnly(true);
    m_promptCurrent->setFixedHeight(110);
    body->addWidget(m_promptCurrent);

    m_imagesSection = new QWidget;
    auto *images = new QVBoxLayout(m_imagesSection);
    images->setContentsMargins(0, 0, 0, 0);
    images->setSpacing(4);
    auto *imagesHead = new QHBoxLayout;
    imagesHead->setSpacing(6);
    imagesHead->addWidget(heading(tr("Images")));
    imagesHead->addStretch();
    m_imageAdd = new QPushButton(tr("Add…"));
    m_imageAdd->setObjectName(QStringLiteral("taskImageAdd"));
    m_imageAdd->setAccessibleName(tr("Add images"));
    m_imageAdd->setAutoDefault(false);
    imagesHead->addWidget(m_imageAdd);
    m_imagePaste = new QPushButton(tr("Paste"));
    m_imagePaste->setObjectName(QStringLiteral("taskImagePaste"));
    m_imagePaste->setAccessibleName(tr("Paste an image"));
    m_imagePaste->setAutoDefault(false);
    imagesHead->addWidget(m_imagePaste);
    images->addLayout(imagesHead);
    m_noImages = new QLabel(tr("No images"));
    m_noImages->setObjectName(QStringLiteral("taskPanelMeta"));
    images->addWidget(m_noImages);
    auto *imageBox = new QWidget;
    imageBox->setObjectName(QStringLiteral("taskImages"));
    m_imageFlow = new TaskFlow(imageBox, 6);
    images->addWidget(imageBox);
    body->addWidget(m_imagesSection);

    auto *details = new QGridLayout;
    details->setHorizontalSpacing(8);
    details->setVerticalSpacing(6);
    details->addWidget(heading(tr("Priority")), 0, 0);
    m_priority = new QComboBox;
    m_priority->setObjectName(QStringLiteral("taskPriority"));
    m_priority->setAccessibleName(tr("Priority"));
    m_priority->addItem(tr("P0 · highest"), 0);
    m_priority->addItem(QStringLiteral("P1"), 1);
    m_priority->addItem(tr("P2 · default"), 2);
    m_priority->addItem(QStringLiteral("P3"), 3);
    m_priority->addItem(tr("P4 · lowest"), 4);
    details->addWidget(m_priority, 0, 1, Qt::AlignLeft);
    details->addWidget(heading(tr("Pin")), 1, 0);
    auto *pinRow = new QHBoxLayout;
    pinRow->setSpacing(6);
    m_pinned = new QLabel;
    m_pinned->setObjectName(QStringLiteral("taskPinned"));
    m_pinned->setTextFormat(Qt::PlainText);
    m_pinned->setWordWrap(true);
    pinRow->addWidget(m_pinned, 1);
    m_pin = new QPushButton(tr("Pin to") + QStringLiteral("  ▾"));
    m_pin->setObjectName(QStringLiteral("taskPin"));
    m_pin->setToolTip(tr("Reserve the task for a session: it waits in that session's queue"));
    m_pin->setAutoDefault(false);
    m_pin->setMenu(new QMenu(m_pin));
    pinRow->addWidget(m_pin);
    m_unpin = new QPushButton(tr("Unpin"));
    m_unpin->setObjectName(QStringLiteral("taskUnpin"));
    m_unpin->setAutoDefault(false);
    pinRow->addWidget(m_unpin);
    details->addLayout(pinRow, 1, 1);
    details->setColumnStretch(1, 1);
    body->addLayout(details);

    body->addWidget(heading(tr("Labels")));
    m_labelsBox = new QWidget;
    m_labelsBox->setObjectName(QStringLiteral("taskLabels"));
    m_labelFlow = new TaskFlow(m_labelsBox, 5);
    body->addWidget(m_labelsBox);
    m_labelAdd = new QLineEdit;
    m_labelAdd->setObjectName(QStringLiteral("taskLabelAdd"));
    m_labelAdd->setPlaceholderText(tr("Add a label"));
    m_labelAdd->setAccessibleName(tr("Add a label"));
    m_labelAdd->setToolTip(tr("Lowercase letters, digits and :_./- ; Enter adds it"));
    body->addWidget(m_labelAdd);

    body->addWidget(heading(tr("Relations")));
    m_relationsBox = new QWidget;
    m_relations = new QVBoxLayout(m_relationsBox);
    m_relations->setContentsMargins(0, 0, 0, 0);
    m_relations->setSpacing(4);
    body->addWidget(m_relationsBox);
    auto *link = new QHBoxLayout;
    link->setSpacing(6);
    m_relationKind = new QComboBox;
    m_relationKind->setObjectName(QStringLiteral("taskRelationKind"));
    m_relationKind->setAccessibleName(tr("Relation"));
    m_relationKind->addItem(tr("Parent"), QStringLiteral("parent"));
    m_relationKind->addItem(tr("Follows"), QStringLiteral("follows"));
    m_relationKind->addItem(tr("Related"), QStringLiteral("related"));
    m_relationKind->setCurrentIndex(1);
    link->addWidget(m_relationKind);
    m_relationTarget = new QLineEdit;
    m_relationTarget->setObjectName(QStringLiteral("taskRelationTarget"));
    m_relationTarget->setPlaceholderText(tr("Task ref, e.g. om-k3f9x"));
    m_relationTarget->setAccessibleName(tr("Related task"));
    link->addWidget(m_relationTarget, 1);
    m_relationAdd = new QPushButton(tr("Link"));
    m_relationAdd->setObjectName(QStringLiteral("taskRelationAdd"));
    m_relationAdd->setAutoDefault(false);
    link->addWidget(m_relationAdd);
    body->addLayout(link);

    m_journalTitle = heading(tr("Journal"));
    body->addWidget(m_journalTitle);
    auto *journal = new QWidget;
    journal->setObjectName(QStringLiteral("taskJournal"));
    m_journalLayout = new QVBoxLayout(journal);
    m_journalLayout->setContentsMargins(0, 0, 0, 0);
    m_journalLayout->setSpacing(10);
    body->addWidget(journal);

    m_delete = new QPushButton(tr("Delete task…"));
    m_delete->setObjectName(QStringLiteral("taskDelete"));
    m_delete->setAutoDefault(false);
    body->addSpacing(6);
    body->addWidget(m_delete, 0, Qt::AlignLeft);
    body->addStretch(1);
    m_scroll->setWidget(content);
    layout->addWidget(m_scroll, 1);

    connect(m_close, &QToolButton::clicked, this, &TaskPanel::closeRequested);
    // Enter saves (and retries a refused title); leaving the field saves a change.
    connect(m_title, &QLineEdit::returnPressed, this, [this] { commitTitle(true); });
    connect(m_title, &QLineEdit::editingFinished, this, [this] { commitTitle(false); });
    connect(m_rework, &QPushButton::clicked, this, [this] {
        if (m_uuid.isEmpty() || m_view->isDelivering(m_uuid)) return;
        const QString name = nameOf(current().value(QStringLiteral("assignee")));
        write(QStringLiteral("rework-send"), {{QStringLiteral("task"), m_uuid}}, name,
              tr("Sent the rework of %1 to %2.").arg(ref(), name.isEmpty() ? tr("its session") : name));
    });
    connect(m_done, &QPushButton::clicked, this, [this] { write(QStringLiteral("done"), {{QStringLiteral("task"), m_uuid}}); });
    connect(m_delivered, &QPushButton::clicked, this, [this] { resolveDelivery(true); });
    connect(m_notDelivered, &QPushButton::clicked, this, [this] { resolveDelivery(false); });
    connect(m_answer, &QPlainTextEdit::textChanged, this, [this] {
        if (m_uuid.isEmpty()) return;
        const QString text = m_answer->toPlainText();
        if (text.isEmpty()) m_answers.remove(m_uuid);
        else m_answers.insert(m_uuid, text);
        m_answerSend->setEnabled(!m_answer->isReadOnly() && !m_view->isDelivering(m_uuid) && !text.trimmed().isEmpty());
    });
    connect(m_answerSend, &QPushButton::clicked, this, &TaskPanel::sendAnswer);
    connect(m_prompt, &QPlainTextEdit::textChanged, this, [this] {
        if (m_settingPrompt || m_uuid.isEmpty() || !m_loaded) return;
        const QString text = m_prompt->toPlainText(), saved = current().value(QStringLiteral("prompt")).toString();
        if (text == saved) m_drafts.remove(m_uuid);
        else if (!m_drafts.contains(m_uuid)) m_drafts.insert(m_uuid, {text, saved});
        else m_drafts[m_uuid].text = text;
        updatePromptControls();
    });
    connect(m_prompt, &QPlainTextEdit::textChanged, this, &TaskPanel::fitPrompt);
    connect(m_promptSave, &QPushButton::clicked, this, &TaskPanel::savePrompt);
    connect(m_promptShowCurrent, &QPushButton::toggled, this, &TaskPanel::updatePromptControls);
    pickImages = [this] {
        return QFileDialog::getOpenFileNames(this, tr("Add images"), {}, tr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)"));
    };
    connect(m_imageAdd, &QPushButton::clicked, this, [this] {
        const QStringList paths = pickImages ? pickImages() : QStringList();
        if (!paths.isEmpty()) addImageFiles(paths);
    });
    connect(m_imagePaste, &QPushButton::clicked, this, &TaskPanel::pasteImage);
    connect(m_promptDiscard, &QPushButton::clicked, this, &TaskPanel::discardPrompt);
    connect(m_priority, &QComboBox::activated, this, [this](int index) {
        const int priority = m_priority->itemData(index).toInt();
        if (m_uuid.isEmpty() || priority == current().value(QStringLiteral("priority")).toInt(2)) return;
        const QString uuid = m_uuid;
        write(QStringLiteral("edit"), {{QStringLiteral("task"), uuid}, {QStringLiteral("priority"), priority}}, {}, {},
              [this, uuid, priority](const OvermindModel::Result &result) {
                  if (uuid != m_uuid) return;
                  if (result.ok) m_detail.insert(QStringLiteral("priority"), priority);
                  const QSignalBlocker block(m_priority);
                  m_priority->setCurrentIndex(m_priority->findData(current().value(QStringLiteral("priority")).toInt(2)));
              });
    });
    connect(m_unpin, &QPushButton::clicked, this, [this] { write(QStringLiteral("unpin"), {{QStringLiteral("task"), m_uuid}}); });
    connect(m_labelAdd, &QLineEdit::returnPressed, this, &TaskPanel::addLabel);
    connect(m_relationTarget, &QLineEdit::returnPressed, this, &TaskPanel::addRelation);
    connect(m_relationAdd, &QPushButton::clicked, this, &TaskPanel::addRelation);
    connect(m_delete, &QPushButton::clicked, this, &TaskPanel::deleteTask);
    connect(m_question, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if ((url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")) && openUrl) openUrl(url);
    });
    for (auto *button : {m_send, m_review, m_pin}) {
        const Purpose purpose = button == m_send ? Purpose::Send : button == m_review ? Purpose::Review : Purpose::Pin;
        connect(button->menu(), &QMenu::aboutToShow, this, [this, button, purpose] { fillSessions(button->menu(), purpose); });
    }
    connect(m_move->menu(), &QMenu::aboutToShow, this, &TaskPanel::fillMoves);
    applyStyle();
    render();
}

void TaskPanel::setTask(const QString &project, const QString &uuid)
{
    if (project == m_project && uuid == m_uuid) {
        if (!uuid.isEmpty() && !m_loaded && !m_reading) load();
        return;
    }
    // A title being typed goes to its own task.
    commitTitle();
    m_project = project;
    m_uuid = uuid;
    m_board = {};
    m_detail = {};
    m_loaded = m_reading = m_again = m_gone = false;
    m_error.clear();
    m_imageErrors.clear();
    ++m_generation;
    if (m_model && !uuid.isEmpty())
        for (const auto &task : m_model->board(project).tasks)
            if (task.uuid == uuid) m_board = task.json;
    {
        const QSignalBlocker block(m_answer);
        m_answer->setPlainText(m_answers.value(uuid));
    }
    m_title->setModified(false);
    render();
    m_scroll->verticalScrollBar()->setValue(0);
    load();
}

void TaskPanel::refresh()
{
    if (m_uuid.isEmpty() || !m_model) return;
    const auto &board = m_model->board(m_project);
    const OvermindModel::Task *found = nullptr;
    for (const auto &task : board.tasks) if (task.uuid == m_uuid) found = &task;
    if (!found) {
        // Deleted, or no longer in this project's board.
        if (board.loaded && board.error.isEmpty() && !m_gone) {
            m_gone = true;
            render();
        }
        return;
    }
    const bool back = m_gone;
    m_gone = false;
    if (found->json == m_board && !back) {
        // A read that failed is tried again with the board.
        if (!m_loaded && !m_reading) load();
        updateActions();
        return;
    }
    m_board = found->json;
    render();
    load();
}

void TaskPanel::load()
{
    if (m_uuid.isEmpty() || !m_model) return;
    if (m_reading) {
        m_again = true;
        return;
    }
    m_reading = true;
    const QString uuid = m_uuid;
    const quint64 generation = m_generation;
    m_model->read(QStringLiteral("show"), {{QStringLiteral("task"), uuid}}, [this, generation](const OvermindModel::Result &result) {
        if (generation != m_generation) return;
        m_reading = false;
        if (result.ok) {
            m_detail = result.reply.value(QStringLiteral("task")).toObject();
            m_loaded = true;
            m_error.clear();
        } else {
            m_error = result.refusal.value(QStringLiteral("message")).toString();
        }
        render();
        if (m_again) {
            m_again = false;
            load();
        }
    }, this);
}

// The show answer and the board entry: the newer one wins where both speak,
// and the board adds what only it has (markers, queue_of).
QJsonObject TaskPanel::current() const
{
    QJsonObject task = m_detail;
    const bool boardNewer = m_detail.isEmpty()
        || m_board.value(QStringLiteral("updated_at")).toInteger() > m_detail.value(QStringLiteral("updated_at")).toInteger();
    for (auto it = m_board.begin(); it != m_board.end(); ++it)
        if (boardNewer || !task.contains(it.key())) task.insert(it.key(), it.value());
    return task;
}

QString TaskPanel::ref() const
{
    const QString ref = current().value(QStringLiteral("ref")).toString();
    return ref.isEmpty() ? refOf(m_uuid) : ref;
}

QString TaskPanel::refOf(const QString &uuid) const
{
    if (m_model)
        for (const auto &task : m_model->board(m_project).tasks)
            if (task.uuid == uuid) return task.ref;
    return uuid.left(8);
}

void TaskPanel::render()
{
    setProperty("task", m_uuid);
    setProperty("loaded", m_loaded);
    if (m_uuid.isEmpty() || !m_model) return;
    const QJsonObject task = current();
    const QString stage = task.value(QStringLiteral("stage")).toString();
    const bool editable = !task.value(QStringLiteral("read_only")).toBool() && !m_gone;

    m_ref->setText(ref());
    m_stage->setText(stage);
    m_stage->setVisible(!stage.isEmpty());
    // A title that was not saved stays until it is.
    const QString title = m_titleDrafts.value(m_uuid, task.value(QStringLiteral("title")).toString());
    if (!m_title->isModified() && m_title->text() != title) {
        m_title->setText(title);
        m_title->setCursorPosition(0);
    }
    m_title->setReadOnly(!editable);
    QStringList meta;
    const QString assignee = nameOf(task.value(QStringLiteral("assignee"))), reviewer = nameOf(task.value(QStringLiteral("reviewer")));
    const QString queue = nameOf(task.value(QStringLiteral("queue_of")));
    if (!assignee.isEmpty()) meta << tr("Assigned to %1").arg(assignee);
    if (!reviewer.isEmpty()) meta << tr("Review by %1").arg(reviewer);
    if (!queue.isEmpty()) meta << tr("In the queue of %1").arg(queue);
    m_meta->setText(meta.join(QStringLiteral(" · ")));
    m_meta->setVisible(!meta.isEmpty());

    QString status;
    if (m_gone) status = tr("%1 is no longer on this board: it was deleted, or its project changed.").arg(ref());
    else if (!m_error.isEmpty()) status = tr("Could not read %1: %2").arg(ref(), m_error);
    else if (!m_loaded) status = tr("Loading %1…").arg(ref());
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_status->setProperty("error", m_gone || !m_error.isEmpty());

    renderNotes(task);
    renderAnswer(task);
    renderPrompt();
    renderImages(task);
    {
        const QSignalBlocker block(m_priority);
        m_priority->setCurrentIndex(m_priority->findData(task.value(QStringLiteral("priority")).toInt(2)));
    }
    m_priority->setEnabled(editable);
    renderPin(task);
    renderLabels(task);
    renderRelations(task);
    renderJournal(task);
    updateActions();
}

void TaskPanel::renderNotes(const QJsonObject &task)
{
    const auto flags = task.value(QStringLiteral("flags")).toArray();
    QStringList notes;
    if (task.value(QStringLiteral("read_only")).toBool())
        notes << tr("A newer version of hgs wrote this task. Update hgs on this computer to change it.");
    if (flags.contains(QStringLiteral("orphaned")))
        notes << tr("The session that held this task has ended. Move the task back to the queue to send it again.");
    if (flags.contains(QStringLiteral("conflict"))) notes << tr("Conflicting changes were recorded for this task; the journal says which.");
    if (task.value(QStringLiteral("labels")).toArray().contains(QStringLiteral("epic")))
        notes << tr("This is an epic: it holds other tasks and is never sent.");
    m_note->setText(notes.join(QLatin1Char('\n')));
    m_note->setVisible(!notes.isEmpty() && !m_gone);

    const bool uncertain = flags.contains(QStringLiteral("delivery_uncertain")) && !m_gone;
    m_delivery->setVisible(uncertain);
    if (!uncertain) return;
    const QString reason = latest(task, QStringLiteral("[sent?]"));
    m_deliveryText->setText(reason.isEmpty() ? tr("The last delivery was not confirmed. Did it arrive?")
                                             : tr("The last delivery was not confirmed: %1. Did it arrive?").arg(reason));
}

void TaskPanel::renderAnswer(const QJsonObject &task)
{
    const bool waiting = m_model && m_model->board(m_project).role(task.value(QStringLiteral("stage")).toString()) == QLatin1String("needs_human");
    m_answerBox->setVisible(waiting && !m_gone);
    if (!waiting) return;
    const QString asked = question(task);
    const QString shown = asked.isEmpty() ? (m_loaded ? tr("The question is in the journal below.") : tr("Loading the question…")) : asked;
    if (m_question->markdownSource() != shown) renderMarkdown(m_question, shown);
}

void TaskPanel::renderPrompt()
{
    const QJsonObject task = current();
    const QString saved = task.value(QStringLiteral("prompt")).toString();
    // A draft that equals what is saved now has nothing left to save.
    if (m_drafts.contains(m_uuid) && m_drafts.value(m_uuid).text == saved && m_loaded) m_drafts.remove(m_uuid);
    const QString text = !m_loaded ? QString() : m_drafts.contains(m_uuid) ? m_drafts.value(m_uuid).text : saved;
    m_prompt->setPlaceholderText(m_loaded ? QString() : tr("Loading the prompt…"));
    m_prompt->setReadOnly(!m_loaded || m_gone || task.value(QStringLiteral("read_only")).toBool());
    if (m_prompt->toPlainText() != text) {
        m_settingPrompt = true;
        m_prompt->setPlainText(text);
        m_settingPrompt = false;
    }
    updatePromptControls();
}

void TaskPanel::updatePromptControls()
{
    const bool draft = !m_uuid.isEmpty() && m_drafts.contains(m_uuid);
    const QString text = m_prompt->toPlainText();
    const qsizetype bytes = text.toUtf8().size();
    const bool empty = text.trimmed().isEmpty(), tooLong = bytes > MaxPromptBytes;
    m_promptSave->setVisible(draft);
    m_promptDiscard->setVisible(draft);
    m_promptSave->setEnabled(draft && !empty && !tooLong && !m_savingPrompts.contains(m_uuid) && !m_gone);
    const bool conflict = promptConflict();
    QString note;
    if (draft && tooLong) note = tr("The prompt is longer than 64 KiB (%1 KiB): shorten it to save it.").arg((bytes + 1023) / 1024);
    else if (draft && empty) note = tr("Write the task's prompt to save it.");
    else if (conflict) note = tr("The prompt was changed elsewhere since you started editing; saving replaces that version.");
    m_promptNote->setText(note);
    m_promptNote->setVisible(!note.isEmpty());
    if (m_promptNote->property("warning").toBool() != conflict) {
        m_promptNote->setProperty("warning", conflict);
        m_promptNote->style()->unpolish(m_promptNote);
        m_promptNote->style()->polish(m_promptNote);
    }
    m_promptShowCurrent->setVisible(conflict);
    if (!conflict && m_promptShowCurrent->isChecked()) {
        const QSignalBlocker block(m_promptShowCurrent);
        m_promptShowCurrent->setChecked(false);
    }
    m_promptShowCurrent->setText(m_promptShowCurrent->isChecked() ? tr("Hide the current version") : tr("Show the current version"));
    const QString saved = current().value(QStringLiteral("prompt")).toString();
    if (conflict && m_promptCurrent->toPlainText() != saved) m_promptCurrent->setPlainText(saved);
    m_promptCurrent->setVisible(conflict && m_promptShowCurrent->isChecked());
}

bool TaskPanel::promptConflict() const
{
    return !m_uuid.isEmpty() && m_loaded && m_drafts.contains(m_uuid)
        && m_drafts.value(m_uuid).base != current().value(QStringLiteral("prompt")).toString();
}

void TaskPanel::fitPrompt()
{
    const int lines = qBound(4, qCeil(m_prompt->document()->documentLayout()->documentSize().height()), 14);
    const QMargins margins = m_prompt->contentsMargins();
    const int height = lines * m_prompt->fontMetrics().lineSpacing() + 2 * qCeil(m_prompt->document()->documentMargin())
        + margins.top() + margins.bottom() + 2 * m_prompt->frameWidth();
    if (m_prompt->height() != height) m_prompt->setFixedHeight(height);
}

bool TaskPanel::eventFilter(QObject *watched, QEvent *event)
{
    // Wrapping changes with the width.
    if (watched == m_prompt && event->type() == QEvent::Resize) QMetaObject::invokeMethod(this, &TaskPanel::fitPrompt, Qt::QueuedConnection);
    return QWidget::eventFilter(watched, event);
}

void TaskPanel::renderImages(const QJsonObject &task)
{
    const auto attachments = task.value(QStringLiteral("attachments")).toArray();
    const bool editable = !task.value(QStringLiteral("read_only")).toBool() && !m_gone;
    m_imageFlow->clear();
    for (const auto &value : attachments) {
        const auto attachment = value.toObject();
        const QString sha = attachment.value(QStringLiteral("sha256")).toString(), name = attachment.value(QStringLiteral("name")).toString();
        auto *chip = new QWidget;
        chip->setObjectName(QStringLiteral("taskImageChip"));
        chip->setFixedSize(84, 64);
        auto *button = new QToolButton(chip);
        button->setObjectName(QStringLiteral("taskImage"));
        button->setProperty("sha256", sha);
        button->setProperty("name", name);
        button->setGeometry(0, 0, 84, 64);
        button->setIconSize(QSize(76, 56));
        button->setAccessibleName(tr("Open %1").arg(name));
        connect(button, &QToolButton::clicked, this, [this, sha, name] { openImage(sha, name); });
        if (editable) {
            auto *remove = new QToolButton(chip);
            remove->setObjectName(QStringLiteral("taskImageRemove"));
            remove->setProperty("sha256", sha);
            remove->setGeometry(84 - 21, 3, 18, 18);
            remove->setIcon(workspaceIcon(QStringLiteral("close"), m_view->palette().fg));
            remove->setIconSize(QSize(10, 10));
            remove->setAccessibleName(tr("Remove %1").arg(name));
            remove->setToolTip(tr("Remove %1").arg(name));
            connect(remove, &QToolButton::clicked, this, [this, sha, name] { removeImage(sha, name); });
        }
        m_imageFlow->addWidget(chip);
        setImage(button, sha);
        fetchImage(sha);
    }
    m_noImages->setVisible(attachments.isEmpty());
    const bool full = attachments.size() >= MaxImages;
    for (auto *button : {m_imageAdd, m_imagePaste}) {
        button->setVisible(editable);
        button->setEnabled(editable && !full);
        button->setToolTip(full ? tr("A task holds at most %1 images.").arg(MaxImages)
                           : button == m_imageAdd ? tr("Add image files, up to 10 MiB each") : tr("Add the image on the clipboard"));
    }
}

void TaskPanel::setImage(QToolButton *button, const QString &sha256)
{
    const QString name = button->property("name").toString();
    const QString path = m_images.value(sha256);
    if (!path.isEmpty()) {
        if (!m_thumbnails.contains(sha256)) m_thumbnails.insert(sha256, QPixmap::fromImage(AttachmentLoader::thumbnail(path)));
        const QPixmap thumbnail = m_thumbnails.value(sha256);
        button->setToolTip(tr("%1 — click to open").arg(name));
        if (!thumbnail.isNull()) {
            button->setIcon(QIcon(thumbnail));
            button->setText({});
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            return;
        }
        button->setText(QStringLiteral("?"));
        button->setToolTip(tr("%1 is not a picture this computer can show").arg(name));
        return;
    }
    const QString error = m_imageErrors.value(sha256);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setText(error.isEmpty() ? QStringLiteral("…") : QStringLiteral("!"));
    button->setToolTip(error.isEmpty() ? tr("%1 — loading").arg(name) : tr("%1: %2").arg(name, error));
}

void TaskPanel::fetchImage(const QString &sha256)
{
    if (!m_model || m_images.contains(sha256) || m_fetching.contains(sha256) || m_imageErrors.contains(sha256)) return;
    // The path `attachment` names is on the machine whose replica is read.
    if (!m_model->host().isEmpty()) {
        m_imageErrors.insert(sha256, tr("the image is stored on %1").arg(m_model->host()));
        return;
    }
    m_fetching.insert(sha256);
    m_model->read(QStringLiteral("attachment"), {{QStringLiteral("task"), m_uuid}, {QStringLiteral("sha256"), sha256}},
                  [this, sha256](const OvermindModel::Result &result) {
                      m_fetching.remove(sha256);
                      const QString path = result.reply.value(QStringLiteral("path")).toString();
                      if (result.ok && !path.isEmpty()) m_images.insert(sha256, path);
                      else m_imageErrors.insert(sha256, result.refusal.value(QStringLiteral("message")).toString());
                      for (auto *button : findChildren<QToolButton *>(QStringLiteral("taskImage")))
                          if (button->property("sha256").toString() == sha256) setImage(button, sha256);
                  }, this);
}

// The store keeps images by hash, without a name, and an agent chooses the
// name: only bytes that decode as a picture are opened, from a copy named for
// their real format, so a name such as notes.command never reaches the desktop.
void TaskPanel::openImage(const QString &sha256, const QString &name)
{
    const QString path = m_images.value(sha256);
    if (path.isEmpty()) {
        const QString error = m_imageErrors.value(sha256);
        m_view->notify(error.isEmpty() ? tr("%1 is still loading.").arg(name) : tr("Cannot open %1: %2").arg(name, error));
        return;
    }
    QByteArray bytes;
    QString error;
    if (!readFile(path, MaxImageBytes, &bytes, &error)) {
        m_view->notify(tr("Cannot open %1: %2").arg(name, error));
        return;
    }
    if (AttachmentFiles::imageFormat(bytes).isEmpty()) {
        m_view->notify(tr("%1 is not a picture this computer can show, so it is not opened.").arg(name));
        return;
    }
    const QString copy = AttachmentFiles::storeImage(name, bytes);
    if (copy.isEmpty()) {
        m_view->notify(tr("Cannot open %1: its copy could not be saved.").arg(name));
        return;
    }
    if (!openUrl || !openUrl(QUrl::fromLocalFile(copy))) m_view->notify(tr("No application could open %1.").arg(name));
}

void TaskPanel::addImageFiles(const QStringList &paths)
{
    QList<Image> added;
    for (const auto &path : paths) {
        const QString fileName = QFileInfo(path).fileName();
        Image image{fileName, {}, {}};
        QString error;
        if (!readFile(path, MaxImageBytes, &image.bytes, &error)) {
            m_view->notify(tr("Cannot add %1: %2.").arg(fileName, error));
            return;
        }
        if (AttachmentFiles::imageFormat(image.bytes).isEmpty()) {
            m_view->notify(tr("%1 is not a picture.").arg(image.name));
            return;
        }
        added.append(image);
    }
    changeImages(added, {});
}

void TaskPanel::pasteImage()
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (data && data->hasImage()) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        QByteArray bytes;
        QBuffer buffer(&bytes);
        if (!image.isNull() && buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
            const QString name = QStringLiteral("pasted-%1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
            changeImages({Image{name, {}, bytes}}, {});
            return;
        }
    }
    QStringList files;
    if (data)
        for (const auto &url : data->urls())
            if (url.isLocalFile()) files << url.toLocalFile();
    if (!files.isEmpty()) {
        addImageFiles(files);
        return;
    }
    m_view->notify(tr("The clipboard holds no image."));
}

void TaskPanel::removeImage(const QString &sha256, const QString &name)
{
    if (m_uuid.isEmpty()) return;
    const QString uuid = m_uuid;
    if (!TasksView::confirm(this, tr("Remove the image"), tr("Remove %1 from %2?").arg(name, ref()), tr("Remove")) || uuid != m_uuid) return;
    changeImages({}, sha256, name);
}

// `edit {attachments}` takes the whole new list: an image the task keeps is
// named by its digest and stays as stored, a new one carries its bytes. The
// CLI's limits apply to that list; they are checked here first.
void TaskPanel::changeImages(const QList<Image> &added, const QString &removed, const QString &removedName)
{
    if (m_uuid.isEmpty() || !m_model) return;
    QJsonArray list;
    qint64 total = 0;
    for (const auto &value : current().value(QStringLiteral("attachments")).toArray()) {
        const auto attachment = value.toObject();
        const QString sha = attachment.value(QStringLiteral("sha256")).toString();
        if (sha == removed) continue;
        total += attachment.value(QStringLiteral("bytes")).toInteger();
        list.append(QJsonObject{{QStringLiteral("sha256"), sha}});
    }
    for (const auto &image : added) {
        QString mime = QMimeDatabase().mimeTypeForData(image.bytes).name();
        if (!mime.startsWith(QLatin1String("image/"))) mime = QStringLiteral("image/") + QString::fromLatin1(AttachmentFiles::imageFormat(image.bytes));
        total += image.bytes.size();
        list.append(QJsonObject{{QStringLiteral("name"), image.name}, {QStringLiteral("mime"), mime},
                                {QStringLiteral("data_base64"), QString::fromLatin1(image.bytes.toBase64())}});
    }
    if (list.size() > MaxImages) {
        m_view->notify(tr("A task holds at most %1 images.").arg(MaxImages));
        return;
    }
    if (total > MaxImagesBytes) {
        m_view->notify(tr("The images of a task are limited to 20 MiB in total."));
        return;
    }
    const QString done = removed.isEmpty() ? tr("Added %n image(s) to %1.", nullptr, int(added.size())).arg(ref())
                                           : tr("Removed %1 from %2.").arg(removedName, ref());
    write(QStringLiteral("edit"), {{QStringLiteral("task"), m_uuid}, {QStringLiteral("attachments"), list}}, {}, done);
}

void TaskPanel::renderPin(const QJsonObject &task)
{
    const QString current = task.value(QStringLiteral("pinned_current")).toString();
    const QString pinned = current.isEmpty() ? nameOf(task.value(QStringLiteral("pinned"))) : current;
    m_pinned->setText(pinned.isEmpty() ? tr("Not pinned") : tr("Pinned to %1").arg(pinned));
    m_unpin->setVisible(!pinned.isEmpty());
}

void TaskPanel::renderLabels(const QJsonObject &task)
{
    const bool editable = !task.value(QStringLiteral("read_only")).toBool() && !m_gone;
    m_labelFlow->clear();
    const auto labels = task.value(QStringLiteral("labels")).toArray();
    for (const auto &value : labels) {
        const QString label = value.toString();
        auto *chip = new QFrame;
        chip->setObjectName(QStringLiteral("taskLabelChip"));
        chip->setProperty("tone", label == QLatin1String("blocked") ? "warning" : label == QLatin1String("bug") ? "danger" : "");
        auto *row = new QHBoxLayout(chip);
        row->setContentsMargins(0, 0, 2, 0);
        row->setSpacing(0);
        auto *text = new QLabel(label);
        text->setObjectName(QStringLiteral("taskLabel"));
        text->setTextFormat(Qt::PlainText);
        row->addWidget(text);
        if (editable && !ReservedLabels.contains(label)) {
            auto *remove = new QToolButton;
            remove->setObjectName(QStringLiteral("taskLabelRemove"));
            remove->setProperty("label", label);
            remove->setIcon(workspaceIcon(QStringLiteral("close"), m_view->palette().muted));
            remove->setIconSize(QSize(10, 10));
            remove->setFixedSize(18, 18);
            remove->setToolTip(tr("Remove the label %1").arg(label));
            remove->setAccessibleName(tr("Remove the label %1").arg(label));
            connect(remove, &QToolButton::clicked, this, [this, label] {
                write(QStringLiteral("label"), {{QStringLiteral("task"), m_uuid}, {QStringLiteral("label"), label}, {QStringLiteral("present"), false}});
            });
            row->addWidget(remove);
        } else {
            row->addSpacing(6);
        }
        m_labelFlow->addWidget(chip);
    }
    m_labelsBox->setVisible(!labels.isEmpty());
    m_labelAdd->setEnabled(editable);
}

void TaskPanel::renderRelations(const QJsonObject &task)
{
    const bool editable = !task.value(QStringLiteral("read_only")).toBool() && !m_gone;
    clearLayout(m_relations);
    QList<QPair<QString, QStringList>> groups;
    const QString parent = task.value(QStringLiteral("parent")).toString();
    if (!parent.isEmpty()) groups.append({QStringLiteral("parent"), {parent}});
    for (const auto &kind : {QStringLiteral("follows"), QStringLiteral("related")}) {
        QStringList targets;
        for (const auto &value : task.value(kind).toArray()) targets << value.toString();
        if (!targets.isEmpty()) groups.append({kind, targets});
    }
    QSet<QString> onBoard;
    if (m_model) for (const auto &other : m_model->board(m_project).tasks) onBoard.insert(other.uuid);
    for (const auto &group : std::as_const(groups)) {
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(6);
        auto *kind = new QLabel(group.first == QLatin1String("parent") ? tr("Parent") : group.first == QLatin1String("follows") ? tr("Follows") : tr("Related"));
        kind->setObjectName(QStringLiteral("taskRelationKindLabel"));
        kind->setFixedWidth(64);
        line->addWidget(kind, 0, Qt::AlignTop);
        auto *chips = new QWidget;
        auto *flow = new TaskFlow(chips, 4);
        for (const auto &target : group.second) {
            auto *chip = new QWidget;
            auto *chipLine = new QHBoxLayout(chip);
            chipLine->setContentsMargins(0, 0, 0, 0);
            chipLine->setSpacing(0);
            auto *open = new QPushButton(refOf(target));
            open->setObjectName(QStringLiteral("taskRelation"));
            open->setProperty("relation", group.first);
            open->setProperty("target", target);
            open->setAutoDefault(false);
            open->setCursor(Qt::PointingHandCursor);
            open->setEnabled(onBoard.contains(target));
            open->setToolTip(onBoard.contains(target) ? tr("Open %1").arg(refOf(target)) : tr("Not on this board"));
            connect(open, &QPushButton::clicked, this, [this, target] { m_view->openTask(target); });
            chipLine->addWidget(open);
            if (editable) {
                auto *remove = new QToolButton;
                remove->setObjectName(QStringLiteral("taskRelationRemove"));
                remove->setProperty("relation", group.first);
                remove->setProperty("target", target);
                remove->setIcon(workspaceIcon(QStringLiteral("close"), m_view->palette().muted));
                remove->setIconSize(QSize(10, 10));
                remove->setFixedSize(18, 18);
                remove->setToolTip(tr("Remove this relation"));
                remove->setAccessibleName(tr("Remove the relation to %1").arg(refOf(target)));
                const QString relation = group.first;
                connect(remove, &QToolButton::clicked, this, [this, relation, target] {
                    write(QStringLiteral("link"), {{QStringLiteral("task"), m_uuid}, {QStringLiteral("relation"), relation},
                                                   {QStringLiteral("target"), target}, {QStringLiteral("present"), false}});
                });
                chipLine->addWidget(remove);
            }
            flow->addWidget(chip);
        }
        line->addWidget(chips, 1);
        m_relations->addWidget(row);
    }
    if (groups.isEmpty()) {
        auto *none = new QLabel(tr("No relations"));
        none->setObjectName(QStringLiteral("taskPanelMeta"));
        m_relations->addWidget(none);
    }
    m_relationKind->setEnabled(editable);
    m_relationTarget->setEnabled(editable);
    m_relationAdd->setEnabled(editable);
}

void TaskPanel::renderJournal(const QJsonObject &task)
{
    const auto journal = task.value(QStringLiteral("journal")).toArray();
    m_journalTitle->setText(journal.isEmpty() ? tr("Journal") : tr("Journal (%1)").arg(journal.size()));
    if (journal == m_journal && m_journalTask == m_uuid && m_journalLayout->count() > 0) return;
    m_journal = journal;
    m_journalTask = m_uuid;
    clearLayout(m_journalLayout);
    const QLocale locale;
    for (const auto &value : journal) {
        const auto entry = value.toObject();
        auto *item = new QWidget;
        item->setObjectName(QStringLiteral("taskJournalEntry"));
        auto *column = new QVBoxLayout(item);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(2);
        QStringList parts{entry.value(QStringLiteral("marker")).toString(), author(entry.value(QStringLiteral("actor")).toObject())};
        const qint64 at = entry.value(QStringLiteral("at")).toInteger();
        if (at > 0) parts << locale.toString(QDateTime::fromSecsSinceEpoch(at).toLocalTime(), QStringLiteral("d MMM HH:mm"));
        auto *header = new QLabel(parts.join(QStringLiteral(" · ")));
        header->setObjectName(QStringLiteral("taskJournalHeader"));
        header->setTextFormat(Qt::PlainText);
        column->addWidget(header);
        const QString text = entry.value(QStringLiteral("text")).toString();
        if (!text.trimmed().isEmpty()) {
            auto *body = new MarkdownText;
            body->setObjectName(QStringLiteral("taskJournalText"));
            connect(body, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
                if ((url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")) && openUrl) openUrl(url);
            });
            renderMarkdown(body, text);
            column->addWidget(body);
        }
        m_journalLayout->addWidget(item);
    }
    if (journal.isEmpty() && m_loaded) {
        auto *none = new QLabel(tr("Nothing recorded yet."));
        none->setObjectName(QStringLiteral("taskPanelMeta"));
        m_journalLayout->addWidget(none);
    }
}

void TaskPanel::renderMarkdown(MarkdownText *text, const QString &markdown)
{
    const auto &c = m_view->palette();
    auto theme = MarkdownTheme::github(m_dark, c.column, 13 / 14.0);
    theme.fg = c.fg;
    // Web links open on a click; anything else is shown as text.
    text->setMarkdownSource(markdown, theme, [](const QString &href) -> MarkdownLink {
        const QUrl url(href);
        if (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http")) return {href, {}, {}};
        return {};
    });
}

void TaskPanel::updateActions()
{
    if (m_uuid.isEmpty() || !m_model) return;
    const QJsonObject task = current();
    const auto &board = m_model->board(m_project);
    const QString stage = task.value(QStringLiteral("stage")).toString(), role = board.role(stage);
    const auto labels = task.value(QStringLiteral("labels")).toArray();
    const bool editable = !task.value(QStringLiteral("read_only")).toBool() && !m_gone;
    const bool epic = labels.contains(QStringLiteral("epic")), busy = m_view->isDelivering(m_uuid);
    const QString busyTip = tr("A delivery of this task is on its way.");

    m_send->setVisible(!m_gone && claimable(board, stage));
    m_send->setEnabled(editable && !epic && !busy);
    m_send->setToolTip(epic ? tr("An epic is never sent.") : busy ? busyTip : tr("Claim the task for a session and type it there"));
    m_review->setVisible(!m_gone && role == QLatin1String("review"));
    m_review->setEnabled(editable && !epic && !busy);
    m_review->setToolTip(busy ? busyTip : tr("Ask another session to review the work"));
    m_rework->setVisible(!m_gone && labels.contains(QStringLiteral("review-failed")) && !nameOf(task.value(QStringLiteral("assignee"))).isEmpty());
    m_rework->setEnabled(editable && !busy);
    const auto targets = m_view->moveTargets(m_uuid);
    QString doneStage;
    for (const auto &candidate : board.stages)
        if (candidate.role == QLatin1String("done")) {
            doneStage = candidate.name;
            break;
        }
    bool canFinish = false;
    for (const auto &target : targets) canFinish |= target.name == doneStage;
    m_done->setVisible(!m_gone && !doneStage.isEmpty() && role != QLatin1String("done") && canFinish);
    m_done->setEnabled(editable);
    m_move->setVisible(!m_gone);
    m_move->setEnabled(editable && !targets.isEmpty());
    scheduleMenus();
    m_pin->setEnabled(editable);
    m_unpin->setEnabled(editable);
    m_delivered->setEnabled(editable && !busy);
    m_notDelivered->setEnabled(editable && !busy);
    m_answerSend->setEnabled(editable && !busy && !m_answer->toPlainText().trimmed().isEmpty());
    m_answer->setReadOnly(!editable);
    m_delete->setVisible(!m_gone);
    m_delete->setEnabled(editable);
}

void TaskPanel::scheduleMenus()
{
    if (m_menusPending) return;
    m_menusPending = true;
    QMetaObject::invokeMethod(this, [this] {
        m_menusPending = false;
        if (m_uuid.isEmpty()) return;
        // A menu on screen keeps its entries.
        if (!m_move->menu()->isVisible()) fillMoves();
        for (auto *button : {m_send, m_review, m_pin})
            if (!button->menu()->isVisible())
                fillSessions(button->menu(), button == m_send ? Purpose::Send : button == m_review ? Purpose::Review : Purpose::Pin);
    }, Qt::QueuedConnection);
}

void TaskPanel::fillMoves()
{
    auto *menu = m_move->menu();
    menu->clear();
    const QString uuid = m_uuid;
    if (uuid.isEmpty()) return;
    for (const auto &target : m_view->moveTargets(uuid)) {
        auto *action = menu->addAction(target.name);
        action->setData(target.name);
        const QString name = target.name;
        // A move may ask first: it runs after the menu, not inside its signal.
        connect(action, &QAction::triggered, this, [this, uuid, name] { m_view->moveTask(uuid, name); }, Qt::QueuedConnection);
    }
}

// The sessions of the project, with their state. A session whose computer
// Overmind does not know yet is shown but cannot be chosen; review skips the
// session that did the work.
void TaskPanel::fillSessions(QMenu *menu, Purpose purpose)
{
    menu->clear();
    const QString uuid = m_uuid;
    if (uuid.isEmpty()) return;
    const QJsonObject task = current();
    const auto recipients = m_view->recipients();
    if (recipients.isEmpty()) menu->addAction(tr("No sessions in this project"))->setEnabled(false);
    for (const auto &recipient : recipients) {
        QString text = recipient.session.name;
        bool enabled = !recipient.session.machine.isEmpty();
        if (!enabled) text += QStringLiteral(" · ") + tr("its computer is not known yet");
        else if (purpose == Purpose::Review && sameSession(task.value(QStringLiteral("assignee")), recipient.session)) {
            text += QStringLiteral(" · ") + tr("did the work");
            enabled = false;
        } else if (!recipient.status.isEmpty()) {
            text += QStringLiteral(" · ") + recipient.status;
        }
        auto *action = menu->addAction(text);
        action->setData(recipient.key);
        action->setEnabled(enabled);
        if (purpose == Purpose::Pin) {
            action->setCheckable(true);
            action->setChecked(sameSession(task.value(QStringLiteral("pinned")), recipient.session));
        }
        const OvermindModel::Session session = recipient.session;
        connect(action, &QAction::triggered, this, [this, uuid, session, purpose] {
            if (uuid != m_uuid) return;
            const QJsonObject target{{QStringLiteral("machine"), session.machine}, {QStringLiteral("name"), session.name}};
            if (purpose == Purpose::Pin) {
                write(QStringLiteral("pin"), {{QStringLiteral("task"), uuid}, {QStringLiteral("session"), target}});
                return;
            }
            if (m_view->isDelivering(uuid)) return;
            if (purpose == Purpose::Send)
                write(QStringLiteral("send"), {{QStringLiteral("task"), uuid}, {QStringLiteral("session"), target}}, session.name,
                      tr("Sent %1 to %2.").arg(ref(), session.name));
            else
                write(QStringLiteral("review-send"), {{QStringLiteral("task"), uuid}, {QStringLiteral("session"), target}}, session.name,
                      tr("Sent %1 to %2 for review.").arg(ref(), session.name));
        });
    }
}

void TaskPanel::write(const QString &command, QJsonObject request, const QString &session, const QString &done, OvermindModel::Callback after)
{
    if (m_uuid.isEmpty()) return;
    m_view->perform({command, std::move(request), m_uuid, ref(), session, done}, std::move(after));
}

void TaskPanel::commitTitle(bool force)
{
    if (m_uuid.isEmpty() || !(m_title->isModified() || force) || m_title->isReadOnly()) return;
    m_title->setModified(false);
    const QString uuid = m_uuid, title = m_title->text().trimmed(), saved = current().value(QStringLiteral("title")).toString();
    if (title.isEmpty() || title == saved) {
        m_titleDrafts.remove(uuid);
        m_title->setText(saved);
        return;
    }
    // Kept, like a prompt draft, until it is saved: a refusal leaves it in the field.
    m_titleDrafts.insert(uuid, title);
    write(QStringLiteral("edit"), {{QStringLiteral("task"), uuid}, {QStringLiteral("title"), title}}, {}, {},
          [this, uuid, title](const OvermindModel::Result &result) {
              if (result.ok && m_titleDrafts.value(uuid) == title) m_titleDrafts.remove(uuid);
              if (uuid != m_uuid) return;
              if (result.ok) m_detail.insert(QStringLiteral("title"), title);
              if (!m_title->isModified()) m_title->setText(m_titleDrafts.value(uuid, current().value(QStringLiteral("title")).toString()));
          });
}

void TaskPanel::savePrompt()
{
    if (m_uuid.isEmpty() || !m_drafts.contains(m_uuid) || !m_promptSave->isEnabled()) return;
    const QString uuid = m_uuid, text = m_drafts.value(uuid).text;
    if (promptConflict()
        && (!TasksView::confirm(this, tr("Replace the prompt"),
                                tr("The prompt of %1 was changed elsewhere since you started editing. Save your version in its place?").arg(ref()),
                                tr("Replace"))
            || uuid != m_uuid || !m_drafts.contains(uuid)))
        return;
    m_savingPrompts.insert(uuid);
    updatePromptControls();
    write(QStringLiteral("edit"), {{QStringLiteral("task"), uuid}, {QStringLiteral("prompt"), text}}, {}, {},
          [this, uuid, text](const OvermindModel::Result &result) {
              m_savingPrompts.remove(uuid);
              if (result.ok && m_drafts.value(uuid).text == text) m_drafts.remove(uuid);
              if (uuid != m_uuid) return;
              if (result.ok) m_detail.insert(QStringLiteral("prompt"), text);
              renderPrompt();
          });
}

void TaskPanel::discardPrompt()
{
    if (m_uuid.isEmpty() || !m_drafts.contains(m_uuid)) return;
    const QString uuid = m_uuid;
    if (!TasksView::confirm(this, tr("Discard changes"), tr("Discard your changes to the prompt of %1?").arg(ref()), tr("Discard"))) return;
    m_drafts.remove(uuid);
    if (uuid == m_uuid) renderPrompt();
}

void TaskPanel::sendAnswer()
{
    const QString text = m_answer->toPlainText();
    if (m_uuid.isEmpty() || text.trimmed().isEmpty() || m_view->isDelivering(m_uuid)) return;
    const QString uuid = m_uuid, name = nameOf(current().value(QStringLiteral("assignee")));
    write(QStringLiteral("answer"), {{QStringLiteral("task"), uuid}, {QStringLiteral("answer"), text}}, name,
          tr("Sent your answer on %1 to %2.").arg(ref(), name.isEmpty() ? tr("its session") : name),
          [this, uuid, text](const OvermindModel::Result &result) {
              if (!result.ok) return;
              if (m_answers.value(uuid) == text) m_answers.remove(uuid);
              if (uuid == m_uuid && m_answer->toPlainText() == text) m_answer->clear();
          });
}

void TaskPanel::resolveDelivery(bool delivered)
{
    if (m_uuid.isEmpty() || m_view->isDelivering(m_uuid)) return;
    const QString uuid = m_uuid, reference = ref();
    if (!delivered
        && !TasksView::confirm(this, tr("Not delivered"),
                               tr("Record that %1 did not arrive? A send goes back to the queue and a review request is withdrawn; "
                                  "an answer or rework stays recorded.").arg(reference),
                               tr("Not delivered")))
        return;
    if (uuid != m_uuid) return;
    write(QStringLiteral("resolve-delivery"), {{QStringLiteral("task"), uuid}, {QStringLiteral("delivered"), delivered}}, {},
          delivered ? tr("Recorded that %1 was delivered.").arg(reference) : tr("Recorded that %1 was not delivered.").arg(reference));
}

void TaskPanel::deleteTask()
{
    if (m_uuid.isEmpty()) return;
    const QString uuid = m_uuid, reference = ref(), title = current().value(QStringLiteral("title")).toString();
    if (!TasksView::confirm(this, tr("Delete %1").arg(reference),
                            tr("Delete %1 “%2”? It disappears from the board on every machine.").arg(reference, title), tr("Delete")))
        return;
    if (uuid != m_uuid) return;
    write(QStringLiteral("delete"), {{QStringLiteral("task"), uuid}}, {}, tr("Deleted %1.").arg(reference), [this, uuid](const OvermindModel::Result &result) {
        if (result.ok && uuid == m_uuid) m_view->closeTask();
    });
}

void TaskPanel::addLabel()
{
    const QString label = m_labelAdd->text().trimmed();
    if (m_uuid.isEmpty() || label.isEmpty()) return;
    const QString uuid = m_uuid;
    m_labelAdd->clear();
    write(QStringLiteral("label"), {{QStringLiteral("task"), uuid}, {QStringLiteral("label"), label}, {QStringLiteral("present"), true}}, {}, {},
          [this, uuid, label](const OvermindModel::Result &result) {
              if (!result.ok && uuid == m_uuid && m_labelAdd->text().isEmpty()) m_labelAdd->setText(label);
          });
}

void TaskPanel::addRelation()
{
    const QString target = m_relationTarget->text().trimmed();
    if (m_uuid.isEmpty() || target.isEmpty()) return;
    const QString uuid = m_uuid;
    m_relationTarget->clear();
    write(QStringLiteral("link"), {{QStringLiteral("task"), uuid}, {QStringLiteral("relation"), m_relationKind->currentData().toString()},
                                   {QStringLiteral("target"), target}, {QStringLiteral("present"), true}}, {}, {},
          [this, uuid, target](const OvermindModel::Result &result) {
              if (!result.ok && uuid == m_uuid && m_relationTarget->text().isEmpty()) m_relationTarget->setText(target);
          });
}

void TaskPanel::setTheme(bool dark)
{
    m_dark = dark;
    applyStyle();
    // Markdown carries its colors: draw it again.
    m_journal = {};
    if (!m_question->markdownSource().isEmpty()) renderMarkdown(m_question, m_question->markdownSource());
    if (!m_uuid.isEmpty()) render();
}

void TaskPanel::applyStyle()
{
    const auto &c = m_view->palette();
    const auto rgba = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return color.name(QColor::HexArgb);
    };
    QString style = QStringLiteral(R"(
        QWidget#taskPanel { background:{column}; border:1px solid {border}; border-radius:10px; }
        QScrollArea#taskPanelScroll, QWidget#taskPanelContent, QWidget#taskJournal, QWidget#taskImages, QWidget#taskLabels { background:transparent; border:0; }
        MarkdownText { background:transparent; border:0; }
        QLabel#taskPanelRef { color:{muted}; font-weight:600; }
        QLabel#taskPanelStage { color:{accent}; background:{accentTint}; border-radius:5px; padding:1px 7px; font-weight:600; }
        QLineEdit#taskTitle { color:{fg}; background:transparent; border:1px solid transparent; border-radius:6px; padding:3px 4px; font-size:15px; font-weight:600; }
        QLineEdit#taskTitle:hover { border-color:{border}; }
        QLineEdit#taskTitle:focus { border-color:{accent}; background:{surface}; }
        QLabel#taskPanelMeta, QLabel#taskPromptNote, QLabel#taskJournalHeader, QLabel#taskRelationKindLabel { color:{muted}; font-size:12px; }
        QLabel#taskPromptNote[warning="true"] { color:{warning}; font-weight:600; }
        QPlainTextEdit#taskPromptCurrent { color:{muted}; background:{column}; border:1px dashed {border}; border-radius:6px; padding:4px; }
        QToolButton#taskImageRemove { background:{surface}; border:1px solid {border}; border-radius:9px; padding:0; }
        QToolButton#taskImageRemove:hover { border-color:{danger}; }
        QLabel#taskPanelStatus { color:{muted}; font-size:12px; }
        QLabel#taskPanelStatus[error="true"] { color:{danger}; }
        QLabel#taskPanelHeading { color:{muted}; font-size:11px; font-weight:600; }
        QFrame#taskDelivery, QFrame#taskAnswerBox { background:{warningFill}; border:1px solid {warningBorder}; border-radius:8px; }
        QLabel#taskDeliveryText, QLabel#taskAnswerTitle { color:{warning}; font-weight:600; }
        QLabel#taskNote { color:{warning}; font-size:12px; }
        QPlainTextEdit#taskPrompt, QPlainTextEdit#taskAnswer { color:{fg}; background:{surface}; border:1px solid {border}; border-radius:6px; padding:4px; }
        QPlainTextEdit#taskPrompt:focus, QPlainTextEdit#taskAnswer:focus { border-color:{accent}; }
        QFrame#taskLabelChip { background:{chip}; border-radius:5px; }
        QFrame#taskLabelChip[tone="warning"] { background:{warningFill}; }
        QFrame#taskLabelChip[tone="danger"] { background:{dangerTint}; }
        QLabel#taskLabel { color:{fg}; padding:2px 2px 2px 7px; font-size:12px; }
        QToolButton#taskLabelRemove, QToolButton#taskRelationRemove { background:transparent; border:0; border-radius:4px; padding:0; }
        QToolButton#taskLabelRemove:hover, QToolButton#taskRelationRemove:hover { background:{hover}; }
        QPushButton#taskRelation { color:{accent}; background:transparent; border:0; padding:1px 3px; min-height:0; font-weight:600; }
        QPushButton#taskRelation:disabled { color:{muted}; }
        QToolButton#taskImage { color:{muted}; background:{surface}; border:1px solid {border}; border-radius:6px; }
        QToolButton#taskImage:hover { border-color:{accent}; }
        QPushButton#taskSend, QPushButton#taskReviewSend, QPushButton#taskRework, QPushButton#taskDone, QPushButton#taskMove,
        QPushButton#taskPin, QPushButton#taskUnpin, QPushButton#taskPromptSave, QPushButton#taskPromptDiscard, QPushButton#taskRelationAdd,
        QPushButton#taskDelivered, QPushButton#taskNotDelivered, QPushButton#taskAnswerSend, QPushButton#taskDelete,
        QPushButton#taskImageAdd, QPushButton#taskImagePaste, QPushButton#taskPromptShowCurrent { padding:5px 10px; min-height:16px; }
        QPushButton#taskSend::menu-indicator, QPushButton#taskReviewSend::menu-indicator, QPushButton#taskMove::menu-indicator,
        QPushButton#taskPin::menu-indicator { image:none; width:0; }
        QPushButton#taskDelete { color:{danger}; }
    )");
    const QList<QPair<QString, QString>> tokens{
        {QStringLiteral("{fg}"), c.fg.name()}, {QStringLiteral("{muted}"), c.muted.name()}, {QStringLiteral("{border}"), c.border.name()},
        {QStringLiteral("{accent}"), c.accent.name()}, {QStringLiteral("{column}"), c.column.name()}, {QStringLiteral("{surface}"), c.surface.name()},
        {QStringLiteral("{hover}"), c.hover.name()}, {QStringLiteral("{chip}"), c.chip.name()}, {QStringLiteral("{warning}"), c.warning.name()},
        {QStringLiteral("{warningFill}"), c.warningFill.name()}, {QStringLiteral("{warningBorder}"), c.warningBorder.name()},
        {QStringLiteral("{danger}"), c.danger.name()}, {QStringLiteral("{dangerTint}"), rgba(c.danger, 30)},
        {QStringLiteral("{accentTint}"), rgba(c.accent, 34)}};
    for (const auto &token : tokens) style.replace(token.first, token.second);
    setWorkspaceStyle(this, style);
    m_close->setIcon(workspaceIcon(QStringLiteral("close"), c.muted));
}
