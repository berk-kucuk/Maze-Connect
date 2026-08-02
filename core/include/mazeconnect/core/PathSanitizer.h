#pragma once

#include <QString>

namespace mazeconnect::core {

/**
 * Turns a peer-supplied filename into something safe to create inside the
 * per-device inbox, or rejects it.
 *
 * A filename arriving over the network is fully attacker-controlled. The
 * rules below are deliberately a strict allow-list-shaped filter rather than
 * a blocklist of known-bad sequences: it is the only layer standing between
 * a hostile peer and arbitrary file writes.
 */
class PathSanitizer {
public:
    /**
     * Reduce @p rawName to a bare, safe filename.
     *
     * Rejects (returns an empty QString):
     *  - empty / whitespace-only names
     *  - anything containing a path separator ('/' or '\\')
     *  - "." and ".." in any Unicode normalization
     *  - NUL and other C0/C1 control characters
     *  - names longer than the filename cap
     *  - Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9),
     *    because transfers may land on a shared/exported directory
     *  - leading '-' (would be read as a flag by shell tooling later)
     *
     * The result is always a single path component with no directory part.
     */
    static QString sanitizeFilename(const QString &rawName);

    /**
     * Final confinement check, applied *after* the file exists on disk.
     *
     * Resolves @p candidatePath and @p confinementRoot to canonical absolute
     * paths and verifies the candidate really sits inside the root. This
     * catches what name filtering alone cannot: a symlink already present in
     * the inbox that redirects the write elsewhere, and any case/normalization
     * surprise from the underlying filesystem.
     *
     * Returns false if either path cannot be resolved — an unresolvable path
     * is treated as unsafe, never as "probably fine".
     */
    static bool isWithinRoot(const QString &candidatePath, const QString &confinementRoot);

    /**
     * Pick a non-colliding name inside @p directory for @p sanitizedName,
     * e.g. "report.pdf" -> "report (2).pdf". Never overwrites an existing
     * file, so a peer cannot replace one it previously sent (or one the user
     * happens to have).
     *
     * Returns an empty QString if no free name is found within a bounded
     * number of attempts.
     */
    static QString uniqueNameIn(const QString &directory, const QString &sanitizedName);
};

} // namespace mazeconnect::core
