#include "crate/cache/cachepaths.h"

#include <QHash>

#include "util/assert.h"

namespace crate::cache {

namespace {

const QString kPartSuffix = QStringLiteral(".part");

/// The extensions the decoders know, by the names the server's scanner and
/// stream use for them.
QString extensionOfFormat(const QString& format) {
    static const QHash<QString, QString> kExtensions = {
            {QStringLiteral("flac"), QStringLiteral("flac")},
            {QStringLiteral("mp3"), QStringLiteral("mp3")},
            {QStringLiteral("m4a"), QStringLiteral("m4a")},
            {QStringLiteral("mp4"), QStringLiteral("m4a")},
            {QStringLiteral("aac"), QStringLiteral("m4a")},
            {QStringLiteral("alac"), QStringLiteral("m4a")},
            {QStringLiteral("ogg"), QStringLiteral("ogg")},
            {QStringLiteral("vorbis"), QStringLiteral("ogg")},
            {QStringLiteral("opus"), QStringLiteral("opus")},
            {QStringLiteral("wav"), QStringLiteral("wav")},
            {QStringLiteral("wave"), QStringLiteral("wav")},
            {QStringLiteral("aif"), QStringLiteral("aiff")},
            {QStringLiteral("aiff"), QStringLiteral("aiff")},
            {QStringLiteral("wv"), QStringLiteral("wv")},
            {QStringLiteral("wavpack"), QStringLiteral("wv")},
    };
    return kExtensions.value(format.trimmed().toLower());
}

QString extensionOfContentType(const QByteArray& contentType) {
    static const QHash<QByteArray, QString> kExtensions = {
            {QByteArrayLiteral("audio/flac"), QStringLiteral("flac")},
            {QByteArrayLiteral("audio/x-flac"), QStringLiteral("flac")},
            {QByteArrayLiteral("audio/mpeg"), QStringLiteral("mp3")},
            {QByteArrayLiteral("audio/mp3"), QStringLiteral("mp3")},
            {QByteArrayLiteral("audio/mp4"), QStringLiteral("m4a")},
            {QByteArrayLiteral("audio/x-m4a"), QStringLiteral("m4a")},
            {QByteArrayLiteral("audio/m4a"), QStringLiteral("m4a")},
            {QByteArrayLiteral("audio/aac"), QStringLiteral("m4a")},
            {QByteArrayLiteral("audio/ogg"), QStringLiteral("ogg")},
            {QByteArrayLiteral("audio/vorbis"), QStringLiteral("ogg")},
            {QByteArrayLiteral("audio/opus"), QStringLiteral("opus")},
            {QByteArrayLiteral("audio/wav"), QStringLiteral("wav")},
            {QByteArrayLiteral("audio/x-wav"), QStringLiteral("wav")},
            {QByteArrayLiteral("audio/wave"), QStringLiteral("wav")},
            {QByteArrayLiteral("audio/aiff"), QStringLiteral("aiff")},
            {QByteArrayLiteral("audio/x-aiff"), QStringLiteral("aiff")},
            {QByteArrayLiteral("audio/wavpack"), QStringLiteral("wv")},
            {QByteArrayLiteral("audio/x-wavpack"), QStringLiteral("wv")},
    };
    // Without parameters such as "; charset=…"
    const QByteArray mediaType = contentType.split(';').first().trimmed().toLower();
    return kExtensions.value(mediaType);
}

} // namespace

bool isSafePathId(const QString& id) {
    if (id.isEmpty() || id.size() > 128) {
        return false;
    }
    for (const QChar character : id) {
        const char16_t code = character.unicode();
        const bool safe = (code >= u'a' && code <= u'z') || (code >= u'A' && code <= u'Z') ||
                (code >= u'0' && code <= u'9') || code == u'-' || code == u'_';
        if (!safe) {
            return false;
        }
    }
    return true;
}

QString extensionFor(const QString& format, const QByteArray& contentType) {
    const QString extension = extensionOfFormat(format);
    if (!extension.isEmpty()) {
        return extension;
    }
    return extensionOfContentType(contentType);
}

QString relativeAudioPath(
        const QString& serverId, const QString& entityUid, const QString& extension) {
    DEBUG_ASSERT(isSafePathId(serverId));
    DEBUG_ASSERT(isSafePathId(entityUid));
    QString fileName = entityUid;
    if (!extension.isEmpty()) {
        fileName += QLatin1Char('.') + extension;
    }
    return serverId + QLatin1Char('/') + entityUid.left(2).toLower() + QLatin1Char('/') + fileName;
}

QString partPathOf(const QString& path) {
    return path + kPartSuffix;
}

bool isPartFile(const QString& fileName) {
    return fileName.endsWith(kPartSuffix);
}

} // namespace crate::cache
