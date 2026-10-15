#include "mainwindow.h"
#include "protocol.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <QtWidgets>

// Test fixture only. Never built into or launched by the production application.
class UiHttpFixture : public QObject {
  public:
    QTcpServer server;
    QHash<QTcpSocket *, QByteArray> buffers;
    UiHttpFixture() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *s = server.nextPendingConnection();
                buffers.insert(s, {});
                connect(s, &QTcpSocket::disconnected, this, [this, s] {
                    buffers.remove(s);
                    s->deleteLater();
                });
                connect(s, &QTcpSocket::readyRead, this, [this, s] {
                    auto &b = buffers[s];
                    b += s->readAll();
                    if (!b.contains("\r\n\r\n") || s->property("responded").toBool())
                        return;
                    s->setProperty("responded", true);
                    const auto path = b.split(' ').value(1);
                    const bool missing = path == "/missing";
                    const QByteArray body =
                        missing
                            ? QByteArray("{\"error\":\"Not Found\",\"status\":404}")
                            : QByteArray(
                                  "{\n  \"service\": \"learningC\",\n  \"fixture\": true,\n  "
                                  "\"status\": \"ok\",\n  \"message\": \"HTTP 客户端联调测试\",\n  "
                                  "\"routes\": [\"/\", \"/hello\", \"/missing\"]\n}\n");
                    const QByteArray response =
                        "HTTP/1.1 " + QByteArray(missing ? "404 Not Found" : "200 OK") +
                        "\r\nContent-Type: application/json; charset=utf-8\r\nX-Test-Fixture: "
                        "true\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    QTimer::singleShot(path == "/slow" ? 700 : 45, s, [s, response] {
                        s->write(response);
                        s->disconnectFromHost();
                    });
                });
            }
        });
    }
    ~UiHttpFixture() {
        for (auto *s : buffers.keys()) {
            s->disconnect(this);
            s->abort();
        }
        server.close();
    }
    QString url(const QString &path = "/hello") const {
        return QString("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path);
    }
};

