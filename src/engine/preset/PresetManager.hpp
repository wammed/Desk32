#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#include <mutex>

namespace desk32::engine::preset {

/// Category identifiers per Specification Section 11.1
namespace PresetCategories {
    inline constexpr const char* ListeningCorrection = "listening_correction";
    inline constexpr const char* MasterDsp = "master_dsp";
    inline constexpr const char* ExternalInsert = "external_insert";
    inline constexpr const char* AudioDevice = "audio_device";
}

/// Common Preset & Profile Management Foundation (Specification Section 11.1).
/// Features:
/// - XDG Base Directory compliant ($XDG_CONFIG_HOME/desk32/presets/)
/// - Category isolation (prevents mixing listening correction and export/mastering profiles)
/// - Atomic, non-destructive JSON persistence
/// - Built-in factory presets for listening correction, master DSP, and external insert
/// - Sanitized preset naming preventing path traversal
class PresetManager : public QObject {
    Q_OBJECT

public:
    static PresetManager& instance();

    explicit PresetManager(QObject* parent = nullptr);
    ~PresetManager() override = default;

    /// Override the preset storage root directory (primarily for testing and custom paths)
    void setCustomRootPath(const QString& customPath);
    [[nodiscard]] QString presetRootPath() const;

    /// Path to a category directory
    [[nodiscard]] QString categoryDirectory(const QString& category) const;

    /// List all preset names in a category (combines factory presets and user saved files)
    [[nodiscard]] QStringList listPresets(const QString& category) const;

    /// Check if a preset is a read-only factory preset
    [[nodiscard]] bool isFactoryPreset(const QString& category, const QString& presetName) const;

    /// Save a preset to JSON (atomic write with .tmp rename)
    bool savePreset(const QString& category, const QString& presetName, const QJsonObject& data, bool overwrite = true);

    /// Load a preset from JSON
    [[nodiscard]] QJsonObject loadPreset(const QString& category, const QString& presetName) const;

    /// Delete a user-saved preset file (factory presets cannot be deleted)
    bool deletePreset(const QString& category, const QString& presetName);

    /// Get factory preset definition
    [[nodiscard]] QJsonObject getFactoryPreset(const QString& category, const QString& presetName) const;

signals:
    void presetSaved(const QString& category, const QString& presetName);
    void presetDeleted(const QString& category, const QString& presetName);

private:
    [[nodiscard]] QString sanitizeName(const QString& name) const;
    void ensureCategoryDirectories() const;

    mutable std::mutex m_mutex;
    QString m_customRootPath;
};

} // namespace desk32::engine::preset
