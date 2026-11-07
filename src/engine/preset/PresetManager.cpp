#include "PresetManager.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QSaveFile>
#include <QDebug>

namespace desk32::engine::preset {

PresetManager& PresetManager::instance() {
    static PresetManager s_instance;
    return s_instance;
}

PresetManager::PresetManager(QObject* parent)
    : QObject(parent)
{
    ensureCategoryDirectories();
}

void PresetManager::setCustomRootPath(const QString& customPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_customRootPath = customPath;
    ensureCategoryDirectories();
}

QString PresetManager::presetRootPath() const {
    if (!m_customRootPath.isEmpty()) {
        return m_customRootPath;
    }
    QString configLocation = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (configLocation.isEmpty()) {
        configLocation = QDir::homePath() + "/.config";
    }
    return configLocation + "/desk32/presets";
}

QString PresetManager::categoryDirectory(const QString& category) const {
    return presetRootPath() + "/" + sanitizeName(category);
}

void PresetManager::ensureCategoryDirectories() const {
    QString root = presetRootPath();
    QDir().mkpath(root + "/" + PresetCategories::ListeningCorrection);
    QDir().mkpath(root + "/" + PresetCategories::MasterDsp);
    QDir().mkpath(root + "/" + PresetCategories::ExternalInsert);
    QDir().mkpath(root + "/" + PresetCategories::AudioDevice);
}

QString PresetManager::sanitizeName(const QString& name) const {
    QString clean = name;
    clean.replace("/", "_");
    clean.replace("\\", "_");
    clean.replace("..", "_");
    clean.replace(":", "_");
    clean = clean.trimmed();
    return clean.isEmpty() ? "unnamed" : clean;
}

bool PresetManager::isFactoryPreset(const QString& category, const QString& presetName) const {
    if (category == PresetCategories::ListeningCorrection) {
        return presetName == "Monitor Flat" ||
               presetName == "Acoustic Clarity" ||
               presetName == "Harman Target" ||
               presetName == "Bass Punch" ||
               presetName == "Vocal Air";
    }
    if (category == PresetCategories::MasterDsp) {
        return presetName == "Default Bypass" ||
               presetName == "Transparent Master" ||
               presetName == "Warm Tape";
    }
    if (category == PresetCategories::ExternalInsert) {
        return presetName == "Carla Default" ||
               presetName == "LSP Plugins Rack";
    }
    if (category == PresetCategories::AudioDevice) {
        return presetName == "Default PipeWire" ||
               presetName == "Direct ALSA 192k";
    }
    return false;
}

QJsonObject PresetManager::getFactoryPreset(const QString& category, const QString& presetName) const {
    QJsonObject root;
    root["category"] = category;
    root["name"] = presetName;
    root["version"] = 1;
    root["is_factory"] = true;
    root["created_at"] = "2026-11-07T00:00:00Z";

    if (category == PresetCategories::ListeningCorrection) {
        QJsonArray bands;
        auto addBand = [&](int idx, double freq, double gain, double q, int type, bool enabled) {
            QJsonObject b;
            b["index"] = idx;
            b["frequency"] = freq;
            b["gain_db"] = gain;
            b["q"] = q;
            b["type"] = type;
            b["enabled"] = enabled;
            bands.append(b);
        };

        if (presetName == "Monitor Flat") {
            for (int i = 0; i < 12; ++i) addBand(i, 1000.0, 0.0, 1.0, 0, false);
        } else if (presetName == "Acoustic Clarity") {
            addBand(0, 250.0, -1.5, 1.4, 0, true);
            addBand(1, 3500.0, 1.8, 1.2, 0, true);
            addBand(2, 10000.0, 2.0, 0.7, 2, true); // High shelf
            for (int i = 3; i < 12; ++i) addBand(i, 1000.0, 0.0, 1.0, 0, false);
        } else if (presetName == "Harman Target") {
            addBand(0, 105.0, 4.5, 0.7, 1, true); // Low shelf
            addBand(1, 3000.0, 3.0, 1.4, 0, true);
            addBand(2, 8000.0, -1.0, 2.0, 0, true);
            for (int i = 3; i < 12; ++i) addBand(i, 1000.0, 0.0, 1.0, 0, false);
        } else if (presetName == "Bass Punch") {
            addBand(0, 60.0, 3.5, 1.8, 0, true);
            addBand(1, 120.0, -1.0, 2.0, 0, true);
            for (int i = 2; i < 12; ++i) addBand(i, 1000.0, 0.0, 1.0, 0, false);
        } else if (presetName == "Vocal Air") {
            addBand(0, 12000.0, 2.5, 0.7, 2, true); // High shelf
            for (int i = 1; i < 12; ++i) addBand(i, 1000.0, 0.0, 1.0, 0, false);
        }
        root["bands"] = bands;
        return root;
    }

    if (category == PresetCategories::MasterDsp) {
        if (presetName == "Transparent Master") {
            QJsonObject comp;
            comp["enabled"] = true;
            comp["threshold_db"] = -6.0;
            comp["ratio"] = 1.5;
            comp["attack_ms"] = 30.0;
            comp["release_ms"] = 150.0;
            comp["makeup_db"] = 0.5;
            comp["knee_db"] = 3.0;
            root["compressor"] = comp;

            QJsonObject limiter;
            limiter["enabled"] = true;
            limiter["ceiling_db"] = -0.3;
            limiter["release_ms"] = 50.0;
            limiter["lookahead_ms"] = 0.5;
            root["limiter"] = limiter;
        } else if (presetName == "Warm Tape") {
            QJsonObject comp;
            comp["enabled"] = true;
            comp["threshold_db"] = -10.0;
            comp["ratio"] = 2.5;
            comp["attack_ms"] = 15.0;
            comp["release_ms"] = 80.0;
            comp["makeup_db"] = 2.0;
            comp["knee_db"] = 6.0;
            root["compressor"] = comp;

            QJsonObject filter;
            filter["lpf_enabled"] = true;
            filter["lpf_frequency"] = 18000.0;
            filter["lpf_slope"] = 1; // 24dB/oct
            root["filter"] = filter;
        }
        return root;
    }

    if (category == PresetCategories::ExternalInsert) {
        if (presetName == "Carla Default") {
            root["client_name"] = "Carla";
            root["send_port_l"] = "carla:audio_in_1";
            root["send_port_r"] = "carla:audio_in_2";
            root["return_port_l"] = "carla:audio_out_1";
            root["return_port_r"] = "carla:audio_out_2";
            root["sample_rate"] = 48000.0;
            root["buffer_size"] = 256;
            root["reported_latency_frames"] = 128;
            root["delay_compensation_frames"] = 0;
        } else if (presetName == "LSP Plugins Rack") {
            root["client_name"] = "lsp-plugins";
            root["send_port_l"] = "lsp:in_l";
            root["send_port_r"] = "lsp:in_r";
            root["return_port_l"] = "lsp:out_l";
            root["return_port_r"] = "lsp:out_r";
            root["sample_rate"] = 48000.0;
            root["buffer_size"] = 256;
            root["reported_latency_frames"] = 240;
            root["delay_compensation_frames"] = 240;
        }
        return root;
    }

    if (category == PresetCategories::AudioDevice) {
        if (presetName == "Default PipeWire") {
            root["backend"] = "PipeWire";
            root["sample_rate"] = 48000.0;
            root["buffer_size"] = 256;
            root["direct_mode"] = false;
        } else if (presetName == "Direct ALSA 192k") {
            root["backend"] = "ALSA";
            root["sample_rate"] = 192000.0;
            root["buffer_size"] = 1024;
            root["direct_mode"] = true;
        }
        return root;
    }

    return root;
}

QStringList PresetManager::listPresets(const QString& category) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    QStringList result;

