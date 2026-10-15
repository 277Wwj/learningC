#include "benchmark.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

class BenchmarkFixture : public QObject {
  public:
    QTcpServer server;
    int connections = 0;
    int requests = 0;
    int active = 0;
    int peak = 0;
    int delayMs = 150;
    bool allHttp11 = true;
    BenchmarkFixture() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                ++connections;
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray data = socket->property("data").toByteArray() + socket->readAll();
                    while (data.contains("\r\n\r\n")) {
                        const auto split = data.indexOf("\r\n\r\n");
                        const auto header = data.left(split);
                        data.remove(0, split + 4);
                        const auto line = header.split('\n').first().trimmed();
                        allHttp11 = allHttp11 && line.endsWith(" HTTP/1.1");
                        const QByteArray path = line.split(' ').value(1);
                        const int number = ++requests;
                        ++active;
                        peak = qMax(peak, active);
                        if (path == "/hang")
                            continue;
                        if (path == "/mixed" && number == 6)
                            continue;
                        QTimer::singleShot(delayMs, socket, [this, socket, path, number] {
                            --active;
                            if (socket->state() != QAbstractSocket::ConnectedState)
                                return;
                            if (path == "/mixed" && number == 5) {
                                socket->write("HTTP/1.1 200 OK\r\nContent-Length: "
                                              "20\r\nConnection: close\r\n\r\nabc");
                                socket->disconnectFromHost();
                                return;
                            }
                            const int status = path == "/mixed" ? (number == 2   ? 404
                                                                   : number == 3 ? 500
                                                                   : number == 4 ? 302
                                                                                 : 200)
                                                                : 200;
                            // Intentionally advertise keep-alive; the client must
                            // nevertheless open N separate connections for N attempts.
                            socket->write(
                                "HTTP/1.1 " + QByteArray::number(status) +
                                " Test\r\nContent-Length: 2\r\nConnection: keep-alive\r\n" +
                                (status == 302 ? QByteArray("Location: /ok\r\n") : QByteArray()) +
                                "\r\nok");
                        });
                    }
                    socket->setProperty("data", data);
                });
            }
        });
    }
    QUrl url(const QString &path = QStringLiteral("/ok")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
    }
};

