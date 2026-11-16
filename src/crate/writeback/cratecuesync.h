#pragma once

#include "crate/writeback/cuemapping.h"
#include "crate/writeback/trackdatasync.h"

namespace crate::writeback {

/// Keeps the user's cues of Crate tracks in sync across their machines
/// (plan F8.C-PR1, contract S7 `/api/decks/cues`).
///
/// - **On load** the server's set of the track, read in a batch with the
///   other tracks loaded at about the same time, replaces the track's when
///   its revision is newer than the last one Decks synced: it takes
///   priority over the Intro and Outro of Crate's profile, which never
///   replace a cue of their type, and over upstream's silence analysis.
///   The hot cue slots 0-35 are the set's alone; main, intro and outro are
///   replaced only when the set has them (applySyncedCues()).
/// - **On edit** the whole set is sent kDebounce after the last change,
///   through the Outbox. Changes of the analysis are not edits: the main,
///   intro and outro cues they place are absorbed into the baseline. A
///   track never synced before sends nothing until it differs from its
///   automatic cues, so the first set on the server is the user's hot cues,
///   not the analysis.
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

  protected:
    bool reconcile(const QString& key,
            const TrackPointer& pTrack,
            qint64 serverRevision,
            const QJsonObject& serverEntry) override;
    QJsonObject contentOf(const QJsonObject& serverEntry) const override;
    QJsonObject applyServerContent(
            const TrackPointer& pTrack, const QJsonObject& content) override;
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
