#pragma once

#include <QWindow>

/**
 * Asks KWin to blur the desktop behind the window.
 *
 * The panel stays mostly opaque — the blur is there to give the rounded
 * edges depth and to keep the window from looking like a flat cut-out, not
 * to make the interface see-through. Content must never be read against
 * whatever happens to be behind it.
 *
 * Uses KWindowEffects, which speaks the Plasma protocol on Wayland as well
 * as X11; maze-control-center does the same thing through raw X11 atoms,
 * which only works under X.
 */
namespace WindowEffects {

/**
 * Enable blur behind @p window, following its rounded corners, and keep it
 * in step as the window is resized.
 *
 * A no-op wherever the compositor does not offer it, so the app simply
 * renders without blur rather than failing. Applied from C++ rather than
 * exposed to QML: it is a property of the window, not of the scene.
 */
void applyBlur(QWindow *window, int cornerRadius);

} // namespace WindowEffects
