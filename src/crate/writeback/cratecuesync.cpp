#include "crate/writeback/cratecuesync.h"

#include <QJsonArray>

#include "crate/writeback/outbox.h"
#include "track/track.h"
#include "util/logger.h"

namespace crate::writeback {

namespace {

const mixxx::Logger kLogger("crate::writeback::CrateCueSync");

const QString kCuesKey = QStringLiteral("cues");

} // namespace

QJsonObject cueSetContent(const SyncedCues& cues) {
    return QJsonObject{{kCuesKey, toJson(cues)}};
}

SyncedCues cuesOfContent(const QJsonObject& content) {
    return syncedCuesFromJson(content.value(kCuesKey).toArray());
}

CrateCueSync::CrateCueSync(Options options, QObject* pParent)
        : TrackDataSync(Endpoint{QStringLiteral("cues"),
                                QStringLiteral("/api/decks/cues/query"),
                                QStringLiteral("cue_sets"),
                                outboxkind::kCueSet},
                  std::move(options),
                  pParent) {
}

void CrateCueSync::flushEdits() {
    runDebounced();
}

QJsonObject CrateCueSync::contentOf(const QJsonObject& serverEntry) const {
    return cueSetContent(syncedCuesFromJson(serverEntry.value(kCuesKey).toArray()));
}

QJsonObject CrateCueSync::applyServerContent(
        const QString& key, const TrackPointer& pTrack, const QJsonObject& content) {
    Q_UNUSED(key);
    if (!applySyncedCues(pTrack, cuesOfContent(content))) {
        kLogger.warning() << "Cannot apply the synced cues of" << pTrack->getLocation()
                          << ": its sample rate is not known yet";
    }
    return cueSetContent(syncedCuesOf(pTrack));
}

QJsonObject CrateCueSync::contentToResend(const QString& key, const QJsonObject& sent) {
    // The track's cues as they are now, which include the edit sent
    if (const TrackPointer pTrack = trackOf(key)) {
        return cueSetContent(syncedCuesOf(pTrack));
    }
    return sent;
}

bool CrateCueSync::reconcile(const QString& key,
        const TrackPointer& pTrack,
        qint64 serverRevision,
        const QJsonObject& serverEntry) {
    if (!pTrack->getSampleRate().isValid()) {
        kLogger.info() << "No sample rate yet for" << pTrack->getLocation()
                       << ": its cues sync at the next load";
        return false;
    }
    if (uploadPending(key)) {
        // Its answer, or its 409, settles the revision
        return true;
    }
    State& known = state(key);
    const SyncedCues local = syncedCuesOf(pTrack);
    if (serverRevision > known.revision) {
        const SyncedCues server = syncedCuesFromJson(serverEntry.value(kCuesKey).toArray());
        if (known.known || withAutomaticCues(local, {}).isEmpty()) {
            // Edits here not uploaded yet: still waiting for their debounce,
            // or made while the track was not reconciled
            const bool editedHere = known.known && cueSetContent(local) != known.baseline;
            // The server's set is the track's whole set
            applyFromServer(key, pTrack, cueSetContent(server), serverRevision);
            if (editedHere) {
                kLogger.info() << "The cues of" << entityUidOf(key)
                               << "changed on another computer before the edits here went up:"
                               << "the server's win";
                emit localEditsReplaced(serverIdOf(key),
                        entityUidOf(key),
                        pTrack->getTitle().isEmpty() ? entityUidOf(key) : pTrack->getTitle());
            }
            return true;
        }
        // The first sync of a track with hot cues set here: the server's
        // stay, the ones of this computer go to free slots, and the merged
        // set goes up (Diego, 2026-11-16)
        const CueMerge merge = mergeLocalCues(server, local);
        for (const SyncedCue& cue : merge.dropped) {
            kLogger.warning() << "No free hot cue slot for the" << cue.type << "at" << *cue.startMs
                              << "ms of" << entityUidOf(key) << ": dropped";
        }
        applyFromServer(key, pTrack, cueSetContent(merge.cues), serverRevision);
        if (merge.cues != server) {
            enqueuePut(key, cueSetContent(syncedCuesOf(pTrack)), editStamp(key));
        }
        if (merge.moved > 0 || !merge.dropped.isEmpty()) {
            emit cuesMerged(serverIdOf(key),
                    entityUidOf(key),
                    pTrack->getTitle().isEmpty() ? entityUidOf(key) : pTrack->getTitle(),
                    merge.moved,
                    static_cast<int>(merge.dropped.size()));
        }
        return true;
    }
    if (!known.known) {
        // Never synced: what the analysis placed is not an edit, the hot cues
        // the user already set here are
        known.revision = serverRevision;
        known.baseline = cueSetContent(automaticCues(local));
        storeState(key);
        if (cueSetContent(local) != known.baseline) {
            debounce(key, [this, key] {
                check(key);
            });
        }
        return true;
    }
    if (serverRevision < known.revision) {
        kLogger.warning() << "The server has revision" << serverRevision << "of the cues of"
                          << entityUidOf(key) << ", older than" << known.revision
                          << "that Decks synced: taking it";
        known.revision = serverRevision;
        storeState(key);
    }
    if (cueSetContent(local) != known.baseline) {
        // Edited here since the last sync
        debounce(key, [this, key] {
            check(key);
        });
    }
    return true;
}

void CrateCueSync::trackChanged(const QString& key, const TrackPointer& pTrack, Change change) {
    Q_UNUSED(pTrack);
    if (change != Change::Cues) {
        return;
    }
    debounce(key, [this, key] {
        check(key);
    });
}

void CrateCueSync::absorb(const QString& key, const TrackPointer& pTrack) {
    State& known = state(key);
    const SyncedCues local = syncedCuesOf(pTrack);
    if (known.revision > 0) {
        // The server has the user's set, which rules main, intro and outro
        // too: what the analysis put there or took away goes back to it
        const SyncedCues wanted = withAutomaticCues(local, cuesOfContent(known.baseline));
        if (wanted != local) {
            changeQuietly(key, [&] {
                applySyncedCues(pTrack, wanted);
            });
        }
    } else {
        // Never on the server: the analysis' cues are no edit
        const QJsonObject baseline =
                cueSetContent(withAutomaticCues(cuesOfContent(known.baseline), local));
        if (baseline != known.baseline) {
            known.baseline = baseline;
            storeState(key);
        }
    }
    // A change of the hot cues meanwhile is still an edit
    if (cueSetContent(syncedCuesOf(pTrack)) != known.baseline) {
        debounce(key, [this, key] {
            check(key);
        });
    }
}

void CrateCueSync::check(const QString& key) {
    const TrackPointer pTrack = trackOf(key);
    if (!pTrack || !isReconciled(key) || !stateOf(key).known) {
        return;
    }
    const QJsonObject content = cueSetContent(syncedCuesOf(pTrack));
    if (content == stateOf(key).baseline) {
        return;
    }
    enqueuePut(key, content, editStamp(key));
}

} // namespace crate::writeback

#include "moc_cratecuesync.cpp"
