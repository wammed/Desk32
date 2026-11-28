#pragma once

#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace desk32::core {

/// Unified XDG-compliant application settings manager ($XDG_CONFIG_HOME/desk32/settings.json).
/// Provides typed Q_PROPERTY accessors for QML settings panels as well as extensible
/// dotted-key / section storage for future backends (JACK, ALSA) and DSP tuning parameters.
class SettingsManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString configFilePath READ configFilePath NOTIFY configFilePathChanged)
    Q_PROPERTY(bool loaded READ isLoaded NOTIFY settingsChanged)

    // General / UI Settings
    Q_PROPERTY(QString languagePreference READ languagePreference WRITE setLanguagePreference NOTIFY settingsChanged)
    Q_PROPERTY(bool fullscreenOnStartup READ fullscreenOnStartup WRITE setFullscreenOnStartup NOTIFY settingsChanged)
    Q_PROPERTY(bool restoreSessionOnStartup READ restoreSessionOnStartup WRITE setRestoreSessionOnStartup NOTIFY settingsChanged)

    // Audio Output Settings (PipeWire / future JACK / ALSA)
    Q_PROPERTY(QString audioBackend READ audioBackend WRITE setAudioBackend NOTIFY settingsChanged)
    Q_PROPERTY(QString audioTargetDevice READ audioTargetDevice WRITE setAudioTargetDevice NOTIFY settingsChanged)
    Q_PROPERTY(QString audioPcmFormat READ audioPcmFormat WRITE setAudioPcmFormat NOTIFY settingsChanged)
    Q_PROPERTY(int audioBufferFrames READ audioBufferFrames WRITE setAudioBufferFrames NOTIFY settingsChanged)
    Q_PROPERTY(double audioFixedSampleRate READ audioFixedSampleRate WRITE setAudioFixedSampleRate NOTIFY settingsChanged)
    Q_PROPERTY(bool directModeDefault READ directModeDefault WRITE setDirectModeDefault NOTIFY settingsChanged)
    Q_PROPERTY(bool liveDitherEnabled READ liveDitherEnabled WRITE setLiveDitherEnabled NOTIFY settingsChanged)

    // DSP / External Insert Defaults
    Q_PROPERTY(double reverbMaxTailSeconds READ reverbMaxTailSeconds WRITE setReverbMaxTailSeconds NOTIFY settingsChanged)
    Q_PROPERTY(double reverbMaxProcessingMs READ reverbMaxProcessingMs WRITE setReverbMaxProcessingMs NOTIFY settingsChanged)
    Q_PROPERTY(QString listeningPreset READ listeningPreset WRITE setListeningPreset NOTIFY settingsChanged)
    Q_PROPERTY(int externalInsertPosition READ externalInsertPosition WRITE setExternalInsertPosition NOTIFY settingsChanged)
    Q_PROPERTY(int externalInsertDisconnectPolicy READ externalInsertDisconnectPolicy WRITE setExternalInsertDisconnectPolicy NOTIFY settingsChanged)

    // Console Controls Persistence (Master Fader L/R, Balance, Link L/R, Deck A-D Trim)
    Q_PROPERTY(double masterGainL READ masterGainL WRITE setMasterGainL NOTIFY settingsChanged)
    Q_PROPERTY(double masterGainR READ masterGainR WRITE setMasterGainR NOTIFY settingsChanged)
    Q_PROPERTY(double masterBalance READ masterBalance WRITE setMasterBalance NOTIFY settingsChanged)
    Q_PROPERTY(bool masterLinkLR READ masterLinkLR WRITE setMasterLinkLR NOTIFY settingsChanged)

