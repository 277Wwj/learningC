#include "benchmark.h"
#include "httpclient.h"

#include <QByteArrayList>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
struct RequestRecord {
    int id = 0;
    int status = 0;
    HttpResult::Outcome outcome = HttpResult::Outcome::NetworkError;
    qint64 elapsed = 0;
    qint64 bytes = 0;
    QString error;
};

QByteArray csvText(QString value) {
    // Quoting alone does not prevent formulas when Excel opens a CSV.
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty() && QStringLiteral("=+-@").contains(trimmed.front()))
        value.prepend('\'');
    if (!value.isEmpty() &&
        (value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
        value.prepend('\'');
    value.replace('"', QStringLiteral("\"\""));
    return '"' + value.toUtf8() + '"';
}

QString outcomeName(HttpResult::Outcome outcome) {
    switch (outcome) {
    case HttpResult::Outcome::Success:
        return QStringLiteral("success");
    case HttpResult::Outcome::HttpError:
        return QStringLiteral("http_error");
    case HttpResult::Outcome::NetworkError:
        return QStringLiteral("network_error");
    case HttpResult::Outcome::Timeout:
        return QStringLiteral("timeout");
    case HttpResult::Outcome::Cancelled:
        return QStringLiteral("cancelled");
    case HttpResult::Outcome::ResponseTooLarge:
        return QStringLiteral("response_too_large");
    }
    return QStringLiteral("unknown");
}
} // namespace

class BenchmarkWorker : public QObject {
    Q_OBJECT
  public:
    void start(const BenchmarkConfig &config) {
        if (snapshot_.running)
            return;
        config_ = config;
        snapshot_ = {};
        snapshot_.total = config.total;
        snapshot_.running = true;
        stopping_ = false;
        records_.clear();
        records_.reserve(config.total);
        latencies_.clear();
        startedUtc_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        elapsed_.start();
        if (!updateTimer_) {
            updateTimer_ = new QTimer(this);
            updateTimer_->setInterval(200);
            updateTimer_->setTimerType(Qt::PreciseTimer);
            connect(updateTimer_, &QTimer::timeout, this, [this] { emit updated(measure()); });
        }
        updateTimer_->start();
        fill();
    }

    void stop() {
        if (!snapshot_.running)
            return;
        stopping_ = true;
        const auto clients = active_.keys();
        for (HttpClient *client : clients) {
            if (active_.contains(client))
                client->cancel();
        }
        if (active_.isEmpty())
            finish();
    }

  signals:
    void updated(const BenchmarkSnapshot &snapshot);
    void finished(const BenchmarkSnapshot &snapshot, const QByteArray &csv);

  private:
    BenchmarkConfig config_;
    BenchmarkSnapshot snapshot_;
    QHash<HttpClient *, int> active_;
    QList<RequestRecord> records_;
    QList<qint64> latencies_;
    QElapsedTimer elapsed_;
    QString startedUtc_;
    QTimer *updateTimer_ = nullptr;
    bool stopping_ = false;

    void fill() {
        if (!snapshot_.running)
            return;
        while (!stopping_ && snapshot_.started < config_.total &&
               active_.size() < config_.concurrency) {
            // Each attempt gets its own manager and connection pool. All these
            // objects are created, used and destroyed on this worker thread.
            auto *client = new HttpClient(this);
            client->setCaptureBody(false);
            const int id = ++snapshot_.started;
            active_.insert(client, id);
            connect(client, &HttpClient::finished, this,
                    [this, client](const HttpResult &result) { consume(client, result); });
            RequestSpec request;
            request.url = config_.url;
            request.timeoutMs = config_.timeoutMs;
            request.headers.insert("Connection", "close");
            if (!client->send(request)) {
                HttpResult failed;
                failed.request = request;
                failed.error = QStringLiteral("请求未能启动");
                consume(client, failed);
            }
        }
        if (active_.isEmpty() && (stopping_ || snapshot_.started == config_.total))
            finish();
    }

    void consume(HttpClient *client, const HttpResult &result) {
        if (!active_.contains(client))
            return;
        const int id = active_.take(client);
        client->deleteLater();
        ++snapshot_.completed;
        if (result.status > 0)
            ++snapshot_.statusCounts[result.status];
        switch (result.outcome) {
        case HttpResult::Outcome::Success:
            ++snapshot_.succeeded;
            latencies_.append(result.elapsed);
            break;
        case HttpResult::Outcome::HttpError:
            ++snapshot_.httpErrors;
            break;
        case HttpResult::Outcome::Timeout:
            ++snapshot_.timeouts;
            break;
        case HttpResult::Outcome::Cancelled:
            ++snapshot_.cancelled;
            break;
        case HttpResult::Outcome::NetworkError:
        case HttpResult::Outcome::ResponseTooLarge:
            ++snapshot_.networkErrors;
            break;
        }
        // Keep only metadata; no response or request bodies survive completion.
        records_.append({id, result.status, result.outcome, result.elapsed, result.bytesReceived,
                         result.error});
        if (active_.isEmpty() && (stopping_ || snapshot_.started == config_.total))
            finish();
        else
            QTimer::singleShot(0, this, [this] { fill(); });
    }

