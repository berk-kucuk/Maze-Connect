#pragma once

#include <QByteArray>
#include <QString>

namespace mazeconnect::core {

/// Frame kinds carried over an established connection.
enum class FrameType : quint8 {
    Control = 0x01, ///< JSON control-plane message
    Data = 0x02,    ///< opaque binary chunk (file transfer)
};

/**
 * Incremental parser for the wire format:
 *
 *     [1 byte type][4 bytes big-endian length][length bytes payload]
 *
 * The length prefix is checked against the per-type cap *before* any payload
 * byte is buffered, so a peer claiming a 4 GiB frame costs us five bytes and
 * a disconnect rather than 4 GiB of RAM.
 *
 * The parser is deliberately a pure function of the bytes fed to it — no
 * sockets, no timers — so the hostile-input paths are directly unit-testable.
 */
class FrameParser {
public:
    enum class Status {
        Incomplete, ///< need more bytes; nothing consumed yet
        Ready,      ///< a complete frame is available
        Error,      ///< protocol violation — caller must drop the connection
    };

    /// Feed freshly-read socket bytes into the internal buffer.
    void append(const QByteArray &bytes);

    /**
     * Try to pull one complete frame out of the buffer.
     *
     * On Status::Ready, @p type and @p payload hold the frame. On
     * Status::Error the parser is poisoned: every later call returns Error,
     * because once a peer has violated the framing there is no safe point to
     * resynchronise from.
     */
    Status next(FrameType &type, QByteArray &payload);

    /// Human-readable reason for the last Status::Error, for logging.
    QString errorString() const { return m_error; }

    /// Bytes currently held but not yet forming a complete frame.
    qsizetype bufferedBytes() const { return m_buffer.size(); }

    /// Build an outgoing frame. Returns an empty QByteArray if the payload
    /// exceeds the cap for its type — callers must check rather than send.
    static QByteArray encode(FrameType type, const QByteArray &payload);

    /// Cap for a given frame type.
    static quint32 maxPayloadFor(FrameType type);

private:
    Status fail(const QString &reason);

    QByteArray m_buffer;
    QString m_error;
    bool m_poisoned = false;
};

} // namespace mazeconnect::core