    // 1. Add factory presets
    if (category == PresetCategories::ListeningCorrection) {
        result << "Monitor Flat" << "Acoustic Clarity" << "Harman Target" << "Bass Punch" << "Vocal Air";
    } else if (category == PresetCategories::MasterDsp) {
        result << "Default Bypass" << "Transparent Master" << "Warm Tape";
    } else if (category == PresetCategories::ExternalInsert) {
        result << "Carla Default" << "LSP Plugins Rack";
    } else if (category == PresetCategories::AudioDevice) {
        result << "Default PipeWire" << "Direct ALSA 192k";
    }

    // 2. Scan user preset directory
    QDir dir(categoryDirectory(category));
    if (dir.exists()) {
        QStringList filters;
        filters << "*.json";
        QFileInfoList list = dir.entryInfoList(filters, QDir::Files, QDir::Name);
        for (const auto& fi : list) {
            QString name = fi.completeBaseName();
            if (!result.contains(name)) {
                result.append(name);
            }
        }
    }

    return result;
}

bool PresetManager::savePreset(const QString& category, const QString& presetName, const QJsonObject& data, bool overwrite) {
    std::lock_guard<std::mutex> lock(m_mutex);
    QString cleanCategory = sanitizeName(category);
    QString cleanName = sanitizeName(presetName);

    // Factory presets cannot be overwritten by users
    if (isFactoryPreset(category, presetName)) {
        qWarning() << "Cannot overwrite read-only factory preset:" << presetName;
        return false;
    }

    ensureCategoryDirectories();
    QString dirPath = categoryDirectory(cleanCategory);
    QString filePath = dirPath + "/" + cleanName + ".json";

    if (!overwrite && QFile::exists(filePath)) {
        return false;
    }

    // Prepare JSON payload with metadata and category isolation validation
    QJsonObject root = data;
    root["category"] = cleanCategory;
    root["name"] = cleanName;
    root["version"] = 1;
    root["is_factory"] = false;
    root["saved_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonDocument doc(root);
    QByteArray bytes = doc.toJson(QJsonDocument::Indented);

    // Atomic write via QSaveFile
    QSaveFile saveFile(filePath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open save file for preset:" << filePath << saveFile.errorString();
        return false;
    }

    saveFile.write(bytes);
    if (!saveFile.commit()) {
        qWarning() << "Failed to commit preset file:" << filePath;
        return false;
    }

    emit presetSaved(category, presetName);
    return true;
}

QJsonObject PresetManager::loadPreset(const QString& category, const QString& presetName) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. Check if it's a factory preset
    if (isFactoryPreset(category, presetName)) {
        return getFactoryPreset(category, presetName);
    }

    // 2. Check user-saved preset file
    QString dirPath = categoryDirectory(category);
    QString filePath = dirPath + "/" + sanitizeName(presetName) + ".json";

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray bytes = file.readAll();
    QJsonParseError parseError{};
    QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "Preset JSON parse error in" << filePath << ":" << parseError.errorString();
        return {};
    }

    QJsonObject obj = doc.object();
    // Validate category isolation: preset category in file must match requested category
    if (obj.contains("category") && obj["category"].toString() != category) {
        qWarning() << "Category mismatch for preset:" << filePath
                   << "Expected:" << category << "Found:" << obj["category"].toString();
        return {};
    }

    return obj;
}

bool PresetManager::deletePreset(const QString& category, const QString& presetName) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Cannot delete factory preset
    if (isFactoryPreset(category, presetName)) {
        qWarning() << "Cannot delete read-only factory preset:" << presetName;
        return false;
    }

    QString filePath = categoryDirectory(category) + "/" + sanitizeName(presetName) + ".json";
    if (!QFile::exists(filePath)) {
        return false;
    }

    bool ok = QFile::remove(filePath);
    if (ok) {
        emit presetDeleted(category, presetName);
    }
    return ok;
}

} // namespace desk32::engine::preset

