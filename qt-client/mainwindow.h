#pragma once
#include "benchmark.h"
#include "httpclient.h"
#include "tcpclient.h"
#include <QJsonArray>
#include <QMainWindow>
#include <QVector>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QListWidget;
class QStackedWidget;
class QTableWidget;
class QProgressBar;
class QDockWidget;
class QSettings;
class QCloseEvent;
class Sparkline;

class MainWindow final : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(bool isolated = false, QWidget *parent = nullptr);
    ~MainWindow() override;
    void showPage(int index);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    struct Profile {
        QString name, variant, base, host;
        int port = 12735, timeout = 5000;
    };
    struct History {
        RequestSpec request;
        int status;
        qint64 elapsed;
        QString error;
    };
    struct Log {
        QString category, text;
    };
    HttpClient http_;
    BenchmarkRunner benchmark_;
    TcpClient tcp_;
    QVector<Profile> profiles_;
    QVector<History> history_;
    QVector<Log> logs_;
    HttpResult lastResponse_;
    bool hasResponse_ = false, isolated_ = false;
    QSettings *settings_ = nullptr;
    QStackedWidget *pages_ = nullptr;
    QVector<QPushButton *> navigation_;
    QComboBox *profile_ = nullptr, *method_ = nullptr, *preset_ = nullptr;
    QLineEdit *url_ = nullptr;
    QSpinBox *httpTimeout_ = nullptr;
    QPlainTextEdit *requestHeaders_ = nullptr, *requestBody_ = nullptr, *responseRaw_ = nullptr,
                   *responsePretty_ = nullptr, *responseHeaders_ = nullptr;
    QLabel *httpStatus_ = nullptr, *httpElapsed_ = nullptr, *httpBytes_ = nullptr,
           *httpNotice_ = nullptr, *responseInfo_ = nullptr;
    QPushButton *sendHttp_ = nullptr, *cancelHttp_ = nullptr;
    QListWidget *historyList_ = nullptr;
    QLineEdit *benchUrl_ = nullptr;
    QSpinBox *benchTotal_ = nullptr, *benchConcurrency_ = nullptr, *benchTimeout_ = nullptr;
    QPushButton *benchStart_ = nullptr, *benchStop_ = nullptr, *benchExport_ = nullptr;
    QLabel *benchDone_ = nullptr, *benchSuccess_ = nullptr, *benchRate_ = nullptr,
           *benchP95_ = nullptr, *benchInfo_ = nullptr, *benchExtra_ = nullptr;
    QProgressBar *benchProgress_ = nullptr;
    QTableWidget *benchErrors_ = nullptr;
    Sparkline *chart_ = nullptr;
    QLineEdit *tcpHost_ = nullptr, *tcpNick_ = nullptr, *tcpTarget_ = nullptr;
    QSpinBox *tcpPort_ = nullptr;
    QComboBox *tcpType_ = nullptr, *tcpMode_ = nullptr;
    QPushButton *tcpConnect_ = nullptr, *tcpDisconnect_ = nullptr, *tcpSend_ = nullptr;
    QLabel *tcpState_ = nullptr, *tcpNotice_ = nullptr;
    QPlainTextEdit *tcpPayload_ = nullptr, *tcpPreview_ = nullptr, *tcpRaw_ = nullptr,
                   *tcpDecoded_ = nullptr;
    QTableWidget *frames_ = nullptr;
    QVector<FrameEvent> frameEvents_;
    QDockWidget *logDock_ = nullptr;
    QComboBox *logFilter_ = nullptr;
    QLineEdit *logSearch_ = nullptr;
    QPlainTextEdit *logView_ = nullptr;
    QWidget *makeHttpPage();
    QWidget *makeBenchmarkPage();
    QWidget *makeTcpPage();
    void makeLogDock();
    void buildUi();
    void wireEvents();
    void loadSettings();
    void saveSettings();
    void applyProfile();
    void editSettings();
    void sendHttp();
    void displayResponse(const HttpResult &);
    void restoreHistory(int row);
    void startBenchmark();
    void updateBenchmark(const BenchmarkSnapshot &);
    QByteArray currentPayload(quint32 &type) const;
    void previewFrame();
    void sendFrame();
    void appendFrame(const FrameEvent &);
    void showFrame(int row);
    void addLog(const QString &category, const QString &message);
    void filterLogs();
    void showNotice(const QString &text, bool error = false);
    bool saveBytes(const QByteArray &bytes, const QString &suggested, const QString &filter);
};
