#pragma once

#include <QDrag>
#include <QDropEvent>
#include <QList>
#include <QMimeData>
#include <QString>
#include <QUrl>

#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/fileinfo.h"
#include "widget/trackdroptarget.h"

/// A drag source that loads what it drags itself: rows that have no local
/// file yet, such as Crate tracks that are not downloaded. Their drag
/// carries no supported file, so a deck asks the source instead.
class TrackDragSource {
  public:
    virtual ~TrackDragSource() = default;
    /// Whether it can load what it drags into a deck
    virtual bool canLoadDraggedTracks() const = 0;
    virtual void loadDraggedTracks(const QString& group) = 0;
};

class DragAndDropHelper final {
  public:
    DragAndDropHelper() = delete;

    static bool urlsContainSupportedTrackFiles(
            const QList<QUrl>& urls,
            bool acceptPlaylists);

    static QList<mixxx::FileInfo> supportedTracksFromUrls(
            const QList<QUrl>& urls,
            bool stopOnFirstMatch,
            bool acceptPlaylists);

    /// For a drop with no supported file: whether its drag source loads
    /// what it drags itself (TrackDragSource), and asking it to.
    static bool canLoadByDragSource(QObject* pSource);
    static bool loadByDragSource(QObject* pSource, const QString& group);

    static bool allowDeckCloneAttempt(
            const QDropEvent& event,
            const QString& group);

    static bool dragEnterAccept(
            const QMimeData& mimeData,
            const QString& sourceIdentifier,
            bool stopOnFirstMatch,
            bool acceptPlaylists);

    static QDrag* dragTrack(
            TrackPointer pTrack,
            QWidget* pDragSource,
            const QString& sourceIdentifier);

    static QDrag* dragTrackLocations(
            const QList<QString>& locations,
            QWidget* pDragSource,
            const QString& sourceIdentifier);

    static void handleTrackDragEnterEvent(
            QDragEnterEvent* pEvent,
            const QString& group,
            UserSettingsPointer pConfig);

    static void handleTrackDropEvent(
            QDropEvent* pEvent,
            TrackDropTarget& target,
            const QString& group,
            UserSettingsPointer pConfig);

    static void mousePressed(QMouseEvent* pEvent);

    static bool mouseMoveInitiatesDrag(QMouseEvent* pEvent);
};