class BenchmarkTests : public QObject {
    Q_OBJECT
  private slots:
    void concurrency_data() {
        QTest::addColumn<int>("concurrency");
        QTest::newRow("sequential") << 1;
        QTest::newRow("four") << 4;
        QTest::newRow("thirty-two") << 32;
    }
    void concurrency() {
        QFETCH(int, concurrency);
        BenchmarkFixture fixture;
        QVERIFY(fixture.server.isListening());
        BenchmarkRunner runner;
        QSignalSpy done(&runner, &BenchmarkRunner::finished);
        QSignalSpy updates(&runner, &BenchmarkRunner::updated);
        BenchmarkConfig config;
        config.url = fixture.url();
        config.concurrency = concurrency;
        config.total = concurrency * 2;
        config.label = "baseline";
        QVERIFY(runner.start(config));
        QVERIFY(runner.running());
        QVERIFY(done.wait(15000));
        const auto result = qvariant_cast<BenchmarkSnapshot>(done.first().first());
        QCOMPARE(result.succeeded, config.total);
        QCOMPARE(result.completed, config.total);
        QCOMPARE(result.started, config.total);
        QCOMPARE(result.active, 0);
        QCOMPARE(result.httpErrors + result.networkErrors + result.timeouts + result.cancelled, 0);
        QCOMPARE(fixture.connections, config.total);
        QCOMPARE(fixture.requests, config.total);
        QCOMPARE(fixture.peak, concurrency);
        QVERIFY(fixture.allHttp11);
        QCOMPARE(result.statusCounts.value(200), config.total);
        QVERIFY(result.elapsedMs >= fixture.delayMs * 2 - 30);
        QVERIFY(result.averageMs >= fixture.delayMs - 20);
        QVERIFY(result.p95Ms >= result.p50Ms);
        QVERIFY(qAbs(result.throughput - result.completed * 1000.0 / result.elapsedMs) < 0.001);
        QVERIFY(updates.size() <= result.elapsedMs / 200 + 1);
        QVERIFY(!runner.running());
        QVERIFY(!result.running);
        QCOMPARE(runner.csv().count('\n'), config.total + 1);
        QVERIFY(runner.csv().contains("fresh connection per request"));
    }
    void mixedOutcomes() {
        BenchmarkFixture fixture;
        fixture.delayMs = 25;
        BenchmarkRunner runner;
        QSignalSpy done(&runner, &BenchmarkRunner::finished);
        BenchmarkConfig config;
        config.url = fixture.url("/mixed");
        config.total = 6;
        config.concurrency = 3;
        config.timeoutMs = 200;
        QVERIFY(runner.start(config));
        QVERIFY(done.wait(10000));
        const auto result = qvariant_cast<BenchmarkSnapshot>(done.first().first());
        QCOMPARE(result.completed, 6);
        QCOMPARE(result.succeeded, 1);
        QCOMPARE(result.httpErrors, 3);
        QCOMPARE(result.networkErrors, 1);
        QCOMPARE(result.timeouts, 1);
        QCOMPARE(result.cancelled, 0);
        QCOMPARE(result.statusCounts.value(200), 2);
        QCOMPARE(result.statusCounts.value(404), 1);
        QCOMPARE(result.statusCounts.value(500), 1);
        QCOMPARE(result.statusCounts.value(302), 1);
        QCOMPARE(fixture.requests, 6);
        QCOMPARE(result.averageMs, result.p50Ms);
        QCOMPARE(result.averageMs, result.p95Ms);
        QVERIFY(runner.csv().contains("network_error"));
        QVERIFY(runner.csv().contains("timeout"));
    }
    void stopRestartAndExport() {
        BenchmarkFixture fixture;
        BenchmarkRunner runner;
        QSignalSpy done(&runner, &BenchmarkRunner::finished);
        BenchmarkConfig config;
        config.url = fixture.url("/hang");
        config.total = 50;
        config.concurrency = 4;
        config.timeoutMs = 10000;
        QVERIFY(runner.start(config));
        QVERIFY(!runner.start(config));
        QTRY_COMPARE(fixture.requests, 4);
        runner.stop();
        QVERIFY(done.wait(5000));
        auto result = qvariant_cast<BenchmarkSnapshot>(done.takeFirst().first());
        QCOMPARE(result.started, 4);
        QCOMPARE(result.completed, 4);
        QCOMPARE(result.cancelled, 4);
        QCOMPARE(result.succeeded + result.httpErrors + result.networkErrors + result.timeouts, 0);
        QCOMPARE(result.active, 0);
        QCOMPARE(result.throughput, 0.0);
        QVERIFY(!runner.running());
        QCOMPARE(runner.csv().count('\n'), 5);
        runner.stop();
        config.url = fixture.url();
        config.total = 3;
        config.concurrency = 1;
        config.label = "  =SUM(1,2) \"test\"\r\nsecond line";
        QVERIFY(runner.start(config));
        QVERIFY(runner.csv().isEmpty());
        QVERIFY(done.wait(5000));
        result = qvariant_cast<BenchmarkSnapshot>(done.takeFirst().first());
        QCOMPARE(result.succeeded, 3);
        QCOMPARE(result.cancelled, 0);
        const auto csv = runner.csv();
        QVERIFY(csv.contains("\"'  =SUM(1,2) \"\"test\"\"\r\nsecond line\""));
        QVERIFY(csv.contains("started_utc,label,url,total,concurrency,timeout_ms"));
        QVERIFY(csv.contains("complete 2xx only; nearest-rank percentiles"));
        QCOMPARE(runner.config().label, config.label);
    }
    void validationAndDestruction() {
        BenchmarkRunner runner;
        QSignalSpy errors(&runner, &BenchmarkRunner::error);
        BenchmarkConfig config;
        config.url = QUrl("http://localhost/");
        config.total = 10001;
        QVERIFY(!runner.start(config));
        config.total = 1;
        config.concurrency = 33;
        QVERIFY(!runner.start(config));
        config.concurrency = 1;
        config.url = QUrl("file:///etc/hosts");
        QVERIFY(!runner.start(config));
        QCOMPARE(errors.size(), 3);
        BenchmarkFixture fixture;
        auto *activeRunner = new BenchmarkRunner;
        config.url = fixture.url("/hang");
        config.timeoutMs = 120000;
        QVERIFY(activeRunner->start(config));
        QTRY_COMPARE(fixture.requests, 1);
        QElapsedTimer elapsed;
        elapsed.start();
        delete activeRunner;
        QVERIFY(elapsed.elapsed() < 2000);
    }
};
QTEST_GUILESS_MAIN(BenchmarkTests)
#include "test_benchmark.moc"
