#pragma once

#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>

class QTcpServer;
class QTcpSocket;

namespace crate {

/// One-shot HTTP server on 127.0.0.1 that receives the browser's return from
/// the native sign-in flow (RFC 8252 loopback redirect).
///
/// It answers the first `GET /callback` with a short page telling the user
/// to go back to Crate Decks, stops listening and emits callbackReceived()
/// with the query (`code` and `state`, or `error`). Any other request gets a
/// 404 and the server keeps waiting. It never checks the query itself: that
/// is the Session's job.
class LoopbackCallbackServer : public QObject {
    Q_OBJECT
  public:
    explicit LoopbackCallbackServer(QObject* parent = nullptr);
    ~LoopbackCallbackServer() override;

    /// Listens on 127.0.0.1 on a free port. Returns false if it cannot.
    bool listen();
    bool isListening() const;
    QHostAddress address() const;
    quint16 port() const;
    /// `http://127.0.0.1:<port>/callback`, the address the server accepts.
    QUrl callbackUrl() const;
    void close();

  signals:
    void callbackReceived(const QUrlQuery& query);

  private:
    void onNewConnection();
    void onReadyRead(QTcpSocket* pSocket);

    QTcpServer* m_pServer;
};

} // namespace crate
