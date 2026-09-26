#include "mazeconnect/core/Thumbnailer.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

namespace mazeconnect::core::thumbnailer {
namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("Thumbnailer", text);
}

} // namespace

bool looksLikeImage(const QString &fileName) {
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    static const QStringList kImages = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp"),
    };
    return kImages.contains(suffix);
}

QByteArray jpegPreview(const QString &path, int maxEdge, QSize &size, QString &error) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
        error = tr("not a file");
        return {};
    }
    if (info.size() > kMaxSourceBytes) {
        error = tr("too large to preview");
        return {};
    }

    QImageReader reader(path);
    // Decide the format from the bytes, never the name: a file called .jpg
    // that is something else is decoded as what it is, or not at all.
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true); // phone photos are rotated by EXIF
    // A hard ceiling on what the decoder may allocate, whatever it reads.
    QImageReader::setAllocationLimit(256);
    const QByteArray format = reader.format().toLower();
    static const QList<QByteArray> kFormats = {"jpeg", "jpg", "png", "gif", "webp", "bmp"};
    if (!kFormats.contains(format)) {
        error = tr("not a picture");
        return {};
    }

    const QSize original = reader.size();
    if (!original.isValid() || original.isEmpty()
        || qint64(original.width()) * original.height() > kMaxSourcePixels) {
        error = tr("that picture cannot be previewed");
        return {};
    }
    const int edge = qBound(16, maxEdge, 4096);
    QSize target = original;
    if (target.width() > edge || target.height() > edge) {
        target.scale(edge, edge, Qt::KeepAspectRatio);
    }
    reader.setScaledSize(target);

    QImage image = reader.read();
    if (image.isNull()) {
        error = tr("that picture could not be read");
        return {};
    }
    // EXIF rotation can swap the edges after scaling; fit again if so.
    if (image.width() > edge || image.height() > edge) {
        image = image.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // JPEG has no alpha: flatten onto black, the app's own background, so a
    // transparent PNG does not come out with a white or garbage matte.
    if (image.hasAlphaChannel()) {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::black);
        flat = flat.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        {
            // Composite without QPainter's font machinery: a plain blend.
            const QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < src.height(); ++y) {
                const QRgb *s = reinterpret_cast<const QRgb *>(src.constScanLine(y));
                QRgb *d = reinterpret_cast<QRgb *>(flat.scanLine(y));
                for (int x = 0; x < src.width(); ++x) {
                    d[x] = qRgb(qRed(s[x]), qGreen(s[x]), qBlue(s[x])); // over black
                }
            }
        }
        image = flat.convertToFormat(QImage::Format_RGB32);
    }

    // Within the byte budget: lower the quality first, then the size.
    QByteArray bytes;
    for (int attempt = 0; attempt < 6; ++attempt) {
        const int quality = edge <= kThumbEdge ? 70 : qMax(45, 80 - attempt * 12);
        bytes.clear();
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "JPEG", quality)) {
            error = tr("that picture could not be previewed");
            return {};
        }
        if (bytes.size() <= kMaxPreviewBytes) {
            size = image.size();
            return bytes;
        }
        if (quality <= 45) {
            image = image.scaled(image.size() * 3 / 4, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
    }
    error = tr("that picture could not be previewed");
    return {};
}

} // namespace mazeconnect::core::thumbnailer
