#include "mainwindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("pentimento");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("pentimento");
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("pentimento · 带版本历史的本地美术资源管理器"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"library", "Library directory", "path"});
    parser.addOption({"screenshot", "Save a UI screenshot and exit (smoke check)", "path"});
    parser.process(app);
    const QString root = parser.isSet("library") ? parser.value("library")
        : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/library";
    try {
        Library library(root);
        MainWindow window(library);
        window.show();
        if (parser.isSet("screenshot")) QTimer::singleShot(400, &window, [&] {
            window.saveScreenshot(parser.value("screenshot")); app.quit();
        });
        return app.exec();
    } catch (const std::exception &error) {
        QMessageBox::critical(nullptr, QStringLiteral("无法打开素材库"), QString::fromUtf8(error.what()));
        return 1;
    }
}
