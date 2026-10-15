#include "httpclient.h"

#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class HttpFixture : public QObject {
  public:
    QTcpServer server;
    QList<QByteArray> requests;
    HttpFixture() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray data = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", data);
                    if (socket->property("answered").toBool())
                        return;
                    const auto split = data.indexOf("\r\n\r\n");
                    if (split < 0)
                        return;
                    int contentLength = 0;
                    const auto lines = data.left(split).split('\n');
                    for (const QByteArray &line : lines) {
                        if (line.toLower().startsWith("content-length:"))
                            contentLength = line.mid(15).trimmed().toInt();
                    }
                    if (data.size() < split + 4 + contentLength)
                        return;
                    socket->setProperty("answered", true);
                    requests.append(data);
                    const QByteArray path = data.split(' ').value(1);
                    if (path == "/slow")
                        return;
                    if (path == "/head") {
                        socket->write(
                            "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (path == "/partial") {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: "
                                      "close\r\n\r\nabc");
                        socket->disconnectFromHost();
                        return;
                    }
                    int status = 200;
                    QByteArray body = "hello";
                    QByteArray extra = "X-Fixture: yes\r\n";
                    if (path == "/404") {
                        status = 404;
                        body = "missing";
                    }
                    if (path == "/redirect") {
                        status = 302;
                        extra += "Location: /ok\r\n";
                        body = "redirect";
                    }
                    if (path == "/large")
                        body = QByteArray(HttpClient::MaxResponse + 1, 'x');
                    if (path == "/echo")
                        body = data.mid(split + 4, contentLength);
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\n" + extra +
                                  "Content-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url(const QString &path = QStringLiteral("/ok")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
    }
};

