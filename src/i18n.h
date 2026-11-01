#pragma once

#include <QTranslator>

// Chinese/English translation mechanism (P-107). Source strings are Chinese;
// English is the translated language. The first batch covers key flows and
// errors (library errors, toolbar/menu actions, dangerous-operation prompts);
// coverage grows as strings are added. Switching language takes effect after an
// application restart (recommended in 30-foundation.md#P-107); the UI shows
// "重启后生效" when the preference changes.
namespace i18n {
// "zh_CN" (default) or "en"; persisted under QSettings("pentimento", "pentimento").
QString languagePreference();
void setLanguagePreference(const QString &language);
// Load the .qm for the current preference into `translator` (parent-less).
// Returns true when a translation file was found and installed by the caller.
bool loadTranslator(QTranslator &translator, const QString &language, const QString &directory = QString());
}
