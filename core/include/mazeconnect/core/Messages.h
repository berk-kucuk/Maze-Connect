#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>

#include "mazeconnect/core/Capabilities.h"

namespace mazeconnect::core {

/// Control-plane message types. Anything not listed here is rejected.
enum class MessageType {
    Unknown,
    Hello,          ///< identity + capability advertisement (first message)
    PairRequest,    ///< initiator -> responder: begin pairing, carries commit(nonce)
    PairResponse,   ///< responder -> initiator: carries nonce
    PairReveal,     ///< initiator -> responder: opens the commitment
    PairResult,     ///< either side: user accepted or rejected the SAS
    Unpair,         ///< either side: forget this peer
    FileOffer,      ///< announce an incoming file, awaits accept/reject
    FileAccept,
    FileReject,
    FileComplete,   ///< sender: all chunks for a transfer have been sent
    FileCancel,
    StatusRequest,  ///< ask the desktop for a dashboard snapshot
    StatusReport,   ///< the snapshot, or the reason there isn't one
    StatusUnchanged,///< nothing has changed since this device's last report
    CommandList,    ///< ask which commands the user has defined
    CommandCatalog, ///< the allow-list: id + label only, never the argv
    CommandRun,     ///< run one entry, **by id**
    CommandResult,  ///< exit code and bounded output
    AiModels,       ///< ask which models the computer has
    AiModelList,    ///< the model names
    AiPrompt,       ///< one line of user text; never a system prompt
    AiChunk,        ///< a piece of the streamed reply
    AiDone,         ///< the reply is complete, or could not be produced
    GuardStatus,    ///< ask the state of every killswitch
    GuardReport,    ///< those states, or why they are unknown
    GuardRequest,   ///< turn one killswitch on or off — **privileged**
    GuardResult,    ///< what the broker reports afterwards
    Ping,           ///< transport-level liveness probe; Connection answers it directly
    Pong,           ///< reply to Ping; Connection consumes it directly
    OpenOnPhone,    ///< computer -> phone only: clipboard text to open there
    MediaRequest,   ///< phone -> computer: send the players now, and (un)subscribe
    MediaState,     ///< computer -> phone: the players, their track and position
    MediaCommand,   ///< phone -> computer: one action from a fixed table
    PhoneStatusRequest, ///< computer -> phone: send your battery/storage/network reading
    PhoneStatus,    ///< phone -> computer: that reading, or why there isn't one
    FindPhone,      ///< computer -> phone: start or stop ringing
    FindPhoneResult,///< phone -> computer: whether it is ringing now, or why not
    ShareText,      ///< phone -> computer: text or a link for the clipboard
    InputSession,   ///< phone -> computer: start/stop controlling, in a mode
    InputEvent,     ///< phone -> computer: one pointer/key/text event
    InputState,     ///< computer -> phone: whether it is controlling now, or why not
    FolderList,     ///< phone -> computer: list one folder of the shared folder
    FolderListing,  ///< computer -> phone: its entries, or why not
    FolderFetch,    ///< phone -> computer: send me this file from the shared folder
    FolderFetchResult, ///< computer -> phone: the transfer id that is coming, or why not
    ClipboardSync,  ///< either way: the sender's clipboard changed
    FolderPreview,  ///< phone -> computer: a small picture of this shared file
    FolderPreviewResult, ///< computer -> phone: that picture (JPEG), or why not
};

/**
 * A parsed control message.
 *
 * Parsing is strict by construction: `parse()` validates the envelope
 * (version, counter, known type) and every accessor below re-validates the
 * field it returns. Nothing here ever hands back an unchecked peer-supplied
 * value — a caller cannot accidentally use a field that was never validated.
 */
class Message {
public:
    Message() = default;
    Message(MessageType type, quint64 counter, QJsonObject body);

    /**
     * Parse a Control frame payload.
     *
     * Returns a Message with type Unknown if the JSON is malformed, the
     * envelope is missing/invalid, the protocol version does not match, or
     * the type is unrecognised. Callers must treat Unknown as a protocol
     * violation.
     */
    static Message parse(const QByteArray &json);

    /// Serialise for transmission. Always compact — no pretty-printing on
    /// the wire.
    QByteArray toJson() const;

    bool isValid() const { return m_type != MessageType::Unknown; }
    MessageType type() const { return m_type; }
    quint64 counter() const { return m_counter; }
    const QJsonObject &body() const { return m_body; }

    // ---- Validated field accessors -------------------------------------
    // Each returns a default-constructed value when the field is absent,
    // the wrong JSON type, or outside its permitted bounds.

    /// Bounded, control-character-free display string.
    QString string(QLatin1StringView key, int maxChars) const;

    /// Non-negative integer within [0, max].
    qint64 integer(QLatin1StringView key, qint64 max, qint64 fallback = -1) const;

    bool boolean(QLatin1StringView key, bool fallback = false) const;

    /// Base64 field decoded to exactly @p expectedSize bytes, or empty.
    QByteArray binary(QLatin1StringView key, int expectedSize) const;

