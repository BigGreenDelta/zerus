#pragma once
#include <QApplication>
#include <QImage>
#include <QMessageBox>
#include <QTimer>

// macOS paints QMessageBox::Question with a legacy translucent dark glyph made
// for light windows, so it disappears on a dark one. Its information, warning
// and critical siblings carry opaque backgrounds and stay readable. Inverting
// the glyph turns its highlight rim into a shadow; the gain gives the body
// enough opacity to read as a light face.
inline QPixmap darkPaletteQuestionGlyph(const QPixmap &glyph)
{
    QImage image = glyph.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = line[x];
            line[x] = qRgba(255 - qRed(c), 255 - qGreen(c), 255 - qBlue(c), qMin(255, qAlpha(c) * 5 / 2));
        }
    }
    QPixmap result = QPixmap::fromImage(image);
    result.setDevicePixelRatio(glyph.devicePixelRatio());
    return result;
}

// Applies the light glyph to every question box shown on a dark palette and
// restores the native one when the palette turns light while it is open.
class MessageBoxIcons : public QObject {
public:
    static void install() {
        if (qApp && !qApp->findChild<QObject *>("messageBoxIcons", Qt::FindDirectChildrenOnly)) new MessageBoxIcons;
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        const auto type = event->type();
        if (type != QEvent::Show && type != QEvent::PaletteChange && type != QEvent::StyleChange) return false;
        auto *box = qobject_cast<QMessageBox *>(object);
        if (!box || (box->icon() != QMessageBox::Question && !box->property(kLightGlyph).toBool())) return false;
        // Show reaches filters before QMessageBox::showEvent sizes the box. A
        // style change resets the native icon in changeEvent, so apply after it.
        if (type == QEvent::Show) apply(box);
        else if (box->isVisible()) QTimer::singleShot(0, box, [box] { apply(box); });
        return false;
    }
private:
    static constexpr const char *kLightGlyph = "lightQuestionGlyph";
    MessageBoxIcons() : QObject(qApp) { setObjectName("messageBoxIcons"); qApp->installEventFilter(this); }
    static void apply(QMessageBox *box) {
        const bool dark = box->palette().color(QPalette::Window).lightness() < 128;
        if (!dark && !box->property(kLightGlyph).toBool()) return;
        // setIcon renders the native glyph at the box's current size and DPR.
        box->setIcon(QMessageBox::Question);
        box->setProperty(kLightGlyph, dark);
        if (dark) box->setIconPixmap(darkPaletteQuestionGlyph(box->iconPixmap()));
    }
};
