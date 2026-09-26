#include "mazeconnect/core/Messages.h"

#include "mazeconnect/core/Limits.h"
#include "mazeconnect/core/Version.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace mazeconnect::core {
namespace {

// Envelope keys, kept short because every message carries them.
constexpr QLatin1StringView kKeyVersion("v");
constexpr QLatin1StringView kKeyType("t");
constexpr QLatin1StringView kKeyCounter("c");

struct TypeEntry {
    MessageType type;
    const char *name;
};

constexpr TypeEntry kTypes[] = {
    {MessageType::Hello, "hello"},
    {MessageType::PairRequest, "pairRequest"},
    {MessageType::PairResponse, "pairResponse"},
    {MessageType::PairReveal, "pairReveal"},
    {MessageType::PairResult, "pairResult"},
    {MessageType::Unpair, "unpair"},
    {MessageType::FileOffer, "fileOffer"},
    {MessageType::FileAccept, "fileAccept"},
    {MessageType::FileReject, "fileReject"},
    {MessageType::FileComplete, "fileComplete"},
    {MessageType::FileCancel, "fileCancel"},
    {MessageType::StatusRequest, "statusRequest"},
    {MessageType::StatusReport, "statusReport"},
    {MessageType::StatusUnchanged, "statusUnchanged"},
    {MessageType::CommandList, "commandList"},
    {MessageType::CommandCatalog, "commandCatalog"},
    {MessageType::CommandRun, "commandRun"},
    {MessageType::CommandResult, "commandResult"},
    {MessageType::AiModels, "aiModels"},
    {MessageType::AiModelList, "aiModelList"},
    {MessageType::AiPrompt, "aiPrompt"},
    {MessageType::AiChunk, "aiChunk"},
    {MessageType::AiDone, "aiDone"},
    {MessageType::GuardStatus, "guardStatus"},
    {MessageType::GuardReport, "guardReport"},
    {MessageType::GuardRequest, "guardRequest"},
    {MessageType::GuardResult, "guardResult"},
    {MessageType::Ping, "ping"},
    {MessageType::Pong, "pong"},
    {MessageType::OpenOnPhone, "openOnPhone"},
    {MessageType::MediaRequest, "mediaRequest"},
    {MessageType::MediaState, "mediaState"},
    {MessageType::MediaCommand, "mediaCommand"},
};

bool hasControlCharacters(const QString &s) {
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            return true;
        }
    }
    return false;
}

} // namespace

QString Message::typeName(MessageType type) {
    for (const TypeEntry &e : kTypes) {
        if (e.type == type) {
            return QString::fromLatin1(e.name);
        }
    }
    return {};
}

MessageType Message::typeFromName(const QString &name) {
    for (const TypeEntry &e : kTypes) {
        if (name == QLatin1StringView(e.name)) {
            return e.type;
        }
    }
    return MessageType::Unknown;
}

Message::Message(MessageType type, quint64 counter, QJsonObject body)
    : m_type(type), m_counter(counter), m_body(std::move(body)) {}

Message Message::parse(const QByteArray &json) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    const QJsonObject obj = doc.object();

    // Reject anything not speaking exactly our version. There is no
    // negotiation to an older dialect: a version mismatch is a hard stop,
    // not something to work around.
    if (!obj.value(kKeyVersion).isDouble()
        || obj.value(kKeyVersion).toInt(-1) != kProtocolVersion) {
        return {};
    }

    const QJsonValue counterValue = obj.value(kKeyCounter);
    if (!counterValue.isDouble()) {
        return {};
    }
    const double rawCounter = counterValue.toDouble(-1);
    // Must be a positive integer that survived the JSON double round-trip
    // exactly; 0 is reserved so a missing/zeroed field is never a valid
    // first message (see ReplayWindow).
    if (rawCounter < 1.0 || rawCounter > 9007199254740992.0
        || rawCounter != static_cast<double>(static_cast<quint64>(rawCounter))) {
        return {};
    }

    const QJsonValue typeValue = obj.value(kKeyType);
    if (!typeValue.isString()) {
        return {};
    }
    const MessageType type = typeFromName(typeValue.toString());
    if (type == MessageType::Unknown) {
        return {};
    }

    return Message(type, static_cast<quint64>(rawCounter), obj);
}

