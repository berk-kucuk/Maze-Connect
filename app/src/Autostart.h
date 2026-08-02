#pragma once

#include <QString>

/**
 * Starting with the session, through a freedesktop autostart entry.
 *
 * On by default, and that is a deliberate choice for this application in
 * particular: a device link that only runs when you remember to open it is a
 * link that is down whenever you actually want it. The phone's own service
 * already starts with the phone; the desktop half doing otherwise is what
 * makes "my dashboard is empty" the normal state.
 *
 * "By default" means **once**. The entry is written the first time the app
 * runs and never rewritten: someone who turns it off, or deletes the file by
 * hand, is not overruled the next time they open the window. That is tracked
 * with its own marker rather than by the entry's existence, because those two
 * are different questions — "has this been decided" and "is it on".
 */
namespace autostart {

/// `~/.config/autostart/maze-connect.desktop`, honouring $XDG_CONFIG_HOME.
QString entryPath();

bool isEnabled();

/// Writes or removes the entry. Returns false if the file could not be
/// written, so the caller can say so rather than show a switch that lies.
bool setEnabled(bool enabled);

/**
 * Turn it on the first time this app is ever run, and never again.
 *
 * Call once at startup. Does nothing on every subsequent launch, whatever
 * the current state.
 */
void applyDefaultOnFirstRun();

} // namespace autostart