    /// String array, each element bounded; capped in length.
    QStringList stringList(QLatin1StringView key, int maxItems, int maxChars) const;

    /**
     * Free text — a clipboard, a shared note — bounded like string() but
     * allowing the three whitespace controls ordinary text is made of: tab,
     * line feed and carriage return. Every other control character, and the
     * bidirectional overrides that make a link read as something it is not,
     * still reject the field outright rather than being stripped.
     */
    QString text(QLatin1StringView key, int maxChars) const;

    /**
     * A nested object, returned **unchecked**, for payloads whose shape this
     * class has no business knowing.
     *
     * Named to be impossible to use by accident. Every other accessor above
     * re-validates what it hands back; this one confirms only that the field
     * is present and is an object. Its contents are whatever the peer sent,
     * bounded solely by the control-frame cap, and a caller must validate
     * each field it actually reads.
     */
    QJsonObject unvalidatedObject(QLatin1StringView key) const;

    // ---- Constructors for outgoing messages ----------------------------

    static Message hello(quint64 counter,
                         const QString &deviceId,
                         const QString &deviceName,
                         const QString &deviceType,
                         Capabilities capabilities,
                         const QString &appVersion);
    /// Carries commit(nonce), never the nonce itself — see Sas.h.
    static Message pairRequest(quint64 counter, const QByteArray &commitment);
    static Message pairResponse(quint64 counter, const QByteArray &nonce);
    /// Opens the initiator's commitment, after the responder is committed.
    static Message pairReveal(quint64 counter, const QByteArray &nonce);
    static Message pairResult(quint64 counter, bool accepted);
    static Message unpair(quint64 counter);
    static Message fileOffer(quint64 counter,
                             quint32 transferId,
                             const QString &filename,
                             qint64 sizeBytes);
    static Message fileAccept(quint64 counter, quint32 transferId);
    static Message fileReject(quint64 counter, quint32 transferId, const QString &reason);
    static Message fileComplete(quint64 counter, quint32 transferId);
    static Message fileCancel(quint64 counter, quint32 transferId, const QString &reason);

    static Message statusRequest(quint64 counter);

    /**
     * A dashboard snapshot.
     *
     * The snapshot is nested whole under "status" rather than flattened into
     * the envelope: it is opaque display data produced by our own helper, and
     * keeping it in one subobject means no future field of it can collide
     * with an envelope or protocol key.
     *
     * Read back with unvalidatedObject("status"), whose name says what it
     * does not do. A consumer must validate each field it displays.
     */
    static Message statusReport(quint64 counter, const QJsonObject &status);

    /// A report carrying why there is no snapshot — maze-tools absent, the
    /// helper failed. The dashboard shows the reason instead of an empty
    /// machine, which would look like a machine with nothing running.
    static Message statusUnavailable(quint64 counter, const QString &reason);

    /**
     * Nothing has changed since this device's last report.
     *
     * A dashboard polls every few seconds and an idle machine's reading is
     * identical each time. Re-sending it would spend the phone's radio to
     * tell it something it already knows, so this says so in a few bytes and
     * the client keeps what it has.
     */
    static Message statusUnchanged(quint64 counter);

    static Message commandList(quint64 counter);

    /**
     * The allow-list as the phone is allowed to see it.
     *
     * Carries `id`, `label` and `confirm` — and deliberately **not** `argv`.
     * The phone has no use for the command line: it sends an id back, and the
     * desktop looks up what to run. Shipping the argv would put the exact
     * text of every command the user runs onto the network and onto a device
     * that is more easily lost, in exchange for nothing.
     *
     * @p error distinguishes "no commands defined" from "not telling you" —
     * an empty list alone cannot, and a phone showing "no commands" for a
     * capability that was switched off would be wrong.
     */
    static Message commandCatalog(quint64 counter, const QJsonArray &entries,
                                  const QString &error = QString());

    /// Ask to run one entry. @p id is matched exactly against the catalogue;
    /// nothing about it is ever interpreted as a command.
    static Message commandRun(quint64 counter, quint32 requestId, const QString &id);

    static Message commandResult(quint64 counter, quint32 requestId, const QString &id,
                                 int exitCode, const QString &output);

    /// A run that never started — unknown id, capability off, too many
    /// already running. Distinct from a command that ran and failed.
    static Message commandRefused(quint64 counter, quint32 requestId, const QString &reason);

    static Message aiModels(quint64 counter);
    static Message aiModelList(quint64 counter, const QStringList &models, const QString &error);

    /**
     * One turn of conversation.
     *
     * Carries the user's text and nothing else. There is no history here and
     * no system prompt: the conversation lives on the computer, keyed by
     * device, so a phone cannot replace Maze AI's persona or invent turns
     * that never happened.
     */
    static Message aiPrompt(quint64 counter, quint32 requestId, const QString &model,
                            const QString &text);

    /// A piece of the streamed reply. An ordinary control frame, so the
    /// monotonic counter, the replay window and the size caps all apply to it
    /// exactly as they do to everything else.
    static Message aiChunk(quint64 counter, quint32 requestId, const QString &text);

