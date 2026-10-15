#include "tcpclient.h"

#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
const QByteArray LoginHi = QByteArray::fromHex("0000000a4c434831000000016869");

// A temporary, per-test TCP peer only; no runnable demo server is shipped.
struct Connection {
    QTcpServer listener;
    TcpClient client;
    QTcpSocket *peer = nullptr;
    QByteArray received;

    bool open(int timeoutMs = 1000) {
        if (!listener.listen(QHostAddress::LocalHost, 0))
            return false;
        return reconnect(timeoutMs);
    }

    bool reconnect(int timeoutMs = 1000) {
        client.connectTo(QStringLiteral("127.0.0.1"), listener.serverPort(), timeoutMs);
        if (!QTest::qWaitFor(
                [this] { return client.connected() && listener.hasPendingConnections(); }, 2000))
            return false;
        peer = listener.nextPendingConnection();
        received.clear();
        QObject::connect(peer, &QTcpSocket::readyRead, peer, [this, socket = peer] {
            if (socket == peer)
                received += socket->readAll();
        });
        return true;
    }
};

FrameEvent eventAt(const QSignalSpy &spy, qsizetype index) {
    return qvariant_cast<FrameEvent>(spy.at(index).at(0));
}
} // namespace

class TcpTests : public QObject {
    Q_OBJECT
  private slots:
    void literalWireVector();
    void directConnectionIgnoresApplicationProxy();
    void wholeAndNoWelcome();
    void splitAndPreventInterleaving();
    void coalescedSend();
    void fragmentedAndCoalescedReceive();
    void maxPayloadAndManyFrames();
    void malformedFrame_data();
    void malformedFrame();
    void refusalAndRemoteClose();
    void disconnectFlushesWholeFrames();
    void disconnectCancelsSplitAndReconnectIsClean();
    void replacingConnectionCancelsSplit();
    void sendQueueIsBounded();
};

void TcpTests::directConnectionIgnoresApplicationProxy() {
    // Qt bypasses proxies for loopback addresses, so use this machine's own LAN address.
    QHostAddress address;
    for (const auto &network : QNetworkInterface::allInterfaces()) {
        if (!network.flags().testFlag(QNetworkInterface::IsUp) ||
            !network.flags().testFlag(QNetworkInterface::IsRunning) ||
            network.flags().testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : network.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                address = entry.ip();
                break;
            }
        }
        if (!address.isNull())
            break;
    }
    if (address.isNull())
        QSKIP("A non-loopback IPv4 interface is required for the proxy regression.");
    QTcpServer listener;
    listener.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(listener.listen(address, 0));
    const auto previous = QNetworkProxy::applicationProxy();
    const bool systemProxy = QNetworkProxyFactory::usesSystemConfiguration();
    const auto restore = qScopeGuard([previous, systemProxy] {
        if (systemProxy)
            QNetworkProxyFactory::setUseSystemConfiguration(true);
        else
            QNetworkProxy::setApplicationProxy(previous);
    });
    // An HTTP caching proxy cannot carry a raw TCP stream: reproduce the reported error.
    QNetworkProxy::setApplicationProxy(
        QNetworkProxy(QNetworkProxy::HttpCachingProxy, "127.0.0.1", 9));
    TcpClient client;
    QSignalSpy errors(&client, &TcpClient::error);
    for (int attempt = 0; attempt < 2; ++attempt) {
        client.connectTo(address.toString(), listener.serverPort(), 1500);
        QVERIFY(QTest::qWaitFor([&] { return client.connected() || !errors.isEmpty(); }, 2500));
        QVERIFY2(client.connected(), errors.isEmpty()
                                         ? "Connection did not complete"
                                         : qPrintable(errors.first().first().toString()));
        QTRY_VERIFY(listener.hasPendingConnections());
        auto *peer = listener.nextPendingConnection();
        QVERIFY(client.sendFrame(Wire::Login, "hi"));
        QTRY_COMPARE(peer->bytesAvailable(), qint64(LoginHi.size()));
        QCOMPARE(peer->readAll(), LoginHi);
        client.disconnectFrom();
        QTRY_COMPARE(peer->state(), QAbstractSocket::UnconnectedState);
        peer->deleteLater();
    }
    QCOMPARE(errors.size(), 0);
}

void TcpTests::literalWireVector() {
    QCOMPARE(Wire::pack(Wire::Login, "hi"), LoginHi);
    QCOMPARE(Wire::pack(Wire::Bye), QByteArray::fromHex("000000084c43483100000006"));
    QByteArray buffer = LoginHi + QByteArray::fromHex("000000084c434831aabbccdd");
    Wire::Frame parsed;
    QString reason;
    QCOMPARE(Wire::take(buffer, parsed, reason), 1);
    QCOMPARE(parsed.type, quint32(Wire::Login));
    QCOMPARE(parsed.payload, QByteArray("hi"));
    QCOMPARE(parsed.raw, LoginHi);
    QCOMPARE(Wire::take(buffer, parsed, reason), 1);
    QCOMPARE(parsed.type, quint32(0xaabbccdd));
    QVERIFY(buffer.isEmpty());
    QVERIFY(Wire::pack(Wire::Login, QByteArray(Wire::MaxPayload + 1, 'x')).isEmpty());
}

