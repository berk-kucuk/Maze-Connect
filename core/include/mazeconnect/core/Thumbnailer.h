#pragma once

#include <QByteArray>
#include <QSize>
#include <QString>

namespace mazeconnect::core {

/**
 * Small JPEG previews of image files, so a phone can see a picture before
 * choosing to download it.
 *
 * The file is the owner's own (the shared folder, or a file being sent), but
 * decoding is still bounded: the format is taken from the content, not the
 * name; files over kMaxSourceBytes and images over kMaxSourcePixels are not
 * decoded; the decoder is asked for the reduced size directly (JPEG decodes
 * at 1/2, 1/4, 1/8 scale, so a 48 MP photo never becomes a 48 MP buffer); and
 * the allocation limit caps whatever the decoder does regardless.
 */
namespace thumbnailer {

inline constexpr qint64 kMaxSourceBytes = 60LL * 1024 * 1024;
inline constexpr qint64 kMaxSourcePixels = 120LL * 1000 * 1000;

/// Longest edge of each size, in pixels.
inline constexpr int kThumbEdge = 160;
inline constexpr int kLargeEdge = 1280;
inline constexpr int kOfferEdge = 480;

/// Whether @p fileName looks like an image worth previewing (cheap, by name;
/// the content is checked again when it is actually decoded).
bool looksLikeImage(const QString &fileName);

/**
 * A JPEG of @p path scaled to fit @p maxEdge, or empty with @p error set.
 * @p size receives the preview's own dimensions.
 */
/// No preview is bigger than this, so one always fits a control frame once
/// base64-encoded (512 KiB cap; base64 adds a third).
inline constexpr int kMaxPreviewBytes = 280 * 1024;

QByteArray jpegPreview(const QString &path, int maxEdge, QSize &size, QString &error);

} // namespace thumbnailer
} // namespace mazeconnect::core
