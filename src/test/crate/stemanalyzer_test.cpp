// stem-analysis/1 in Crate Decks (plan ST5.PR2, STEM-D1): the same level bytes
// as the server's producer for the same stems.

#include "crate/stems/stemanalyzer.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <variant>
#include <vector>

#include "test/mixxxtest.h"

namespace {

using crate::StemAnalysis;
using crate::StemAnalyzer;

// The stems of tools/crate-stems/stem_analysis_fixture.py
constexpr SINT kFrames = 48510;
constexpr SINT kWindow = 11025;
const QString kSourceRevision = QString(64, QLatin1Char('5'));
constexpr int kSilence = -1;
constexpr int kFull = -2;
constexpr std::array<std::array<int, 5>, 4> kGains = {{
        {0, 1, 2, 4, 8},
        {kSilence, 0, 6, 12, kFull},
        {3, kSilence, 10, 1, 5},
        {16, 9, kSilence, 0, 7},
}};

/// The fixture's stems: 8 planar rows of kFrames samples, drums left and
/// right, then bass, other and vocals.
std::vector<float> fixtureStems() {
    std::vector<float> stems(8 * kFrames);
    for (int stem = 0; stem < 4; ++stem) {
        for (int channel = 0; channel < 2; ++channel) {
            quint32 state = static_cast<quint32>(1000 * (stem + 1) + channel);
            float* pRow = stems.data() + (2 * stem + channel) * kFrames;
            for (SINT frame = 0; frame < kFrames; ++frame) {
                state = state * 1664525u + 1013904223u;
                const int value = static_cast<int>((state >> 16) & 0xFFFFu) - 32768;
                const int gain = kGains[stem][frame / kWindow];
                float sample = 0.0f;
                if (gain == kFull) {
                    sample = frame % 2 == 0 ? 32767.0f : -32767.0f;
                } else if (gain != kSilence) {
                    sample = static_cast<float>(value) * std::ldexp(1.0f, -gain);
                }
                pRow[frame] = sample / 32768.0f;
            }
        }
    }
    return stems;
}

/// Writes `stems` to `pAnalyzer` in blocks of `block` frames.
void feed(StemAnalyzer* pAnalyzer, const std::vector<float>& stems, SINT block) {
    std::vector<float> chunk;
    for (SINT first = 0; first < kFrames; first += block) {
        const SINT count = std::min(block, kFrames - first);
        chunk.resize(8 * count);
        for (int row = 0; row < 8; ++row) {
            std::copy_n(stems.begin() + row * kFrames + first, count, chunk.begin() + row * count);
        }
        QString error;
        ASSERT_TRUE(pAnalyzer->write(first, count, chunk, &error)) << error.toStdString();
    }
}

class StemAnalyzerTest : public MixxxTest {
  protected:
    QJsonObject serverFixture() const {
        QFile file(getTestDir().filePath(
                QStringLiteral("crate/data/stems/stem-analysis-fixture.json")));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return QJsonDocument::fromJson(file.readAll()).object();
    }
};

} // namespace

// The fixture is the server's own producer on these stems
// (tools/crate-stems/stem_analysis_fixture.py), whole windows, a short last
// one, silence and full scale included.
TEST_F(StemAnalyzerTest, MatchesTheServerFixtureByteForByte) {
    const std::vector<float> stems = fixtureStems();
    const QJsonObject fixture = serverFixture();
    const QJsonObject levels = fixture.value(QStringLiteral("levels")).toObject();
    // Any block size gives the same bytes
    for (const SINT block : {kFrames, SINT{4096}, SINT{11025}, SINT{777}}) {
        StemAnalyzer analyzer;
        feed(&analyzer, stems, block);
        const StemAnalysis analysis = analyzer.finish(kSourceRevision);
        EXPECT_EQ(fixture.value(QStringLiteral("durationMs")).toInteger(), analysis.durationMs);
        EXPECT_EQ(250, analysis.hopMs);
        for (int stem = 0; stem < 4; ++stem) {
            const QString name = StemAnalysis::kSources[stem];
            EXPECT_EQ(QByteArray::fromBase64(levels.value(name).toString().toLatin1()),
                    analysis.levels[stem])
                    << name.toStdString() << " in blocks of " << block;
        }
    }
}

