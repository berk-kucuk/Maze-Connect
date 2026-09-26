#include "mazeconnect/core/PhoneStatus.h"

#include <QJsonValue>

#include <cmath>
#include <initializer_list>

namespace mazeconnect::core::phonestatus {
namespace {

bool isClean(const QString &s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)
            || (u >= 0x202A && u <= 0x202E) || (u >= 0x2066 && u <= 0x2069)) {
            return false;
        }
    }
    return true;
}

/// A whole number in [min, max], or false.
bool wholeNumber(const QJsonValue &value, qint64 min, qint64 max, qint64 &out) {
    if (!value.isDouble()) {
        return false;
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < static_cast<double>(min) || raw > static_cast<double>(max)
        || raw != std::floor(raw)) {
        return false;
    }
    out = static_cast<qint64>(raw);
    return true;
}

void copyBool(const QJsonObject &from, QLatin1StringView key, QVariantMap &to,
              const QString &as) {
    const QJsonValue value = from.value(key);
    if (value.isBool()) {
        to.insert(as, value.toBool());
    }
}

void copyEnum(const QJsonObject &from, QLatin1StringView key,
              std::initializer_list<const char *> allowed, QVariantMap &to, const QString &as) {
    const QJsonValue value = from.value(key);
    if (!value.isString()) {
        return;
    }
    const QString s = value.toString();
    for (const char *candidate : allowed) {
        if (s == QLatin1StringView(candidate)) {
            to.insert(as, s);
            return;
        }
    }
}

void copyLabel(const QJsonObject &from, QLatin1StringView key, QVariantMap &to,
               const QString &as) {
    const QJsonValue value = from.value(key);
    if (!value.isString()) {
        return;
    }
    const QString s = value.toString().trimmed();
    if (!s.isEmpty() && s.size() <= kMaxLabelChars && isClean(s)) {
        to.insert(as, s);
    }
}

/// A (free, total) pair, kept only as a pair and only when it makes sense:
/// a free figure larger than the total is a broken reading, not a full disk.
void copyCapacity(const QJsonObject &section, QLatin1StringView freeKey, QVariantMap &to,
                  const QString &freeAs, const QString &totalAs) {
    qint64 free = 0;
    qint64 total = 0;
    if (!wholeNumber(section.value(freeKey), 0, kMaxBytes, free)
        || !wholeNumber(section.value(QLatin1StringView("total")), 1, kMaxBytes, total)
        || free > total) {
        return;
    }
    to.insert(freeAs, static_cast<double>(free));
    to.insert(totalAs, static_cast<double>(total));
}

} // namespace

QVariantMap sanitize(const QJsonObject &status) {
    QVariantMap out;

    const QJsonObject battery = status.value(QLatin1StringView("battery")).toObject();
    if (!battery.isEmpty()) {
        qint64 level = 0;
        if (wholeNumber(battery.value(QLatin1StringView("level")), 0, 100, level)) {
            out.insert(QStringLiteral("batteryLevel"), static_cast<int>(level));
        }
        copyBool(battery, QLatin1StringView("charging"), out, QStringLiteral("charging"));
        copyEnum(battery, QLatin1StringView("plug"), {"ac", "usb", "wireless", "dock", "none"},
                 out, QStringLiteral("plug"));
        copyEnum(battery, QLatin1StringView("health"),
                 {"good", "overheat", "dead", "cold", "overvoltage", "failure", "unknown"}, out,
                 QStringLiteral("batteryHealth"));
        // Tenths of a degree, as Android reports it. -40..+100 °C covers
        // every reading a working phone can produce.
        qint64 tenths = 0;
        const QJsonValue temp = battery.value(QLatin1StringView("temperature"));
        if (temp.isDouble()) {
            const double raw = temp.toDouble();
            if (std::isfinite(raw) && raw >= -400 && raw <= 1000 && raw == std::floor(raw)) {
                tenths = static_cast<qint64>(raw);
                out.insert(QStringLiteral("batteryTemp"), static_cast<double>(tenths) / 10.0);
            }
        }
    }

    copyCapacity(status.value(QLatin1StringView("storage")).toObject(),
                 QLatin1StringView("free"), out, QStringLiteral("storageFree"),
                 QStringLiteral("storageTotal"));
    copyCapacity(status.value(QLatin1StringView("memory")).toObject(),
                 QLatin1StringView("available"), out, QStringLiteral("memoryAvailable"),
                 QStringLiteral("memoryTotal"));

    const QJsonObject network = status.value(QLatin1StringView("network")).toObject();
    if (!network.isEmpty()) {
        copyEnum(network, QLatin1StringView("type"),
                 {"wifi", "cellular", "ethernet", "vpn", "none"}, out, QStringLiteral("network"));
        qint64 signal = 0;
        if (wholeNumber(network.value(QLatin1StringView("signal")), 0, 4, signal)) {
            out.insert(QStringLiteral("signal"), static_cast<int>(signal));
        }
        copyBool(network, QLatin1StringView("metered"), out, QStringLiteral("metered"));
    }

    copyEnum(status, QLatin1StringView("ringer"), {"normal", "vibrate", "silent"}, out,
             QStringLiteral("ringer"));
    copyBool(status, QLatin1StringView("dnd"), out, QStringLiteral("dnd"));
    copyBool(status, QLatin1StringView("powerSave"), out, QStringLiteral("powerSave"));
    copyBool(status, QLatin1StringView("screenOn"), out, QStringLiteral("screenOn"));

    copyLabel(status, QLatin1StringView("model"), out, QStringLiteral("model"));
    copyLabel(status, QLatin1StringView("manufacturer"), out, QStringLiteral("manufacturer"));
    copyLabel(status, QLatin1StringView("android"), out, QStringLiteral("android"));

    qint64 uptime = 0;
    // A year of uptime is generous; anything beyond is not a real reading.
    if (wholeNumber(status.value(QLatin1StringView("uptimeMs")), 0, 366LL * 24 * 3600 * 1000,
                    uptime)) {
        out.insert(QStringLiteral("uptimeMs"), static_cast<double>(uptime));
    }

    return out;
}

} // namespace mazeconnect::core::phonestatus
