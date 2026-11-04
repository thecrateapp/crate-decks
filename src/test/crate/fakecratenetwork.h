#pragma once

#include <gtest/gtest.h>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpSocket>
#include <chrono>
#include <cstring>
#include <functional>
#include <utility>

#include "crate/client/crateclient.h"
#include "crate/session/cratesecretstore.h"

namespace crate::test {

/// A network reply that the test finishes when it wants: with an HTTP
/// answer, with a network failure, or not at all. abort() finishes it at
/// once, like Qt's own replies.
class FakeReply : public QNetworkReply {
  public:
    FakeReply(QNetworkAccessManager::Operation operation,
            const QNetworkRequest& request,
            QObject* pParent)
            : QNetworkReply(pParent) {
        setOperation(operation);
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    using Headers = QList<std::pair<QByteArray, QByteArray>>;

    void respond(int status, const QByteArray& body = QByteArray(), const Headers& headers = {}) {
        if (isFinished()) {
            return;
        }
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        for (const auto& [name, value] : headers) {
            setRawHeader(name, value);
        }
        if (status >= 400) {
            setError(status >= 500 ? UnknownServerError : UnknownContentError,
                    QStringLiteral("HTTP %1").arg(status));
        }
        m_body = body;
        m_position = 0;
        emit metaDataChanged();
        emit downloadProgress(body.size(), body.size());
        emit readyRead();
        setFinished(true);
        emit finished();
    }

    /// An HTTP answer whose transfer broke off after the headers, as Qt
    /// reports it: the status stays, error() is set.
    void respondTruncated(int status, const QByteArray& partialBody, NetworkError error) {
        if (isFinished()) {
            return;
        }
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        setError(error, QStringLiteral("Remote host closed the connection"));
        m_body = partialBody;
        m_position = 0;
        emit metaDataChanged();
        emit readyRead();
        emit errorOccurred(error);
        setFinished(true);
        emit finished();
    }

    void failNetwork() {
        if (isFinished()) {
            return;
        }
        setError(ConnectionRefusedError, QStringLiteral("Connection refused"));
        emit errorOccurred(ConnectionRefusedError);
        setFinished(true);
        emit finished();
    }

    bool aborted() const {
        return m_aborted;
    }

    void abort() override {
        if (isFinished()) {
            return;
        }
        m_aborted = true;
        setError(OperationCanceledError, QStringLiteral("Operation canceled"));
        emit errorOccurred(OperationCanceledError);
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override {
        return m_body.size() - m_position + QIODevice::bytesAvailable();
    }

  protected:
    qint64 readData(char* pData, qint64 maxSize) override {
        const qint64 size = qMin(maxSize, m_body.size() - m_position);
        std::memcpy(pData, m_body.constData() + m_position, static_cast<size_t>(size));
        m_position += size;
        return size;
    }

  private:
    QByteArray m_body;
    qint64 m_position = 0;
    bool m_aborted = false;
};

/// Records every request and answers each with a FakeReply that stays open
/// until the test finishes it.
class FakeNetworkAccessManager : public QNetworkAccessManager {
  public:
    struct Sent {
        Operation operation;
        QNetworkRequest request;
        QByteArray body;
        QPointer<FakeReply> pReply;
    };

    QList<Sent> sent;

    /// The last request. If none was sent, the test fails and a placeholder
    /// with a reply nobody listens to comes back, so that a red test fails
    /// instead of crashing.
    Sent last() const {
        if (sent.isEmpty()) {
            ADD_FAILURE() << "No request was sent";
            if (!m_pOrphanReply) {
                m_pOrphanReply = new FakeReply(GetOperation,
                        QNetworkRequest(),
                        const_cast<FakeNetworkAccessManager*>(this));
            }
            return {GetOperation, QNetworkRequest(), QByteArray(), m_pOrphanReply};
        }
        return sent.last();
    }

    /// The replies that have not finished, in the order they were sent.
    QList<FakeReply*> openReplies() const {
        QList<FakeReply*> replies;
        for (const Sent& request : sent) {
            if (request.pReply && !request.pReply->isFinished()) {
                replies.append(request.pReply.data());
            }
        }
        return replies;
    }

  protected:
    QNetworkReply* createRequest(Operation operation,
            const QNetworkRequest& request,
            QIODevice* pOutgoingData) override {
        auto* pReply = new FakeReply(operation, request, this);
        sent.append({operation,
                request,
                pOutgoingData ? pOutgoingData->readAll() : QByteArray(),
                pReply});
        return pReply;
    }

  private:
    mutable QPointer<FakeReply> m_pOrphanReply;
};

/// Collects what the Client schedules, so tests decide when time passes.
class FakeScheduler {
  public:
    struct Task {
        std::chrono::milliseconds delay;
        std::function<void()> callback;
    };

    QList<Task> tasks;

    std::function<void(std::chrono::milliseconds, const std::function<void()>&)> function() {
        return [this](std::chrono::milliseconds delay, const std::function<void()>& callback) {
            tasks.append({delay, callback});
        };
    }

    void runAll() {
        const QList<Task> due = std::exchange(tasks, {});
        for (const Task& task : due) {
            task.callback();
        }
    }
};

/// Sends `request` to a server on 127.0.0.1:`port`, as a browser would, and
/// returns the whole answer (empty if nothing listens there). Runs the event
/// loop meanwhile so that a server in this thread can reply.
inline QByteArray sendHttpRequest(quint16 port, const QByteArray& request) {
    QTcpSocket socket;
    QSignalSpy connected(&socket, &QTcpSocket::connected);
    QSignalSpy errorOccurred(&socket, &QTcpSocket::errorOccurred);
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    if (socket.state() != QAbstractSocket::ConnectedState) {
        for (int i = 0; i < 50 && connected.isEmpty() && errorOccurred.isEmpty(); ++i) {
            connected.wait(100);
        }
        if (connected.isEmpty()) {
            return QByteArray();
        }
    }
    socket.write(request);
    QSignalSpy disconnected(&socket, &QTcpSocket::disconnected);
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        disconnected.wait(5000);
    }
    return socket.readAll();
}

/// A SecretStore in memory that the test can make unavailable.
class FakeSecretStore : public crate::SecretStore {
  public:
    QHash<QString, QString> secrets;
    bool unavailable = false;