TEST_F(StemAnalyzerTest, EncodesLevelsLikeTheServer) {
    EXPECT_EQ(0, crate::encodeStemLevel(-std::numeric_limits<double>::infinity()));
    EXPECT_EQ(0, crate::encodeStemLevel(-60.0));
    EXPECT_EQ(0, crate::encodeStemLevel(-90.0));
    EXPECT_EQ(255, crate::encodeStemLevel(0.0));
    EXPECT_EQ(255, crate::encodeStemLevel(3.0));
    // floor((dB + 60) * 255 / 60 + 0.5): -30 dB is 127.5, rounded half up
    EXPECT_EQ(128, crate::encodeStemLevel(-30.0));
    EXPECT_EQ(4, crate::encodeStemLevel(-59.0));
}

// What Decks uploads reads back as the server checks it, and the server's
// own document reads too.
TEST_F(StemAnalyzerTest, ReadsAndWritesTheDocument) {
    StemAnalyzer analyzer;
    feed(&analyzer, fixtureStems(), kFrames);
    StemAnalysis analysis = analyzer.finish(kSourceRevision, QStringLiteral("crate-decks/1.0.0"));
    analysis.drumBeatsMs = {100, 600};
    QString error;
    const std::optional<StemAnalysis> read = StemAnalysis::fromJson(analysis.toJson(), &error);
    ASSERT_TRUE(read) << error.toStdString();
    EXPECT_EQ(analysis.levels, read->levels);
    EXPECT_EQ(QStringLiteral("crate-decks/1.0.0"), read->producer);
    EXPECT_EQ(QStringLiteral("htdemucs-onnx-1"), read->separatorVersion);
    EXPECT_EQ(kSourceRevision, read->sourceRevision);
    EXPECT_EQ((QList<qint64>{100, 600}), read->drumBeatsMs);
    EXPECT_TRUE(read->isFromDecks());

    const std::optional<StemAnalysis> server = StemAnalysis::fromJson(
            QJsonDocument(serverFixture()).toJson(QJsonDocument::Compact), &error);
    ASSERT_TRUE(server) << error.toStdString();
    EXPECT_FALSE(server->isFromDecks());
    EXPECT_EQ(analysis.levels, server->levels);
}

TEST_F(StemAnalyzerTest, RejectsADocumentTheServerWouldReject) {
    StemAnalyzer analyzer;
    feed(&analyzer, fixtureStems(), kFrames);
    const QJsonObject good = QJsonDocument::fromJson(analyzer.finish(kSourceRevision).toJson())
                                     .object();
    const auto rejects = [](const QJsonObject& document) {
        QString error;
        const bool rejected = !StemAnalysis::fromJson(
                QJsonDocument(document).toJson(QJsonDocument::Compact), &error);
        return rejected && !error.isEmpty();
    };
    QJsonObject other = good;
    other.insert(QStringLiteral("schema"), QStringLiteral("stem-analysis/2"));
    EXPECT_TRUE(rejects(other));
    other = good;
    other.insert(QStringLiteral("sourceRevision"), QStringLiteral("abc"));
    EXPECT_TRUE(rejects(other));
    other = good;
    // Levels longer than the duration plus a window
    other.insert(QStringLiteral("durationMs"), 500);
    EXPECT_TRUE(rejects(other));
    other = good;
    QJsonObject levels = other.value(QStringLiteral("levels")).toObject();
    levels.insert(QStringLiteral("bass"), QStringLiteral("AAA="));
    other.insert(QStringLiteral("levels"), levels);
    EXPECT_TRUE(rejects(other));
    other = good;
    QJsonObject separator = other.value(QStringLiteral("separator")).toObject();
    separator.insert(QStringLiteral("model"), QStringLiteral("htdemucs_6s"));
    other.insert(QStringLiteral("separator"), separator);
    EXPECT_TRUE(rejects(other));
}
