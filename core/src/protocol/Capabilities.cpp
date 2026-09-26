#include "mazeconnect/core/Capabilities.h"

namespace mazeconnect::core {
namespace {

struct Entry {
    Capability capability;
    const char *name;
};

constexpr Entry kEntries[] = {
    {Capability::FileTransfer, "fileTransfer"},
    {Capability::SystemStatus, "systemStatus"},
    {Capability::Commands, "commands"},
    {Capability::Ai, "ai"},
    {Capability::GuardControl, "guardControl"},
    {Capability::OpenOnPhone, "openOnPhone"},
    {Capability::Media, "media"},
    {Capability::PhoneStatus, "phoneStatus"},
    {Capability::FindPhone, "findPhone"},
    {Capability::ShareText, "shareText"},
};

} // namespace

QString capabilityName(Capability capability) {
    for (const Entry &e : kEntries) {
        if (e.capability == capability) {
            return QString::fromLatin1(e.name);
        }
    }
    return {};
}

Capability capabilityFromName(const QString &name) {
    for (const Entry &e : kEntries) {
        if (name == QLatin1StringView(e.name)) {
            return e.capability;
        }
    }
    // Unknown capability names are simply not understood. Returning None
    // means a future peer advertising something new degrades gracefully
    // instead of being misread as an existing capability.
    return Capability::None;
}

QStringList capabilitiesToNames(Capabilities capabilities) {
    QStringList names;
    for (const Entry &e : kEntries) {
        if (capabilities.testFlag(e.capability)) {
            names << QString::fromLatin1(e.name);
        }
    }
    return names;
}

Capabilities capabilitiesFromNames(const QStringList &names) {
    Capabilities caps = Capability::None;
    for (const QString &name : names) {
        caps |= capabilityFromName(name);
    }
    return caps;
}

} // namespace mazeconnect::core
