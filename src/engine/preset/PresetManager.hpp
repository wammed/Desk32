#pragma once

#include "DspPresetTypes.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QStandardPaths>
#include <mutex>
#include <vector>

namespace desk32::engine::preset {

/// Category identifiers per Specification Section 11.1
namespace PresetCategories {
    inline constexpr const char* ListeningCorrection = "listening_correction";
    inline constexpr const char* MasterDsp = "master_dsp";
    inline constexpr const char* ExternalInsert = "external_insert";
    inline constexpr const char* AudioDevice = "audio_device";
}

struct DspPresetSummary {
    QString id;
    QString name;
    DspPresetKind kind = DspPresetKind::Global;
    DspPresetScope scope = DspPresetScope::Chain;
    DspPresetTarget target = DspPresetTarget::Peq;
    bool isFactory = false;
    QString createdAt;
    QString modifiedAt;
};

/// Common Preset & Profile Management Foundation (Specification Section 11.1).
/// Features:
/// - XDG Base Directory compliant ($XDG_CONFIG_HOME/desk32/presets/)
/// - Category isolation (prevents mixing listening correction and export/mastering profiles)
/// - Atomic, non-destructive JSON persistence
/// - Built-in factory presets for listening correction, master DSP (v3.8.3 48-preset suite), and external insert
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

    /// Path to a category directory (legacy & non-DSP categories)
    [[nodiscard]] QString categoryDirectory(const QString& category) const;

    /// Path to v3.8.3 typed DSP preset directory (dsp/global or dsp/<target>)
    [[nodiscard]] QString dspPresetDirectory(DspPresetKind kind,
                                             DspPresetTarget target = DspPresetTarget::Peq) const;

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

    // -------------------------------------------------------------------------
    // Spec v3.8.3 §11.1 / Appendix A: Typed Global & Per-DSP Preset API
    // -------------------------------------------------------------------------
    [[nodiscard]] std::vector<DspPresetSummary> listDspPresets(
        DspPresetKind kind,
        DspPresetTarget target = DspPresetTarget::Peq) const;

    [[nodiscard]] bool hasUserDspPresetNamed(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& presetName) const;

    [[nodiscard]] bool isBuiltInDspPreset(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& idOrName) const;

    [[nodiscard]] bool loadDspPreset(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& idOrName,
        DspPresetDocument& outDoc,
        PresetValidationReport& outReport) const;

    [[nodiscard]] bool loadDspPresetFromFile(
        const QString& filePath,
        DspPresetDocument& outDoc,
        PresetValidationReport& outReport) const;

    /// Save a user Global or Per-DSP preset atomically. Refuses to overwrite built-ins;
    /// requires overwrite=true when a user preset with the same name already exists.
    bool saveUserDspPreset(
        DspPresetDocument& doc,
        bool overwrite = false,
        QString* outError = nullptr);

    /// Rename a user preset while preserving its immutable `id` (§11.1).
    bool renameUserDspPreset(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& id,
        const QString& newName,
        bool overwrite = false,
        QString* outError = nullptr);

    /// Delete a user preset by id or name (never deletes built-in presets).
    bool deleteUserDspPreset(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& idOrName,
        QString* outError = nullptr);

signals:
    void presetSaved(const QString& category, const QString& presetName);
    void presetDeleted(const QString& category, const QString& presetName);
    void dspPresetListChanged();

private:
    [[nodiscard]] QString sanitizeName(const QString& name) const;
    void ensureCategoryDirectories() const;
    [[nodiscard]] QString findUserDspPresetFileLocked(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& idOrName) const;

    mutable std::mutex m_mutex;
    QString m_customRootPath;
};

} // namespace desk32::engine::preset

