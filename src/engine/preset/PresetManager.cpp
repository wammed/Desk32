#include "PresetManager.hpp"
#include "BuiltInDspPresets.hpp"
#include "DspPresetSerializer.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QSaveFile>
#include <QUuid>
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

QString PresetManager::dspPresetDirectory(DspPresetKind kind, DspPresetTarget target) const {
    if (kind == DspPresetKind::Global) {
        return presetRootPath() + QStringLiteral("/dsp/global");
    }
    return presetRootPath() + QStringLiteral("/dsp/") + sanitizeName(dspTargetToString(target));
}

void PresetManager::ensureCategoryDirectories() const {
    QString root = presetRootPath();
    QDir().mkpath(root + "/" + PresetCategories::ListeningCorrection);
    QDir().mkpath(root + "/" + PresetCategories::MasterDsp);
    QDir().mkpath(root + "/" + PresetCategories::ExternalInsert);
    QDir().mkpath(root + "/" + PresetCategories::AudioDevice);
    QDir().mkpath(root + QStringLiteral("/dsp/global"));
    for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
        QDir().mkpath(root + QStringLiteral("/dsp/") +
                      dspTargetToString(static_cast<DspPresetTarget>(i)));
    }
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
        const auto names = ParametricEQ::factoryPresetNames();
        for (const auto& n : names) {
            if (presetName.toStdString() == n) {
                return true;
            }
        }
        return false;
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
    root["format_version"] = 1;
    root["is_factory"] = true;
    root["created_at"] = "2026-11-07T00:00:00Z";

    if (category == PresetCategories::ListeningCorrection) {
        std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> lBands{};
        std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> rBands{};
        if (!ParametricEQ::getPresetBandConfigs(presetName.toStdString(), lBands, rBands)) {
            return {};
        }

        auto toBandArray = [](const std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL>& src) {
            QJsonArray arr;
            for (size_t i = 0; i < src.size(); ++i) {
                QJsonObject b;
                b["index"] = static_cast<int>(i);
                b["frequency"] = src[i].frequency;
                b["gain_db"] = src[i].gainDb;
                b["q"] = src[i].Q;
                b["type"] = static_cast<int>(src[i].type);
                b["enabled"] = src[i].enabled;
                arr.append(b);
            }
            return arr;
        };

        QJsonArray leftArr = toBandArray(lBands);
        QJsonArray rightArr = toBandArray(rBands);
        root["left_bands"] = leftArr;
        root["right_bands"] = rightArr;
        root["bands"] = leftArr; // Legacy single-array alias
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
            filter["hpf_q"] = 0.70710678;
            filter["lpf_enabled"] = true;
            filter["lpf_frequency"] = 18000.0;
            filter["lpf_slope"] = 1; // 24dB/oct
            filter["lpf_q"] = 0.70710678;
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
        for (const auto& name : ParametricEQ::factoryPresetNames()) {
            result << QString::fromStdString(name);
        }
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
    root["format_version"] = 1;
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

    // Validate & migrate format_version / version schema (Spec §11.1)
    constexpr int CURRENT_FORMAT_VERSION = 1;
    int fmtVer = obj.contains("format_version")
                     ? obj["format_version"].toInt(-1)
                     : (obj.contains("version") ? obj["version"].toInt(1) : 1);
    if (fmtVer < 1 || fmtVer > CURRENT_FORMAT_VERSION) {
        qWarning() << "Unsupported preset format_version:" << fmtVer << "in" << filePath;
        return {};
    }
    if (!obj.contains("format_version")) {
        obj["format_version"] = fmtVer;
    }
    if (!obj.contains("version")) {
        obj["version"] = fmtVer;
    }
    if (category == PresetCategories::ListeningCorrection && obj.contains("bands") &&
        (!obj.contains("left_bands") || !obj.contains("right_bands"))) {
        if (!obj.contains("left_bands")) obj["left_bands"] = obj["bands"];
        if (!obj.contains("right_bands")) obj["right_bands"] = obj["bands"];
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

QString PresetManager::findUserDspPresetFileLocked(DspPresetKind kind,
                                                   DspPresetTarget target,
                                                   const QString& idOrName) const {
    const QDir dir(dspPresetDirectory(kind, target));
    if (!dir.exists()) return {};

    // Fast path: direct filename match
    const QString directPath = dir.filePath(sanitizeName(idOrName) + QStringLiteral(".json"));
    if (QFile::exists(directPath)) {
        return directPath;
    }

    // Scan files in directory for matching `id` or `name`
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument jdoc = QJsonDocument::fromJson(f.readAll());
        if (!jdoc.isObject()) continue;
        const QJsonObject obj = jdoc.object();
        if (obj.value(QStringLiteral("id")).toString() == idOrName ||
            obj.value(QStringLiteral("name")).toString() == idOrName ||
            fi.completeBaseName() == idOrName) {
            return fi.absoluteFilePath();
        }
    }

    // For Global presets, also allow loading legacy v1 master_dsp/*.json user files
    if (kind == DspPresetKind::Global) {
        const QDir legacyDir(categoryDirectory(QString::fromLatin1(PresetCategories::MasterDsp)));
        if (legacyDir.exists()) {
            const QString legacyPath = legacyDir.filePath(sanitizeName(idOrName) + QStringLiteral(".json"));
            if (QFile::exists(legacyPath)) {
                return legacyPath;
            }
        }
    }
    return {};
}

std::vector<DspPresetSummary> PresetManager::listDspPresets(DspPresetKind kind,
                                                            DspPresetTarget target) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DspPresetSummary> result;

    // 1. Read-only built-in presets (Appendix A.12)
    for (const auto& doc : BuiltInDspPresets::presetsFor(kind, target)) {
        DspPresetSummary s;
        s.id = doc.id;
        s.name = doc.name;
        s.kind = doc.kind;
        s.scope = doc.scope;
        s.target = doc.target;
        s.isFactory = true;
        s.createdAt = doc.createdAt;
        s.modifiedAt = doc.modifiedAt;
        result.push_back(std::move(s));
    }

    // 2. User-saved presets in $XDG_CONFIG_HOME/desk32/presets/dsp/...
    const QDir dir(dspPresetDirectory(kind, target));
    if (dir.exists()) {
        const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const auto& fi : files) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            DspPresetDocument doc;
            PresetValidationReport rep;
            if (!DspPresetSerializer::fromJsonBytes(f.readAll(), doc, rep)) continue;
            if (doc.kind != kind) continue;
            if (kind == DspPresetKind::PerDsp && doc.target != target) continue;

            DspPresetSummary s;
            s.id = doc.id;
            s.name = doc.name.isEmpty() ? fi.completeBaseName() : doc.name;
            s.kind = doc.kind;
            s.scope = doc.scope;
            s.target = doc.target;
            s.isFactory = false;
            s.createdAt = doc.createdAt;
            s.modifiedAt = doc.modifiedAt;
            result.push_back(std::move(s));
        }
    }

    return result;
}

