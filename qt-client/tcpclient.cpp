#include "tcpclient.h"

#include <QNetworkProxy>
#include <QTcpSocket>
#include <algorithm>

namespace {
constexpr int SplitDelayMs = 100;
constexpr int DisconnectTimeoutMs = 1000;
constexpr int ResponseHintMs = 1500;
constexpr qint64 ReadChunkSize = 16 * 1024;
} // namespace

TcpClient::TcpClient(QObject *parent) : QObject(parent) {
    qRegisterMetaType<FrameEvent>();
    responseHint_.setSingleShot(true);
    connect(&responseHint_, &QTimer::timeout, this, [this] {
        if (connected())
            emit activity(QStringLiteral(
                "暂未收到完整响应帧，TCP 连接保持；请核对帧类型和服务端日志，广播发送者不会收到自己的 Chat 回显。"));
    });
}

TcpClient::~TcpClient() {
    // Suppress outward signals during destruction.
    blockSignals(true);
    releaseSocket();
}

bool TcpClient::connected() const {
    return socket_ && !closing_ && socket_->state() == QAbstractSocket::ConnectedState;
}

qint64 TcpClient::queuedBytes() const {
    const qint64 reservedTail =
        splitPending_ ? splitFrame_.raw.size() - splitFrame_.raw.size() / 2 : 0;
    return (socket_ ? socket_->bytesToWrite() : 0) + reservedTail;
}

void TcpClient::cancelSplit() {
    ++splitToken_;
    splitFrame_ = {};
    if (splitPending_) {
        splitPending_ = false;
        emit busyChanged(false);
    }
}

void TcpClient::releaseSocket() {
    ++session_;
    responseHint_.stop();
    QTcpSocket *old = socket_;
    socket_ = nullptr;
    connecting_ = false;
    closing_ = false;
    input_.clear();
    cancelSplit();
    if (old) {
        old->disconnect(this);
        old->abort();
        old->deleteLater();
    }
}

void TcpClient::connectTo(const QString &host, quint16 port, int timeoutMs) {
    if (host.trimmed().isEmpty() || port == 0 || timeoutMs <= 0) {
        emit error(QStringLiteral("请输入服务器地址、有效端口和大于零的连接超时。"));
        return;
    }
    releaseSocket();
    socket_ = new QTcpSocket(this);
    // Protocol debugging targets the entered server directly, independent of system proxies.
    socket_->setProxy(QNetworkProxy::NoProxy);
    QTcpSocket *socket = socket_;
    const quint64 session = session_;
    connecting_ = true;
    socket->setReadBufferSize(Wire::MaxFrameSize * 2);

    connect(socket, &QTcpSocket::connected, this, [this, socket, session] {
        if (socket != socket_ || session != session_)
            return;
        connecting_ = false;
        emit stateChanged(QStringLiteral("TCP 已连接"), true);
        emit activity(
            QStringLiteral("TCP 连接已建立；可手动发送协议帧，是否回应取决于服务端实现。"));
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket, session] {
        if (socket == socket_ && session == session_)
            receive();
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket, session] {
        if (socket != socket_ || session != session_)
            return;
        const bool requested = closing_;
        const qsizetype incompleteBytes = input_.size();
        releaseSocket();
        emit stateChanged(QStringLiteral("未连接"), false);
        if (incompleteBytes > 0)
            emit activity(
                QStringLiteral("连接关闭，丢弃 %1 字节未完成的接收帧。").arg(incompleteBytes));
        emit activity(requested ? QStringLiteral("TCP 已断开。")
                                : QStringLiteral("服务端已关闭 TCP 连接。"));
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket, session](QAbstractSocket::SocketError code) {
                if (socket != socket_ || session != session_)
                    return;
                // Qt emits disconnected after this notification; this is a normal peer close.
                if (code == QAbstractSocket::RemoteHostClosedError)
                    return;
                fail(QStringLiteral("TCP 错误：%1").arg(socket->errorString()));
            });
    emit stateChanged(QStringLiteral("正在连接…"), false);
    socket->connectToHost(host.trimmed(), port);
    QTimer::singleShot(timeoutMs, this, [this, socket, session] {
        if (socket == socket_ && session == session_ && connecting_)
            fail(QStringLiteral("TCP 连接超时，请检查地址、端口和服务端监听状态。"));
    });
}

void TcpClient::disconnectFrom() {
    responseHint_.stop();
    if (!socket_)
        return;
    if (closing_)
        return;
    closing_ = true;
    connecting_ = false;
    const bool hadSplit = splitPending_;
    cancelSplit();
    if (hadSplit)
        emit activity(QStringLiteral("已取消拆分发送的后半段；此前入队的前半段可能已到达对端。"));
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        releaseSocket();
        emit stateChanged(QStringLiteral("未连接"), false);
        return;
    }
    QTcpSocket *socket = socket_;
    const quint64 session = session_;
    emit stateChanged(QStringLiteral("正在断开（等待发送队列清空）…"), false);
    // Qt drains queued bytes before closing. Bound that wait without blocking the UI.
    socket->disconnectFromHost();
    QTimer::singleShot(DisconnectTimeoutMs, this, [this, socket, session] {
        if (socket != socket_ || session != session_ || !closing_)
            return;
        const qint64 pending = socket->bytesToWrite();
        releaseSocket();
        emit stateChanged(QStringLiteral("未连接"), false);
        emit activity(QStringLiteral("等待断连超过 1000 ms，已关闭套接字；剩余本机队列 %1 字节。")
                          .arg(pending));
    });
}

