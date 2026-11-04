#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <chrono>
#include <cstring>
#include <functional>
#include <utility>

#include "crate/client/crateclient.h"

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
};

/// Collects what the Client schedules, so tests decide when time passes.
class FakeScheduler {
  public:
    struct Task {
        std::chrono::milliseconds delay;
        std::function<void()> callback;
    };

    QList<Task> tasks;

    std::function<void(std::chrono::milliseconds, std::function<void()>)> function() {
        return [this](std::chrono::milliseconds delay, std::function<void()> callback) {
            tasks.append({delay, std::move(callback)});
        };
    }

    void runAll() {
        const QList<Task> due = std::exchange(tasks, {});
        for (const Task& task : due) {
            task.callback();
        }
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

    void renewAccessToken(std::function<void(bool)> done) override {
        ++renewals;
        m_done = std::move(done);
    }

    bool renewing() const {
        return static_cast<bool>(m_done);
    }

    void finishRenewal(bool renewed, const QString& newToken = QString()) {
        if (renewed) {
            token = newToken;
        }
        const auto done = std::exchange(m_done, nullptr);
        done(renewed);
    }

  private:
    std::function<void(bool)> m_done;
};

} // namespace crate::test
