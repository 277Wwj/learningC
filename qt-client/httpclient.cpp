#include "httpclient.h"

#include <QNetworkRequest>
#include <QPointer>

namespace {
bool token(const QByteArray &text) {
    if (text.isEmpty())
        return false;
    for (const char c : text) {
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !QByteArray("!#$%&'*+-.^_`|~").contains(c))
            return false;
    }
    return true;
}

bool httpStatusError(QNetworkReply::NetworkError error) {
    // Qt reports HTTP 4xx/5xx using content/server errors. Connection and
    // protocol failures must still fail a response that has received status 200.
    return (error >= QNetworkReply::ContentAccessDenied &&
            error <= QNetworkReply::UnknownContentError &&
            error != QNetworkReply::ContentReSendError) ||
           (error >= QNetworkReply::InternalServerError &&
            error <= QNetworkReply::UnknownServerError) ||
           error == QNetworkReply::ProtocolInvalidOperationError;
}
} // namespace

HttpClient::HttpClient(QObject *parent) : QObject(parent) {
    qRegisterMetaType<HttpResult>();
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        if (!reply_ || finishing_)
            return;
        result_.timedOut = true;
        result_.outcome = HttpResult::Outcome::Timeout;
        result_.error = QStringLiteral("请求超时");
        reply_->abort();
    });
}

HttpClient::~HttpClient() {
    timeout_.stop();
    if (reply_) {
        // Prevent synchronous abort notifications from observing members while
        // the owning QObject is being torn down.
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
        reply_ = nullptr;
    }
}

QString HttpClient::validationError(const RequestSpec &request) {
    if (!request.url.isValid() || request.url.host().isEmpty() ||
        (request.url.scheme() != "http" && request.url.scheme() != "https") ||
        !request.url.userInfo().isEmpty())
        return QStringLiteral("请输入有效的 http:// 或 https:// 地址（不含用户凭据）");
    if (request.method.toLatin1() != request.method.toUtf8() || !token(request.method.toLatin1()))
        return QStringLiteral("HTTP 方法无效");
    if (request.method == QStringLiteral("HEAD") && !request.body.isEmpty())
        return QStringLiteral("HEAD 请求不支持正文，请清空请求正文");
    if (request.timeoutMs < 1 || request.timeoutMs > 120000)
        return QStringLiteral("超时应为 1–120000 毫秒");
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it) {
        if (!token(it.key()))
            return QStringLiteral("请求头名称无效");
        for (const char c : it.value()) {
            if ((static_cast<unsigned char>(c) < 32 && c != '\t') || c == 127)
                return QStringLiteral("请求头值不能包含控制字符");
        }
    }
    return {};
}

bool HttpClient::send(const RequestSpec &spec) {
    if (busy()) {
        emit error(QStringLiteral("已有请求正在进行"));
        return false;
    }
    const QString invalid = validationError(spec);
    if (!invalid.isEmpty()) {
        emit error(invalid);
        return false;
    }
    result_ = {};
    result_.request = spec;
    finishing_ = false;
    QNetworkRequest request(spec.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    for (auto it = spec.headers.cbegin(); it != spec.headers.cend(); ++it)
        request.setRawHeader(it.key(), it.value());
    elapsed_.start();
    // QNetworkAccessManager must know this is a HEAD operation to avoid
    // interpreting its representation Content-Length as a missing body.
    reply_ = spec.method == QStringLiteral("HEAD")
                 ? manager_.head(request)
                 : manager_.sendCustomRequest(request, spec.method.toLatin1(), spec.body);
    QNetworkReply *const reply = reply_;
    reply->setReadBufferSize(64 * 1024);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (reply_ != reply || finishing_)
            return;
        drain();
        if (result_.truncated)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish(reply); });
    timeout_.start(spec.timeoutMs);
    emit busyChanged(true);
    return true;
}

void HttpClient::drain() {
    if (!reply_ || !reply_->isOpen())
        return;
    while (reply_->bytesAvailable() > 0) {
        const QByteArray bytes = reply_->read(64 * 1024);
        if (bytes.isEmpty())
            break;
        const qint64 room = qMax<qint64>(0, MaxResponse - result_.bytesReceived);
        result_.bytesReceived += bytes.size();
        if (captureBody_)
            result_.body.append(bytes.constData(), qMin<qint64>(room, bytes.size()));
        if (bytes.size() > room) {
            result_.truncated = true;
            result_.outcome = HttpResult::Outcome::ResponseTooLarge;
            result_.error = QStringLiteral("响应超过 4 MiB，已停止接收");
            break;
        }
    }
}

void HttpClient::finish(QNetworkReply *reply) {
    if (reply_ != reply || finishing_)
        return;
    finishing_ = true;
    timeout_.stop();
    drain();
    result_.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result_.headers = reply->rawHeaderPairs();
    result_.elapsed = elapsed_.elapsed();
    if (!result_.timedOut && !result_.cancelled && !result_.truncated) {
        const auto networkError = reply->error();
        if (networkError != QNetworkReply::NoError &&
            !(result_.status >= 300 && httpStatusError(networkError))) {
            result_.outcome = HttpResult::Outcome::NetworkError;
            result_.error = reply->errorString();
        } else if (result_.status >= 200 && result_.status < 300) {
            result_.outcome = HttpResult::Outcome::Success;
        } else if (result_.status > 0) {
            result_.outcome = HttpResult::Outcome::HttpError;
            result_.error = QStringLiteral("HTTP %1").arg(result_.status);
        } else {
            result_.outcome = HttpResult::Outcome::NetworkError;
            result_.error = QStringLiteral("未收到有效的 HTTP 响应");
        }
    }
    const HttpResult completed = result_;
    reply_ = nullptr;
    finishing_ = false;
    reply->deleteLater();
    QPointer<HttpClient> alive(this);
    emit busyChanged(false);
    if (alive)
        emit finished(completed);
}

void HttpClient::cancel() {
    if (!reply_ || finishing_)
        return;
    result_.cancelled = true;
    result_.outcome = HttpResult::Outcome::Cancelled;
    result_.error = QStringLiteral("请求已取消");
    reply_->abort();
}