void TcpClient::fail(const QString &reason) {
    releaseSocket();
    emit stateChanged(QStringLiteral("未连接"), false);
    emit error(reason);
}

bool TcpClient::writeBytes(const QByteArray &bytes) {
    if (!connected())
        return false;
    if (socket_->bytesToWrite() + bytes.size() > MaxQueuedBytes) {
        emit error(QStringLiteral("发送队列达到 1 MiB 上限，请等待后重试。"));
        return false;
    }
    const qint64 accepted = socket_->write(bytes);
    if (accepted != bytes.size()) {
        fail(QStringLiteral("TCP 写入未完整入队（%1 / %2 字节），已断开以避免后续帧错位。")
                 .arg(accepted)
                 .arg(bytes.size()));
        return false;
    }
    return true;
}

void TcpClient::scheduleResponseHint() {
    // One reusable timer, so repeated sends cannot accumulate delayed callbacks.
    if (connected() && !responseHint_.isActive())
        responseHint_.start(ResponseHintMs);
}

void TcpClient::reportQueued(const FrameEvent &event) {
    emit frame(event);
    emit activity(QStringLiteral("%1 帧 %2 字节已加入本机发送队列；不代表对端已收到或处理。")
                      .arg(Wire::typeName(event.type))
                      .arg(event.raw.size()));
    scheduleResponseHint();
}

bool TcpClient::sendFrame(quint32 type, const QByteArray &payload, TransmissionMode mode) {
    if (!connected()) {
        emit error(QStringLiteral("请先建立 TCP 连接。"));
        return false;
    }
    if (splitPending_) {
        emit error(QStringLiteral("拆分帧尚未发送完成，请等待后半段入队后重试。"));
        return false;
    }
    if (payload.size() > Wire::MaxPayload) {
        emit error(QStringLiteral("payload 超过 64 KiB 协议上限。"));
        return false;
    }
    const QByteArray raw = Wire::pack(type, payload);
    const qint64 total = raw.size() * (mode == TransmissionMode::Coalesced ? 2 : 1);
    if (socket_->bytesToWrite() + total > MaxQueuedBytes) {
        emit error(QStringLiteral("发送队列达到 1 MiB 上限，请等待后重试。"));
        return false;
    }
    const FrameEvent event{QDateTime::currentDateTime(), QStringLiteral("TX queued"), type, payload,
                           raw};
    if (mode == TransmissionMode::Split) {
        const qsizetype midpoint = raw.size() / 2;
        splitFrame_ = event;
        splitPending_ = true;
        const quint64 token = ++splitToken_;
        const quint64 session = session_;
        if (!writeBytes(raw.first(midpoint))) {
            cancelSplit();
            return false;
        }
        emit busyChanged(true);
        emit activity(QStringLiteral("拆分发送：前 %1 字节已入队，100 ms 后写入后 %2 字节。")
                          .arg(midpoint)
                          .arg(raw.size() - midpoint));
        QTimer::singleShot(SplitDelayMs, this, [this, token, session] {
            if (session != session_ || token != splitToken_ || !splitPending_ || !connected())
                return;
            FrameEvent completed = splitFrame_;
            completed.timestamp = QDateTime::currentDateTime();
            if (!writeBytes(completed.raw.mid(completed.raw.size() / 2))) {
                cancelSplit();
                return;
            }
            cancelSplit();
            reportQueued(completed);
        });
        return true;
    }
    if (!writeBytes(mode == TransmissionMode::Coalesced ? raw + raw : raw))
        return false;
    reportQueued(event);
    if (mode == TransmissionMode::Coalesced)
        reportQueued(event);
    return true;
}

void TcpClient::receive() {
    QTcpSocket *socket = socket_;
    const quint64 session = session_;
    while (socket == socket_ && session == session_ && socket->bytesAvailable() > 0) {
        const qint64 capacity = Wire::MaxFrameSize - input_.size();
        if (capacity <= 0) {
            fail(QStringLiteral("接收缓冲超过最大帧长度，已断开。"));
            return;
        }
        input_ += socket->read(std::min(capacity, ReadChunkSize));
        for (;;) {
            Wire::Frame received;
            QString reason;
            const int result = Wire::take(input_, received, reason);
            if (result < 0) {
                fail(reason);
                return;
            }
            if (result == 0)
                break;
            responseHint_.stop();
            emit frame(FrameEvent{QDateTime::currentDateTime(), QStringLiteral("RX"), received.type,
                                  received.payload, received.raw});
            // Slots may disconnect or reconnect while handling a received frame.
            if (socket != socket_ || session != session_)
                return;
            if (received.type < Wire::Login || received.type > Wire::Error)
                emit activity(
                    QStringLiteral("收到未知类型 %1，保留原始帧供检查。").arg(received.type));
            if (socket != socket_ || session != session_)
                return;
        }
    }
}
