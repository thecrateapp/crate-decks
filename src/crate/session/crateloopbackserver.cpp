#include "crate/session/crateloopbackserver.h"

#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "util/logger.h"

namespace crate {

namespace {

const mixxx::Logger kLogger("crate::LoopbackCallbackServer");

/// A browser's request line and headers are far below this.
constexpr qint64 kMaxRequestBytes = 16 * 1024;
/// A connection that sends no complete request in this time is dropped.
constexpr int kRequestTimeoutMs = 10000;

QByteArray httpResponse(int status, const QByteArray& reason, const QByteArray& body) {
    return QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' + reason +
            QByteArrayLiteral(
                    "\r\nContent-Type: text/html; charset=utf-8\r\n"
                    "Cache-Control: no-store\r\n"
                    "Connection: close\r\n"
                    "Content-Length: ") +
            QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
}

QByteArray page(const QString& text) {
    return QStringLiteral(
            "<!doctype html><html><head><meta charset=\"utf-8\">"
            "<title>Crate Decks</title></head>"
            "<body style=\"font-family: sans-serif; margin: 4em; text-align: center\">"
            "<h1>Crate Decks</h1><p>%1</p></body></html>")
            .arg(text.toHtmlEscaped())
            .toUtf8();
}

void answer(QTcpSocket* pSocket, const QByteArray& response) {
    pSocket->write(response);
    // Sends what is buffered, then closes; the socket deletes itself when
    // it is disconnected.
    pSocket->disconnectFromHost();
}

} // namespace

LoopbackCallbackServer::LoopbackCallbackServer(QObject* parent)
        : QObject(parent),
          m_pServer(new QTcpServer(this)) {
    connect(m_pServer,
            &QTcpServer::newConnection,
            this,
            &LoopbackCallbackServer::onNewConnection);
}

LoopbackCallbackServer::~LoopbackCallbackServer() = default;

bool LoopbackCallbackServer::listen() {
    if (m_pServer->isListening()) {
        return true;
    }
    // Only the loopback interface: the server contract accepts
    // http://127.0.0.1:<port>/callback and nothing else.
    if (!m_pServer->listen(QHostAddress(QHostAddress::LocalHost), 0)) {
        kLogger.warning() << "Cannot listen on 127.0.0.1:" << m_pServer->errorString();
        return false;
    }
    return true;
}

bool LoopbackCallbackServer::isListening() const {
    return m_pServer->isListening();
}

QHostAddress LoopbackCallbackServer::address() const {
    return m_pServer->serverAddress();
}

quint16 LoopbackCallbackServer::port() const {
    return m_pServer->serverPort();
}

QUrl LoopbackCallbackServer::callbackUrl() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/callback").arg(port()));
}

void LoopbackCallbackServer::close() {
    m_pServer->close();
}

void LoopbackCallbackServer::onNewConnection() {
    while (QTcpSocket* pSocket = m_pServer->nextPendingConnection()) {
        // Not a child of the server: the session deletes the server right
        // after the callback, and the browser must still get its page. The
        // socket deletes itself once disconnected, or after the timeout.
        pSocket->setParent(nullptr);
        connect(pSocket, &QTcpSocket::disconnected, pSocket, &QObject::deleteLater);
        connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket] {
            onReadyRead(pSocket);
        });
        QTimer::singleShot(kRequestTimeoutMs, pSocket, [pSocket] {
            pSocket->abort();
            pSocket->deleteLater();
        });
    }
}

void LoopbackCallbackServer::onReadyRead(QTcpSocket* pSocket) {
    // Answer only once the whole request head has arrived, so that closing
    // the connection does not reset it while the browser is still sending.
    const QByteArray head = pSocket->peek(kMaxRequestBytes + 1);
    if (!head.contains("\r\n\r\n")) {
        if (head.size() > kMaxRequestBytes) {
            pSocket->abort();
            pSocket->deleteLater();
        }
        return;
    }
    disconnect(pSocket, &QTcpSocket::readyRead, this, nullptr);
    pSocket->readAll();

    const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
    if (requestLine.size() != 3 || !requestLine[2].startsWith("HTTP/")) {
        answer(pSocket, httpResponse(400, "Bad Request", page(tr("Bad request."))));
        return;
    }
    const QUrl target(QString::fromLatin1(requestLine[1]));
    if (target.path() != QLatin1String("/callback") || !m_pServer->isListening()) {
        answer(pSocket, httpResponse(404, "Not Found", page(tr("Not found."))));
        return;
    }
    if (requestLine[0] != "GET") {
        answer(pSocket, httpResponse(405, "Method Not Allowed", page(tr("Method not allowed."))));
        return;
    }

    m_pServer->close();
    answer(pSocket,
            httpResponse(200,
                    "OK",
                    page(tr("You can close this window and go back to Crate Decks."))));
    // Last: a slot may delete this server.
    emit callbackReceived(QUrlQuery(target));
}

} // namespace crate

#include "moc_crateloopbackserver.cpp"
