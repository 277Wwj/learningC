#pragma once

#include "protocol.h"

#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QTimer>

class QTcpSocket;

struct FrameEvent {
    QDateTime timestamp;
    QString direction; // "TX queued" = accepted by Qt, "RX" = parsed complete frame.
    quint32 type = 0;
    QByteArray payload;
    QByteArray raw;
};
Q_DECLARE_METATYPE(FrameEvent)

enum class TransmissionMode { Whole, Split, Coalesced };

class TcpClient : public QObject {
    Q_OBJECT
  public:
    explicit TcpClient(QObject *parent = nullptr);
    ~TcpClient() override;

    void connectTo(const QString &host, quint16 port, int timeoutMs = 5000);
    void disconnectFrom();
    bool sendFrame(quint32 type, const QByteArray &payload,
                   TransmissionMode mode = TransmissionMode::Whole);
    bool connected() const;
    bool busy() const {
        return splitPending_;
    }
    qint64 queuedBytes() const;
    static constexpr qint64 MaxQueuedBytes = 1024 * 1024;

  signals:
    void stateChanged(const QString &state, bool connected);
    void busyChanged(bool busy);
    void frame(const FrameEvent &event);
    void activity(const QString &text);
    void error(const QString &text);

  private:
    QTcpSocket *socket_ = nullptr;
    QByteArray input_;
    FrameEvent splitFrame_;
    quint64 session_ = 0;
    quint64 splitToken_ = 0;
    QTimer responseHint_;
    bool connecting_ = false;
    bool closing_ = false;
    bool splitPending_ = false;

    void receive();
    void fail(const QString &reason);
    void releaseSocket();
    void cancelSplit();
    void reportQueued(const FrameEvent &event);
    void scheduleResponseHint();
    bool writeBytes(const QByteArray &bytes);
};
