#include "crate/ui/skin/crateserverstatus.h"

#include <QLabel>
#include <QStyle>
#include <QWidget>

#include "crate/crateservers.h"
#include "crate/session/cratesession.h"
#include "moc_crateserverstatus.cpp"
#include "widget/wlabel.h"

namespace crate {

namespace {

const QString kStatusLabelName = QStringLiteral("MonoCrateStatus");
const QString kStatusDotName = QStringLiteral("CrateStatusDot");
const char kStateProperty[] = "crateState";

QString hostOf(const ServerProfile& profile) {
    const QString host = profile.baseUrl.host();
    return host.isEmpty() ? profile.displayName : host;
}

ServerStatus::State stateOf(Session::State sessionState) {
    switch (sessionState) {
    case Session::State::SigningIn:
        return ServerStatus::State::Connecting;
    case Session::State::SignedIn:
        return ServerStatus::State::Connected;
    case Session::State::SignedOut:
        return ServerStatus::State::SignedOut;
    }
    return ServerStatus::State::None;
}

} // namespace

ServerStatus::ServerStatus(CrateServers* pServers, QObject* parent)
        : QObject(parent),
          m_pServers(pServers) {
    if (m_pServers) {
        connect(m_pServers, &CrateServers::serversChanged, this, &ServerStatus::followServers);
    }
    followServers();
}

QString ServerStatus::text() const {
    switch (m_state) {
    case State::None:
        return tr("Crate · no server");
    case State::SignedOut:
        return tr("Crate · %1 · signed out").arg(m_host);
    case State::Connecting:
    case State::Connected:
        break;
    }
    return tr("Crate · %1").arg(m_host);
}

QString ServerStatus::stateName() const {
    switch (m_state) {
    case State::Connecting:
        return QStringLiteral("connecting");
    case State::Connected:
        return QStringLiteral("connected");
    case State::SignedOut:
        return QStringLiteral("signedOut");
    case State::None:
        break;
    }
    return QStringLiteral("none");
}

void ServerStatus::showStatus(const QString& host, State state) {
    m_host = host;
    m_state = state;
    emit changed();
}

void ServerStatus::followServers() {
    for (const QMetaObject::Connection& connection : std::as_const(m_sessionConnections)) {
        disconnect(connection);
    }
    m_sessionConnections.clear();
    if (m_pServers) {
        const QList<Session*> sessions = m_pServers->sessions();
        if (!sessions.isEmpty()) {
            Session* pSession = sessions.first();
            m_sessionConnections.append(connect(
                    pSession, &Session::stateChanged, this, &ServerStatus::update));
            m_sessionConnections.append(connect(
                    pSession, &Session::profileChanged, this, &ServerStatus::update));
        }
    }
    update();
}

void ServerStatus::update() {
    const QList<Session*> sessions = m_pServers ? m_pServers->sessions() : QList<Session*>();
    if (sessions.isEmpty()) {
        showStatus(QString(), State::None);
        return;
    }
    const Session* pSession = sessions.first();
    showStatus(hostOf(pSession->profile()), stateOf(pSession->state()));
}

void bindServerStatus(ServerStatus* pStatus, QWidget* pSkinRoot) {
    if (!pStatus || !pSkinRoot) {
        return;
    }
    QList<QWidget*> widgets;
    const QList<QLabel*> labels = pSkinRoot->findChildren<QLabel*>(kStatusLabelName);
    const QList<QWidget*> dots = pSkinRoot->findChildren<QWidget*>(kStatusDotName);
    widgets.append(QList<QWidget*>(labels.cbegin(), labels.cend()));
    widgets.append(dots);
    if (widgets.isEmpty()) {
        return;
    }
    const auto show = [pStatus, labels, widgets]() {
        for (QLabel* pLabel : labels) {
            // WLabel keeps its own copy of the text (for eliding), which
            // QLabel::setText() would not update
            if (auto* pWLabel = qobject_cast<WLabel*>(pLabel)) {
                pWLabel->setText(pStatus->text());
            } else {
                pLabel->setText(pStatus->text());
            }
        }
        for (QWidget* pWidget : widgets) {
            pWidget->setProperty(kStateProperty, pStatus->stateName());
            // Restyle, as the skin's property connections do: QSS selects
            // on the property
            pWidget->style()->polish(pWidget);
            pWidget->update();
        }
    };
    QObject::connect(pStatus, &ServerStatus::changed, pSkinRoot, show);
    show();
}

} // namespace crate
