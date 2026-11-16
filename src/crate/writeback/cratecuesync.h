#pragma once

#include "crate/writeback/cuemapping.h"
#include "crate/writeback/trackdatasync.h"

namespace crate::writeback {

/// Keeps the user's cues of Crate tracks in sync across their machines
/// (plan F8.C-PR1, contract S7 `/api/decks/cues`).
///
/// - **On load** the server's set of the track, read in a batch with the
///   other tracks loaded at about the same time, replaces the track's when
///   its revision is newer than the last one Decks synced. Once the server
///   has a set it rules every cue type (Diego, 2026-11-16): a set without
///   an intro removes the track's, and what Crate's profile or upstream's
///   silence analysis put back is taken away again, so deletions
///   propagate. The first sync of a track with hot cues set here merges
///   (mergeLocalCues()): the server's stay, the local ones move to free
///   slots, and the merged set goes up; cuesMerged() tells the library.
/// - **On edit** the whole set is sent kDebounce after the last change,
///   through the Outbox. Changes of the analysis are not edits: the main,
///   intro and outro cues they place are absorbed into the baseline. A
///   track never synced before sends nothing until it differs from its
///   automatic cues, so the analysis alone never puts a set on the server.
/// - **On a 409** the server's set wins when another computer wrote it
///   after the edit here (localEditsReplaced(), which the library shows);
///   otherwise the edit is sent again on top of it.
class CrateCueSync : public TrackDataSync {
    Q_OBJECT
  public:
    explicit CrateCueSync(Options options, QObject* pParent = nullptr);

    /// Sends now the edits that wait for their debounce: at shutdown, so
    /// that the outbox keeps them.
    void flushEdits();

  signals:
    /// The first sync of a track kept the server's cues and moved `moved`
    /// hot cues, loops and jumps set here to free slots; `dropped` found
    /// none free.
    void cuesMerged(const QString& serverId,
            const QString& entityUid,
            const QString& trackTitle,
            int moved,
            int dropped);

  protected:
    bool reconcile(const QString& key,
            const TrackPointer& pTrack,
            qint64 serverRevision,
            const QJsonObject& serverEntry) override;
    QJsonObject contentOf(const QJsonObject& serverEntry) const override;
    QJsonObject applyServerContent(const QString& key,
            const TrackPointer& pTrack,
            const QJsonObject& content) override;
    QJsonObject contentToResend(const QString& key, const QJsonObject& sent) override;
    void trackChanged(const QString& key, const TrackPointer& pTrack, Change change) override;
    void absorb(const QString& key, const TrackPointer& pTrack) override;

  private:
    /// Sends the track's cues if they differ from the baseline.
    void check(const QString& key);
};

/// The content a cue set keeps in the baseline and sends: `{"cues": [...]}`.
QJsonObject cueSetContent(const SyncedCues& cues);
SyncedCues cuesOfContent(const QJsonObject& content);

} // namespace crate::writeback
