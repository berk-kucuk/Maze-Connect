#include "mazeconnect/core/DeviceStore.h"

#include "mazeconnect/core/Identity.h"
#include "mazeconnect/core/Limits.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace mazeconnect::core {
namespace {

bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) {
        return false;
    }
    quint8 diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff |= static_cast<quint8>(a[i]) ^ static_cast<quint8>(b[i]);
    }
    return diff == 0;
}

} // namespace

bool PairedDevice::isValid() const {
    return !deviceId.isEmpty() && Identity::isPlausiblePublicKey(publicKey);
}

QString PairedDevice::fingerprint() const {
    return QString::fromLatin1(Identity::fingerprintOf(publicKey).toHex());
}

DeviceStore::DeviceStore(QString path) : m_path(std::move(path)) {}

bool DeviceStore::load() {
    m_devices.clear();

    QFile file(m_path);
    if (!file.exists()) {
        return true; // nothing paired yet is a normal state, not an error
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }

    bool migrated = false;
    for (const QJsonValue &value : doc.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();

        PairedDevice device;
        device.deviceId = obj.value(QLatin1StringView("deviceId")).toString();
        device.deviceName = obj.value(QLatin1StringView("deviceName")).toString();
        device.deviceType = obj.value(QLatin1StringView("deviceType")).toString();
        device.publicKey = QByteArray::fromBase64(
            obj.value(QLatin1StringView("publicKey")).toString().toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        device.pairedAt = QDateTime::fromString(
            obj.value(QLatin1StringView("pairedAt")).toString(), Qt::ISODate);

        const QJsonArray caps = obj.value(QLatin1StringView("enabledCapabilities")).toArray();
        QStringList capNames;
        for (const QJsonValue &c : caps) {
            capNames << c.toString();
        }
        device.enabledCapabilities = capabilitiesFromNames(capNames);

        // Capabilities this build has that the record has never heard of are
        // granted, like everything else is at pairing. The enabled set used
        // to be frozen at pairing time, so a feature added later — media
        // control, in 1.2.0 — was switched off for every device paired
        // before it, with nothing anywhere saying so. The record now lists
        // what it knew about, which keeps a capability the user revoked
        // revoked: only ones that are *new to the record* are added.
        Capabilities known = legacyKnownCapabilities();
        const QJsonValue knownValue = obj.value(QLatin1StringView("knownCapabilities"));
        if (knownValue.isArray()) {
            QStringList knownNames;
            for (const QJsonValue &c : knownValue.toArray()) {
                knownNames << c.toString();
            }
            known = capabilitiesFromNames(knownNames);
        }
        const Capabilities added = supportedCapabilities() & ~known;
        if (added) {
            device.enabledCapabilities |= added;
            migrated = true;
        }

        // A record we cannot fully validate is dropped rather than loaded
        // half-trusted — a truncated key must never become a pin.
        if (device.isValid()) {
            m_devices.append(device);
        }
    }
    if (migrated) {
        save(); // so the grant is recorded, and a later revocation sticks
    }
    return true;
}

bool DeviceStore::save() const {
    const QFileInfo info(m_path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    QJsonArray array;
    for (const PairedDevice &device : m_devices) {
        QJsonObject obj;
        obj.insert(QLatin1StringView("deviceId"), device.deviceId);
        obj.insert(QLatin1StringView("deviceName"), device.deviceName);
        obj.insert(QLatin1StringView("deviceType"), device.deviceType);
        obj.insert(QLatin1StringView("publicKey"),
                   QString::fromLatin1(device.publicKey.toBase64()));
        obj.insert(QLatin1StringView("pairedAt"), device.pairedAt.toString(Qt::ISODate));
        obj.insert(QLatin1StringView("enabledCapabilities"),
                   QJsonArray::fromStringList(capabilitiesToNames(device.enabledCapabilities)));
        obj.insert(QLatin1StringView("knownCapabilities"),
                   QJsonArray::fromStringList(capabilitiesToNames(supportedCapabilities())));
        array.append(obj);
    }

    // QSaveFile gives an atomic replace: a crash mid-write leaves the old
    // pin list intact rather than a truncated one.
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(QJsonDocument(array).toJson(QJsonDocument::Indented)) < 0) {
        return false;
    }
    // Pins are not secret, but they are integrity-critical: another local
    // user must not be able to edit which keys we trust.
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    return file.commit();
}

bool DeviceStore::isTrusted(const QByteArray &publicKey) const {
    if (!Identity::isPlausiblePublicKey(publicKey)) {
        return false;
    }
    for (const PairedDevice &device : m_devices) {
        if (constantTimeEquals(device.publicKey, publicKey)) {
            return true;
        }
    }
    return false;
}

PairedDevice DeviceStore::deviceForKey(const QByteArray &publicKey) const {
    if (!Identity::isPlausiblePublicKey(publicKey)) {
        return {};
    }
    for (const PairedDevice &device : m_devices) {
        if (constantTimeEquals(device.publicKey, publicKey)) {
            return device;
        }
    }
    return {};
}

PairedDevice DeviceStore::deviceForId(const QString &deviceId) const {
    for (const PairedDevice &device : m_devices) {
        if (device.deviceId == deviceId) {
            return device;
        }
    }
    return {};
}

bool DeviceStore::add(const PairedDevice &device) {
    if (!device.isValid()) {
        return false;
    }
    for (const PairedDevice &existing : m_devices) {
        if (constantTimeEquals(existing.publicKey, device.publicKey)) {
            return false; // already pinned
        }
        // Same claimed identity, different key: refuse. Overwriting here
        // would let anyone who learns a deviceId take that device's place.
        if (existing.deviceId == device.deviceId) {
            return false;
        }
    }
    m_devices.append(device);
    return save();
}

bool DeviceStore::update(const QByteArray &publicKey,
                         const QString &deviceName,
                         Capabilities enabledCapabilities) {
    for (PairedDevice &device : m_devices) {
        if (constantTimeEquals(device.publicKey, publicKey)) {
            device.deviceName = deviceName;
            device.enabledCapabilities = enabledCapabilities;
            return save();
        }
    }
    return false;
}

bool DeviceStore::remove(const QByteArray &publicKey) {
    for (qsizetype i = 0; i < m_devices.size(); ++i) {
        if (constantTimeEquals(m_devices[i].publicKey, publicKey)) {
            m_devices.removeAt(i);
            return save();
        }
    }
    return false;
}

void DeviceStore::clear() {
    m_devices.clear();
    save();
}

} // namespace mazeconnect::core
