#include "mazeconnect/core/Framing.h"

#include "mazeconnect/core/Limits.h"

namespace mazeconnect::core {
namespace {

constexpr qsizetype kHeaderSize = 5; // 1 type + 4 length

bool isKnownType(quint8 raw) {
    return raw == static_cast<quint8>(FrameType::Control)
        || raw == static_cast<quint8>(FrameType::Data);
}

} // namespace

quint32 FrameParser::maxPayloadFor(FrameType type) {
    switch (type) {
    case FrameType::Control:
        return limits::kMaxControlFrame;
    case FrameType::Data:
        return limits::kMaxDataFrame;
    }
    return 0;
}

void FrameParser::append(const QByteArray &bytes) {
    if (m_poisoned) {
        return;
    }
    m_buffer.append(bytes);
}

FrameParser::Status FrameParser::fail(const QString &reason) {
    m_poisoned = true;
    m_error = reason;
    m_buffer.clear();
    return Status::Error;
}

FrameParser::Status FrameParser::next(FrameType &type, QByteArray &payload) {
    if (m_poisoned) {
        return Status::Error;
    }
    if (m_buffer.size() < kHeaderSize) {
        return Status::Incomplete;
    }

    const quint8 rawType = static_cast<quint8>(m_buffer[0]);
    if (!isKnownType(rawType)) {
        return fail(QStringLiteral("unknown frame type 0x%1").arg(rawType, 2, 16, u'0'));
    }
    const auto frameType = static_cast<FrameType>(rawType);

    const quint32 length = (static_cast<quint32>(static_cast<quint8>(m_buffer[1])) << 24)
        | (static_cast<quint32>(static_cast<quint8>(m_buffer[2])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(m_buffer[3])) << 8)
        | static_cast<quint32>(static_cast<quint8>(m_buffer[4]));

    // The whole point of the length prefix check: reject before buffering.
    const quint32 cap = maxPayloadFor(frameType);
    if (length > cap) {
        return fail(QStringLiteral("frame length %1 exceeds cap %2").arg(length).arg(cap));
    }

    if (static_cast<quint64>(m_buffer.size()) < static_cast<quint64>(kHeaderSize) + length) {
        return Status::Incomplete;
    }

    type = frameType;
    payload = m_buffer.mid(kHeaderSize, static_cast<qsizetype>(length));
    m_buffer.remove(0, kHeaderSize + static_cast<qsizetype>(length));
    return Status::Ready;
}

QByteArray FrameParser::encode(FrameType type, const QByteArray &payload) {
    const quint32 cap = maxPayloadFor(type);
    if (static_cast<quint64>(payload.size()) > cap) {
        return {};
    }
    const quint32 length = static_cast<quint32>(payload.size());

    QByteArray frame;
    frame.reserve(kHeaderSize + payload.size());
    frame.append(static_cast<char>(type));
    frame.append(static_cast<char>((length >> 24) & 0xFF));
    frame.append(static_cast<char>((length >> 16) & 0xFF));
    frame.append(static_cast<char>((length >> 8) & 0xFF));
    frame.append(static_cast<char>(length & 0xFF));
    frame.append(payload);
    return frame;
}

} // namespace mazeconnect::core
