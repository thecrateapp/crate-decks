// The NI STEM manifest reader that Decks' own stem files go through (plan
// ST5.PR2): MP4 boxes with a 64-bit size, as an `mdat` over 4 GiB has.

#include "track/steminfoimporter.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

namespace {

QByteArray box(const char* type, const QByteArray& payload) {
    QByteArray bytes(4, '\0');
    qToBigEndian(static_cast<quint32>(8 + payload.size()), bytes.data());
    bytes.append(type, 4);
    bytes.append(payload);
    return bytes;
}

/// A box whose size is in the 64-bit field after its type (size 1).
QByteArray extendedBox(const char* type, const QByteArray& payload) {
    QByteArray bytes(4, '\0');
    qToBigEndian(quint32{1}, bytes.data());
    bytes.append(type, 4);
    QByteArray size(8, '\0');
    qToBigEndian(static_cast<quint64>(16 + payload.size()), size.data());
    bytes.append(size);
    bytes.append(payload);
    return bytes;
}

const QByteArray kManifest = QByteArrayLiteral(
        R"({"version":1,"mastering_dsp":{},"stems":[)"
        R"({"name":"Drums","color":"#112233"},{"name":"Bass","color":"#445566"},)"
        R"({"name":"Other","color":"#778899"},{"name":"Vocals","color":"#aabbcc"}]})");

} // namespace

TEST(StemInfoImporterTest, ReadsTheManifestAfterABoxWithA64BitSize) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("extended.stem.mp4"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(box("ftyp", QByteArrayLiteral("isom\0\0\2\0isommp41")));
    file.write(extendedBox("mdat", QByteArray(64, '\x5a')));
    file.write(box("moov", box("udta", box("stem", kManifest))));
    file.close();

    EXPECT_TRUE(mixxx::StemInfoImporter::hasStemAtom(path));
    const QList<StemInfo> stems = mixxx::StemInfoImporter::importStemInfos(path);
    ASSERT_EQ(4, stems.size());
    EXPECT_EQ(QStringLiteral("Drums"), stems[0].getLabel());
    EXPECT_EQ(QColor(QStringLiteral("#aabbcc")), stems[3].getColor());
}