void TcpTests::wholeAndNoWelcome() {
    Connection connection;
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    QSignalSpy errors(&connection.client, &TcpClient::error);
    QSignalSpy activity(&connection.client, &TcpClient::activity);
    QVERIFY(connection.open(80));
    QTest::qWait(120);
    QVERIFY(connection.client.connected());
    QVERIFY(connection.received.isEmpty()); // Connecting never sends an automatic Login.
    QVERIFY(connection.client.sendFrame(Wire::Login, "hi"));
    QTRY_COMPARE(connection.received, LoginHi);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(eventAt(frames, 0).direction, QStringLiteral("TX queued"));
    QCOMPARE(eventAt(frames, 0).raw, LoginHi);
    QVERIFY(eventAt(frames, 0).timestamp.isValid());
    // The fixture deliberately never sends a Welcome or any other reply.
    QTest::qWait(1700);
    QVERIFY(connection.client.connected());
    QCOMPARE(errors.size(), 0);
    bool sawNoResponseHint = false;
    for (const auto &arguments : activity)
        sawNoResponseHint |=
            arguments.at(0).toString().contains(QStringLiteral("暂未收到完整响应帧"));
    QVERIFY(sawNoResponseHint);
}

void TcpTests::splitAndPreventInterleaving() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    QSignalSpy errors(&connection.client, &TcpClient::error);
    QVERIFY(connection.client.sendFrame(Wire::Login, "hi", TransmissionMode::Split));
    QVERIFY(connection.client.busy());
    QVERIFY(!connection.client.sendFrame(Wire::Bye, {}));
    QCOMPARE(errors.size(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!connection.received.isEmpty(), 80);
    QCOMPARE(connection.received, LoginHi.first(LoginHi.size() / 2));
    QCOMPARE(frames.size(), 0); // The second half is not yet queued.
    QTRY_VERIFY(!connection.client.busy());
    QTRY_COMPARE(connection.received, LoginHi);
    QCOMPARE(frames.size(), 1);
    QVERIFY(connection.client.sendFrame(Wire::Bye, {}));
    QTRY_COMPARE(connection.received, LoginHi + QByteArray::fromHex("000000084c43483100000006"));
    QVERIFY(connection.client.connected()); // A manual Bye frame does not close TCP.
}

void TcpTests::coalescedSend() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    QVERIFY(connection.client.sendFrame(Wire::Login, "hi", TransmissionMode::Coalesced));
    QTRY_COMPARE(connection.received, LoginHi + LoginHi);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(eventAt(frames, 0).raw, LoginHi);
    QCOMPARE(eventAt(frames, 1).raw, LoginHi);
    QVERIFY(!connection.client.busy());
}

void TcpTests::fragmentedAndCoalescedReceive() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    QSignalSpy errors(&connection.client, &TcpClient::error);
    const QByteArray welcome = QByteArray::fromHex("0000000a4c434831000000026869");
    const QByteArray unknown = QByteArray::fromHex("0000000a4c434831ffffffffc328");
    const QByteArray applicationError = QByteArray::fromHex("000000094c4348310000000778");
    const QByteArray bye = QByteArray::fromHex("000000084c43483100000006");
    QCOMPARE(connection.peer->write(welcome.first(3)), qint64(3));
    connection.peer->flush();
    QTest::qWait(20);
    QCOMPARE(frames.size(), 0);
    connection.peer->write(welcome.mid(3, 8));
    connection.peer->flush();
    QTest::qWait(20);
    QCOMPARE(frames.size(), 0);
    connection.peer->write(welcome.mid(11) + unknown + applicationError + bye);
    QTRY_COMPARE(frames.size(), 4);
    QCOMPARE(eventAt(frames, 0).direction, QStringLiteral("RX"));
    QCOMPARE(eventAt(frames, 0).raw, welcome);
    QCOMPARE(eventAt(frames, 1).type, quint32(0xffffffff));
    QCOMPARE(eventAt(frames, 1).payload, QByteArray::fromHex("c328"));
    QCOMPARE(eventAt(frames, 2).type, quint32(Wire::Error));
    QCOMPARE(eventAt(frames, 3).type, quint32(Wire::Bye));
    QCOMPARE(errors.size(), 0);
    QVERIFY(connection.client.connected());
}

