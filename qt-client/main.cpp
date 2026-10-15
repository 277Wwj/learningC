#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("LearningC Studio");
    app.setOrganizationName("learningC");
    app.setApplicationVersion("1.0.2");
    app.setStyle("Fusion");
    app.setFont(QFont("Microsoft YaHei UI", 9));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("learningC 服务器调试工作台"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"screenshot", "Save an isolated window screenshot and exit.", "file"});
    parser.addOption({"page", "Initial page: http, benchmark or tcp.", "page", "http"});
    parser.addOption(
        {"isolated", "Use temporary defaults without reading or saving user settings."});
    parser.process(app);
    MainWindow window(parser.isSet("isolated") || parser.isSet("screenshot"));
    const QString page = parser.value("page");
    window.showPage(page == "benchmark" ? 1 : page == "tcp" ? 2 : 0);
    window.show();
    if (parser.isSet("screenshot"))
        QTimer::singleShot(500, &window, [&] {
            const auto path = parser.value("screenshot");
            QDir().mkpath(QFileInfo(path).absolutePath());
            app.exit(window.grab().save(path) ? 0 : 2);
        });
    return app.exec();
}