QByteArray Message::toJson() const {
    QJsonObject obj = m_body;
    obj.insert(kKeyVersion, kProtocolVersion);
    obj.insert(kKeyType, typeName(m_type));
    obj.insert(kKeyCounter, static_cast<double>(m_counter));
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QString Message::string(QLatin1StringView key, int maxChars) const {
    const QJsonValue value = m_body.value(key);
    if (!value.isString()) {
        return {};
    }
    const QString s = value.toString();
    if (s.size() > maxChars || hasControlCharacters(s)) {
        return {};
    }
    return s;
}

qint64 Message::integer(QLatin1StringView key, qint64 max, qint64 fallback) const {
    const QJsonValue value = m_body.value(key);
    if (!value.isDouble()) {
        return fallback;
    }
    const double raw = value.toDouble(-1);
    if (raw < 0.0 || raw > static_cast<double>(max)
        || raw != static_cast<double>(static_cast<qint64>(raw))) {
        return fallback;
    }
    return static_cast<qint64>(raw);
}

bool Message::boolean(QLatin1StringView key, bool fallback) const {
    const QJsonValue value = m_body.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

QByteArray Message::binary(QLatin1StringView key, int expectedSize) const {
    const QJsonValue value = m_body.value(key);
    if (!value.isString()) {
        return {};
    }
    const QByteArray decoded = QByteArray::fromBase64(
        value.toString().toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.size() != expectedSize) {
        return {};
    }
    return decoded;
}

QStringList Message::stringList(QLatin1StringView key, int maxItems, int maxChars) const {
    const QJsonValue value = m_body.value(key);
    if (!value.isArray()) {
        return {};
    }
    const QJsonArray array = value.toArray();
    if (array.size() > maxItems) {
        return {};
    }
    QStringList out;
    out.reserve(array.size());
    for (const QJsonValue &item : array) {
        if (!item.isString()) {
            return {};
        }
        const QString s = item.toString();
        if (s.size() > maxChars || hasControlCharacters(s)) {
            return {};
        }
        out << s;
    }
    return out;
}

QJsonObject Message::unvalidatedObject(QLatin1StringView key) const {
    const QJsonValue value = m_body.value(key);
    return value.isObject() ? value.toObject() : QJsonObject();
}

// ---- Outgoing message constructors -------------------------------------

Message Message::hello(quint64 counter,
                       const QString &deviceId,
                       const QString &deviceName,
                       const QString &deviceType,
                       Capabilities capabilities,
                       const QString &appVersion) {
    QJsonObject body;
    body.insert(QLatin1StringView("deviceId"), deviceId);
    body.insert(QLatin1StringView("deviceName"), deviceName);
    body.insert(QLatin1StringView("deviceType"), deviceType);
    body.insert(QLatin1StringView("appVersion"), appVersion);
    body.insert(QLatin1StringView("capabilities"),
                QJsonArray::fromStringList(capabilitiesToNames(capabilities)));
    return Message(MessageType::Hello, counter, body);
}

Message Message::pairRequest(quint64 counter, const QByteArray &commitment) {
    QJsonObject body;
    body.insert(QLatin1StringView("commitment"),
                QString::fromLatin1(commitment.toBase64()));
    return Message(MessageType::PairRequest, counter, body);
}

Message Message::pairReveal(quint64 counter, const QByteArray &nonce) {
    QJsonObject body;
    body.insert(QLatin1StringView("nonce"), QString::fromLatin1(nonce.toBase64()));
    return Message(MessageType::PairReveal, counter, body);
}

Message Message::pairResponse(quint64 counter, const QByteArray &nonce) {
    QJsonObject body;
    body.insert(QLatin1StringView("nonce"), QString::fromLatin1(nonce.toBase64()));
    return Message(MessageType::PairResponse, counter, body);
}

Message Message::pairResult(quint64 counter, bool accepted) {
    QJsonObject body;
    body.insert(QLatin1StringView("accepted"), accepted);
    return Message(MessageType::PairResult, counter, body);
}

Message Message::unpair(quint64 counter) {
    return Message(MessageType::Unpair, counter, QJsonObject());
}

Message Message::fileOffer(quint64 counter,
                           quint32 transferId,
                           const QString &filename,
                           qint64 sizeBytes) {
    QJsonObject body;
    body.insert(QLatin1StringView("transferId"), static_cast<double>(transferId));
    body.insert(QLatin1StringView("filename"), filename);
    body.insert(QLatin1StringView("size"), static_cast<double>(sizeBytes));
    return Message(MessageType::FileOffer, counter, body);
}

Message Message::fileAccept(quint64 counter, quint32 transferId) {
    QJsonObject body;
    body.insert(QLatin1StringView("transferId"), static_cast<double>(transferId));
    return Message(MessageType::FileAccept, counter, body);
}

Message Message::fileReject(quint64 counter, quint32 transferId, const QString &reason) {
    QJsonObject body;
    body.insert(QLatin1StringView("transferId"), static_cast<double>(transferId));
    body.insert(QLatin1StringView("reason"), reason);
    return Message(MessageType::FileReject, counter, body);
}

Message Message::fileComplete(quint64 counter, quint32 transferId) {
    QJsonObject body;
    body.insert(QLatin1StringView("transferId"), static_cast<double>(transferId));
    return Message(MessageType::FileComplete, counter, body);
}

Message Message::fileCancel(quint64 counter, quint32 transferId, const QString &reason) {
    QJsonObject body;
    body.insert(QLatin1StringView("transferId"), static_cast<double>(transferId));
    body.insert(QLatin1StringView("reason"), reason);
    return Message(MessageType::FileCancel, counter, body);
}

Message Message::statusRequest(quint64 counter) {
    return Message(MessageType::StatusRequest, counter, QJsonObject());
}

Message Message::statusReport(quint64 counter, const QJsonObject &status) {
    QJsonObject body;
    body.insert(QLatin1StringView("status"), status);
    return Message(MessageType::StatusReport, counter, body);
}

Message Message::statusUnavailable(quint64 counter, const QString &reason) {
    QJsonObject body;
    body.insert(QLatin1StringView("error"), reason);
    return Message(MessageType::StatusReport, counter, body);
}

Message Message::statusUnchanged(quint64 counter) {
    return Message(MessageType::StatusUnchanged, counter, QJsonObject());
}

Message Message::commandList(quint64 counter) {
    return Message(MessageType::CommandList, counter, QJsonObject());
}

Message Message::commandCatalog(quint64 counter, const QJsonArray &entries,
                                const QString &error) {
    QJsonObject body;
    body.insert(QLatin1StringView("commands"), entries);
    if (!error.isEmpty()) {
        body.insert(QLatin1StringView("error"), error);
    }
    return Message(MessageType::CommandCatalog, counter, body);
}

Message Message::commandRun(quint64 counter, quint32 requestId, const QString &id) {
    QJsonObject body;
    body.insert(QLatin1StringView("requestId"), static_cast<double>(requestId));
    body.insert(QLatin1StringView("id"), id);
    return Message(MessageType::CommandRun, counter, body);
}

Message Message::commandResult(quint64 counter, quint32 requestId, const QString &id,
                               int exitCode, const QString &output) {
    QJsonObject body;
    body.insert(QLatin1StringView("requestId"), static_cast<double>(requestId));
    body.insert(QLatin1StringView("id"), id);
    body.insert(QLatin1StringView("exitCode"), exitCode);
    body.insert(QLatin1StringView("output"), output);
    return Message(MessageType::CommandResult, counter, body);
}

Message Message::commandRefused(quint64 counter, quint32 requestId, const QString &reason) {
    QJsonObject body;
    body.insert(QLatin1StringView("requestId"), static_cast<double>(requestId));
    body.insert(QLatin1StringView("error"), reason);
    return Message(MessageType::CommandResult, counter, body);
}

Message Message::aiModels(quint64 counter) {
    return Message(MessageType::AiModels, counter, QJsonObject());
}

Message Message::aiModelList(quint64 counter, const QStringList &models, const QString &error) {
    QJsonObject body;
    if (error.isEmpty()) {
        body.insert(QLatin1StringView("models"), QJsonArray::fromStringList(models));
    } else {
        body.insert(QLatin1StringView("error"), error);
    }
    return Message(MessageType::AiModelList, counter, body);
}

Message Message::aiPrompt(quint64 counter, quint32 requestId, const QString &model,
                          const QString &text) {
    QJsonObject body;
    body.insert(QLatin1StringView("requestId"), static_cast<double>(requestId));
    body.insert(QLatin1StringView("model"), model);
    body.insert(QLatin1StringView("text"), text);
    return Message(MessageType::AiPrompt, counter, body);
}

Message Message::aiChunk(quint64 counter, quint32 requestId, const QString &text) {
    QJsonObject body;
    body.insert(QLatin1StringView("requestId"), static_cast<double>(requestId));
    body.insert(QLatin1StringView("text"), text);
    return Message(MessageType::AiChunk, counter, body);
}

Message Message::aiDone(quint64 counter, quint32 requestId, const QString &error) {
    QJsonObject body;
    body.insert(QLatin1StringView("requestId"), static_cast<double>(requestId));
    if (!error.isEmpty()) {
        body.insert(QLatin1StringView("error"), error);
    }
    return Message(MessageType::AiDone, counter, body);
}

Message Message::guardStatus(quint64 counter) {
    return Message(MessageType::GuardStatus, counter, QJsonObject());
}

Message Message::guardReport(quint64 counter, const QJsonArray &devices, const QString &error) {
    QJsonObject body;
    if (error.isEmpty()) {
        body.insert(QLatin1StringView("devices"), devices);
    } else {
        body.insert(QLatin1StringView("error"), error);
    }
    return Message(MessageType::GuardReport, counter, body);
}

Message Message::guardRequest(quint64 counter, const QString &device, bool on) {
    QJsonObject body;
    body.insert(QLatin1StringView("device"), device);
    body.insert(QLatin1StringView("on"), on);
    return Message(MessageType::GuardRequest, counter, body);
}

Message Message::guardResult(quint64 counter, const QString &device, const QString &state,
                             const QString &error) {
    QJsonObject body;
    body.insert(QLatin1StringView("device"), device);
    body.insert(QLatin1StringView("state"), state);
    if (!error.isEmpty()) {
        body.insert(QLatin1StringView("error"), error);
    }
    return Message(MessageType::GuardResult, counter, body);
}

Message Message::ping(quint64 counter) {
    return Message(MessageType::Ping, counter, QJsonObject());
}

Message Message::pong(quint64 counter) {
    return Message(MessageType::Pong, counter, QJsonObject());
}

Message Message::openOnPhone(quint64 counter, const QString &text) {
    QJsonObject body;
    body.insert(QLatin1StringView("text"), text);
    return Message(MessageType::OpenOnPhone, counter, body);
}

Message Message::mediaRequest(quint64 counter, bool subscribe) {
    QJsonObject body;
    body.insert(QLatin1StringView("subscribe"), subscribe);
    return Message(MessageType::MediaRequest, counter, body);
}

Message Message::mediaState(quint64 counter, const QJsonObject &media) {
    QJsonObject body;
    body.insert(QLatin1StringView("media"), media);
    return Message(MessageType::MediaState, counter, body);
}

Message Message::mediaCommand(quint64 counter, const QString &player, const QString &action,
                              qint64 value) {
    QJsonObject body;
    body.insert(QLatin1StringView("player"), player);
    body.insert(QLatin1StringView("action"), action);
    body.insert(QLatin1StringView("value"), value);
    return Message(MessageType::MediaCommand, counter, body);
}

// ---- Data chunk framing -------------------------------------------------

namespace datachunk {

QByteArray encode(quint32 transferId, const QByteArray &chunk) {
    QByteArray out;
    out.reserve(4 + chunk.size());
    out.append(static_cast<char>((transferId >> 24) & 0xFF));
    out.append(static_cast<char>((transferId >> 16) & 0xFF));
    out.append(static_cast<char>((transferId >> 8) & 0xFF));
    out.append(static_cast<char>(transferId & 0xFF));
    out.append(chunk);
    return out;
}

bool decode(const QByteArray &payload, quint32 &transferId, QByteArray &chunk) {
    if (payload.size() < 4) {
        return false;
    }
    transferId = (static_cast<quint32>(static_cast<quint8>(payload[0])) << 24)
        | (static_cast<quint32>(static_cast<quint8>(payload[1])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(payload[2])) << 8)
        | static_cast<quint32>(static_cast<quint8>(payload[3]));
    chunk = payload.mid(4);
    return true;
}

} // namespace datachunk
} // namespace mazeconnect::core