void TcpTests::maxPayloadAndManyFrames() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    const QByteArray payload(Wire::MaxPayload, '\x7f');
    const QByteArray large = Wire::pack(Wire::Chat, payload);
    QByteArray batch = large + large;
    for (int i = 0; i < 100; ++i)
        batch += LoginHi;
    QCOMPARE(connection.peer->write(batch), qint64(batch.size()));
    QTRY_COMPARE(frames.size(), 102);
    QCOMPARE(eventAt(frames, 0).payload, payload);
    QCOMPARE(eventAt(frames, 1).raw, large);
    QCOMPARE(eventAt(frames, 101).raw, LoginHi);
    QVERIFY(connection.client.connected());
}

void TcpTests::malformedFrame_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("reason");
    QTest::newRow("body-too-small") << QByteArray::fromHex("00000007") << QStringLiteral("长度");
    QTest::newRow("payload-over-64KiB")
        << QByteArray::fromHex("00010009") << QStringLiteral("长度");
    QTest::newRow("uint32-overflow") << QByteArray::fromHex("ffffffff") << QStringLiteral("长度");
    QTest::newRow("bad-magic-before-body")
        << QByteArray::fromHex("0000000858585858") << QStringLiteral("魔数");
}

void TcpTests::malformedFrame() {
    QFETCH(QByteArray, bytes);
    QFETCH(QString, reason);
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy errors(&connection.client, &TcpClient::error);
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    connection.peer->write(bytes);
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(reason));
    QVERIFY(!connection.client.connected());
    QCOMPARE(frames.size(), 0);
}

void TcpTests::refusalAndRemoteClose() {
    QTcpServer reserve;
    QVERIFY(reserve.listen(QHostAddress::LocalHost, 0));
    const quint16 unusedPort = reserve.serverPort();
    reserve.close();
    TcpClient client;
    QSignalSpy errors(&client, &TcpClient::error);
    client.connectTo(QStringLiteral("127.0.0.1"), unusedPort, 1000);
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(!client.connected());

    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy states(&connection.client, &TcpClient::stateChanged);
    connection.peer->disconnectFromHost();
    QTRY_VERIFY(!connection.client.connected());
    QTRY_VERIFY(!states.isEmpty());
    QCOMPARE(states.last().at(1).toBool(), false);
}

void TcpTests::disconnectFlushesWholeFrames() {
    Connection connection;
    QVERIFY(connection.open());
    const QByteArray payload(Wire::MaxPayload, 'z');
    const QByteArray expected = Wire::pack(Wire::Say, payload);
    QVERIFY(connection.client.sendFrame(Wire::Say, payload));
    connection.client.disconnectFrom();
    QVERIFY(!connection.client.connected());
    QTRY_COMPARE(connection.received, expected);
    QTRY_COMPARE(connection.peer->state(), QAbstractSocket::UnconnectedState);
}

void TcpTests::disconnectCancelsSplitAndReconnectIsClean() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    QVERIFY(connection.client.sendFrame(Wire::Login, "hi", TransmissionMode::Split));
    connection.client.disconnectFrom();
    QVERIFY(!connection.client.busy());
    QVERIFY(connection.reconnect());
    QVERIFY(connection.client.sendFrame(Wire::Login, "hi"));
    QTest::qWait(200); // Old split callback and original connect timeout must be harmless.
    QCOMPARE(connection.received, LoginHi);
    QCOMPARE(frames.size(), 1);
    QVERIFY(connection.client.connected());
}

void TcpTests::replacingConnectionCancelsSplit() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy frames(&connection.client, &TcpClient::frame);
    QVERIFY(connection.client.sendFrame(Wire::Login, "hi", TransmissionMode::Split));
    QVERIFY(connection.reconnect()); // Also covers direct connectTo replacing an active socket.
    QVERIFY(connection.client.sendFrame(Wire::Bye, {}));
    QTest::qWait(200);
    QCOMPARE(connection.received, QByteArray::fromHex("000000084c43483100000006"));
    QCOMPARE(frames.size(), 1);
    QVERIFY(!connection.client.busy());
}

void TcpTests::sendQueueIsBounded() {
    Connection connection;
    QVERIFY(connection.open());
    QSignalSpy errors(&connection.client, &TcpClient::error);
    const QByteArray payload(Wire::MaxPayload, 'q');
    int accepted = 0;
    // No event processing between writes: bytes remain in Qt's local queue.
    while (accepted < 100 && connection.client.sendFrame(Wire::Say, payload)) {
        ++accepted;
        QVERIFY(connection.client.queuedBytes() <= TcpClient::MaxQueuedBytes);
    }
    QVERIFY(accepted > 0);
    QVERIFY(accepted < 100);
    QCOMPARE(errors.size(), 1);
    QVERIFY(connection.client.connected());
    QVERIFY(!connection.client.sendFrame(Wire::Say, QByteArray(Wire::MaxPayload + 1, 'x')));
    QCOMPARE(errors.size(), 2);
}

QTEST_GUILESS_MAIN(TcpTests)
#include "test_tcp.moc"