bool PresetManager::hasUserDspPresetNamed(DspPresetKind kind,
                                          DspPresetTarget target,
                                          const QString& presetName) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const QString clean = sanitizeName(presetName);
    const QString filePath = dspPresetDirectory(kind, target) + QLatin1Char('/') + clean + QStringLiteral(".json");
    if (QFile::exists(filePath)) return true;

    const QDir dir(dspPresetDirectory(kind, target));
    if (!dir.exists()) return false;
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const auto& fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument jdoc = QJsonDocument::fromJson(f.readAll());
        if (jdoc.isObject() && jdoc.object().value(QStringLiteral("name")).toString() == presetName.trimmed()) {
            return true;
        }
    }
    return false;
}

bool PresetManager::isBuiltInDspPreset(DspPresetKind kind,
                                       DspPresetTarget target,
                                       const QString& idOrName) const {
    return BuiltInDspPresets::isBuiltIn(kind, target, idOrName);
}

bool PresetManager::loadDspPresetFromFile(const QString& filePath,
                                          DspPresetDocument& outDoc,
                                          PresetValidationReport& outReport) const {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        outReport = PresetValidationReport{};
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Failed to open preset file '%1': %2")
                                     .arg(filePath, f.errorString());
        return false;
    }
    return DspPresetSerializer::fromJsonBytes(f.readAll(), outDoc, outReport);
}

bool PresetManager::loadDspPreset(DspPresetKind kind,
                                  DspPresetTarget target,
                                  const QString& idOrName,
                                  DspPresetDocument& outDoc,
                                  PresetValidationReport& outReport) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    outReport = PresetValidationReport{};

    // 1. Check built-in read-only presets first
    if (BuiltInDspPresets::findPreset(kind, target, idOrName, outDoc)) {
        outReport.ok = true;
        return true;
    }

    const QString path = findUserDspPresetFileLocked(kind, target, idOrName);
    if (path.isEmpty()) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("DSP preset '%1' not found").arg(idOrName);
        return false;
    }

    if (!loadDspPresetFromFile(path, outDoc, outReport)) {
        return false;
    }
    if (outDoc.kind != kind) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Preset kind mismatch: expected '%1', got '%2'")
                                     .arg(dspKindToString(kind), dspKindToString(outDoc.kind));
        return false;
    }
    if (kind == DspPresetKind::PerDsp && outDoc.target != target) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Preset target mismatch: expected '%1', got '%2'")
                                     .arg(dspTargetToString(target), dspTargetToString(outDoc.target));
        return false;
    }
    return true;
}

