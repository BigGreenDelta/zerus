#pragma once

#include "MarkdownHtml.h"

#include <QTextBrowser>
#include <memory>

// Read-only Markdown sized to its content, for disclosures inside forms. It never
// scrolls: wheel events reach the surrounding scroll area. Links activate only on
// an explicit click (anchorClicked); only drawn hgs-md: decorations load.
class MarkdownText final : public QTextBrowser {
    Q_OBJECT
public:
    explicit MarkdownText(QWidget *parent = nullptr);
    ~MarkdownText() override;
    void setMarkdownSource(const QString &markdown, const MarkdownTheme &theme, const MarkdownLinkPolicy &links);
    QString markdownSource() const { return m_source; }
    // Clicks outside links reach the parent, e.g. to select an option row; text
    // is not selectable then.
    void setClickThrough(bool clickThrough);
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    QMimeData *createMimeDataFromSelection() const override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    bool passesThrough(QMouseEvent *event) const;
    void invalidateHeight();

    QString m_source;
    // A copy laid out at the widths the layout asks about, so measuring never
    // relayouts the visible document.
    mutable std::unique_ptr<QTextDocument> m_measure;
    mutable int m_measuredWidth = -1, m_measuredHeight = 0;
    bool m_clickThrough = false;
};
