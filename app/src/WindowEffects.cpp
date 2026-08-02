#include "WindowEffects.h"

#include <QPainterPath>
#include <QRegion>
#include <QWindow>

#include <KWindowEffects>

namespace {

void applyOnce(QWindow *window, int cornerRadius) {
    // Blur the rounded shape rather than the bounding box: a square blur
    // region behind a rounded panel shows as four bright corners.
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, window->width(), window->height()),
                        cornerRadius, cornerRadius);
    KWindowEffects::enableBlurBehind(window, true,
                                     QRegion(path.toFillPolygon().toPolygon()));
}

} // namespace

namespace WindowEffects {

void applyBlur(QWindow *window, int cornerRadius) {
    if (window == nullptr) {
        return;
    }
    applyOnce(window, cornerRadius);

    // The region is in window coordinates, so it has to be rebuilt whenever
    // the window is resized or the blur stops matching the panel.
    QObject::connect(window, &QWindow::widthChanged, window,
                     [window, cornerRadius]() { applyOnce(window, cornerRadius); });
    QObject::connect(window, &QWindow::heightChanged, window,
                     [window, cornerRadius]() { applyOnce(window, cornerRadius); });
}

} // namespace WindowEffects
