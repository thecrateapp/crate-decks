#pragma once

#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include "preferences/configobject.h"
#include "track/track_decl.h"

namespace crate {

/// Whether the track of each deck has an artist and a genre, for the skin's
/// "Artist · Genre" row under the deck's title (plan V2, gaps of V2.PR2): a
/// TrackProperty does not tell whether its field is empty, and no control
/// does, so the row's separator would stay alone, as in "SNAYX ·".
///
/// Sets [CrateDecks],deckN_has_artist and deckN_has_genre to 1 while the
/// field has text other than blanks, and to 0 otherwise or with no track.
/// It follows the decks' tracks through PlayerInfo and their edits through
/// the tracks' signals. Without widgets: the skin binds the row to the
/// controls, which its manifest declares; a deck whose controls do not exist
/// is left alone.
class DeckTrackFields : public QObject {
    Q_OBJECT
  public:
    /// Starts from the tracks that PlayerInfo has for the decks already, as
    /// after a skin reload, which puts the controls back to 0.
    explicit DeckTrackFields(QObject* parent = nullptr);
    ~DeckTrackFields() override;

    static ConfigKey hasArtistKey(int deck);
    static ConfigKey hasGenreKey(int deck);

  private slots:
    void slotTrackChanged(const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack);

  private:
    void follow(int deck, const TrackPointer& pTrack);

    /// The connections to the track of each deck, by deck number.
    QHash<int, QList<QMetaObject::Connection>> m_trackConnections;
};

} // namespace crate
