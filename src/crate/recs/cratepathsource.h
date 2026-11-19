#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#include "crate/client/crateresult.h"
#include "crate/recs/recsjson.h"
#include "crate/ui/dialogs/cratedialog.h"

class QButtonGroup;
class QLabel;
class QRadioButton;
class QSpinBox;

namespace crate {
class Client;
class Reply;
} // namespace crate

namespace crate::recs {

class RecommendationQueue;

/// One Music Path to ask for: from one Crate track to another, in `steps`
/// tracks, both ends included.
struct PathRequest {
    QString serverId;
    QString fromEntityUid;
    QString toEntityUid;
    /// The server takes 5 to 100.
    int steps = 20;

    enum class Destination {
        /// The end of Auto DJ's queue.
        AutoDjQueue,
        /// A new playlist of this computer's library.
        Playlist,
    };
    Destination destination = Destination::AutoDjQueue;
    /// The playlist's name; empty takes the path's ("A -> B").
    QString playlistName;

    static constexpr int kMinSteps = 5;
    static constexpr int kMaxSteps = 100;
};

/// Crate's Music Paths as a source of Auto DJ (plan F5.C-PR3): a path from
/// one track to another, which the server walks through the similarity of
/// their sound, goes in order into Auto DJ's queue or into a new playlist.
///
/// It asks `POST /api/paths/preview`, which computes the path without
/// saving it among the user's paths on the server (Listen's). One path at a
/// time: a new request drops the one in flight. Lives on the main thread.
class CratePathSource : public QObject {
    Q_OBJECT
  public:
    /// Not owned: `pQueue` must outlive it.
    explicit CratePathSource(RecommendationQueue* pQueue, QObject* parent = nullptr);
    ~CratePathSource() override;

    void request(Client* pClient, const PathRequest& request);
    bool isBusy() const {
        return m_pReply;
    }

  signals:
    /// The path is in the queue, or in the playlist `playlistName`.
    void finished(const crate::recs::MusicPath& path, const QString& playlistName);
    void failed(const crate::Error& error);

  private:
    void onReply(Reply* pReply, const PathRequest& request, const JsonResult& result);

    RecommendationQueue* const m_pQueue;
    QPointer<Reply> m_pReply;
};

/// "Music Path from A to B": how many tracks, and where they go. A dialog
/// of Decks' own (plan V3.PR3): the tracks and the destination are sections
/// of the first start's grammar, and "Make the path" accepts it.
class CratePathDialog : public CrateDialog {
    Q_OBJECT
  public:
    CratePathDialog(const QString& fromLabel, const QString& toLabel, QWidget* pParent = nullptr);

    int steps() const;
    PathRequest::Destination destination() const;

    QSpinBox* stepsBox() const {
        return m_pSteps;
    }
    QRadioButton* toQueue() const {
        return m_pToQueue;
    }
    QRadioButton* toPlaylist() const {
        return m_pToPlaylist;
    }

  private:
    QSpinBox* m_pSteps;
    QRadioButton* m_pToQueue;
    QRadioButton* m_pToPlaylist;
};

} // namespace crate::recs
