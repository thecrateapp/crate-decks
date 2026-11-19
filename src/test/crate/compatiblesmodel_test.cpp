#include "crate/recs/compatiblesmodel.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSqlQuery>

#include "crate/db/crateserverschema.h"
#include "crate/library/cratelibrarystore.h"
#include "crate/recs/recsjson.h"
#include "crate/ui/delegates/delegateroles.h"
#include "test/mixxxdbtest.h"

// The model of the Compatibles view (plan F5.C-PR1): the compatible tracks
// go into crateserver_tracks with what the answer carries, and come back with
// everything the catalogue knows of them, plus their scores.

namespace {

namespace lib = crate::library;
using crate::recs::CompatiblesModel;
using crate::recs::CompatibleTracks;
using crate::recs::StoredCompatibles;

const QString kServer = QStringLiteral("srv-1");
/// The seed of real-compatibles.json, from the dev stack: "I Feel Loved
/// (Danny Tenaglia's Labor Of Love Dub Edit)", 128.06 BPM, energy 0.81.
const QString kSeed = QStringLiteral("afe794ee-23ea-53d3-8280-ff633310da60");
/// Its first item, "Der Leiermann (Live)" by Covenant.
const QString kFirst = QStringLiteral("8e618027-6f6d-57c5-9769-f73a02b14d54");

class CrateCompatiblesModelTest : public MixxxDbTest {
  protected:
    CrateCompatiblesModelTest()
            : MixxxDbTest(true) {
    }

    void SetUp() override {
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection()));
        ASSERT_TRUE(crate::db::createCrateServerSchema(dbConnection()));
    }

    QByteArray fixture(const QString& name) const {
        QFile file(getTestDir().filePath(QStringLiteral("crate/data/recs/") + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    }

    CompatibleTracks realCompatibles() const {
        const auto parsed = crate::recs::parseCompatibleTracks(
                fixture(QStringLiteral("real-compatibles.json")));
        EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message.toStdString());
        return parsed.ok() ? parsed.value() : CompatibleTracks();
    }

    /// The seed and the first item as a container sync left them.
    void storeKnownTracks() {
        lib::RemoteTrack seed;
        seed.entityUid = kSeed;
        seed.title = QStringLiteral("I Feel Loved");
        seed.bpm = 128.05983;
        seed.energy = 0.81485087;
        seed.camelot = QStringLiteral("7B");
        lib::RemoteTrack first;
        first.entityUid = kFirst;
        first.title = QStringLiteral("An older title");
        first.format = QStringLiteral("flac");
        first.byteLength = 41'000'000;
        first.sourceRevision = QStringLiteral("rev-7");
        first.profileQuality = QStringLiteral("full");
        first.bpm = 120.0;
        ASSERT_EQ(lib::WriteStatus::Ok,
                lib::storeTracks(
                        dbConnection(), kServer, {seed, first}, QStringLiteral("2026-11-10")));
    }

    StoredCompatibles store() {
        const auto stored = crate::recs::storeCompatibles(dbConnection(),
                kServer,
                kSeed,
                realCompatibles(),
                QStringLiteral("2026-11-16"));
        EXPECT_TRUE(stored.ok());
        return stored.ok() ? stored.value() : StoredCompatibles();
    }
};

TEST_F(CrateCompatiblesModelTest, ParsesTheRealAnswer) {
    const CompatibleTracks compatibles = realCompatibles();
    EXPECT_QSTRING_EQ(kSeed, compatibles.seedEntityUid);
    EXPECT_QSTRING_EQ(QStringLiteral("auto"), compatibles.context);
    ASSERT_EQ(8, compatibles.items.size());
    const auto& first = compatibles.items.first();
    EXPECT_QSTRING_EQ(kFirst, first.track.entityUid);
    EXPECT_EQ(5779, first.trackId);
    EXPECT_QSTRING_EQ(QStringLiteral("Der Leiermann (Live)"), first.track.title);
    EXPECT_DOUBLE_EQ(0.892495, first.score);
    EXPECT_QSTRING_EQ(QStringLiteral("same"), first.harmonicRelationship);
    EXPECT_EQ(356306, first.track.durationMs.value_or(0));
    EXPECT_QSTRING_EQ(QStringLiteral("7B"), first.track.camelot);
    EXPECT_QSTRING_EQ(QStringLiteral("/api/vdj/albums/492/cover?size=512"), first.track.coverUrl);
    EXPECT_QSTRING_EQ(QStringLiteral("relative"), compatibles.items[6].harmonicRelationship);
    EXPECT_QSTRING_EQ(QStringLiteral("adjacent"), compatibles.items[7].harmonicRelationship);

    // An item without its uid is not something Decks can load
    EXPECT_FALSE(crate::recs::parseCompatibleTracks(
            R"({"items": [{"trackId": 1, "score": 0.5}]})")
                    .ok());
    EXPECT_FALSE(crate::recs::parseCompatibleTracks(R"({"items": "none"})").ok());
}