public:
    static SettingsManager& instance();

    explicit SettingsManager(QObject* parent = nullptr);
    ~SettingsManager() override = default;

    /// Returns default XDG config path ($XDG_CONFIG_HOME/desk32/settings.json) or test override.
    [[nodiscard]] Q_INVOKABLE QString configFilePath() const;

    /// Testing isolation hooks (must be called before load() in unit tests).
    void setConfigFilePathForTesting(const QString& path);
    void resetForTesting();

    [[nodiscard]] bool isLoaded() const noexcept;
    [[nodiscard]] QString lastError() const;

    /// Load settings.json (creating default file on disk if absent, and merging missing keys).
    Q_INVOKABLE bool load();

    /// Atomically persist current settings to disk via QSaveFile.
    Q_INVOKABLE bool save() const;

    /// Reset all settings in memory to factory defaults (optionally persisting immediately).
    Q_INVOKABLE void resetToDefaults(bool autoSave = true);

    /// Extensible dotted-key access (e.g. "audio.jack.auto_connect", "audio.alsa.period_size").
    Q_INVOKABLE QVariant value(const QString& dottedKey,
                               const QVariant& defaultValue = QVariant()) const;
    Q_INVOKABLE void setValue(const QString& dottedKey,
                              const QVariant& val,
                              bool autoSave = true);

    /// Section-level read/write (e.g. "general", "audio", "dsp", "external_insert", "render").
    Q_INVOKABLE QVariantMap section(const QString& sectionName) const;
    Q_INVOKABLE void setSection(const QString& sectionName,
                                const QVariantMap& map,
                                bool autoSave = true);

    // Typed Getters
    [[nodiscard]] QString languagePreference() const;
    [[nodiscard]] bool fullscreenOnStartup() const;
    [[nodiscard]] bool restoreSessionOnStartup() const;

    [[nodiscard]] QString audioBackend() const;
    [[nodiscard]] QString audioTargetDevice() const;
    [[nodiscard]] QString audioPcmFormat() const;
    [[nodiscard]] int audioBufferFrames() const;
    [[nodiscard]] double audioFixedSampleRate() const;
    [[nodiscard]] bool directModeDefault() const;
    [[nodiscard]] bool liveDitherEnabled() const;

    [[nodiscard]] double reverbMaxTailSeconds() const;
    [[nodiscard]] double reverbMaxProcessingMs() const;
    [[nodiscard]] QString listeningPreset() const;
    [[nodiscard]] int externalInsertPosition() const;
    [[nodiscard]] int externalInsertDisconnectPolicy() const;

    [[nodiscard]] double masterGainL() const;
    [[nodiscard]] double masterGainR() const;
    [[nodiscard]] double masterBalance() const;
    [[nodiscard]] bool masterLinkLR() const;
    [[nodiscard]] double deckTrimDb(char deckId) const;

    // Typed Setters (auto-save when loaded)
    void setLanguagePreference(const QString& lang);
    void setFullscreenOnStartup(bool fullscreen);
    void setRestoreSessionOnStartup(bool restore);

    void setAudioBackend(const QString& backend);
    void setAudioTargetDevice(const QString& deviceIdOrName);
    void setAudioPcmFormat(const QString& formatName);
    void setAudioBufferFrames(int frames);
    void setAudioFixedSampleRate(double sampleRate);
    void setDirectModeDefault(bool enabled);
    void setLiveDitherEnabled(bool enabled);

    void setReverbMaxTailSeconds(double seconds);
    void setReverbMaxProcessingMs(double ms);
    void setListeningPreset(const QString& presetName);
    void setExternalInsertPosition(int positionIndex);
    void setExternalInsertDisconnectPolicy(int policy);

    void setMasterGainL(double gain, bool autoSave = true);
    void setMasterGainR(double gain, bool autoSave = true);
    void setMasterBalance(double balance, bool autoSave = true);
    void setMasterLinkLR(bool linked, bool autoSave = true);
    void setDeckTrimDb(char deckId, double db, bool autoSave = true);

signals:
    void configFilePathChanged();
    void settingsChanged();
    void settingChanged(const QString& dottedKey);

private:
    static QJsonObject buildDefaultRoot();
    static void mergeMissingDefaults(QJsonObject& target, const QJsonObject& defaults);
    static QVariant readDotted(const QJsonObject& obj, const QStringList& parts);
    static void writeDotted(QJsonObject& obj, const QStringList& parts, const QJsonValue& val);

    [[nodiscard]] QString resolveConfigPathLocked() const;
    bool saveLocked() const;
    void setDottedAndPersist(const QString& dottedKey, const QJsonValue& val);

    mutable QMutex m_mutex;
    QJsonObject m_root;
    QString m_testConfigPath;
    QString m_lastError;
    bool m_loaded{false};
};

} // namespace desk32::core
