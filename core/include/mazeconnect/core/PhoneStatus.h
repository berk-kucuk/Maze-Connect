#pragma once

#include <QJsonObject>
#include <QVariantMap>

namespace mazeconnect::core {

/**
 * A paired phone's reading — battery, storage, memory, network, ringer —
 * turned from what the phone sent into what the UI may draw.
 *
 * The link is authenticated; the phone is not trusted. So nothing from the
 * snapshot is passed through: every field is looked up by name, checked for
 * type and range, and copied into a fresh map only if it passes. A field that
 * fails is *absent*, never a default — a missing battery reading drawn as 0%
 * would tell the user their phone is about to die when nothing of the sort
 * was said.
 *
 * Enumerated values (plug, health, network type, ringer) are matched against
 * fixed tables and anything else is dropped, so a peer cannot put arbitrary
 * words on the dashboard through a field the UI renders verbatim.
 */
namespace phonestatus {

/// Longest model / manufacturer / Android release string kept.
inline constexpr int kMaxLabelChars = 64;

/// Largest byte count believed for storage or memory: 64 TiB. Anything above
/// is not a phone and is dropped rather than drawn as a nonsense ratio.
inline constexpr qint64 kMaxBytes = 64LL * 1024 * 1024 * 1024 * 1024;

/**
 * Validate @p status field by field.
 *
 * Returns an empty map when the object is not a snapshot at all (no section
 * survived). The keys of the result are the same as the wire's, flattened:
 *
 *   batteryLevel (int 0-100), charging (bool), plug (string), batteryTemp
 *   (double °C), batteryHealth (string), storageFree / storageTotal (double
 *   bytes), memoryAvailable / memoryTotal (double bytes), network (string),
 *   signal (int 0-4), metered (bool), ringer (string), dnd (bool), powerSave
 *   (bool), screenOn (bool), model / manufacturer / android (string),
 *   uptimeMs (double)
 *
 * Bytes are doubles because QML numbers are; 64 TiB fits a double exactly.
 */
QVariantMap sanitize(const QJsonObject &status);

} // namespace phonestatus
} // namespace mazeconnect::core
