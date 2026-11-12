// The optional NI STEM export of plan ST5.PR2 (STEM-D9): a .stem.mp4 for
// other software, from a track and its sidecar, with Crate Decks' colours.

#include "crate/stems/nistemexport.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <numbers>
#include <vector>

#include "crate/stems/stemsidecar.h"
#include "crate/ui/skin/stemcolors.h"
#include "sources/soundsourceproxy.h"
#include "test/crate/wavwriter.h"
#include "test/mixxxtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/steminfoimporter.h"
#include "track/track.h"

namespace {

class NiStemExportTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_original = m_dir.filePath(QStringLiteral("track.wav"));
        crate::test::writeWav(m_original, 44100, 2 * 44100, [](SINT frame, int channel) {
            const double t = static_cast<double>(frame) / 44100.0;
            return static_cast<int>(std::lround(
                    12000.0 * std::sin(2.0 * std::numbers::pi * (220.0 + 3.0 * channel) * t)));
        });
        // Its sidecar, from tones as stems
        crate::StemSidecarWriter::Options options;
        options.pTrack = Track::newTemporary(m_original);
        options.sourceRevision = QString(64, QLatin1Char('a'));
        crate::StemSidecarWriter writer(options);
        crate::StemSeparationSource source;
        source.originalSampleRate = 44100;
        source.frames = 2 * 44100;
        QString error;
        ASSERT_TRUE(writer.begin(source, &error)) << error.toStdString();
        std::vector<float> stems(static_cast<std::size_t>(8 * source.frames));
        for (std::size_t row = 0; row < 8; ++row) {
            for (SINT i = 0; i < source.frames; ++i) {
                const double t = static_cast<double>(i) / 44100.0;
                stems[row * static_cast<std::size_t>(source.frames) +
                        static_cast<std::size_t>(i)] =
                        static_cast<float>(0.1 *
                                std::sin(2.0 * std::numbers::pi * 80.0 *
                                        static_cast<double>(row / 2 + 1) * t));
            }
        }
        ASSERT_TRUE(writer.write(0, source.frames, stems, &error)) << error.toStdString();
        ASSERT_TRUE(writer.finish(&error)) << error.toStdString();
    }

    QTemporaryDir m_dir;
    QString m_original;
};

} // namespace

TEST_F(NiStemExportTest, TheExportedFileOpensAsAStemFileWithFourStemsAndOurColours) {
    const QString exported = m_dir.filePath(QStringLiteral("track.stem.mp4"));
    QString error;
    ASSERT_TRUE(crate::exportNiStem(Track::newTemporary(m_original),
            crate::StemSidecar::pathFor(m_original),
            exported,
            nullptr,
            &error))
            << error.toStdString();
    EXPECT_FALSE(QFile::exists(exported + QStringLiteral(".part")));

    // The manifest: four stems, named, in Crate Decks' colours
    EXPECT_TRUE(mixxx::StemInfoImporter::hasStemAtom(exported));
    const QList<StemInfo> stems = mixxx::StemInfoImporter::importStemInfos(exported);
    ASSERT_EQ(4, stems.size());
    const std::array<QColor, 4> colors = crate::stemColors();
    const QStringList names = {QStringLiteral("Drums"),
            QStringLiteral("Bass"),
            QStringLiteral("Other"),
            QStringLiteral("Vocals")};
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(colors[static_cast<std::size_t>(i)].isValid());
        EXPECT_EQ(names[i], stems[i].getLabel());
        EXPECT_EQ(colors[static_cast<std::size_t>(i)], stems[i].getColor()) << i;
    }

    // The decks open it as a stem file: the master's audio, four stems
    const TrackPointer pTrack = Track::newTemporary(exported);
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stem());
    const auto pSource = SoundSourceProxy(pTrack).openAudioSource(params);
    ASSERT_NE(nullptr, pSource);
    EXPECT_EQ(mixxx::audio::ChannelCount::stem(), pSource->getSignalInfo().getChannelCount());
    EXPECT_EQ(mixxx::audio::SampleRate(44100), pSource->getSignalInfo().getSampleRate());
    // AAC adds at most a frame of priming and padding
    EXPECT_NEAR(2 * 44100, pSource->frameIndexRange().length(), 2048);
    pSource->close();
}

// The colours come from the design tokens, not from any default
TEST_F(NiStemExportTest, StemColoursAreTheDesignTokens) {
    const std::array<QColor, 4> colors = crate::stemColors();
    EXPECT_EQ(QColor(QStringLiteral("#67e8f9")), colors[0]);
    EXPECT_EQ(QColor(QStringLiteral("#0e7490")), colors[1]);
    EXPECT_EQ(QColor(QStringLiteral("#999ca0")), colors[2]);
    EXPECT_EQ(QColor(QStringLiteral("#fbbf24")), colors[3]);
}
