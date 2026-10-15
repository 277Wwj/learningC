#pragma once

#include <QElapsedTimer>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QTimer>
#include <QUrl>

struct RequestSpec {
    QString method = QStringLiteral("GET");
    QUrl url;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    int timeoutMs = 5000;
};
Q_DECLARE_METATYPE(RequestSpec)

struct HttpResult {
    enum class Outcome { Success, HttpError, NetworkError, Timeout, Cancelled, ResponseTooLarge };
    RequestSpec request;
    int status = 0;
    qint64 elapsed = 0;
    qint64 bytesReceived = 0;
    QByteArray body;
    QList<QNetworkReply::RawHeaderPair> headers;
    QString error;
    Outcome outcome = Outcome::NetworkError;
    bool timedOut = false;
    bool cancelled = false;
    bool truncated = false;
    bool successful() const {
        return outcome == Outcome::Success;
    }
};
Q_DECLARE_METATYPE(HttpResult)

class HttpClient : public QObject {
    Q_OBJECT
  public:
    explicit HttpClient(QObject *parent = nullptr);
    ~HttpClient() override;
    bool send(const RequestSpec &request);
    void cancel();
    bool busy() const {
        return reply_ != nullptr;
    }
    void setCaptureBody(bool capture) {
        if (!busy())
            captureBody_ = capture;
    }
    static QString validationError(const RequestSpec &request);
    static constexpr qint64 MaxResponse = 4 * 1024 * 1024;
  signals:
    void finished(const HttpResult &result);
    void busyChanged(bool busy);
    void error(const QString &text);

  private:
    QNetworkAccessManager manager_;
    QNetworkReply *reply_ = nullptr;
    QTimer timeout_;
    QElapsedTimer elapsed_;
    HttpResult result_;
    bool captureBody_ = true;
    bool finishing_ = false;
    void drain();
    void finish(QNetworkReply *reply);
};