    /// End of a reply. @p error is non-empty when there is no answer at all.
    static Message aiDone(quint64 counter, quint32 requestId, const QString &error);

    static Message guardStatus(quint64 counter);
    static Message guardReport(quint64 counter, const QJsonArray &devices, const QString &error);

    /**
     * Turn one killswitch on or off.
     *
     * `device` is a name from a fixed table and `on` is a boolean. There is
     * deliberately no free-text field: the verb table on the far side is
     * `STATUS` and `KILL <device> <on|off>` and nothing else, and PANIC and
     * RESTORE are not merely refused there — they cannot be expressed here.
     */
    static Message guardRequest(quint64 counter, const QString &device, bool on);

    /// The state the broker reports *after* the change, which is not always
    /// the state that was asked for.
    static Message guardResult(quint64 counter, const QString &device, const QString &state,
                               const QString &error);

    /// A liveness probe. Handled entirely inside Connection — see its class
    /// comment — and never seen by DeviceManager.
    static Message ping(quint64 counter);
    static Message pong(quint64 counter);

    /// Clipboard text pushed to a phone to open. Computer -> phone only;
    /// the phone has no way to send this back.
    static Message openOnPhone(quint64 counter, const QString &text);

    /**
     * Ask for the computer's media players.
     *
     * Always answered with one mediaState. With @p subscribe true the
     * computer also pushes a fresh one whenever a player changes, until a
     * request with false arrives or the link drops — a phone showing
     * now-playing controls must not have to poll for a track change.
     */
    static Message mediaRequest(quint64 counter, bool subscribe);

    /**
     * The players, nested under "media" like the status snapshot, for the
     * same reason: display data produced here, whose fields must not be able
     * to collide with the envelope. The phone validates every field it shows.
     */
    static Message mediaState(quint64 counter, const QJsonObject &media);

    /**
     * One action on one player.
     *
     * @p action is a name from a fixed table (see MediaBridge) and @p player
     * an id the computer itself handed out in mediaState. Neither is ever
     * turned into a bus name, a method name or an argv by string building on
     * the receiving side: both are looked up, and anything not found is
     * refused.
     */
    static Message mediaCommand(quint64 counter, const QString &player, const QString &action,
                                qint64 value);

    /// Ask a phone for its reading. Envelope only, like statusRequest.
    static Message phoneStatusRequest(quint64 counter);

    /// Built by the phone; here so the interop vectors and tests can make it.
    static Message phoneStatus(quint64 counter, const QJsonObject &status);
    static Message phoneStatusUnavailable(quint64 counter, const QString &reason);

    /// Start (@p ring true) or stop ringing a phone.
    static Message findPhone(quint64 counter, bool ring);

    /// Built by the phone; here so tests can make it.
    static Message findPhoneResult(quint64 counter, bool ringing, const QString &error);

    /// Built by the phone; here so tests can make it.
    static Message shareText(quint64 counter, const QString &text);

    static Message inputSession(quint64 counter, bool start, const QString &mode);
    /// @p event carries kind + its fields; see docs/PROTOCOL.md § remoteInput.
    static Message inputEvent(quint64 counter, const QJsonObject &event);
    /// @p pending: not active yet, waiting for the computer's owner to answer.
    static Message inputState(quint64 counter, bool active, const QString &mode,
                              const QString &error, bool pending = false);
    static Message folderList(quint64 counter, quint32 requestId, const QString &path);
    static Message folderListing(quint64 counter, quint32 requestId, const QString &path,
                                 const QJsonArray &entries, const QString &error);
    static Message folderFetch(quint64 counter, quint32 requestId, const QString &path);
    static Message folderFetchResult(quint64 counter, quint32 requestId, quint32 transferId,
                                     const QString &error);
    static Message clipboardSync(quint64 counter, const QString &text);
    static Message folderPreview(quint64 counter, quint32 requestId, const QString &path,
                                 bool large);
    static Message folderPreviewResult(quint64 counter, quint32 requestId, const QString &path,
                                       const QByteArray &jpeg, int width, int height,
                                       const QString &error);

    static QString typeName(MessageType type);
    static MessageType typeFromName(const QString &name);

private:
    MessageType m_type = MessageType::Unknown;
    quint64 m_counter = 0;
    QJsonObject m_body;
};

/**
 * Data-frame helpers. A Data frame carries
 *     [4 bytes big-endian transferId][chunk bytes]
 * so several transfers can interleave without a per-chunk JSON envelope.
 */
namespace datachunk {

QByteArray encode(quint32 transferId, const QByteArray &chunk);

/// Returns false if the frame is too short to carry a transfer id.
bool decode(const QByteArray &payload, quint32 &transferId, QByteArray &chunk);

} // namespace datachunk

} // namespace mazeconnect::core

// Message travels through queued signal/slot connections, so it needs to be
// a registered metatype.
Q_DECLARE_METATYPE(mazeconnect::core::Message)