TEST_F(CrateCompatiblesModelTest, UpsertsTrackMetadataFromResponse) {
    storeKnownTracks();
    const StoredCompatibles stored = store();

    // Every result, in the server's order, ranked from 1
    ASSERT_EQ(8, stored.rows.size());
    for (int i = 0; i < stored.rows.size(); ++i) {
        EXPECT_EQ(i + 1, stored.rows[i].position);
        EXPECT_GT(stored.rows[i].id, 0);
        EXPECT_QSTRING_EQ(kServer, stored.rows[i].serverId);
    }
    // A known track takes the answer's metadata and keeps what only a sync
    // brings: format, size, revision and profile quality
    const lib::CrateServerTrackModel::Row& first = stored.rows.first();
    EXPECT_QSTRING_EQ(kFirst, first.entityUid);
    EXPECT_QSTRING_EQ(QStringLiteral("Der Leiermann (Live)"), first.title);
    EXPECT_QSTRING_EQ(QStringLiteral("Covenant"), first.artist);
    EXPECT_DOUBLE_EQ(128.01382, first.bpm.value_or(0));
    EXPECT_QSTRING_EQ(QStringLiteral("7B"), first.camelot);
    EXPECT_EQ(356306, first.durationMs.value_or(0));
    EXPECT_QSTRING_EQ(QStringLiteral("flac"), first.format);
    EXPECT_EQ(41'000'000, first.byteLength.value_or(0));
    EXPECT_QSTRING_EQ(QStringLiteral("rev-7"), first.sourceRevision);
    EXPECT_QSTRING_EQ(QStringLiteral("full"), first.profileQuality);
    // A new one joins the catalogue with what the answer carries
    QSqlQuery query(dbConnection());
    query.prepare(QStringLiteral(
            "SELECT title, artist, genre, cover_url, format FROM crateserver_tracks "
            "WHERE server_id = :server AND entity_uid = :uid"));
    query.bindValue(QStringLiteral(":server"), kServer);
    query.bindValue(QStringLiteral(":uid"), stored.rows[6].entityUid);
    ASSERT_TRUE(query.exec());
    ASSERT_TRUE(query.next());
    EXPECT_QSTRING_EQ(QStringLiteral("Fannie Likes 2 Dance"), query.value(0).toString());
    EXPECT_FALSE(query.value(1).toString().isEmpty());
    // The server leaves out what it does not know
    EXPECT_TRUE(query.value(2).isNull());
    EXPECT_TRUE(query.value(3).toString().startsWith(QStringLiteral("/api/vdj/albums/")));
    EXPECT_TRUE(query.value(4).isNull());

    // The scores, with the deltas against the followed track
    ASSERT_TRUE(stored.scores.contains(kFirst));
    const crate::recs::CompatibleScores scores = stored.scores.value(kFirst);
    EXPECT_DOUBLE_EQ(0.892495, scores.score);
    EXPECT_QSTRING_EQ(QStringLiteral("same"), scores.harmonicRelationship);
    EXPECT_NEAR(128.01382 - 128.05983, scores.bpmDelta.value_or(99), 1e-9);
    // 0.78 and 0.81 are both level 8
    EXPECT_EQ(0, scores.energyDelta.value_or(99));
}

TEST_F(CrateCompatiblesModelTest, DeltasNeedTheFollowedTrack) {
    // The seed is not in the catalogue: no energy delta, and the BPM delta
    // from the deck's BPM if there is one
    const CompatibleTracks compatibles = realCompatibles();
    const auto stored = crate::recs::storeCompatibles(
            dbConnection(), kServer, kSeed, compatibles, QStringLiteral("2026-11-16"), 128.0);
    ASSERT_TRUE(stored.ok());
    const crate::recs::CompatibleScores scores = stored.value().scores.value(kFirst);
    EXPECT_NEAR(0.01382, scores.bpmDelta.value_or(99), 1e-9);
    EXPECT_FALSE(scores.energyDelta);

    const auto none = crate::recs::storeCompatibles(
            dbConnection(), kServer, kSeed, compatibles, QStringLiteral("2026-11-16"));
    ASSERT_TRUE(none.ok());
    EXPECT_FALSE(none.value().scores.value(kFirst).bpmDelta);
}

