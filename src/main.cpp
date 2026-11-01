#include "i18n.h"
#include "mainwindow.h"
#include "recent.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>
#include <QTranslator>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("pentimento");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setOrganizationName("pentimento");
    // Translation mechanism (P-107): install the translator for the saved
    // language preference; switching language takes effect after restart.
    QTranslator translator;
    if (i18n::loadTranslator(translator, i18n::languagePreference()))
        app.installTranslator(&translator);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("pentimento · 带版本历史的本地美术资源管理器"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"library", "Library directory", "path"});
    parser.addOption({"screenshot", "Save a UI screenshot and exit (smoke check)", "path"});
    parser.process(app);
    // P-202: no explicit --library falls back to the most recently used one if
    // it still exists; otherwise the default per-user location.
    QString root;
    if (parser.isSet("library")) {
        root = parser.value("library");
    } else {
        const auto recentLibs = recent::libraries();
        for (const auto &entry : recentLibs)
            if (QFileInfo::exists(entry.path)) { root = entry.path; break; }
        if (root.isEmpty())
            root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/library";
    }
    try {
        Library library(root);
        recent::remember(library.libraryId(), library.root());
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