    void read(const QString& key, const ReadDone& done) override {
        if (unavailable) {
            done(crate::SecretStore::Status::Unavailable, QString());
            return;
        }
        const auto it = secrets.constFind(key);
        if (it == secrets.cend()) {
            done(crate::SecretStore::Status::NotFound, QString());
            return;
        }
        done(crate::SecretStore::Status::Ok, it.value());
    }

    void write(const QString& key, const QString& secret, const Done& done) override {
        if (unavailable) {
            done(crate::SecretStore::Status::Unavailable);
            return;
        }
        secrets.insert(key, secret);
        done(crate::SecretStore::Status::Ok);
    }

    void remove(const QString& key, const Done& done) override {
        if (unavailable) {
            done(crate::SecretStore::Status::Unavailable);
            return;
        }
        done(secrets.remove(key) > 0 ? crate::SecretStore::Status::Ok
                                     : crate::SecretStore::Status::NotFound);
    }
};

/// A session whose renewal the test completes by hand.
class FakeTokenSource : public crate::TokenSource {
  public:
    QString token = QStringLiteral("access-1");
    int renewals = 0;

    QString accessToken() const override {
        return token;
    }

    void renewAccessToken(const std::function<void(Renewal)>& done) override {
        ++renewals;
        m_done = done;
    }

    bool renewing() const {
        return static_cast<bool>(m_done);
    }

    /// Renewed with `newToken`, or SessionEnded.
    void finishRenewal(bool renewed, const QString& newToken = QString()) {
        if (renewed) {
            token = newToken;
        }
        finish(renewed ? Renewal::Renewed : Renewal::SessionEnded);
    }

    void finishRenewalUnreachable() {
        finish(Renewal::Unreachable);
    }

  private:
    void finish(Renewal renewal) {
        const auto done = std::exchange(m_done, nullptr);
        done(renewal);
    }

    std::function<void(Renewal)> m_done;
};

} // namespace crate::test
