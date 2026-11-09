#include "crate/ui/skin/decktrackfields.h"

#include "control/controlobject.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_decktrackfields.cpp"
#include "track/track.h"

namespace crate {

namespace {

const QString kGroup = QStringLiteral("[CrateDecks]");

/// 1 while `text` has more than blanks
double hasText(const QString& text) {
    return text.trimmed().isEmpty() ? 0.0 : 1.0;
}

} // namespace

DeckTrackFields::DeckTrackFields(QObject* parent)
        : QObject(parent) {
    PlayerInfo& playerInfo = PlayerInfo::instance();
    connect(&playerInfo, &PlayerInfo::trackChanged, this, &DeckTrackFields::slotTrackChanged);
    // The skin declares the controls of its decks, from the first one on
    for (int deck = 1; ControlObject::exists(hasArtistKey(deck)); ++deck) {
        follow(deck, playerInfo.getTrackInfo(PlayerManager::groupForDeck(deck - 1)));
    }
}

DeckTrackFields::~DeckTrackFields() = default;

ConfigKey DeckTrackFields::hasArtistKey(int deck) {
    return ConfigKey(kGroup, QStringLiteral("deck%1_has_artist").arg(deck));
}

ConfigKey DeckTrackFields::hasGenreKey(int deck) {
    return ConfigKey(kGroup, QStringLiteral("deck%1_has_genre").arg(deck));
}

void DeckTrackFields::slotTrackChanged(const QString& group,
        TrackPointer pNewTrack,
        TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    int deck = 0;
    if (!PlayerManager::isDeckGroup(group, &deck) || !ControlObject::exists(hasArtistKey(deck))) {
        return;
    }
    follow(deck, pNewTrack);
}

void DeckTrackFields::follow(int deck, const TrackPointer& pTrack) {
    for (const QMetaObject::Connection& connection : m_trackConnections.take(deck)) {
        disconnect(connection);
    }
    const ConfigKey artistKey = hasArtistKey(deck);
    const ConfigKey genreKey = hasGenreKey(deck);
    if (!pTrack) {
        ControlObject::set(artistKey, 0.0);
        ControlObject::set(genreKey, 0.0);
        return;
    }
    // Tracks can change in other threads: the context queues those calls
    m_trackConnections.insert(deck,
            {connect(pTrack.get(),
                     &Track::artistChanged,
                     this,
                     [artistKey](const QString& artist) {
                         ControlObject::set(artistKey, hasText(artist));
                     }),
                    connect(pTrack.get(),
                            &Track::genreChanged,
                            this,
                            [genreKey](const QString& genre) {
                                ControlObject::set(genreKey, hasText(genre));
                            })});
    ControlObject::set(artistKey, hasText(pTrack->getArtist()));
    ControlObject::set(genreKey, hasText(pTrack->getGenre()));
}

} // namespace crate
