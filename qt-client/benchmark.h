#pragma once

#include <QMap>
#include <QObject>
#include <QThread>
#include <QUrl>

struct BenchmarkConfig {
    QUrl url;
    int total = 100;
    int concurrency = 4;
    int timeoutMs = 5000;
    QString label;
};
Q_DECLARE_METATYPE(BenchmarkConfig)

struct BenchmarkSnapshot {
    int total = 0;
    int started = 0;
    int completed = 0;
    int succeeded = 0;
    int httpErrors = 0;
    int networkErrors = 0;
    int timeouts = 0;
    int cancelled = 0;
    int active = 0;
    qint64 elapsedMs = 0;
    double throughput = 0;
    double averageMs = 0;
    double p50Ms = 0;
    double p95Ms = 0;
    bool running = false;
    QMap<int, int> statusCounts;
};
Q_DECLARE_METATYPE(BenchmarkSnapshot)

class BenchmarkWorker;
class BenchmarkRunner : public QObject {
    Q_OBJECT
  public:
    explicit BenchmarkRunner(QObject *parent = nullptr);
    ~BenchmarkRunner() override;
    bool start(const BenchmarkConfig &config);
    void stop();
    bool running() const {
        return running_;
    }
    QByteArray csv() const {
        return csv_;
    }
    BenchmarkConfig config() const {
        return config_;
    }
  signals:
    void updated(const BenchmarkSnapshot &snapshot);
    void finished(const BenchmarkSnapshot &snapshot);
    void error(const QString &message);

  private:
    QThread thread_;
    BenchmarkWorker *worker_ = nullptr;
    BenchmarkConfig config_;
    QByteArray csv_;
    bool running_ = false;
};
