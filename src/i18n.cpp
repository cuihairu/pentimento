#include "i18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace i18n {

QString languagePreference() {
    QSettings settings(QStringLiteral("pentimento"), QStringLiteral("pentimento"));
    const QString lang = settings.value(QStringLiteral("ui/language"), QStringLiteral("zh_CN")).toString();
    return lang == QLatin1String("en") ? QStringLiteral("en") : QStringLiteral("zh_CN");
}

void setLanguagePreference(const QString &language) {
    QSettings settings(QStringLiteral("pentimento"), QStringLiteral("pentimento"));
    settings.setValue(QStringLiteral("ui/language"),
                      language == QLatin1String("en") ? QStringLiteral("en") : QStringLiteral("zh_CN"));
    settings.sync();
}

bool loadTranslator(QTranslator &translator, const QString &language, const QString &directory) {
    // Resolve the translation directory: explicit directory, else next to the
    // application binary (dev build copies .qm there), else the source tree.
    const QString file = QStringLiteral("pentimento_%1").arg(language);
    QStringList candidates;
    if (!directory.isEmpty()) candidates << directory;
    if (QCoreApplication::instance())
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
    candidates << QStringLiteral(PENTIMENTO_SOURCE_TRANSLATIONS_DIR);
    for (const QString &base : candidates) {
        const QString path = base + QLatin1Char('/') + file + QStringLiteral(".qm");
        if (QFileInfo::exists(path) && translator.load(path)) return true;
    }
    return false;
}

}