TEST_F(CrateCompatiblesModelTest, ShowsScoresAndFiltersByKey) {
    storeKnownTracks();
    CompatiblesModel model(dbConnection(), dbConnectionPooler());
    model.setResults(kServer, kSeed, store());

    ASSERT_EQ(8, model.rowCount());
    EXPECT_EQ(CompatiblesModel::kCompatiblesColumnCount, model.columnCount());
    EXPECT_QSTRING_EQ(kFirst, model.entityUid(0));
    EXPECT_QSTRING_EQ(QStringLiteral("0.89"),
            model.data(model.index(0, CompatiblesModel::kScoreColumn)).toString());
    EXPECT_NEAR(0.89,
            model.data(model.index(0, CompatiblesModel::kScoreColumn),
                         crate::delegates::ScoreRole)
                    .toDouble(),
            0.005);
    // The Harmony column reads the two keys: the seed is 7B
    EXPECT_QSTRING_EQ(QStringLiteral("7B"), model.seedCamelot());
    EXPECT_QSTRING_EQ(QStringLiteral("I Feel Loved"), model.seed().title);
    EXPECT_NEAR(128.05983, model.seed().bpm.value_or(0), 1e-6);
    EXPECT_GT(model.resultsTimeMs(), 0);
    EXPECT_QSTRING_EQ(crate::recs::harmonyLabel(crate::recs::Harmony::Same),
            model.data(model.index(0, CompatiblesModel::kHarmonicColumn)).toString());
    EXPECT_TRUE(model.data(model.index(0, CompatiblesModel::kHarmonicColumn),
                             crate::delegates::HarmonyMatchRole)
                    .toBool());
    EXPECT_QSTRING_EQ(QStringLiteral("−0.05"),
            model.data(model.index(0, CompatiblesModel::kBpmDeltaColumn)).toString());
    EXPECT_FALSE(model.data(model.index(0, CompatiblesModel::kBpmDeltaColumn),
                              crate::delegates::WarningRole)
                    .toBool());
    EXPECT_QSTRING_EQ(QStringLiteral("0"),
            model.data(model.index(0, CompatiblesModel::kEnergyDeltaColumn)).toString());
    EXPECT_EQ(CompatiblesModel::kScoreColumn, model.fieldIndex(CompatiblesModel::kScoreField));
    EXPECT_QSTRING_EQ(CompatiblesModel::kHarmonicField,
            model.headerData(CompatiblesModel::kHarmonicColumn,
                         Qt::Horizontal,
                         TrackModel::kHeaderNameRole)
                    .toString());
    EXPECT_EQ((QStringList{QStringLiteral("7B"), QStringLiteral("7A"), QStringLiteral("8B")}),
            model.keysInResults());

    // The Camelot wheel's click: only the relative key
    model.setKeyFilter(QStringLiteral("7a"));
    ASSERT_EQ(1, model.rowCount());
    EXPECT_QSTRING_EQ(QStringLiteral("relative"),
            model.scoresOf(model.entityUid(0))->harmonicRelationship);
    EXPECT_QSTRING_EQ(crate::recs::harmonyLabel(crate::recs::Harmony::Relative),
            model.data(model.index(0, CompatiblesModel::kHarmonicColumn)).toString());
    // The band counts every result, filtered or not
    const crate::recs::CompatiblesSummary summary = model.summary();
    EXPECT_EQ(8, summary.results);
    EXPECT_EQ(summary.same + summary.step + summary.relative + summary.energy + summary.other,
            summary.results);
    EXPECT_EQ(summary.harmonic, summary.results - summary.other);
    EXPECT_GE(summary.relative, 1);
    EXPECT_EQ(0, summary.cached);
    EXPECT_EQ(5, model.bestCache(5).size());
    model.setKeyFilter(QString());
    EXPECT_EQ(8, model.rowCount());

    // Sorted by score, worst first, and back to the server's ranking
    model.sort(CompatiblesModel::kScoreColumn, Qt::AscendingOrder);
    EXPECT_LE(model.scoresOf(model.entityUid(0))->score,
            model.scoresOf(model.entityUid(7))->score);
    model.sort(CompatiblesModel::kPositionColumn, Qt::AscendingOrder);
    EXPECT_QSTRING_EQ(kFirst, model.entityUid(0));

    model.clear();
    EXPECT_EQ(0, model.rowCount());
    EXPECT_EQ(0, model.summary().results);
    EXPECT_TRUE(model.keysInResults().isEmpty());
}

} // namespace
