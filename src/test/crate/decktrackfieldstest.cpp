#include <gtest/gtest.h>

#include <QString>
#include <memory>
#include <utility>
#include <vector>

#include "control/controlobject.h"
#include "crate/ui/skin/decktrackfields.h"
#include "mixer/playerinfo.h"
#include "test/mixxxtest.h"
#include "track/track.h"

namespace {

using crate::DeckTrackFields;

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");

TrackPointer trackWith(const QString& artist, const QString& genre) {
    TrackPointer pTrack = Track::newTemporary();
    pTrack->setArtist(artist);
    pTrack->updateGenre(genre);
    return pTrack;
}

class CrateDeckTrackFieldsTest : public MixxxTest {
  protected:
    void SetUp() override {
        // The controls that PlayerInfo reads when it is created. In the app
        // CoreServices creates them first; a debug build asserts without them.
        for (const auto& [group, item] : {
                     std::pair{QStringLiteral("[Master]"), QStringLiteral("crossfader")},
                     std::pair{QStringLiteral("[App]"), QStringLiteral("num_decks")},
                     std::pair{QStringLiteral("[App]"), QStringLiteral("num_samplers")},
                     std::pair{QStringLiteral("[App]"),
                             QStringLiteral("num_preview_decks")}}) {
            m_controls.push_back(std::make_unique<ControlObject>(ConfigKey(group, item)));
        }
        // As in the app, where CoreServices creates it before any skin
        PlayerInfo::create();
        // As the CrateDecks skin's manifest declares them, for two decks
        for (int deck = 1; deck <= 2; ++deck) {
            m_controls.push_back(std::make_unique<ControlObject>(
                    DeckTrackFields::hasArtistKey(deck)));
            m_controls.push_back(std::make_unique<ControlObject>(
                    DeckTrackFields::hasGenreKey(deck)));
        }
    }

    void TearDown() override {
        PlayerInfo::destroy();
    }

    /// The controls of `deck` as "<has artist> <has genre>": "1 0" is a
    /// track with an artist and no genre.
    static QString fieldsOf(int deck) {
        return QStringLiteral("%1 %2")
                .arg(ControlObject::get(DeckTrackFields::hasArtistKey(deck)))
                .arg(ControlObject::get(DeckTrackFields::hasGenreKey(deck)));
    }

    std::vector<std::unique_ptr<ControlObject>> m_controls;
};

TEST_F(CrateDeckTrackFieldsTest, FollowsTheTrackOfEachDeck) {
    DeckTrackFields fields;
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(1));
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(2));

    PlayerInfo::instance().setTrackInfo(kDeck1, trackWith(QStringLiteral("SNAYX"), QString()));
    EXPECT_QSTRING_EQ(QStringLiteral("1 0"), fieldsOf(1));
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(2));

    PlayerInfo::instance().setTrackInfo(kDeck2,
            trackWith(QStringLiteral("Aya Lindqvist"), QStringLiteral("Downtempo")));
    EXPECT_QSTRING_EQ(QStringLiteral("1 0"), fieldsOf(1));
    EXPECT_QSTRING_EQ(QStringLiteral("1 1"), fieldsOf(2));

    // Blanks are no artist
    PlayerInfo::instance().setTrackInfo(kDeck1,
            trackWith(QStringLiteral("  "), QStringLiteral("Organic House")));
    EXPECT_QSTRING_EQ(QStringLiteral("0 1"), fieldsOf(1));

    // An empty deck has neither
    PlayerInfo::instance().setTrackInfo(kDeck2, TrackPointer());
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(2));

    // Samplers and preview decks are no decks
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Sampler1]"),
            trackWith(QStringLiteral("SNAYX"), QStringLiteral("Alternative")));
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[PreviewDeck1]"),
            trackWith(QStringLiteral("SNAYX"), QStringLiteral("Alternative")));
    EXPECT_QSTRING_EQ(QStringLiteral("0 1"), fieldsOf(1));
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(2));
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Sampler1]"), TrackPointer());
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[PreviewDeck1]"), TrackPointer());
}

TEST_F(CrateDeckTrackFieldsTest, FollowsEditsOfTheTrack) {
    DeckTrackFields fields;
    const TrackPointer pTrack = trackWith(QString(), QString());
    PlayerInfo::instance().setTrackInfo(kDeck1, pTrack);
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(1));

    pTrack->setArtist(QStringLiteral("SNAYX"));
    EXPECT_QSTRING_EQ(QStringLiteral("1 0"), fieldsOf(1));
    pTrack->updateGenre(QStringLiteral("Alternative"));
    EXPECT_QSTRING_EQ(QStringLiteral("1 1"), fieldsOf(1));
    pTrack->setArtist(QString());
    EXPECT_QSTRING_EQ(QStringLiteral("0 1"), fieldsOf(1));

    // Once another track is in the deck, the first one no longer counts
    PlayerInfo::instance().setTrackInfo(kDeck1,
            trackWith(QStringLiteral("Lea Fournier"), QString()));
    EXPECT_QSTRING_EQ(QStringLiteral("1 0"), fieldsOf(1));
    pTrack->setArtist(QStringLiteral("SNAYX"));
    pTrack->updateGenre(QString());
    pTrack->updateGenre(QStringLiteral("Alternative"));
    EXPECT_QSTRING_EQ(QStringLiteral("1 0"), fieldsOf(1));
}

TEST_F(CrateDeckTrackFieldsTest, StartsFromTheTracksAlreadyInTheDecks) {
    PlayerInfo::instance().setTrackInfo(kDeck2,
            trackWith(QStringLiteral("SNAYX"), QStringLiteral("Alternative")));
    // A skin reload puts the controls back to 0 before the skin's objects
    // start again
    ControlObject::set(DeckTrackFields::hasArtistKey(2), 0);
    ControlObject::set(DeckTrackFields::hasGenreKey(2), 0);

    DeckTrackFields fields;
    EXPECT_QSTRING_EQ(QStringLiteral("0 0"), fieldsOf(1));
    EXPECT_QSTRING_EQ(QStringLiteral("1 1"), fieldsOf(2));
}

TEST_F(CrateDeckTrackFieldsTest, LeavesDecksWithoutControlsAlone) {
    DeckTrackFields fields;
    // The skin has no controls for a third deck here: nothing to set, and
    // nothing created
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel3]"),
            trackWith(QStringLiteral("SNAYX"), QStringLiteral("Alternative")));
    EXPECT_FALSE(ControlObject::exists(DeckTrackFields::hasArtistKey(3)));
    EXPECT_FALSE(ControlObject::exists(DeckTrackFields::hasGenreKey(3)));
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel3]"), TrackPointer());
}

} // namespace
