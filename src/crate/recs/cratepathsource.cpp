#include "crate/recs/cratepathsource.h"

#include <QButtonGroup>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

#include "crate/client/crateclient.h"
#include "crate/recs/recommendationqueue.h"
#include "moc_cratepathsource.cpp"
#include "util/logger.h"

namespace crate::recs {

namespace {

const mixxx::Logger kLogger("crate::recs::CratePathSource");

/// Computes a path without saving it among the user's paths (Listen's).
const QString kPreviewPath = QStringLiteral("/api/paths/preview");

QJsonObject trackEndpoint(const QString& entityUid) {
    return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("track")},
            {QStringLiteral("value"), entityUid},
    };
}

} // namespace

CratePathSource::CratePathSource(RecommendationQueue* pQueue, QObject* parent)
        : QObject(parent),
          m_pQueue(pQueue) {
}

CratePathSource::~CratePathSource() {
    if (Reply* pReply = m_pReply.data()) {
        pReply->disconnect(this);
        pReply->cancel();
    }
}

void CratePathSource::request(Client* pClient, const PathRequest& request) {
    if (Reply* pOld = m_pReply.data()) {
        m_pReply = nullptr;
        pOld->disconnect(this);
        pOld->cancel();
    }
    if (!pClient) {
        emit failed(Error::network(QStringLiteral("The Crate server is gone")));
        return;
    }
    const QJsonObject body{
            {QStringLiteral("origin"), trackEndpoint(request.fromEntityUid)},
            {QStringLiteral("destination"), trackEndpoint(request.toEntityUid)},
            {QStringLiteral("step_count"),
                    std::clamp(request.steps, PathRequest::kMinSteps, PathRequest::kMaxSteps)},
    };
    RequestOptions options;
    options.rawBody = true;
    Reply* pReply = pClient->post(kPreviewPath, QJsonDocument(body), options);
    m_pReply = pReply;
    connect(pReply, &Reply::finished, this, [this, pReply, request](const JsonResult& result) {
        if (m_pReply == pReply) {
            m_pReply = nullptr;
        }
        onReply(pReply, request, result);
    });
}

void CratePathSource::onReply(Reply* pReply, const PathRequest& request, const JsonResult& result) {
    if (!result.ok()) {
        if (result.error().kind != Error::Kind::Cancelled) {
            kLogger.warning() << "Cannot get the Music Path:" << result.error().message;
            emit failed(result.error());
        }
        return;
    }
    const Result<MusicPath> path = parseMusicPath(pReply->body());
    if (!path.ok()) {
        kLogger.warning() << "Cannot read the Music Path:" << path.error().message;
        emit failed(path.error());
        return;
    }
    if (path.value().tracks.isEmpty()) {
        emit failed(Error::decode(QStringLiteral("The path has no tracks")));
        return;
    }
    QString playlistName;
    if (request.destination == PathRequest::Destination::Playlist) {
        QString name = request.playlistName.trimmed();
        if (name.isEmpty()) {
            name = path.value().name;
        }
        if (name.isEmpty()) {
            name = tr("Music Path");
        }
        playlistName = m_pQueue->saveAsPlaylist(name, request.serverId, path.value().tracks);
        if (playlistName.isEmpty()) {
            emit failed(Error::storage(QStringLiteral("Cannot create the playlist")));
            return;
        }
    } else {
        m_pQueue->enqueue(request.serverId, path.value().tracks);
    }
    emit finished(path.value(), playlistName);
}

CratePathDialog::CratePathDialog(
        const QString& fromLabel, const QString& toLabel, QWidget* pParent)
        : QDialog(pParent),
          m_pSteps(new QSpinBox(this)),
          m_pToQueue(new QRadioButton(tr("Add to the Auto DJ queue"), this)),
          m_pToPlaylist(new QRadioButton(tr("Save as a new playlist"), this)) {
    //: The title of the dialog that asks Crate for a Music Path between two tracks
    setWindowTitle(tr("Music Path"));
    auto* pLayout = new QVBoxLayout(this);
    auto* pIntro = new QLabel(
            //: %1 and %2 are tracks: "Artist - Title"
            tr("A path of tracks from %1 to %2, through the sound of your library.")
                    .arg(fromLabel, toLabel),
            this);
    pIntro->setWordWrap(true);
    pLayout->addWidget(pIntro);

    auto* pForm = new QFormLayout();
    m_pSteps->setRange(PathRequest::kMinSteps, PathRequest::kMaxSteps);
    m_pSteps->setValue(20);
    pForm->addRow(tr("Tracks"), m_pSteps);
    pLayout->addLayout(pForm);

    auto* pDestinations = new QButtonGroup(this);
    pDestinations->addButton(m_pToQueue);
    pDestinations->addButton(m_pToPlaylist);
    m_pToQueue->setChecked(true);
    pLayout->addWidget(m_pToQueue);
    pLayout->addWidget(m_pToPlaylist);

    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(pButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    pLayout->addWidget(pButtons);
}

int CratePathDialog::steps() const {
    return m_pSteps->value();
}

PathRequest::Destination CratePathDialog::destination() const {
    return m_pToPlaylist->isChecked() ? PathRequest::Destination::Playlist
                                      : PathRequest::Destination::AutoDjQueue;
}

} // namespace crate::recs
