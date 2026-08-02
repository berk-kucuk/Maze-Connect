#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include "mazeconnect/core/Capabilities.h"

namespace mazeconnect::core {

/// A device the user has explicitly paired with.
struct PairedDevice {
    QString deviceId;
    QString deviceName;
    QString deviceType;
    QByteArray publicKey;   ///< SPKI DER public key — the pinned identity
    QDateTime pairedAt;
    Capabilities enabledCapabilities = defaultEnabledCapabilities();

    bool isValid() const;
    QString fingerprint() const;
};

/**
 * Persistent record of paired devices, and the authority on whether a
 * presented key is trusted.
 *
 * The store is the *only* thing that grants trust. A certificate that
 * chains to nothing, has expired, or names anything at all is irrelevant;
 * what matters is whether its public key is byte-for-byte one we pinned.
 */
class DeviceStore {
public:
    /// @param path  JSON file to persist to. The parent directory is created
    ///              if needed and the file is written with owner-only
    ///              permissions.
    explicit DeviceStore(QString path);

    bool load();
    bool save() const;

    /// Where this store persists to.
    QString path() const { return m_path; }

    QList<PairedDevice> devices() const { return m_devices; }
    int count() const { return m_devices.size(); }

    /**
     * Is this exact public key pinned?
     *
     * Compared in constant time. Not because a timing oracle here is
     * especially practical, but because "compare secrets in constant time"
     * is a rule worth keeping unconditional rather than case-by-case.
     */
    bool isTrusted(const QByteArray &publicKey) const;

    /// Look up by pinned key; returns an invalid device if not paired.
    PairedDevice deviceForKey(const QByteArray &publicKey) const;
    PairedDevice deviceForId(const QString &deviceId) const;

    /**
     * Record a newly paired device.
     *
     * Refuses — returning false — if a *different* key is already pinned for
     * this deviceId. Silently re-pinning would let a peer that learned an id
     * displace the real device, which is exactly the substitution pairing is
     * meant to prevent. Re-pairing requires an explicit remove() first.
     */
    bool add(const PairedDevice &device);

    /// Update mutable metadata (name, enabled capabilities) for an already
    /// pinned key. Never changes the pinned key itself.
    bool update(const QByteArray &publicKey,
                const QString &deviceName,
                Capabilities enabledCapabilities);

    bool remove(const QByteArray &publicKey);
    void clear();

private:
    QString m_path;
    QList<PairedDevice> m_devices;
};

} // namespace mazeconnect::core
