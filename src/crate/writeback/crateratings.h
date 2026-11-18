#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <functional>
#include <optional>

#include "crate/cache/audiocache.h"
#include "track/track_decl.h"
#include "track/trackid.h"
#include "util/db/dbconnectionpool.h"

namespace crate::writeback {

class Outbox;

/// Sends the rating (0-5) the user gives a Crate track in Decks to its
/// server, `POST /api/track/rate`, through the Outbox (plan F7.C-PR2).
///
/// It listens to TrackCollection::tracksChanged, which upstream's tracks
/// emit for any change, and queues a rating only when a Crate track's rating
/// differs from the last one sent, which `crateserver_ratings` keeps across
/// runs. A track never sent before is sent from its first rating above 0:
/// a 0 there means Decks has no rating, not that the user cleared the one
/// Crate has. No upstream code changes: there is no hook in the track's
/// save.
class CrateRatings : public QObject {
    Q_OBJECT
  public:
    struct Options {
        /// Not owned; required.
        Outbox* pOutbox = nullptr;
        mixxx::DbConnectionPoolPtr pDbConnectionPool;
        /// The Crate track whose cached file is at `location`, if any.
        std::function<std::optional<cache::TrackKey>(const QString& location)> trackOfFile;
        /// The track with this id if it is in memory; nullptr otherwise.
        /// Defaults to upstream's track cache.
        std::function<TrackPointer(TrackId trackId)> trackById;
    };

    explicit CrateRatings(Options options, QObject* pParent = nullptr);
    ~CrateRatings() override;

    /// Reads the ratings sent so far; changes before it ends wait for it.
    void start();
    bool isLoaded() const {
        return m_loaded;
    }
    /// Waits for the table's reads and writes in flight. For tests.
    void waitForStorage();

  public slots:
    /// TrackCollection::tracksChanged.
    void onTracksChanged(const QSet<TrackId>& trackIds);

  private:
    void onLoaded(const QHash<QString, int>& sent);
    void check(const TrackPointer& pTrack);

    Options m_options;
    QThreadPool m_pool;
    bool m_loaded = false;
    bool m_loading = false;
    /// The last rating sent of each track, by "<server>\n<lowercase uid>".
    QHash<QString, int> m_sent;
    /// Tracks changed before the ratings were read.
    QSet<TrackId> m_waiting;
};

} // namespace crate::writeback
