#include "mainwindow.h"
#include "protocol.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtWidgets>
#include <algorithm>

namespace {
class PageCanvas final : public QWidget {
  public:
    int heightForWidth(int width) const override {
        // QScrollArea otherwise uses preferred editor heights as a hard minimum.
        return qMax(minimumHeight(), layout() ? layout()->minimumHeightForWidth(width) : 0);
    }
};

QString stamp() {
    return QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
}
QString bytesText(qint64 n) {
    return n < 1024          ? QString::number(n) + " B"
           : n < 1024 * 1024 ? QString::number(double(n) / 1024, 'f', 1) + " KiB"
                             : QString::number(double(n) / (1024 * 1024), 'f', 2) + " MiB";
}
QIcon icon(const QString &kind, const QColor &color = QColor("#6b7d89")) {
    QPixmap p(40, 40);
    p.fill(Qt::transparent);
    QPainter q(&p);
    q.setRenderHint(QPainter::Antialiasing);
    q.scale(2, 2);
    q.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (kind == "http") {
        q.drawRoundedRect(QRectF(2, 3, 16, 14), 3, 3);
        q.drawLine(2, 7, 18, 7);
        q.drawLine(7, 11, 5, 13);
        q.drawLine(5, 13, 7, 15);
        q.drawLine(13, 11, 15, 13);
        q.drawLine(15, 13, 13, 15);
    } else if (kind == "bench") {
        q.drawLine(3, 3, 3, 17);
        q.drawLine(3, 17, 18, 17);
        QPolygonF a;
        a << QPointF(5, 13) << QPointF(9, 9) << QPointF(12, 11) << QPointF(17, 4);
        q.drawPolyline(a);
    } else if (kind == "tcp") {
        q.drawRoundedRect(QRectF(2, 7, 5, 6), 1, 1);
        q.drawRoundedRect(QRectF(13, 7, 5, 6), 1, 1);
        q.drawLine(7, 9, 13, 9);
        q.drawLine(7, 11, 13, 11);
        q.drawLine(4, 4, 4, 7);
        q.drawLine(16, 13, 16, 16);
    } else if (kind == "settings") {
        q.drawEllipse(QPointF(10, 10), 5, 5);
        q.drawEllipse(QPointF(10, 10), 1.5, 1.5);
        for (int i = 0; i < 8; ++i) {
            q.save();
            q.translate(10, 10);
            q.rotate(i * 45);
            q.drawLine(0, 5, 0, 8);
            q.restore();
        }
    } else if (kind == "copy") {
        q.drawRoundedRect(QRectF(6, 6, 11, 12), 2, 2);
        q.drawLine(3, 14, 3, 3);
        q.drawLine(3, 3, 13, 3);
    } else if (kind == "export") {
        q.drawLine(10, 2, 10, 12);
        q.drawLine(6, 8, 10, 12);
        q.drawLine(14, 8, 10, 12);
        q.drawLine(3, 13, 3, 17);
        q.drawLine(3, 17, 17, 17);
        q.drawLine(17, 17, 17, 13);
    } else {
        q.drawRoundedRect(QRectF(2, 3, 16, 14), 2, 2);
        q.drawLine(5, 7, 8, 10);
        q.drawLine(8, 10, 5, 13);
        q.drawLine(11, 13, 15, 13);
    }
    return QIcon(p);
}
QLabel *label(const QString &text, const QString &name = {}) {
    auto *w = new QLabel(text);
    w->setTextFormat(Qt::PlainText);
    if (!name.isEmpty())
        w->setObjectName(name);
    return w;
}
QFrame *card() {
    auto *f = new QFrame;
    f->setObjectName("Card");
    return f;
}
QVBoxLayout *vertical(QWidget *w, int margin = 18, int spacing = 12) {
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(margin, margin, margin, margin);
    l->setSpacing(spacing);
    return l;
}
QPushButton *button(const QString &text, const QString &name = {}, bool primary = false) {
    auto *b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setMinimumHeight(36);
    if (!name.isEmpty())
        b->setObjectName(name);
    b->setProperty("primary", primary);
    return b;
}
QPlainTextEdit *editor(const QString &name, bool readOnly = false) {
    auto *w = new QPlainTextEdit;
    w->setObjectName(name);
    w->setProperty("code", true);
    w->setReadOnly(readOnly);
    w->setFont(QFont("Consolas", 10));
    w->setLineWrapMode(QPlainTextEdit::NoWrap);
    w->setTabStopDistance(32);
    return w;
}
QLineEdit *line(const QString &name, const QString &placeholder = {}) {
    auto *w = new QLineEdit;
    w->setObjectName(name);
    w->setPlaceholderText(placeholder);
    w->setMinimumHeight(36);
    w->setClearButtonEnabled(true);
    return w;
}
QSpinBox *spin(const QString &name, int low, int high, int value, const QString &suffix = {}) {
    auto *w = new QSpinBox;
    w->setObjectName(name);
    w->setRange(low, high);
    w->setValue(value);
    w->setSuffix(suffix);
    w->setMinimumHeight(36);
    w->setButtonSymbols(QAbstractSpinBox::PlusMinus);
    return w;
}
QWidget *metricCard(const QString &caption, QLabel *&value, const QString &detail = {}) {
    auto *f = card();
    auto *l = vertical(f, 16, 3);
    l->addWidget(label(caption, "Muted"));
    value = label("—", "Metric");
    l->addWidget(value);
    if (!detail.isEmpty())
        l->addWidget(label(detail, "Small"));
    return f;
}
QWidget *pageHeading(const QString &eyebrow, const QString &title, const QString &description) {
    auto *w = new QWidget;
    auto *l = vertical(w, 0, 4);
    l->addWidget(label(eyebrow, "Eyebrow"));
    l->addWidget(label(title, "PageTitle"));
    l->addWidget(label(description, "Muted"));
    return w;
}
QString hexDump(const QByteArray &b) {
    QString out;
    for (qsizetype i = 0; i < b.size(); i += 16) {
        out += QString("%1   ").arg(i, 4, 16, QChar('0'));
        const auto row = b.mid(i, 16);
        out += QString::fromLatin1(row.toHex(' ')).leftJustified(48, ' ') + "  ";
        for (char c : row)
            out += c >= 32 && c < 127 ? QChar(c) : QChar('.');
        out += '\n';
    }
    return out;
}
QString frameType(quint32 t) {
    static const QStringList names = {"?",   "Login", "Welcome", "Users",
                                      "Say", "Chat",  "Bye",     "Error"};
    return t < quint32(names.size()) ? names[int(t)] : QString("Unknown (%1)").arg(t);
}
const char *studioStyle = R"(
QMainWindow,QWidget#Root,QWidget#WorkPage {background:#f3f6f9;color:#263b48;}
QWidget {font-family:'Microsoft YaHei UI','Segoe UI';font-size:13px;color:#263b48;}
QFrame#Sidebar {background:#142c36;border:0;} QFrame#Sidebar QLabel {color:#a4bac3;}
QFrame#Sidebar QLabel#Brand {color:white;font-size:23px;font-weight:700;} QFrame#Sidebar QLabel#BrandSub {font-size:9px;color:#75a3af;letter-spacing:1px;}
QFrame#Sidebar QLabel#SideTitle {font-size:10px;color:#6f94a1;letter-spacing:2px;}
QFrame#Sidebar QPushButton {background:transparent;border:0;text-align:left;color:#adbec6;padding:13px 16px;border-radius:8px;font-size:14px;}
QFrame#Sidebar QPushButton:hover {background:#203d47;color:white;} QFrame#Sidebar QPushButton:checked {background:#24524f;color:#a4f2de;}
QFrame#Sidebar QLabel#SideMark {color:#8ce4ce;font-family:'Consolas';font-size:26px;font-weight:bold;}
QFrame#Sidebar QFrame#SideHint {background:#1b3842;border:1px solid #294953;border-radius:10px;}
QFrame#Topbar {background:white;border-bottom:1px solid #e2e9ee;}
QFrame#Card {background:white;border:1px solid #e2e9ee;border-radius:12px;}
QLabel#Eyebrow {color:#68858e;font-size:10px;font-weight:600;letter-spacing:2px;}
QLabel#PageTitle {font-size:25px;font-weight:700;color:#193643;}
QLabel#Muted {color:#73838e;} QLabel#Small {color:#8c9aa4;font-size:11px;}
QLabel#Section {font-size:14px;font-weight:600;} QLabel#Metric {font-family:'Segoe UI';font-size:25px;font-weight:600;color:#194c4b;}
QLabel#Pill {background:#e8f5ef;color:#28775d;border-radius:10px;padding:5px 10px;font-size:11px;}
QLabel#Notice {background:#eaf3f7;color:#4e7587;border:1px solid #dce9f0;border-radius:7px;padding:8px 10px;}
QLabel#Notice[error=true] {background:#fff0eb;color:#a45238;border:1px solid #f5d7c9;}
QPushButton {background:white;border:1px solid #dce5e9;border-radius:7px;padding:6px 13px;color:#47616c;}
QPushButton:hover {background:#f0f7f7;border-color:#a7cfc8;} QPushButton:pressed {background:#e3f2ef;}
QPushButton[primary=true] {background:#137c70;border-color:#137c70;color:white;font-weight:600;}
QPushButton[primary=true]:hover {background:#0d9180;} QPushButton:disabled {background:#eef2f4;color:#a8b4bb;border-color:#e6ecef;}
QLineEdit,QComboBox,QSpinBox {background:white;border:1px solid #dce5e9;border-radius:6px;padding:5px 9px;selection-background-color:#bde9df;}
QLineEdit:focus,QComboBox:focus,QSpinBox:focus {border:1px solid #2da590;}
QComboBox::drop-down {border:0;width:22px;} QComboBox QAbstractItemView {background:white;selection-background-color:#def3ec;color:#264e49;}
QPlainTextEdit {background:white;border:0;border-radius:4px;padding:6px;selection-background-color:#c7eae4;}
QPlainTextEdit[code=true] {font-family:'Consolas','Microsoft YaHei UI';font-size:13px;background:#fafcfd;color:#355567;}
QTabWidget::pane {border:0;} QTabBar::tab {background:transparent;color:#7c8c97;padding:10px 13px;border-bottom:2px solid transparent;}
QTabBar::tab:selected {color:#137c70;border-bottom:2px solid #137c70;font-weight:600;}
QListWidget,QTableWidget {background:white;border:0;outline:0;alternate-background-color:#f7fafb;selection-background-color:#e0f2ed;selection-color:#186a5d;}
QListWidget::item {padding:10px 8px;border-bottom:1px solid #f0f3f5;} QListWidget::item:selected {background:#e8f6f1;border-radius:6px;}
QTableWidget::item {padding:5px;border:0;} QHeaderView::section {background:#f5f8fa;border:0;border-bottom:1px solid #e7edf0;padding:9px 6px;color:#7e8c96;font-size:11px;}
QScrollBar:vertical {background:transparent;width:9px;margin:2px;} QScrollBar::handle:vertical {background:#d2dfe4;min-height:26px;border-radius:4px;} QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical {height:0;}
QScrollBar:horizontal {height:9px;background:transparent;} QScrollBar::handle:horizontal {background:#d2dfe4;min-width:26px;border-radius:4px;} QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal {width:0;}
QSplitter::handle {background:transparent;width:10px;height:10px;}
QProgressBar {border:0;border-radius:5px;background:#e8eef0;height:10px;text-align:center;color:#526a74;} QProgressBar::chunk {background:#24a58d;border-radius:5px;}
QDockWidget {color:#526a74;} QDockWidget::title {background:#eaf0f4;padding:8px;}
QToolTip {background:#203d47;color:white;border:0;padding:6px;}
)";
} // namespace

class Sparkline final : public QWidget {
  public:
    explicit Sparkline(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(175);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void reset() {
        values_.clear();
        update();
    }
    void append(double value) {
        values_.append(value);
        if (values_.size() > 90)
            values_.removeFirst();
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF area = rect().adjusted(45, 20, -16, -28);
        if (area.width() < 1 || area.height() < 1)
            return;
        const double maximum =
            values_.isEmpty() ? 100
                              : qMax(1.0, *std::max_element(values_.begin(), values_.end()) * 1.2);
        for (int i = 0; i < 5; ++i) {
            const auto y = area.top() + area.height() * i / 4;
            p.setPen(QPen(QColor("#e9eff2"), 1, Qt::DashLine));
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
            p.setPen(QColor("#93a3ad"));
            p.setFont(QFont("Segoe UI", 8));
            p.drawText(QRectF(0, y - 9, 37, 18), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(maximum * (4 - i) / 4, 'f', 0));
        }
        p.setPen(QColor("#92a4ad"));
        p.drawText(QRectF(area.left(), area.bottom() + 10, area.width(), 18), Qt::AlignRight,
                   "时间 →");
        if (values_.isEmpty()) {
            p.setPen(QColor("#9baeb6"));
            p.setFont(QFont("Microsoft YaHei UI", 11));
            p.drawText(area, Qt::AlignCenter, "运行测试后，吞吐曲线将显示在这里");
            return;
        }
        QPainterPath path;
        for (int i = 0; i < values_.size(); ++i) {
            const QPointF point(area.left() + area.width() * i / qMax(1, values_.size() - 1),
                                area.bottom() - area.height() * values_[i] / maximum);
            if (!i)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        QPainterPath fill = path;
        fill.lineTo(area.right(), area.bottom());
        fill.lineTo(area.left(), area.bottom());
        fill.closeSubpath();
        QLinearGradient gradient(area.topLeft(), area.bottomLeft());
        gradient.setColorAt(0, QColor(33, 163, 139, 65));
        gradient.setColorAt(1, QColor(33, 163, 139, 2));
        p.fillPath(fill, gradient);
        p.setPen(QPen(QColor("#20a18b"), 2.3));
        p.drawPath(path);
    }

  private:
    QVector<double> values_;
};

MainWindow::MainWindow(bool isolated, QWidget *parent) : QMainWindow(parent), isolated_(isolated) {
    setWindowTitle("learningC Studio · 服务器调试工作台");
    setMinimumSize(960, 580);
    resize(1320, 860);
    setStyleSheet(studioStyle);
    loadSettings();
    buildUi();
    wireEvents();
    applyProfile();
    showPage(0);
    addLog("系统", "工作台已就绪。选择服务器配置，开始一次真实请求。");
    if (auto *screen = QGuiApplication::primaryScreen()) {
        const auto a = screen->availableGeometry();
        resize(qMin(width(), a.width() - 40), qMin(height(), a.height() - 60));
    }
}
MainWindow::~MainWindow() {
    saveSettings();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    http_.cancel();
    benchmark_.stop();
    tcp_.disconnectFrom();
    saveSettings();
    QMainWindow::closeEvent(event);
}
void MainWindow::buildUi() {
    auto *root = new QWidget;
    root->setObjectName("Root");
    setCentralWidget(root);
    auto *layout = new QHBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *side = new QFrame;
    side->setObjectName("Sidebar");
    side->setFixedWidth(206);
    auto *sl = vertical(side, 18, 12);
    auto *brand = new QHBoxLayout;
    brand->addWidget(label("‹/›", "SideMark"));
    auto *word = new QVBoxLayout;
    word->setSpacing(0);
    word->addWidget(label("learningC", "Brand"));
    word->addWidget(label("NETWORK STUDIO", "BrandSub"));
    brand->addLayout(word);
    sl->addLayout(brand);
    sl->addSpacing(32);
    sl->addWidget(label("工作空间  /  WORKSPACE", "SideTitle"));
    const QStringList names = {"HTTP 请求", "轻量压测", "TCP 协议"},
                      icons = {"http", "bench", "tcp"};
    for (int i = 0; i < 3; ++i) {
        auto *b = button(names[i], QString("nav%1").arg(i));
        b->setCheckable(true);
        b->setIcon(icon(icons[i], QColor("#8fb9bf")));
        b->setIconSize({21, 21});
        sl->addWidget(b);
        navigation_ += b;
        connect(b, &QPushButton::clicked, this, [this, i] { showPage(i); });
    }
    sl->addStretch();
    auto *hint = new QFrame;
    hint->setObjectName("SideHint");
    auto *hl = vertical(hint, 13, 8);
    hl->addWidget(label("连接 · 观察 · 验证", "SideTitle"));
    auto *text = label("让每一次调试\n都有迹可循。");
    text->setStyleSheet("color:#b9d6dd;line-height:1.5;");
    hl->addWidget(text);
    sl->addWidget(hint);
    sl->addSpacing(14);
    auto *help = button("快速上手");
    help->setIcon(icon("log", QColor("#8fb9bf")));
    sl->addWidget(help);
    connect(help, &QPushButton::clicked, this, [this] {
        QMessageBox::information(
            this, "快速上手",
            "1. 在 Linux 中启动一个现有 HTTP 服务。\n2. 在右上角设置中填写服务器 IP。\n3. 发送 "
            "HTTP 请求，或将地址复制到压测页面。\n4. TCP 页面用于观察 LCH1 "
            "协议帧；连接后手动发送 Login，在收帧记录中检查 Welcome/Error，再测试消息转发。\n\nCtrl+Enter：发送 / "
            "开始测试\nCtrl+L：编辑 HTTP 地址\nCtrl+Shift+L：展开日志\n\n完整说明见随程序附带的 "
            "README.md。");
    });
    sl->addWidget(label("C++ / Qt 6    ·    v1.0.2", "BrandSub"));
    layout->addWidget(side);
    auto *main = new QWidget;
    auto *ml = vertical(main, 0, 0);
    auto *bar = new QFrame;
    bar->setObjectName("Topbar");
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(26, 12, 26, 12);
    bl->addWidget(label("工作台", "Section"));
    bl->addWidget(label("/   learningC", "Muted"));
    bl->addStretch();
    bl->addWidget(label("服务器配置", "Muted"));
    profile_ = new QComboBox;
    profile_->setObjectName("profileCombo");
    profile_->setMinimumWidth(175);
    for (const auto &p : profiles_)
        profile_->addItem(p.name);
    bl->addWidget(profile_);
    auto *logs = button("日志");
    logs->setIcon(icon("log"));
    bl->addWidget(logs);
    auto *settings = button("设置", "settingsButton");
    settings->setIcon(icon("settings"));
    bl->addWidget(settings);
    ml->addWidget(bar);
    pages_ = new QStackedWidget;
    pages_->setObjectName("mainPages");
    for (auto *page : {makeHttpPage(), makeBenchmarkPage(), makeTcpPage()}) {
        // Keep editors usable on short/high-DPI screens instead of compressing them to zero.
        page->setObjectName("WorkPage");
        page->setMinimumSize(780, 760);
        auto *scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        pages_->addWidget(scroll);
    }
    ml->addWidget(pages_, 1);
    layout->addWidget(main, 1);
    makeLogDock();
    connect(logs, &QPushButton::clicked, this,
            [this] { logDock_->setVisible(!logDock_->isVisible()); });
    connect(settings, &QPushButton::clicked, this, &MainWindow::editSettings);
    auto *sendShortcut = new QShortcut(QKeySequence("Ctrl+Return"), this);
    connect(sendShortcut, &QShortcut::activated, this, [this] {
        if (pages_->currentIndex() == 0)
            sendHttp();
        else if (pages_->currentIndex() == 1)
            startBenchmark();
        else
            sendFrame();
    });
    connect(new QShortcut(QKeySequence("Ctrl+L"), this), &QShortcut::activated, this, [this] {
        showPage(0);
        url_->setFocus();
        url_->selectAll();
    });
    connect(new QShortcut(QKeySequence("Ctrl+Shift+L"), this), &QShortcut::activated, this,
            [this] { logDock_->setVisible(!logDock_->isVisible()); });
}

QWidget *MainWindow::makeHttpPage() {
    auto *page = new PageCanvas;
    auto *l = vertical(page, 26, 14);
    l->addWidget(pageHeading("01  /  HTTP INSPECTOR", "HTTP 请求调试",
                             "从一个请求开始，查看服务器给出的真实答案。"));
    auto *request = card();
    auto *rl = new QHBoxLayout(request);
    rl->setContentsMargins(14, 13, 14, 13);
    rl->setSpacing(10);
    method_ = new QComboBox;
    method_->setObjectName("httpMethod");
    method_->addItems({"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"});
    method_->setFixedWidth(102);
    method_->setStyleSheet("font-weight:700;color:#137c70;");
    url_ = line("httpUrl", "http://127.0.0.1:8080/hello");
    url_->setFont(QFont("Consolas", 10));
    sendHttp_ = button("发送请求  →", "sendHttp", true);
    cancelHttp_ = button("取消", "cancelHttp");
    cancelHttp_->setEnabled(false);
    rl->addWidget(method_);
    rl->addWidget(url_, 1);
    rl->addWidget(sendHttp_);
    rl->addWidget(cancelHttp_);
    l->addWidget(request);
    auto *aux = new QHBoxLayout;
    aux->addWidget(label("请求预设", "Muted"));
    preset_ = new QComboBox;
    preset_->setObjectName("httpPreset");
    preset_->setMinimumWidth(195);
    aux->addWidget(preset_);
    aux->addStretch();
    aux->addWidget(label("超时", "Muted"));
    httpTimeout_ = spin("httpTimeout", 500, 120000, 5000, " ms");
    httpTimeout_->setFixedWidth(125);
    aux->addWidget(httpTimeout_);
    l->addLayout(aux);
    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(12);
    metrics->addWidget(metricCard("响应状态", httpStatus_, "最近一次请求"));
    metrics->addWidget(metricCard("请求耗时", httpElapsed_, "包含网络传输时间"));
    metrics->addWidget(metricCard("响应大小", httpBytes_, "已接收的正文"));
    l->addLayout(metrics);
    httpNotice_ = label("已就绪 · 选择预设或输入地址，发送你的第一个请求。", "Notice");
    httpNotice_->setWordWrap(true);
    httpNotice_->setObjectName("Notice");
    l->addWidget(httpNotice_);
    auto *split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    auto *left = card();
    left->setMinimumWidth(230);
    auto *ll = vertical(left, 16, 8);
    auto *lh = new QHBoxLayout;
    lh->addWidget(label("请求参数", "Section"));
    lh->addStretch();
    auto *save = button("保存");
    save->setToolTip("导出当前请求配置为 JSON");
    lh->addWidget(save);
    ll->addLayout(lh);
    auto *tabs = new QTabWidget;
    requestHeaders_ = editor("requestHeaders");
    requestHeaders_->setPlaceholderText(
        "每行一个请求头\nAccept: */*\nContent-Type: application/json");
    requestBody_ = editor("requestBody");
    requestBody_->setPlaceholderText(
        "可选请求正文\n\n现有 learningC HTTP 服务主要支持 GET，请求体能力取决于服务端。");
    tabs->addTab(requestHeaders_, "请求头");
    tabs->addTab(requestBody_, "请求体");
    ll->addWidget(tabs, 1);
    auto *hh = new QHBoxLayout;
    hh->addWidget(label("请求历史", "Section"));
    hh->addStretch();
    auto *clear = button("清空");
    hh->addWidget(clear);
    ll->addLayout(hh);
    historyList_ = new QListWidget;
    historyList_->setObjectName("httpHistory");
    historyList_->setMinimumHeight(85);
    historyList_->setMaximumHeight(140);
    historyList_->setToolTip("双击记录重新载入请求，不会自动发送");
    ll->addWidget(historyList_);
    auto *empty = label("最近 50 条 · 双击重新载入", "Small");
    ll->addWidget(empty);
    split->addWidget(left);
    auto *right = card();
    right->setMinimumWidth(350);
    auto *rightLayout = vertical(right, 16, 8);
    auto *head = new QHBoxLayout;
    head->addWidget(label("响应内容", "Section"));
    head->addStretch();
    auto *copy = button("复制");
    copy->setIcon(icon("copy"));
    auto *exportBtn = button("导出");
    exportBtn->setIcon(icon("export"));
    head->addWidget(copy);
    head->addWidget(exportBtn);
    rightLayout->addLayout(head);
    responseInfo_ = label("等待响应", "Small");
    responseInfo_->setWordWrap(true);
    responseInfo_->setMinimumWidth(0);
    rightLayout->addWidget(responseInfo_);
    auto *responseTabs = new QTabWidget;
    responseRaw_ = editor("responseRaw", true);
    responseRaw_->setPlaceholderText(
        "// 响应内容将在这里显示\n\n发送一个请求，查看状态、响应头和正文。");
    responsePretty_ = editor("responsePretty", true);
    responseHeaders_ = editor("responseHeaders", true);
    responseTabs->addTab(responseRaw_, "正文");
    responseTabs->addTab(responsePretty_, "JSON 格式化");
    responseTabs->addTab(responseHeaders_, "响应头");
    rightLayout->addWidget(responseTabs, 1);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 2);
    split->setSizes({290, 640});
    l->addWidget(split, 1);
    connect(clear, &QPushButton::clicked, this, [this] {
        history_.clear();
        historyList_->clear();
    });
    connect(copy, &QPushButton::clicked, this, [this, responseTabs] {
        auto *e = qobject_cast<QPlainTextEdit *>(responseTabs->currentWidget());
        if (e && !e->toPlainText().isEmpty()) {
            QApplication::clipboard()->setText(e->toPlainText());
            showNotice("响应内容已复制到剪贴板。");
        }
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        if (hasResponse_)
            saveBytes(lastResponse_.body, "response.txt", "所有文件 (*)");
        else
            showNotice("请先完成一次请求。", true);
    });
    connect(save, &QPushButton::clicked, this, [this] {
        QJsonObject j{{"method", method_->currentText()},
                      {"url", url_->text()},
                      {"headers", requestHeaders_->toPlainText()},
                      {"body", requestBody_->toPlainText()},
                      {"timeoutMs", httpTimeout_->value()}};
        saveBytes(QJsonDocument(j).toJson(), "request.json", "JSON (*.json)");
    });
    connect(historyList_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) { restoreHistory(historyList_->row(item)); });
    return page;
}

QWidget *MainWindow::makeBenchmarkPage() {
    auto *page = new PageCanvas;
    auto *l = vertical(page, 26, 14);
    l->addWidget(pageHeading("02  /  LOAD LAB", "轻量压测",
                             "用可复现的请求观察服务器表现，每一项指标都来自实际通信。"));
    auto *config = card();
    auto *cl = vertical(config, 16, 12);
    auto *urlRow = new QHBoxLayout;
    urlRow->addWidget(label("GET", "Section"));
    benchUrl_ = line("benchUrl", "http://127.0.0.1:8080/");
    urlRow->addWidget(benchUrl_, 1);
    auto *use = button("使用当前请求");
    urlRow->addWidget(use);
    cl->addLayout(urlRow);
    auto *parameters = new QHBoxLayout;
    benchTotal_ = spin("benchTotal", 1, 10000, 100);
    benchConcurrency_ = spin("benchConcurrency", 1, 32, 4);
    benchTimeout_ = spin("benchTimeout", 1, 120000, 5000, " ms");
    for (auto pair : {qMakePair(QString("请求次数"), benchTotal_),
                      qMakePair(QString("并发数"), benchConcurrency_),
                      qMakePair(QString("单次超时"), benchTimeout_)}) {
        auto *v = new QVBoxLayout;
        v->setSpacing(5);
        v->addWidget(label(pair.first, "Small"));
        v->addWidget(pair.second);
        parameters->addLayout(v);
    }
    parameters->addStretch();
    benchStart_ = button("开始测试  →", "benchStart", true);
    benchStop_ = button("停止", "benchStop");
    benchStop_->setEnabled(false);
    parameters->addWidget(benchStart_, 0, Qt::AlignBottom);
    parameters->addWidget(benchStop_, 0, Qt::AlignBottom);
    cl->addLayout(parameters);
    l->addWidget(config);
    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(12);
    metrics->addWidget(metricCard("已完成 / 计划", benchDone_));
    metrics->addWidget(metricCard("成功率", benchSuccess_));
    metrics->addWidget(metricCard("客户端吞吐", benchRate_, "次 / 秒"));
    metrics->addWidget(metricCard("P95 延迟", benchP95_, "成功请求 · 毫秒"));
    l->addLayout(metrics);
    auto *split = new QHBoxLayout;
    split->setSpacing(14);
    auto *graph = card();
    auto *gl = vertical(graph, 18, 10);
    auto *gh = new QHBoxLayout;
    gh->addWidget(label("吞吐趋势", "Section"));
    gh->addStretch();
    gh->addWidget(label("●  累计平均吞吐", "Muted"));
    gl->addLayout(gh);
    chart_ = new Sparkline;
    chart_->setObjectName("benchmarkChart");
    gl->addWidget(chart_, 1);
    benchExtra_ = label("平均延迟 —    ·    P50 —    ·    有效样本 0", "Small");
    gl->addWidget(benchExtra_);
    split->addWidget(graph, 3);
    auto *distribution = card();
    distribution->setMinimumWidth(215);
    auto *dl = vertical(distribution, 16, 10);
    dl->addWidget(label("结果分布", "Section"));
    benchErrors_ = new QTableWidget(0, 2);
    benchErrors_->setObjectName("benchErrors");
    benchErrors_->setHorizontalHeaderLabels({"分类", "数量"});
    benchErrors_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    benchErrors_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    benchErrors_->verticalHeader()->hide();
    benchErrors_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    benchErrors_->setSelectionMode(QAbstractItemView::NoSelection);
    benchErrors_->setShowGrid(false);
    dl->addWidget(benchErrors_, 1);
    split->addWidget(distribution, 1);
    l->addLayout(split, 1);
    auto *progressCard = card();
    auto *pl = vertical(progressCard, 16, 10);
    auto *pr = new QHBoxLayout;
    benchInfo_ = label("尚未开始 · 独立短连接 / HTTP 1.1", "Muted");
    benchInfo_->setWordWrap(true);
    pr->addWidget(benchInfo_, 1);
    benchExport_ = button("导出 CSV", "benchExport");
    benchExport_->setIcon(icon("export"));
    benchExport_->setEnabled(false);
    pr->addWidget(benchExport_);
    pl->addLayout(pr);
    benchProgress_ = new QProgressBar;
    benchProgress_->setObjectName("benchProgress");
    benchProgress_->setRange(0, 100);
    benchProgress_->setValue(0);
    benchProgress_->setTextVisible(false);
    benchProgress_->setFixedHeight(8);
    pl->addWidget(benchProgress_);
    l->addWidget(progressCard);
    auto *note = label("测试结果反映客户端观测，包含连接与传输开销；请在相同条件下比较。", "Small");
    note->setWordWrap(true);
    l->addWidget(note);
    connect(use, &QPushButton::clicked, this, [this] {
        if (!benchmark_.running())
            benchUrl_->setText(url_->text());
    });
    connect(benchExport_, &QPushButton::clicked, this,
            [this] { saveBytes(benchmark_.csv(), "benchmark.csv", "CSV (*.csv)"); });
    return page;
}

QWidget *MainWindow::makeTcpPage() {
    auto *page = new PageCanvas;
    auto *l = vertical(page, 26, 14);
    l->addWidget(pageHeading("03  /  PROTOCOL LAB", "TCP 协议调试",
                             "把字节变成可读的证据，观察 LCH1 帧的构造与收发。"));
    auto *notice = label(
        "聊天服务支持登录、在线名单与消息转发。连接后请手动发送 Login，并在收帧记录中检查 Welcome 或 Error。",
        "Notice");
    notice->setWordWrap(true);
    l->addWidget(notice);
    auto *connection = card();
    auto *c = new QHBoxLayout(connection);
    c->setContentsMargins(16, 14, 16, 14);
    c->addWidget(label("服务器", "Muted"));
    tcpHost_ = line("tcpHost", "127.0.0.1");
    c->addWidget(tcpHost_, 1);
    c->addWidget(label("端口", "Muted"));
    tcpPort_ = spin("tcpPort", 1, 65535, 12735);
    tcpPort_->setFixedWidth(110);
    c->addWidget(tcpPort_);
    tcpConnect_ = button("连接", "tcpConnect", true);
    tcpDisconnect_ = button("断开", "tcpDisconnect");
    tcpDisconnect_->setEnabled(false);
    c->addWidget(tcpConnect_);
    c->addWidget(tcpDisconnect_);
    tcpState_ = label("未连接", "Pill");
    tcpState_->setObjectName("tcpState");
    tcpState_->setMinimumWidth(95);
    c->addWidget(tcpState_);
    l->addWidget(connection);
    auto *split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    auto *builder = card();
    builder->setMinimumWidth(270);
    auto *bl = vertical(builder, 16, 9);
    bl->addWidget(label("构造协议帧", "Section"));
    tcpType_ = new QComboBox;
    tcpType_->setObjectName("tcpType");
    tcpType_->addItems({"Login · 登录昵称", "Say · 广播消息", "Say · 私聊消息", "Bye · 退出帧"});
    bl->addWidget(tcpType_);
    tcpNick_ = line("tcpNick", "昵称（1–32 UTF-8 字节）");
    tcpNick_->setText("Wwj");
    bl->addWidget(tcpNick_);
    tcpTarget_ = line("tcpTarget", "私聊目标昵称");
    tcpTarget_->setVisible(false);
    bl->addWidget(tcpTarget_);
    tcpPayload_ = editor("tcpPayload");
    tcpPayload_->setPlaceholderText("输入要发送的消息正文…");
    tcpPayload_->setMinimumHeight(65);
    tcpPayload_->setMaximumHeight(150);
    tcpPayload_->setVisible(false);
    bl->addWidget(tcpPayload_);
    bl->addWidget(label("发送方式", "Small"));
    tcpMode_ = new QComboBox;
    tcpMode_->setObjectName("tcpMode");
    tcpMode_->addItems({"完整帧 · 一次写入", "分段发送 · 间隔 100 ms", "连续两帧 · 一次写入"});
    bl->addWidget(tcpMode_);
    bl->addWidget(label("帧预览 / HEX", "Small"));
    tcpPreview_ = editor("tcpPreview", true);
    tcpPreview_->setMinimumHeight(75);
    tcpPreview_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    bl->addWidget(tcpPreview_, 1);
    tcpSend_ = button("发送协议帧  →", "tcpSend", true);
    tcpSend_->setEnabled(false);
    bl->addWidget(tcpSend_);
    split->addWidget(builder);
    auto *viewer = card();
    viewer->setMinimumWidth(350);
    auto *vl = vertical(viewer, 16, 8);
    auto *head = new QHBoxLayout;
    head->addWidget(label("帧收发记录", "Section"));
    head->addStretch();
    auto *clear = button("清空");
    auto *exportBtn = button("导出", "tcpExport");
    head->addWidget(clear);
    head->addWidget(exportBtn);
    vl->addLayout(head);
    frames_ = new QTableWidget(0, 4);
    frames_->setObjectName("tcpFrames");
    frames_->setHorizontalHeaderLabels({"时间", "方向", "类型", "载荷"});
    frames_->verticalHeader()->hide();
    frames_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    frames_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    frames_->setSelectionBehavior(QAbstractItemView::SelectRows);
    frames_->setSelectionMode(QAbstractItemView::SingleSelection);
    frames_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    frames_->setAlternatingRowColors(true);
    frames_->setShowGrid(false);
    vl->addWidget(frames_, 1);
    auto *tabs = new QTabWidget;
    tcpDecoded_ = editor("tcpDecoded", true);
    tcpDecoded_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    tcpDecoded_->setPlaceholderText("选择一条记录，查看长度、类型和载荷。\n\n发出的字节只表示进入发"
                                    "送队列，不代表服务端已处理。");
    tcpRaw_ = editor("tcpRaw", true);
    tabs->addTab(tcpDecoded_, "字段解析");
    tabs->addTab(tcpRaw_, "原始字节");
    vl->addWidget(tabs, 1);
    split->addWidget(viewer);
    split->setSizes({310, 610});
    split->setStretchFactor(1, 2);
    l->addWidget(split, 1);
    tcpNotice_ = label("等待连接 · TCP 分段写入与服务端 recv 次数没有一一对应关系。", "Small");
    tcpNotice_->setWordWrap(true);
    l->addWidget(tcpNotice_);
    connect(clear, &QPushButton::clicked, this, [this] {
        frames_->setRowCount(0);
        frameEvents_.clear();
        tcpRaw_->clear();
        tcpDecoded_->clear();
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        QJsonArray array;
        for (const auto &e : frameEvents_)
            array.append(QJsonObject{{"time", e.timestamp.toString(Qt::ISODateWithMs)},
                                     {"direction", e.direction},
                                     {"type", qint64(e.type)},
                                     {"payloadHex", QString::fromLatin1(e.payload.toHex())},
                                     {"frameHex", QString::fromLatin1(e.raw.toHex())}});
        saveBytes(QJsonDocument(array).toJson(), "frames.json", "JSON (*.json)");
    });
    connect(frames_, &QTableWidget::currentCellChanged, this,
            [this](int row, int, int, int) { showFrame(row); });
    return page;
}

void MainWindow::makeLogDock() {
    logDock_ = new QDockWidget("运行日志", this);
    logDock_->setObjectName("logDock");
    logDock_->setAllowedAreas(Qt::BottomDockWidgetArea);
    auto *content = new QWidget;
    auto *l = vertical(content, 10, 6);
    auto *bar = new QHBoxLayout;
    logFilter_ = new QComboBox;
    logFilter_->addItems({"全部", "HTTP", "压测", "TCP", "系统"});
    bar->addWidget(logFilter_);
    logSearch_ = line("logSearch", "搜索日志");
    bar->addWidget(logSearch_, 1);
    auto *clear = button("清空");
    auto *exportBtn = button("导出日志");
    bar->addWidget(clear);
    bar->addWidget(exportBtn);
    l->addLayout(bar);
    logView_ = editor("logView", true);
    logView_->setMinimumHeight(90);
    logView_->setMaximumBlockCount(1000);
    l->addWidget(logView_);
    logDock_->setWidget(content);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    logDock_->hide();
    connect(logFilter_, &QComboBox::currentTextChanged, this, &MainWindow::filterLogs);
    connect(logSearch_, &QLineEdit::textChanged, this, &MainWindow::filterLogs);
    connect(clear, &QPushButton::clicked, this, [this] {
        logs_.clear();
        filterLogs();
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        saveBytes(logView_->toPlainText().toUtf8(), "studio.log", "日志 (*.log);;文本 (*.txt)");
    });
}

void MainWindow::wireEvents() {
    connect(profile_, &QComboBox::currentIndexChanged, this, [this] { applyProfile(); });
    connect(preset_, &QComboBox::activated, this, [this](int row) {
        const QString path = preset_->itemData(row).toString();
        if (!path.isEmpty()) {
            QUrl target(url_->text());
            if (!target.isValid() || target.host().isEmpty())
                target = QUrl(profiles_[profile_->currentIndex()].base);
            QString prefix = QUrl(profiles_[profile_->currentIndex()].base).path();
            while (prefix.endsWith('/'))
                prefix.chop(1);
            target.setPath(prefix + path);
            target.setQuery(QString());
            target.setFragment(QString());
            url_->setText(target.toString());
            method_->setCurrentText("GET");
        }
    });
    connect(sendHttp_, &QPushButton::clicked, this, &MainWindow::sendHttp);
    connect(cancelHttp_, &QPushButton::clicked, &http_, &HttpClient::cancel);
    connect(&http_, &HttpClient::busyChanged, this, [this](bool busy) {
        sendHttp_->setEnabled(!busy);
        cancelHttp_->setEnabled(busy);
        sendHttp_->setText(busy ? "请求中…" : "发送请求  →");
        profile_->setEnabled(!busy && !benchmark_.running() && !tcpDisconnect_->isEnabled());
    });
    connect(&http_, &HttpClient::error, this, [this](const QString &s) {
        showNotice(s, true);
        addLog("HTTP", s);
    });
    connect(&http_, &HttpClient::finished, this, &MainWindow::displayResponse);
    connect(benchStart_, &QPushButton::clicked, this, &MainWindow::startBenchmark);
    connect(benchStop_, &QPushButton::clicked, &benchmark_, &BenchmarkRunner::stop);
    connect(&benchmark_, &BenchmarkRunner::updated, this, &MainWindow::updateBenchmark);
    connect(&benchmark_, &BenchmarkRunner::finished, this, [this](const BenchmarkSnapshot &s) {
        updateBenchmark(s);
        benchExport_->setEnabled(!benchmark_.csv().isEmpty());
        addLog("压测", QString("测试结束：完成 %1，成功 %2，取消 %3，吞吐 %4 次/秒")
                           .arg(s.completed)
                           .arg(s.succeeded)
                           .arg(s.cancelled)
                           .arg(s.throughput, 0, 'f', 1));
    });
    connect(&benchmark_, &BenchmarkRunner::error, this, [this](const QString &s) {
        benchInfo_->setText(s);
        addLog("压测", s);
    });
    connect(tcpConnect_, &QPushButton::clicked, this,
            [this] { tcp_.connectTo(tcpHost_->text(), quint16(tcpPort_->value()), 5000); });
    connect(tcpDisconnect_, &QPushButton::clicked, &tcp_, &TcpClient::disconnectFrom);
    connect(tcpSend_, &QPushButton::clicked, this, &MainWindow::sendFrame);
    connect(&tcp_, &TcpClient::stateChanged, this, [this](const QString &s, bool connected) {
        tcpState_->setText(s);
        tcpState_->setStyleSheet(connected ? "color:#137c70;font-weight:600;" : "color:#84949e;");
        tcpSend_->setEnabled(connected && !tcp_.busy());
        const bool idle = !connected && s == "未连接";
        tcpConnect_->setEnabled(idle);
        tcpDisconnect_->setEnabled(!idle);
        tcpHost_->setEnabled(idle);
        tcpPort_->setEnabled(idle);
        profile_->setEnabled(idle && !http_.busy() && !benchmark_.running());
        addLog("TCP", s);
    });
    connect(&tcp_, &TcpClient::busyChanged, this, [this](bool busy) {
        tcpSend_->setEnabled(tcp_.connected() && !busy);
        tcpSend_->setText(busy ? "分段发送中…" : "发送协议帧  →");
    });
    connect(&tcp_, &TcpClient::frame, this, &MainWindow::appendFrame);
    connect(&tcp_, &TcpClient::activity, this, [this](const QString &s) {
        tcpNotice_->setText(s);
        addLog("TCP", s);
    });
    connect(&tcp_, &TcpClient::error, this, [this](const QString &s) {
        tcpNotice_->setText(s);
        tcpNotice_->setStyleSheet("color:#b45b41;");
        addLog("TCP", s);
    });
    connect(tcpType_, &QComboBox::currentIndexChanged, this, [this](int index) {
        tcpNick_->setVisible(index == 0);
        tcpTarget_->setVisible(index == 2);
        tcpPayload_->setVisible(index == 1 || index == 2);
        previewFrame();
    });
    connect(tcpNick_, &QLineEdit::textChanged, this, &MainWindow::previewFrame);
    connect(tcpTarget_, &QLineEdit::textChanged, this, &MainWindow::previewFrame);
    connect(tcpPayload_, &QPlainTextEdit::textChanged, this, &MainWindow::previewFrame);
    previewFrame();
}

void MainWindow::showPage(int index) {
    index = qBound(0, index, 2);
    pages_->setCurrentIndex(index);
    for (int i = 0; i < navigation_.size(); ++i)
        navigation_[i]->setChecked(i == index);
}
void MainWindow::loadSettings() {
    profiles_ = {{"单 Reactor", "single", "http://127.0.0.1:8080", "127.0.0.1", 12735, 5000},
                 {"线程池", "pool", "http://127.0.0.1:8080", "127.0.0.1", 12735, 5000},
                 {"主从 Reactor", "multi", "http://127.0.0.1:8080", "127.0.0.1", 12735, 5000}};
    if (isolated_)
        return;
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(path);
    settings_ = new QSettings(path + "/studio.ini", QSettings::IniFormat, this);
    const auto json = QJsonDocument::fromJson(settings_->value("profiles").toByteArray());
    if (!json.isArray() || json.array().isEmpty())
        return;
    QVector<Profile> saved;
    for (const auto &value : json.array()) {
        const auto p = value.toObject();
        const QString base = p["base"].toString();
        const QUrl u(base);
        if (u.host().isEmpty() || (u.scheme() != "http" && u.scheme() != "https") ||
            !u.userInfo().isEmpty())
            continue;
        saved.append({p["name"].toString("自定义服务器"), p["variant"].toString("custom"), base,
                      p["host"].toString("127.0.0.1"), qBound(1, p["port"].toInt(12735), 65535),
                      qBound(500, p["timeout"].toInt(5000), 120000)});
        if (saved.size() == 20)
            break;
    }
    if (!saved.isEmpty())
        profiles_ = saved;
}
void MainWindow::saveSettings() {
    if (!settings_ || !profile_)
        return;
    QJsonArray a;
    for (const auto &p : profiles_)
        a.append(QJsonObject{{"name", p.name},
                             {"variant", p.variant},
                             {"base", p.base},
                             {"host", p.host},
                             {"port", p.port},
                             {"timeout", p.timeout}});
    settings_->setValue("profiles", QJsonDocument(a).toJson(QJsonDocument::Compact));
    settings_->setValue("nickname", tcpNick_->text());
    settings_->sync();
}
void MainWindow::applyProfile() {
    if (profile_->currentIndex() < 0 || profile_->currentIndex() >= profiles_.size())
        return;
    const auto &p = profiles_[profile_->currentIndex()];
    QUrl u(p.base);
    if (u.path().isEmpty())
        u.setPath("/");
    url_->setText(u.toString());
    benchUrl_->setText(u.toString());
    tcpHost_->setText(p.host);
    tcpPort_->setValue(p.port);
    httpTimeout_->setValue(p.timeout);
    benchTimeout_->setValue(p.timeout);
    method_->setCurrentText("GET");
    requestHeaders_->clear();
    requestBody_->clear();
    if (settings_)
        tcpNick_->setText(settings_->value("nickname", "Wwj").toString());
    {
        QSignalBlocker block(preset_);
        preset_->clear();
        preset_->addItem("选择接口预设…", "");
        preset_->addItem("GET /  · 首页", "/");
        if (p.variant == "single")
            preset_->addItem("GET /hello  · Hello", "/hello");
        if (p.variant == "pool")
            preset_->addItem("GET /slow  · 2 秒慢请求", "/slow");
        preset_->addItem(p.variant == "multi" ? "GET /missing  · 当前仍返回 200"
                                              : "GET /missing  · 404 检查",
                         "/missing");
    }
    showNotice(p.variant == "multi"
                   ? "主从 Reactor 预设 · 当前源码对不同路径返回固定 200；版本标签由你选择。"
               : p.variant == "pool"
                   ? "线程池预设 · /slow 包含约 2 秒的慢任务，响应后关闭连接。"
                   : "已选择服务器配置 · 现有项目主要支持 GET，其他方法的行为取决于服务端。");
    saveSettings();
}
void MainWindow::editSettings() {
    if (http_.busy() || benchmark_.running() || tcpDisconnect_->isEnabled()) {
        QMessageBox::information(this, "服务器设置",
                                 "请先结束请求、停止压测并断开 TCP，再修改服务器配置。");
        return;
    }
    const int index = profile_->currentIndex();
    const auto current = profiles_.value(index);
    QDialog dialog(this);
    dialog.setWindowTitle("服务器设置");
    dialog.setMinimumWidth(510);
    auto *l = vertical(&dialog, 24, 14);
    l->addWidget(label("管理服务器配置", "PageTitle"));
    l->addWidget(label("填写 Linux 主机的可访问地址；版本标签用于选择请求预设。", "Muted"));
    auto *form = new QFormLayout;
    form->setSpacing(12);
    auto *name = line("profileName");
    name->setText(current.name);
    auto *variant = new QComboBox;
    variant->addItem("单 Reactor", "single");
    variant->addItem("线程池", "pool");
    variant->addItem("主从 Reactor", "multi");
    variant->addItem("自定义 HTTP 服务", "custom");
    variant->setCurrentIndex(qMax(0, variant->findData(current.variant)));
    auto *base = line("profileBase");
    base->setText(current.base);
    auto *host = line("profileTcpHost");
    host->setText(current.host);
    auto *port = spin("profileTcpPort", 1, 65535, current.port);
    auto *timeout = spin("profileTimeout", 500, 120000, current.timeout, " ms");
    form->addRow("配置名称", name);
    form->addRow("服务端版本", variant);
    form->addRow("HTTP 基础地址", base);
    form->addRow("TCP 主机", host);
    form->addRow("TCP 端口", port);
    form->addRow("默认超时", timeout);
    l->addLayout(form);
    auto *validation = label("", "Notice");
    validation->hide();
    l->addWidget(validation);
    auto *buttons = new QHBoxLayout;
    auto *remove = button("删除配置");
    remove->setEnabled(profiles_.size() > 1);
    auto *reset = button("恢复默认值");
    auto *add = button("另存为新配置");
    auto *save = button("保存", "", true);
    buttons->addWidget(remove);
    buttons->addWidget(reset);
    buttons->addStretch();
    buttons->addWidget(add);
    buttons->addWidget(save);
    l->addLayout(buttons);
    int action = 0;
    auto validate = [&](int mode) {
        QUrl u(base->text().trimmed());
        if (name->text().trimmed().isEmpty() || u.host().isEmpty() ||
            (u.scheme() != "http" && u.scheme() != "https") || !u.userInfo().isEmpty() ||
            u.hasQuery() || u.hasFragment() || host->text().trimmed().isEmpty()) {
            validation->setText(
                "请填写名称、有效的 HTTP(S) 基础地址和 TCP 主机。基础地址不含凭据、查询或片段。");
            validation->show();
            return;
        }
        if (mode == 2 && profiles_.size() >= 20) {
            validation->setText("最多保存 20 个配置。");
            validation->show();
            return;
        }
        action = mode;
        dialog.accept();
    };
    connect(save, &QPushButton::clicked, &dialog, [&] { validate(1); });
    connect(add, &QPushButton::clicked, &dialog, [&] { validate(2); });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        action = 3;
        dialog.accept();
    });
    connect(reset, &QPushButton::clicked, &dialog, [&] {
        base->setText("http://127.0.0.1:8080");
        host->setText("127.0.0.1");
        port->setValue(12735);
        timeout->setValue(5000);
        variant->setCurrentIndex(0);
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    int selected = index;
    if (action == 3) {
        profiles_.removeAt(index);
        selected = 0;
    } else {
        Profile p{name->text().trimmed(), variant->currentData().toString(),
                  base->text().trimmed(), host->text().trimmed(),
                  port->value(),          timeout->value()};
        if (action == 2) {
            profiles_.append(p);
            selected = profiles_.size() - 1;
        } else
            profiles_[index] = p;
    }
    {
        QSignalBlocker b(profile_);
        profile_->clear();
        for (const auto &p : profiles_)
            profile_->addItem(p.name);
        profile_->setCurrentIndex(selected);
    }
    applyProfile();
    addLog("系统", "服务器配置已保存。");
}

void MainWindow::sendHttp() {
    if (http_.busy())
        return;
    RequestSpec spec;
    spec.method = method_->currentText();
    spec.url = QUrl(url_->text().trimmed());
    spec.timeoutMs = httpTimeout_->value();
    spec.body = requestBody_->toPlainText().toUtf8();
    if (spec.body.size() > 1024 * 1024) {
        showNotice("请求正文上限为 1 MiB。", true);
        return;
    }
    for (const QString &s : requestHeaders_->toPlainText().split('\n')) {
        if (s.trimmed().isEmpty())
            continue;
        const auto colon = s.indexOf(':');
        if (colon <= 0) {
            showNotice("请求头格式错误，请使用：名称: 值", true);
            return;
        }
        spec.headers.insert(s.left(colon).trimmed().toUtf8(), s.mid(colon + 1).trimmed().toUtf8());
    }
    if (http_.send(spec)) {
        showNotice("正在等待服务器响应… 可随时取消。");
        addLog("HTTP", QString("→ %1 %2").arg(spec.method, spec.url.toDisplayString()));
    }
}
void MainWindow::displayResponse(const HttpResult &result) {
    lastResponse_ = result;
    hasResponse_ = true;
    const bool complete = result.outcome == HttpResult::Outcome::Success ||
                          result.outcome == HttpResult::Outcome::HttpError;
    httpStatus_->setText(result.status
                             ? QString::number(result.status) + (complete ? QString() : " · 不完整")
                         : result.cancelled ? "已取消"
                                            : "未收到");
    httpStatus_->setStyleSheet(result.successful() ? "color:#137c70;" : "color:#b36b43;");
    httpElapsed_->setText(QString::number(result.elapsed) + " ms");
    httpBytes_->setText(bytesText(result.body.size()));
    responseRaw_->setPlainText(QString::fromUtf8(result.body));
    QJsonParseError jsonError;
    const auto json = QJsonDocument::fromJson(result.body, &jsonError);
    responsePretty_->setPlainText(
        jsonError.error == QJsonParseError::NoError
            ? QString::fromUtf8(json.toJson())
            : QString("当前正文不是有效的 JSON。\n%1\n\n原始内容请查看「正文」标签。")
                  .arg(jsonError.errorString()));
    QString headers;
    for (const auto &h : result.headers)
        headers += QString::fromLatin1(h.first) + ": " + QString::fromLatin1(h.second) + "\n";
    responseHeaders_->setPlainText(headers);
    responseInfo_->setText(QString("%1 %2 · %3")
                               .arg(result.request.method, result.request.url.toDisplayString(),
                                    QTime::currentTime().toString("HH:mm:ss")));
    responseInfo_->setToolTip(responseInfo_->text());
    const auto message =
        result.error.isEmpty()
            ? QString("请求完成 · HTTP %1 · %2 ms").arg(result.status).arg(result.elapsed)
            : result.error;
    showNotice(message, !result.successful());
    addLog(
        "HTTP",
        QString("← %1 · %2 · %3").arg(result.status).arg(bytesText(result.body.size()), message));
    history_.prepend({result.request, result.status, result.elapsed, result.error});
    if (history_.size() > 50)
        history_.removeLast();
    historyList_->clear();
    for (const auto &h : history_) {
        const auto status = h.status ? QString::number(h.status) : QString("失败");
        auto *item = new QListWidgetItem(QString("%1  %2\n%3  ·  %4 ms")
                                             .arg(h.request.method, h.request.url.path(), status)
                                             .arg(h.elapsed),
                                         historyList_);
        item->setToolTip(h.request.url.toDisplayString() + "\n" + h.error);
    }
}
void MainWindow::restoreHistory(int row) {
    if (row < 0 || row >= history_.size() || http_.busy())
        return;
    const auto &r = history_[row].request;
    method_->setCurrentText(r.method);
    url_->setText(r.url.toString());
    httpTimeout_->setValue(r.timeoutMs);
    requestBody_->setPlainText(QString::fromUtf8(r.body));
    QString headers;
    for (auto it = r.headers.cbegin(); it != r.headers.cend(); ++it)
        headers += QString::fromLatin1(it.key()) + ": " + QString::fromUtf8(it.value()) + "\n";
    requestHeaders_->setPlainText(headers);
    showNotice("已载入历史请求，点击「发送请求」重新执行。");
}
void MainWindow::showNotice(const QString &text, bool error) {
    httpNotice_->setText(text);
    httpNotice_->setProperty("error", error);
    httpNotice_->style()->unpolish(httpNotice_);
    httpNotice_->style()->polish(httpNotice_);
}

void MainWindow::startBenchmark() {
    if (benchmark_.running())
        return;
    BenchmarkConfig config;
    config.url = QUrl(benchUrl_->text().trimmed());
    config.total = benchTotal_->value();
    config.concurrency = benchConcurrency_->value();
    config.timeoutMs = benchTimeout_->value();
    config.label = profile_->currentText();
    if (benchmark_.start(config)) {
        chart_->reset();
        BenchmarkSnapshot initial;
        initial.total = config.total;
        initial.running = true;
        updateBenchmark(initial);
        benchExport_->setEnabled(false);
        addLog("压测", QString("启动 %1 · %2 次 / 并发 %3 / 每请求新连接")
                           .arg(config.url.toDisplayString())
                           .arg(config.total)
                           .arg(config.concurrency));
    }
}
void MainWindow::updateBenchmark(const BenchmarkSnapshot &s) {
    benchDone_->setText(QString("%1 / %2").arg(s.completed).arg(s.total));
    const int measured = s.completed - s.cancelled;
    benchSuccess_->setText(measured ? QString::number(100.0 * s.succeeded / measured, 'f', 1) + "%"
                                    : "—");
    benchRate_->setText(QString::number(s.throughput, 'f', 1));
    benchP95_->setText(s.succeeded ? QString::number(s.p95Ms, 'f', 1) : "—");
    benchExtra_->setText(QString("平均延迟 %1 ms    ·    P50 %2 ms    ·    有效样本 %3")
                             .arg(s.averageMs, 0, 'f', 1)
                             .arg(s.p50Ms, 0, 'f', 1)
                             .arg(s.succeeded));
    chart_->append(s.throughput);
    benchStart_->setEnabled(!s.running);
    benchStop_->setEnabled(s.running);
    benchUrl_->setEnabled(!s.running);
    benchTotal_->setEnabled(!s.running);
    benchConcurrency_->setEnabled(!s.running);
    benchTimeout_->setEnabled(!s.running);
    profile_->setEnabled(!s.running && !http_.busy() && !tcpDisconnect_->isEnabled());
    benchProgress_->setRange(0, qMax(1, s.total));
    benchProgress_->setValue(s.completed);
    benchInfo_->setText(QString("%1 · 已发起 %2 / 活动 %3 / 取消 %4 / 未发起 %5 · %6 s")
                            .arg(s.running ? "测试中" : "测试结束")
                            .arg(s.started)
                            .arg(s.active)
                            .arg(s.cancelled)
                            .arg(s.total - s.started)
                            .arg(s.elapsedMs / 1000.0, 0, 'f', 1));
    QList<QPair<QString, int>> rows = {{"成功（完整 2xx）", s.succeeded},
                                       {"HTTP 错误", s.httpErrors},
                                       {"传输错误", s.networkErrors},
                                       {"超时", s.timeouts},
                                       {"已取消", s.cancelled}};
    for (auto it = s.statusCounts.cbegin(); it != s.statusCounts.cend(); ++it)
        rows.append({QString("HTTP %1").arg(it.key()), it.value()});
    benchErrors_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        benchErrors_->setItem(i, 0, new QTableWidgetItem(rows[i].first));
        benchErrors_->setItem(i, 1, new QTableWidgetItem(QString::number(rows[i].second)));
    }
}

QByteArray MainWindow::currentPayload(quint32 &type) const {
    const int index = tcpType_->currentIndex();
    type = index == 0 ? Wire::Login : index == 3 ? Wire::Bye : Wire::Say;
    return index == 0 ? tcpNick_->text().toUtf8()
           : index == 3
               ? QByteArray()
               : Wire::say(index == 2 ? 'D' : 'B', tcpTarget_->text(), tcpPayload_->toPlainText());
}
void MainWindow::previewFrame() {
    quint32 type;
    const auto payload = currentPayload(type);
    const auto bytes = Wire::pack(type, payload);
    tcpPreview_->setPlainText(QString("%1 · payload %2 B · frame %3 B\n\n")
                                  .arg(frameType(type))
                                  .arg(payload.size())
                                  .arg(bytes.size()) +
                              QString::fromLatin1(bytes.left(256).toHex(' ')) +
                              (bytes.size() > 256 ? "\n… 预览仅显示前 256 字节" : ""));
}
void MainWindow::sendFrame() {
    if (!tcp_.connected() || tcp_.busy())
        return;
    quint32 type;
    const auto payload = currentPayload(type);
    QString error;
    if (type == Wire::Login && !Wire::validNick(payload))
        error = "昵称必须为 1–32 个 UTF-8 字节，不能含空白或控制字符。";
    if (type == Wire::Say && (tcpPayload_->toPlainText().trimmed().isEmpty() ||
                              tcpPayload_->toPlainText().contains(QChar(0))))
        error = "消息正文不能为空或包含 NUL。";
    if (tcpType_->currentIndex() == 2 && !Wire::validNick(tcpTarget_->text().toUtf8()))
        error = "请输入有效的私聊目标昵称。";
    if (payload.size() > Wire::MaxPayload)
        error = "协议载荷超过 64 KiB。";
    if (!error.isEmpty()) {
        tcpNotice_->setText(error);
        addLog("TCP", error);
        return;
    }
    tcpNotice_->setStyleSheet("");
    tcp_.sendFrame(type, payload, static_cast<TransmissionMode>(tcpMode_->currentIndex()));
}
void MainWindow::appendFrame(const FrameEvent &e) {
    if (frameEvents_.size() >= 500) {
        frameEvents_.removeFirst();
        frames_->removeRow(0);
    }
    frameEvents_.append(e);
    const int row = frames_->rowCount();
    frames_->insertRow(row);
    const QString direction = e.direction == "RX" ? "接收" : "发送队列";
    const QStringList values = {e.timestamp.toString("HH:mm:ss.zzz"), direction, frameType(e.type),
                                bytesText(e.payload.size())};
    for (int i = 0; i < values.size(); ++i)
        frames_->setItem(row, i, new QTableWidgetItem(values[i]));
    frames_->selectRow(row);
    frames_->scrollToBottom();
    addLog("TCP",
           QString("%1 %2 · %3 字节").arg(direction, frameType(e.type)).arg(e.payload.size()));
}
void MainWindow::showFrame(int row) {
    if (row < 0 || row >= frameEvents_.size())
        return;
    const auto &e = frameEvents_[row];
    tcpRaw_->setPlainText(hexDump(e.raw));
    QString text = QString("方向       %1\n时间       %2\n长度字段   %3（不含长度字段自身）\n魔数  "
                           "     0x4C434831 / LCH1\n类型       %4 (%5)\n载荷       %6 字节\n\n")
                       .arg(e.direction, e.timestamp.toString(Qt::ISODateWithMs))
                       .arg(e.payload.size() + 8)
                       .arg(frameType(e.type))
                       .arg(e.type)
                       .arg(e.payload.size());
    if (e.type == Wire::Say && !e.payload.isEmpty()) {
        text += QString("模式       %1\n").arg(e.payload[0] == 'D' ? "私聊 D" : "广播 B");
        const auto pos = e.payload.indexOf('\0', 1);
        if (e.payload[0] == 'D' && pos >= 1)
            text += "目标       " + QString::fromUtf8(e.payload.mid(1, pos - 1)) + "\n正文       " +
                    QString::fromUtf8(e.payload.mid(pos + 1));
        else
            text += "正文       " + QString::fromUtf8(e.payload.mid(1));
    } else if (e.type == Wire::Chat) {
        char mode;
        QString sender, message;
        if (Wire::parseMessage(e.payload, mode, sender, message))
            text += QString("模式       %1\n发送者     %2\n正文       %3")
                        .arg(mode == 'D' ? "私聊 D" : "广播 B", sender, message);
        else
            text += "Chat 载荷无法按模式、发送者和正文解析，请查看原始字节。";
    } else if (e.type == Wire::Login || e.type == Wire::Welcome)
        text += "昵称       " + QString::fromUtf8(e.payload);
    else if (e.type == Wire::Users)
        text += "用户列表\n" + QString::fromUtf8(e.payload);
    else if (e.type == Wire::Bye && e.payload.isEmpty())
        text += "空载荷";
    else
        text += "UTF-8 载荷\n" + QString::fromUtf8(e.payload);
    if (e.direction != "RX")
        text += "\n\n此记录表示完整帧已进入 Qt 发送队列，不表示服务端已处理。";
    tcpDecoded_->setPlainText(text);
}
void MainWindow::addLog(const QString &category, const QString &message) {
    QString safe = message;
    safe.replace('\r', " ");
    safe.replace('\n', " | ");
    logs_.append({category, QString("[%1] [%2] %3").arg(stamp(), category, safe)});
    if (logs_.size() > 1000)
        logs_.removeFirst();
    filterLogs();
}
void MainWindow::filterLogs() {
    if (!logView_)
        return;
    QStringList lines;
    for (const auto &entry : logs_)
        if ((logFilter_->currentText() == "全部" || logFilter_->currentText() == entry.category) &&
            (logSearch_->text().isEmpty() ||
             entry.text.contains(logSearch_->text(), Qt::CaseInsensitive)))
            lines += entry.text;
    logView_->setPlainText(lines.join('\n'));
    logView_->verticalScrollBar()->setValue(logView_->verticalScrollBar()->maximum());
}
bool MainWindow::saveBytes(const QByteArray &bytes, const QString &suggested,
                           const QString &filter) {
    const QString path =
        QFileDialog::getSaveFileName(this, "导出文件", QDir::homePath() + "/" + suggested, filter);
    if (path.isEmpty())
        return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        QMessageBox::warning(this, "导出失败", f.errorString());
        return false;
    }
    addLog("系统", "已导出文件：" + path);
    return true;
}