bool PresetManager::saveUserDspPreset(DspPresetDocument& doc,
                                      bool overwrite,
                                      QString* outError) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const QString trimmedName = doc.name.trimmed();
        if (trimmedName.isEmpty()) {
            if (outError) *outError = QStringLiteral("Preset name must not be empty");
            return false;
        }
        if (isBuiltInDspPreset(doc.kind, doc.target, trimmedName) ||
            (!doc.id.isEmpty() && isBuiltInDspPreset(doc.kind, doc.target, doc.id))) {
            if (outError) *outError = QStringLiteral("Cannot overwrite read-only built-in preset '%1'").arg(trimmedName);
            return false;
        }

        ensureCategoryDirectories();
        const QString dirPath = dspPresetDirectory(doc.kind, doc.target);
        const QString filePath = dirPath + QLatin1Char('/') + sanitizeName(trimmedName) + QStringLiteral(".json");

        if (!overwrite && QFile::exists(filePath)) {
            if (outError) *outError = QStringLiteral("A preset named '%1' already exists").arg(trimmedName);
            return false;
        }

        // Preserve existing id/created_at when overwriting an existing user preset by name
        const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (QFile::exists(filePath)) {
            DspPresetDocument existing;
            PresetValidationReport existingRep;
            if (loadDspPresetFromFile(filePath, existing, existingRep)) {
                if (doc.id.isEmpty()) doc.id = existing.id;
                if (doc.createdAt.isEmpty()) doc.createdAt = existing.createdAt;
            }
        }

        doc.format = QString::fromLatin1(kDspPresetFormat);
        doc.version = kDspPresetCurrentVersion;
        doc.name = trimmedName;
        doc.isFactory = false;
        doc.scope = (doc.kind == DspPresetKind::PerDsp && doc.target == DspPresetTarget::Peq)
                        ? DspPresetScope::MasteringChain
                        : DspPresetScope::Chain;
        if (doc.id.isEmpty()) {
            doc.id = QStringLiteral("user.") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        if (doc.createdAt.isEmpty()) {
            doc.createdAt = nowIso;
        }
        doc.modifiedAt = nowIso;

        const QByteArray bytes = DspPresetSerializer::toJsonBytes(doc);
        QSaveFile saveFile(filePath);
        if (!saveFile.open(QIODevice::WriteOnly)) {
            if (outError) *outError = QStringLiteral("Failed to open preset file for writing: %1").arg(saveFile.errorString());
            return false;
        }
        saveFile.write(bytes);
        if (!saveFile.commit()) {
            if (outError) *outError = QStringLiteral("Failed to atomically commit preset file '%1'").arg(filePath);
            return false;
        }
    }

    emit dspPresetListChanged();
    return true;
}

bool PresetManager::renameUserDspPreset(DspPresetKind kind,
                                        DspPresetTarget target,
                                        const QString& id,
                                        const QString& newName,
                                        bool overwrite,
                                        QString* outError) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const QString trimmedNew = newName.trimmed();
        if (trimmedNew.isEmpty()) {
            if (outError) *outError = QStringLiteral("New preset name must not be empty");
            return false;
        }
        if (isBuiltInDspPreset(kind, target, id) || isBuiltInDspPreset(kind, target, trimmedNew)) {
            if (outError) *outError = QStringLiteral("Cannot rename or overwrite a built-in preset");
            return false;
        }

        const QString oldPath = findUserDspPresetFileLocked(kind, target, id);
        if (oldPath.isEmpty()) {
            if (outError) *outError = QStringLiteral("User preset '%1' not found").arg(id);
            return false;
        }

        DspPresetDocument doc;
        PresetValidationReport rep;
        if (!loadDspPresetFromFile(oldPath, doc, rep)) {
            if (outError) *outError = rep.errorMessage;
            return false;
        }

        const QString newPath = dspPresetDirectory(kind, target) + QLatin1Char('/') +
                                sanitizeName(trimmedNew) + QStringLiteral(".json");
        if (!overwrite && newPath != oldPath && QFile::exists(newPath)) {
            if (outError) *outError = QStringLiteral("A preset named '%1' already exists").arg(trimmedNew);
            return false;
        }

        // Preserve immutable `id` and `createdAt`, update `name` and `modifiedAt`
        doc.name = trimmedNew;
        doc.modifiedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

        const QByteArray bytes = DspPresetSerializer::toJsonBytes(doc);
        QSaveFile saveFile(newPath);
        if (!saveFile.open(QIODevice::WriteOnly)) {
            if (outError) *outError = QStringLiteral("Failed to open renamed preset file: %1").arg(saveFile.errorString());
            return false;
        }
        saveFile.write(bytes);
        if (!saveFile.commit()) {
            if (outError) *outError = QStringLiteral("Failed to commit renamed preset file");
            return false;
        }
        if (newPath != oldPath) {
            QFile::remove(oldPath);
        }
    }

    emit dspPresetListChanged();
    return true;
}

bool PresetManager::deleteUserDspPreset(DspPresetKind kind,
                                        DspPresetTarget target,
                                        const QString& idOrName,
                                        QString* outError) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (isBuiltInDspPreset(kind, target, idOrName)) {
            if (outError) *outError = QStringLiteral("Cannot delete read-only built-in preset '%1'").arg(idOrName);
            return false;
        }
        const QString path = findUserDspPresetFileLocked(kind, target, idOrName);
        if (path.isEmpty()) {
            if (outError) *outError = QStringLiteral("User preset '%1' not found").arg(idOrName);
            return false;
        }
        if (!QFile::remove(path)) {
            if (outError) *outError = QStringLiteral("Failed to remove preset file '%1'").arg(path);
            return false;
        }
    }

    emit dspPresetListChanged();
    return true;
}

} // namespace desk32::engine::preset

