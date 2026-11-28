#pragma once

#include <QObject>
#include <QTranslator>
#include <QGuiApplication>
#include <QQmlEngine>

class LanguageManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentLanguage READ currentLanguage NOTIFY languageChanged)

public:
    static LanguageManager& instance();

    QString currentLanguage() const;

    Q_INVOKABLE void setLanguage(const QString& langCode);
    void init(QGuiApplication* app, QQmlEngine* engine);

signals:
    void languageChanged();

private:
    explicit LanguageManager(QObject* parent = nullptr);
    ~LanguageManager() override = default;
    void applyLanguageInternal(const QString& langCode, bool persist);

    QGuiApplication* m_app = nullptr;
    QQmlEngine* m_engine = nullptr;
    QTranslator m_translator;
    bool m_translatorInstalled = false;
    QString m_currentLanguage = "en";
};