class HttpTests : public QObject {
    Q_OBJECT
  private slots:
    void response_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("outcome");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("success") << QString("/ok") << 200 << int(HttpResult::Outcome::Success)
                                 << QByteArray("hello");
        QTest::newRow("not-found") << QString("/404") << 404 << int(HttpResult::Outcome::HttpError)
                                   << QByteArray("missing");
        QTest::newRow("manual-redirect")
            << QString("/redirect") << 302 << int(HttpResult::Outcome::HttpError)
            << QByteArray("redirect");
        QTest::newRow("partial-200") << QString("/partial") << 200
                                     << int(HttpResult::Outcome::NetworkError) << QByteArray("abc");
    }
    void response() {
        QFETCH(QString, path);
        QFETCH(int, status);
        QFETCH(int, outcome);
        QFETCH(QByteArray, body);
        HttpFixture fixture;
        QVERIFY(fixture.server.isListening());
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        RequestSpec request;
        request.url = fixture.url(path);
        QVERIFY(client.send(request));
        QVERIFY(done.wait(5000));
        const auto result = qvariant_cast<HttpResult>(done.takeFirst().first());
        QCOMPARE(result.status, status);
        QCOMPARE(int(result.outcome), outcome);
        QCOMPARE(result.body, body);
        QCOMPARE(result.successful(), outcome == int(HttpResult::Outcome::Success));
        QVERIFY(!client.busy());
        QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("GET " + path.toUtf8() + " HTTP/1.1\r\n"));
        QVERIFY(result.elapsed >= 0);
        if (status != 200 || path == "/partial")
            QVERIFY(!result.error.isEmpty());
    }
    void bodyAndHeaders() {
        HttpFixture fixture;
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        RequestSpec request;
        request.url = fixture.url("/echo");
        request.method = "PATCH";
        request.headers.insert("X-Custom", "abc123");
        request.body = QByteArray("a\0b", 3);
        QVERIFY(client.send(request));
        QVERIFY(done.wait(5000));
        const auto result = qvariant_cast<HttpResult>(done.first().first());
        QVERIFY(result.successful());
        QCOMPARE(result.body, request.body);
        QCOMPARE(result.request.method, request.method);
        QCOMPARE(result.request.body, request.body);
        QVERIFY(fixture.requests.first().toLower().contains("x-custom: abc123\r\n"));
        bool fixtureHeader = false;
        for (const auto &header : result.headers)
            fixtureHeader =
                fixtureHeader || (header.first.toLower() == "x-fixture" && header.second == "yes");
        QVERIFY(fixtureHeader);
    }
    void connectionFailure() {
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        RequestSpec request;
        request.url = QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(port));
        QVERIFY(client.send(request));
        QVERIFY(done.wait(5000));
        const auto result = qvariant_cast<HttpResult>(done.first().first());
        QCOMPARE(result.outcome, HttpResult::Outcome::NetworkError);
        QCOMPARE(result.status, 0);
        QVERIFY(!result.error.isEmpty());
    }
    void headResponse() {
        HttpFixture fixture;
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        RequestSpec request;
        request.url = fixture.url("/head");
        request.method = "HEAD";
        QVERIFY(client.send(request));
        QVERIFY(done.wait(5000));
        const auto result = qvariant_cast<HttpResult>(done.first().first());
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.status, 200);
        QVERIFY(result.body.isEmpty());
    }
    void timeoutAndCancel() {
        HttpFixture fixture;
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        RequestSpec request;
        request.url = fixture.url("/slow");
        request.timeoutMs = 75;
        QVERIFY(client.send(request));
        QVERIFY(done.wait(5000));
        auto result = qvariant_cast<HttpResult>(done.takeFirst().first());
        QCOMPARE(result.outcome, HttpResult::Outcome::Timeout);
        QVERIFY(result.timedOut);
        QVERIFY(!result.cancelled);
        request.timeoutMs = 5000;
        QVERIFY(client.send(request));
        QTRY_COMPARE(fixture.requests.size(), 2);
        client.cancel();
        QTRY_COMPARE(done.size(), 1);
        result = qvariant_cast<HttpResult>(done.takeFirst().first());
        QCOMPARE(result.outcome, HttpResult::Outcome::Cancelled);
        QVERIFY(result.cancelled);
        QVERIFY(!result.timedOut);
        client.cancel();
        QCOMPARE(done.size(), 0);
    }
    void responseCapAndMetadataOnly() {
        HttpFixture fixture;
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        RequestSpec request;
        request.url = fixture.url("/large");
        QVERIFY(client.send(request));
        QVERIFY(done.wait(10000));
        auto result = qvariant_cast<HttpResult>(done.takeFirst().first());
        QCOMPARE(result.outcome, HttpResult::Outcome::ResponseTooLarge);
        QVERIFY(result.truncated);
        QCOMPARE(result.body.size(), HttpClient::MaxResponse);
        QVERIFY(!result.successful());
        client.setCaptureBody(false);
        request.url = fixture.url();
        QVERIFY(client.send(request));
        QVERIFY(done.wait(5000));
        result = qvariant_cast<HttpResult>(done.takeFirst().first());
        QVERIFY(result.successful());
        QVERIFY(result.body.isEmpty());
        QCOMPARE(result.bytesReceived, 5);
    }
    void validation() {
        HttpClient client;
        QSignalSpy errors(&client, &HttpClient::error);
        RequestSpec request;
        request.url = QUrl("ftp://localhost/");
        QVERIFY(!client.send(request));
        request.url = QUrl("http://user:password@localhost/");
        QVERIFY(!client.send(request));
        request.url = QUrl("http://localhost/");
        request.headers.insert("X-Test", "ok\r\nInjected: true");
        QVERIFY(!client.send(request));
        request.headers.clear();
        request.method = "GET\r\n";
        QVERIFY(!client.send(request));
        request.method = "GET";
        request.timeoutMs = 0;
        QVERIFY(!client.send(request));
        QCOMPARE(errors.size(), 5);
        QVERIFY(!client.busy());
    }
    void reentrantCompletion() {
        HttpFixture fixture;
        HttpClient client;
        QSignalSpy done(&client, &HttpClient::finished);
        bool restarted = false;
        connect(&client, &HttpClient::busyChanged, &client, [&](bool busy) {
            if (!busy && !restarted) {
                restarted = true;
                RequestSpec second;
                second.url = fixture.url("/404");
                QVERIFY(client.send(second));
            }
        });
        RequestSpec first;
        first.url = fixture.url();
        QVERIFY(client.send(first));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(qvariant_cast<HttpResult>(done[0][0]).status, 200);
        QCOMPARE(qvariant_cast<HttpResult>(done[1][0]).status, 404);
        QVERIFY(!client.busy());
    }
    void destroyActiveRequest() {
        HttpFixture fixture;
        auto *client = new HttpClient;
        RequestSpec request;
        request.url = fixture.url("/slow");
        QVERIFY(client->send(request));
        QTRY_COMPARE(fixture.requests.size(), 1);
        delete client;
        QCoreApplication::processEvents();
    }
};
QTEST_GUILESS_MAIN(HttpTests)
#include "test_http.moc"
