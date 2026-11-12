// Local separation in the application (plan ST5.PR2, STEM-D7): the model
// downloads on first use only after the user says yes, seeing its size, and
// then separates with it.

#include "crate/stems/appstemseparator.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <numbers>
#include <optional>

#include "crate/db/crateserverschema.h"
#include "crate/inference/modelstore.h"
#include "crate/stems/stemdsp.h"
#include "test/crate/fakecratenetwork.h"
#include "test/crate/wavwriter.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace {

using crate::AppStemSeparator;
using crate::ModelManifestEntry;
using crate::ModelStore;

const QByteArray kModelBytes = QByteArrayLiteral("a stand-in for the htdemucs graph");
constexpr int kWaitMs = 20000;

/// Each source is a quarter of the mix.
class QuarterModel : public crate::StemModel {
  public:
    bool run(std::span<const float> mix,
            std::span<const float> spec,
            std::vector<float>* pSpecOut,
            std::vector<float>* pWaveOut,
            QString* pError) override {
        Q_UNUSED(pError);
        pSpecOut->assign(crate::stemdsp::kSources * spec.size(), 0.0f);
        pWaveOut->resize(crate::stemdsp::kSources * mix.size());
        for (int source = 0; source < crate::stemdsp::kSources; ++source) {
            for (std::size_t i = 0; i < mix.size(); ++i) {
                (*pWaveOut)[static_cast<std::size_t>(source) * mix.size() + i] = mix[i] / 4.0f;
            }
        }
        return true;
    }
    void abort() override {
    }
    void clearAbort() override {
    }
    QString description() const override {
        return QStringLiteral("quarter");
    }
};

/// Counts the frames of the stems it gets.
class CountingSink : public crate::StemSink {
  public:
    bool write(SINT firstFrame,
            SINT frameCount,
            std::span<const float> stems,
            QString* pError) override {
        Q_UNUSED(firstFrame);
        Q_UNUSED(stems);
        Q_UNUSED(pError);
        frames += frameCount;
        return true;
    }
    std::atomic<SINT> frames = 0;
};

class AppStemSeparatorTest : public MixxxDbTest, SoundSourceProviderRegistration {
  protected:
    AppStemSeparatorTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(m_root.isValid());
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
        ModelManifestEntry entry;
        entry.name = crate::kHtdemucsModel;
        entry.version = QStringLiteral("1");
        entry.url = QUrl(QStringLiteral("https://models.crate.test/htdemucs-onnx-1.onnx"));
        entry.sha256 = QCryptographicHash::hash(kModelBytes, QCryptographicHash::Sha256).toHex();
        entry.size = kModelBytes.size();
        ModelStore::Dependencies dependencies;
        dependencies.pDbConnectionPool = dbConnectionPooler();
        dependencies.pNetworkAccessManager = &m_network;
        dependencies.dataDirectory = m_root.path();
        m_pModels = std::make_unique<ModelStore>(QList<ModelManifestEntry>{entry}, dependencies);
        m_track = QDir(m_root.path()).filePath(QStringLiteral("track.wav"));
        crate::test::writeWav(m_track, 44100, 44100, [](SINT frame, int channel) {
            Q_UNUSED(channel);
            return static_cast<int>(std::lround(
                    9000.0 * std::sin(2.0 * std::numbers::pi * 330.0 * frame / 44100.0)));
        });
    }

    std::unique_ptr<AppStemSeparator> makeSeparator() {
        AppStemSeparator::Options options;
        options.pModels = m_pModels.get();
        options.confirmDownload = [this](qint64 bytes, std::function<void(bool)> answer) {
            m_askedBytes.append(bytes);
            m_pendingAnswer = std::move(answer);
        };
        options.useGpu = [] {
            return false;
        };
        options.modelFactory = [this](const QString& path,
                                       crate::InferenceProvider provider,
                                       QString* pError) -> std::unique_ptr<crate::StemModel> {
            Q_UNUSED(pError);
            m_modelPath = path;
            m_provider = provider;
            return std::make_unique<QuarterModel>();
        };
        return std::make_unique<AppStemSeparator>(options);
    }

    QTemporaryDir m_root;
    crate::test::FakeNetworkAccessManager m_network;
    std::unique_ptr<ModelStore> m_pModels;
    QString m_track;
    QList<qint64> m_askedBytes;
    std::function<void(bool)> m_pendingAnswer;
    QString m_modelPath;
    std::optional<crate::InferenceProvider> m_provider;
};

} // namespace

// STEM-D7: models download on first use; Diego (2026-11-12): only after a
// question that shows the size, never silently. A no lasts for the run.
TEST_F(AppStemSeparatorTest, AsksBeforeDownloadingTheModel) {
    const auto pSeparator = makeSeparator();
    EXPECT_TRUE(pSeparator->isAvailable());
    std::optional<std::pair<bool, QString>> result;
    pSeparator->separate(Track::newTemporary(m_track),
            std::make_shared<CountingSink>(),
            [&result](bool done, const QString& error) {
                result = std::make_pair(done, error);
            });
    ASSERT_EQ(1, m_askedBytes.size());
    EXPECT_EQ(kModelBytes.size(), m_askedBytes.first());
    // Nothing downloads before the answer
    QTest::qWait(50);
    EXPECT_TRUE(m_network.sent.isEmpty());
    EXPECT_FALSE(result.has_value());

    m_pendingAnswer(false);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->first);
    EXPECT_TRUE(m_network.sent.isEmpty());
    // Not asked again in this run
    EXPECT_FALSE(pSeparator->isAvailable());
    result.reset();
    pSeparator->separate(Track::newTemporary(m_track),
            std::make_shared<CountingSink>(),
            [&result](bool done, const QString& error) {
                result = std::make_pair(done, error);
            });
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->first);
    EXPECT_EQ(1, m_askedBytes.size());
}

// A yes downloads and checks the model, then separates with it, on the CPU
// when the preference does not allow the GPU.
TEST_F(AppStemSeparatorTest, DownloadsTheModelOnAYesAndSeparatesWithIt) {
    const auto pSeparator = makeSeparator();
    auto pSink = std::make_shared<CountingSink>();
    std::optional<std::pair<bool, QString>> result;
    pSeparator->separate(Track::newTemporary(m_track),
            pSink,
            [&result](bool done, const QString& error) {
                result = std::make_pair(done, error);
            });
    ASSERT_TRUE(m_pendingAnswer);
    m_pendingAnswer(true);
    ASSERT_EQ(1, m_network.sent.size());
    EXPECT_EQ(QUrl(QStringLiteral("https://models.crate.test/htdemucs-onnx-1.onnx")),
            m_network.last().request.url());
    m_network.last().pReply->respond(200, kModelBytes);

    ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, kWaitMs));
    EXPECT_TRUE(result->first) << result->second.toStdString();
    EXPECT_EQ(m_pModels->installedPath(crate::kHtdemucsModel), m_modelPath);
    EXPECT_EQ(crate::InferenceProvider::Cpu, m_provider);
    EXPECT_EQ(44100, pSink->frames.load());

    // Installed: the next one separates without asking
    auto pNext = std::make_shared<CountingSink>();
    result.reset();
    pSeparator->separate(Track::newTemporary(m_track),
            pNext,
            [&result](bool done, const QString& error) {
                result = std::make_pair(done, error);
            });
    ASSERT_TRUE(QTest::qWaitFor([&] { return result.has_value(); }, kWaitMs));
    EXPECT_TRUE(result->first);
    EXPECT_EQ(1, m_askedBytes.size());
    EXPECT_EQ(1, m_network.sent.size());
}
