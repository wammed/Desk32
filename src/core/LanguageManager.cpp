#include "LanguageManager.hpp"
#include "SettingsManager.hpp"
#include <QLocale>
#include <QDebug>

LanguageManager& LanguageManager::instance()
{
    static LanguageManager s_instance;
    return s_instance;
}

LanguageManager::LanguageManager(QObject* parent)
    : QObject(parent)
{
}

void LanguageManager::init(QGuiApplication* app, QQmlEngine* engine)
{
    m_app = app;
    m_engine = engine;

    const QString pref = desk32::core::SettingsManager::instance().languagePreference();
    if (pref == QStringLiteral("ja") || pref == QStringLiteral("en")) {
        applyLanguageInternal(pref, false);
        return;
    }

    // Detect system locale when set to "auto"
    QLocale sysLocale;
    if (sysLocale.language() == QLocale::Japanese) {
        applyLanguageInternal(QStringLiteral("ja"), false);
    } else {
        applyLanguageInternal(QStringLiteral("en"), false);
    }
}

QString LanguageManager::currentLanguage() const
{
    return m_currentLanguage;
}

void LanguageManager::setLanguage(const QString& langCode)
{
    applyLanguageInternal(langCode, true);
}

void LanguageManager::applyLanguageInternal(const QString& langCode, bool persist)
{
    if (!m_app) return;

    if (m_translator.isEmpty() == false) {
        m_app->removeTranslator(&m_translator);
    }

    const QString qmFile = QString("desk32_%1").arg(langCode);
    if (m_translator.load(qmFile, ":/i18n")) {
        m_app->installTranslator(&m_translator);
    } else if (m_translator.load(qmFile, "i18n")) {
        m_app->installTranslator(&m_translator);
    }

    m_currentLanguage = langCode;
    if (persist && desk32::core::SettingsManager::instance().isLoaded()) {
        desk32::core::SettingsManager::instance().setLanguagePreference(langCode);
    }
    if (m_engine) {
        m_engine->retranslate();
    }
    emit languageChanged();
}