class UiTests : public QObject {
    Q_OBJECT
    template <class T> T *get(MainWindow &w, const char *name) {
        return w.findChild<T *>(name);
    }
    void capture(MainWindow &w, const QString &name) {
        const QString out = qEnvironmentVariable("STUDIO_SCREENSHOTS");
        if (out.isEmpty())
            return;
        QDir().mkpath(out);
        QTest::qWait(100);
        QVERIFY(w.grab().save(out + "/" + name + ".png"));
    }
  private slots:
    void initTestCase() {
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setStyle("Fusion");
        QApplication::setFont(QFont("Microsoft YaHei UI", 9));
    }
    void httpRoundTripAndHistory() {
        UiHttpFixture fixture;
        MainWindow w(true);
        w.resize(1320, 860);
        w.show();
        auto *url = get<QLineEdit>(w, "httpUrl");
        auto *send = get<QPushButton>(w, "sendHttp");
        QVERIFY(url && send);
        url->setText(fixture.url());
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        QVERIFY(get<QPlainTextEdit>(w, "responseRaw")->toPlainText().contains("learningC"));
        QVERIFY(get<QPlainTextEdit>(w, "responseHeaders")
                    ->toPlainText()
                    .contains("Content-Type", Qt::CaseInsensitive));
        auto *history = get<QListWidget>(w, "httpHistory");
        QCOMPARE(history->count(), 1);
        capture(w, "01-http");
        url->setText(fixture.url("/missing"));
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(history->count(), 2, 5000);
        QVERIFY(get<QPlainTextEdit>(w, "responseRaw")->toPlainText().contains("404"));
        const auto r = history->visualItemRect(history->item(1));
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
        QTest::mouseDClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
        QCOMPARE(url->text(), fixture.url());
        url->setText(fixture.url("/slow"));
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY(get<QPushButton>(w, "cancelHttp")->isEnabled());
        QTest::mouseClick(get<QPushButton>(w, "cancelHttp"), Qt::LeftButton);
        QTRY_VERIFY(send->isEnabled());
        QCOMPARE(history->count(), 3);
        url->setText("bad-address");
        QTest::mouseClick(send, Qt::LeftButton);
        QVERIFY(send->isEnabled());
        QCOMPARE(history->count(), 3);
    }
    void benchmarkInteraction() {
        UiHttpFixture fixture;
        MainWindow w(true);
        w.resize(1320, 860);
        w.show();
        w.showPage(1);
        get<QLineEdit>(w, "benchUrl")->setText(fixture.url());
        get<QSpinBox>(w, "benchTotal")->setValue(100);
        get<QSpinBox>(w, "benchConcurrency")->setValue(4);
        auto *start = get<QPushButton>(w, "benchStart");
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(!start->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(get<QPushButton>(w, "benchExport")->isEnabled(), 15000);
        QCOMPARE(get<QProgressBar>(w, "benchProgress")->value(), 100);
        QVERIFY(start->isEnabled());
        capture(w, "02-benchmark");
        get<QLineEdit>(w, "benchUrl")->setText(fixture.url("/slow"));
        get<QSpinBox>(w, "benchTotal")->setValue(1000);
        QTest::mouseClick(start, Qt::LeftButton);
        QTRY_VERIFY(get<QPushButton>(w, "benchStop")->isEnabled());
        QTest::mouseClick(get<QPushButton>(w, "benchStop"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 5000);
    }
    void tcpInteraction() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QTcpSocket *peer = nullptr;
        QByteArray received;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            peer = server.nextPendingConnection();
            connect(peer, &QTcpSocket::readyRead, &server, [&] { received += peer->readAll(); });
        });
        MainWindow w(true);
        w.resize(1320, 860);
        w.show();
        w.showPage(2);
        get<QSpinBox>(w, "tcpPort")->setValue(server.serverPort());
        auto *send = get<QPushButton>(w, "tcpSend");
        QTest::mouseClick(get<QPushButton>(w, "tcpConnect"), Qt::LeftButton);
        QTRY_VERIFY(send->isEnabled());
        QVERIFY(peer);
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_VERIFY(received == Wire::pack(Wire::Login, "Wwj"));
        QCOMPARE(get<QTableWidget>(w, "tcpFrames")->rowCount(), 1);
        capture(w, "03-tcp");
        get<QComboBox>(w, "tcpMode")->setCurrentIndex(2);
        QTest::mouseClick(send, Qt::LeftButton);
        QTRY_COMPARE(get<QTableWidget>(w, "tcpFrames")->rowCount(), 3);
        QByteArray incoming("BAlice");
        incoming += '\0';
        incoming += "你好，协议检查";
        peer->write(Wire::pack(Wire::Chat, incoming));
        QTRY_COMPARE(get<QTableWidget>(w, "tcpFrames")->rowCount(), 4);
        QVERIFY(get<QPlainTextEdit>(w, "tcpDecoded")->toPlainText().contains("发送者     Alice"));
        QVERIFY(get<QPlainTextEdit>(w, "tcpDecoded")
                    ->toPlainText()
                    .contains("正文       你好，协议检查"));
        capture(w, "03-tcp-received");
        peer->write(Wire::pack(0xffffffffU, "opaque"));
        QTRY_COMPARE(get<QTableWidget>(w, "tcpFrames")->rowCount(), 5);
        QTemporaryDir temporary;
        const QString path = temporary.filePath("frames.json");
        QTimer::singleShot(100, &w, [&] {
            auto *d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            d->selectFile(path);
            QMetaObject::invokeMethod(d, "accept", Qt::QueuedConnection);
        });
        QTest::mouseClick(get<QPushButton>(w, "tcpExport"), Qt::LeftButton);
        QFile output(path);
        QVERIFY(output.open(QIODevice::ReadOnly));
        const auto exported = QJsonDocument::fromJson(output.readAll()).array();
        QCOMPARE(exported.last().toObject()["type"].toInteger(), qint64(0xffffffffU));
        QTest::mouseClick(get<QPushButton>(w, "tcpDisconnect"), Qt::LeftButton);
        QTRY_VERIFY(!send->isEnabled());
        QTRY_VERIFY(get<QPushButton>(w, "tcpConnect")->isEnabled());
    }
    void settingsPreserveBasePath() {
        MainWindow w(true);
        w.show();
        QTimer::singleShot(0, &w, [&] {
            auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            d->findChild<QLineEdit *>("profileBase")->setText("http://127.0.0.1:8080/api");
            for (auto *b : d->findChildren<QPushButton *>())
                if (b->text() == "保存") {
                    b->click();
                    return;
                }
            QFAIL("Save button missing");
        });
        QTest::mouseClick(get<QPushButton>(w, "settingsButton"), Qt::LeftButton);
        QCOMPARE(get<QLineEdit>(w, "httpUrl")->text(), QString("http://127.0.0.1:8080/api"));
        QCOMPARE(get<QLineEdit>(w, "benchUrl")->text(), QString("http://127.0.0.1:8080/api"));
        auto *preset = get<QComboBox>(w, "httpPreset");
        preset->setCurrentIndex(2);
        QMetaObject::invokeMethod(preset, "activated", Q_ARG(int, 2));
        QCOMPARE(get<QLineEdit>(w, "httpUrl")->text(), QString("http://127.0.0.1:8080/api/hello"));
    }
    void navigationAndSmallWindow() {
        MainWindow w(true);
        w.show();
        QTest::qWait(100);
        auto *httpScroll =
            qobject_cast<QScrollArea *>(get<QStackedWidget>(w, "mainPages")->currentWidget());
        if (w.height() >= 850)
            QCOMPARE(httpScroll->verticalScrollBar()->maximum(), 0);
        w.resize(1024, 600);
        w.show();
        QTest::qWait(100);
        QCOMPARE(w.size(), QSize(1024, 600));
        auto *headers = get<QPlainTextEdit>(w, "requestHeaders");
        QVERIFY(headers->height() >= 60);
        auto *scroll = get<QStackedWidget>(w, "mainPages")->currentWidget();
        QVERIFY(qobject_cast<QScrollArea *>(scroll)->verticalScrollBar()->maximum() > 0);
        for (int i = 0; i < 3; ++i) {
            QTest::mouseClick(get<QPushButton>(w, qPrintable(QString("nav%1").arg(i))),
                              Qt::LeftButton);
            QVERIFY(w.findChild<QPushButton *>(QString("nav%1").arg(i))->isChecked());
        }
        QVERIFY(get<QPushButton>(w, "tcpSend")->geometry().width() > 100);
        capture(w, "04-small-window");
    }
};
QTEST_MAIN(UiTests)
#include "test_ui.moc"