    BenchmarkSnapshot measure() {
        snapshot_.elapsedMs = elapsed_.elapsed();
        snapshot_.active = static_cast<int>(active_.size());
        snapshot_.throughput =
            snapshot_.elapsedMs > 0
                ? (snapshot_.completed - snapshot_.cancelled) * 1000.0 / snapshot_.elapsedMs
                : 0;
        if (!latencies_.isEmpty()) {
            auto sorted = latencies_;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0;
            for (const qint64 latency : sorted)
                sum += latency;
            snapshot_.averageMs = sum / sorted.size();
            // Nearest-rank percentiles, including only fully received 2xx.
            snapshot_.p50Ms = sorted[qMax<qsizetype>(
                0, static_cast<qsizetype>(std::ceil(sorted.size() * 0.50)) - 1)];
            snapshot_.p95Ms = sorted[qMax<qsizetype>(
                0, static_cast<qsizetype>(std::ceil(sorted.size() * 0.95)) - 1)];
        }
        return snapshot_;
    }

    QByteArray makeCsv() const {
        QByteArray output =
            "started_utc,label,url,total,concurrency,timeout_ms,protocol,connection_policy,latency_"
            "policy,run_elapsed_ms,completed,succeeded,http_errors,network_errors,timeouts,"
            "cancelled,throughput_rps,average_ms,p50_ms,p95_ms,request_id,status,outcome,elapsed_"
            "ms,response_bytes,error\r\n";
        const QList<QByteArray> context{
            csvText(startedUtc_),
            csvText(config_.label),
            csvText(config_.url.toString(QUrl::FullyEncoded)),
            QByteArray::number(config_.total),
            QByteArray::number(config_.concurrency),
            QByteArray::number(config_.timeoutMs),
            csvText(QStringLiteral("HTTP/1.1")),
            csvText(QStringLiteral("fresh connection per request")),
            csvText(QStringLiteral("complete 2xx only; nearest-rank percentiles")),
            QByteArray::number(snapshot_.elapsedMs),
            QByteArray::number(snapshot_.completed),
            QByteArray::number(snapshot_.succeeded),
            QByteArray::number(snapshot_.httpErrors),
            QByteArray::number(snapshot_.networkErrors),
            QByteArray::number(snapshot_.timeouts),
            QByteArray::number(snapshot_.cancelled),
            QByteArray::number(snapshot_.throughput, 'f', 3),
            QByteArray::number(snapshot_.averageMs, 'f', 3),
            QByteArray::number(snapshot_.p50Ms, 'f', 3),
            QByteArray::number(snapshot_.p95Ms, 'f', 3)};
        const QByteArray prefix = context.join(',') + ',';
        auto records = records_;
        std::sort(records.begin(), records.end(),
                  [](const RequestRecord &a, const RequestRecord &b) { return a.id < b.id; });
        for (const auto &record : records) {
            const QList<QByteArray> columns{
                QByteArray::number(record.id),        QByteArray::number(record.status),
                csvText(outcomeName(record.outcome)), QByteArray::number(record.elapsed),
                QByteArray::number(record.bytes),     csvText(record.error)};
            output += prefix + columns.join(',') + "\r\n";
        }
        return output;
    }

    void finish() {
        if (!snapshot_.running)
            return;
        updateTimer_->stop();
        measure();
        snapshot_.running = false;
        const QByteArray csv = makeCsv();
        emit finished(snapshot_, csv);
    }
};

BenchmarkRunner::BenchmarkRunner(QObject *parent) : QObject(parent) {
    qRegisterMetaType<BenchmarkSnapshot>();
    qRegisterMetaType<BenchmarkConfig>();
    worker_ = new BenchmarkWorker;
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &BenchmarkWorker::updated, this, &BenchmarkRunner::updated);
    connect(worker_, &BenchmarkWorker::finished, this,
            [this](const BenchmarkSnapshot &snapshot, const QByteArray &csv) {
                running_ = false;
                csv_ = csv;
                emit finished(snapshot);
            });
    thread_.start();
}

BenchmarkRunner::~BenchmarkRunner() {
    disconnect(worker_, nullptr, this, nullptr);
    if (thread_.isRunning()) {
        QMetaObject::invokeMethod(
            worker_, [worker = worker_] { worker->stop(); }, Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
    }
}

bool BenchmarkRunner::start(const BenchmarkConfig &config) {
    if (running_) {
        emit error(QStringLiteral("压测正在进行"));
        return false;
    }
    if (config.total < 1 || config.total > 10000 || config.concurrency < 1 ||
        config.concurrency > 32) {
        emit error(QStringLiteral("请求总数应为 1–10000，并发数应为 1–32"));
        return false;
    }
    RequestSpec request;
    request.url = config.url;
    request.timeoutMs = config.timeoutMs;
    const QString invalid = HttpClient::validationError(request);
    if (!invalid.isEmpty()) {
        emit error(invalid);
        return false;
    }
    config_ = config;
    csv_.clear();
    running_ = true;
    QMetaObject::invokeMethod(
        worker_, [worker = worker_, config] { worker->start(config); }, Qt::QueuedConnection);
    return true;
}

void BenchmarkRunner::stop() {
    if (!running_)
        return;
    QMetaObject::invokeMethod(
        worker_, [worker = worker_] { worker->stop(); }, Qt::QueuedConnection);
}

#include "benchmark.moc"
