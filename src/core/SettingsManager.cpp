#include "SettingsManager.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace desk32::core {

namespace {
constexpr int kSettingsFormatVersion = 1;
} // namespace

SettingsManager& SettingsManager::instance()
{
    static SettingsManager s_instance;
    return s_instance;
}

SettingsManager::SettingsManager(QObject* parent)
    : QObject(parent)
    , m_root(buildDefaultRoot())
{
}

QJsonObject SettingsManager::buildDefaultRoot()
{
    QJsonObject general;
    general.insert(QStringLiteral("language"), QStringLiteral("auto"));
    general.insert(QStringLiteral("fullscreen_on_startup"), true);
    general.insert(QStringLiteral("restore_session_on_startup"), false);

    QJsonObject jack;
    jack.insert(QStringLiteral("auto_connect"), true);
    jack.insert(QStringLiteral("client_name"), QStringLiteral("Desk32"));

    QJsonObject alsa;
    alsa.insert(QStringLiteral("device"), QStringLiteral("default"));
    alsa.insert(QStringLiteral("period_frames"), 256);
    alsa.insert(QStringLiteral("periods_per_buffer"), 2);
    alsa.insert(QStringLiteral("exclusive_hw_direct"), false);

    QJsonObject audio;
    audio.insert(QStringLiteral("backend"), QStringLiteral("PipeWire"));
    audio.insert(QStringLiteral("target_device"), QString());
    audio.insert(QStringLiteral("pcm_format"), QStringLiteral("F32LE"));
    audio.insert(QStringLiteral("buffer_frames"), 256);
    audio.insert(QStringLiteral("fixed_sample_rate"), 48000.0);
    audio.insert(QStringLiteral("direct_mode_default"), false);
    audio.insert(QStringLiteral("live_dither_enabled"), true);
    audio.insert(QStringLiteral("jack"), jack);
    audio.insert(QStringLiteral("alsa"), alsa);

    QJsonObject dsp;
    dsp.insert(QStringLiteral("reverb_max_tail_seconds"), 5.0);
    dsp.insert(QStringLiteral("reverb_max_processing_ms"), 0.0);
    dsp.insert(QStringLiteral("listening_preset"), QStringLiteral("Monitor Flat"));
    dsp.insert(QStringLiteral("auto_level_match_target"), QStringLiteral("active_deck"));

    QJsonObject extInsert;
    extInsert.insert(QStringLiteral("insert_position"), 4);
    extInsert.insert(QStringLiteral("disconnect_policy"), 0);

    QJsonObject render;
    render.insert(QStringLiteral("default_bit_depth"), 1);
    render.insert(QStringLiteral("default_rate_mode"), 0);
    render.insert(QStringLiteral("default_custom_rate"), 48000.0);
    render.insert(QStringLiteral("default_apply_dither"), true);
    render.insert(QStringLiteral("sync_master_dsp"), true);

    QJsonObject root;
    root.insert(QStringLiteral("format_version"), kSettingsFormatVersion);
    root.insert(QStringLiteral("general"), general);
    root.insert(QStringLiteral("audio"), audio);
    root.insert(QStringLiteral("dsp"), dsp);
    root.insert(QStringLiteral("external_insert"), extInsert);
    root.insert(QStringLiteral("render"), render);
    return root;
}

void SettingsManager::mergeMissingDefaults(QJsonObject& target, const QJsonObject& defaults)
{
    for (auto it = defaults.constBegin(); it != defaults.constEnd(); ++it) {
        const QString& key = it.key();
        const QJsonValue& defVal = it.value();
        if (!target.contains(key)) {
            target.insert(key, defVal);
        } else if (defVal.isObject() && target.value(key).isObject()) {
            QJsonObject child = target.value(key).toObject();
            mergeMissingDefaults(child, defVal.toObject());
            target.insert(key, child);
        }
    }
}

QVariant SettingsManager::readDotted(const QJsonObject& obj, const QStringList& parts)
{
    if (parts.isEmpty()) {
        return QVariant();
    }
    QJsonValue cur(obj);
    for (const QString& part : parts) {
        if (!cur.isObject()) {
            return QVariant();
        }
        const QJsonObject curObj = cur.toObject();
        if (!curObj.contains(part)) {
            return QVariant();
        }
        cur = curObj.value(part);
    }
    return cur.toVariant();
}

void SettingsManager::writeDotted(QJsonObject& obj, const QStringList& parts, const QJsonValue& val)
{
    if (parts.isEmpty()) {
        return;
    }
    if (parts.size() == 1) {
        obj.insert(parts.first(), val);
        return;
    }
    const QString& head = parts.first();
    QJsonObject child = obj.value(head).isObject() ? obj.value(head).toObject() : QJsonObject{};
    writeDotted(child, parts.mid(1), val);
    obj.insert(head, child);
}

QString SettingsManager::resolveConfigPathLocked() const
{
    if (!m_testConfigPath.isEmpty()) {
        return m_testConfigPath;
    }
    QString cfgDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (cfgDir.isEmpty()) {
        cfgDir = QDir::homePath() + QStringLiteral("/.config");
    }
    return cfgDir + QStringLiteral("/desk32/settings.json");
}

QString SettingsManager::configFilePath() const
{
    QMutexLocker locker(&m_mutex);
    return resolveConfigPathLocked();
}

void SettingsManager::setConfigFilePathForTesting(const QString& path)
{
    {
        QMutexLocker locker(&m_mutex);
        m_testConfigPath = path;
        m_loaded = false;
        m_lastError.clear();
        m_root = buildDefaultRoot();
    }
    emit configFilePathChanged();
    emit settingsChanged();
}

void SettingsManager::resetForTesting()
{
    {
        QMutexLocker locker(&m_mutex);
        m_testConfigPath.clear();
        m_loaded = false;
        m_lastError.clear();
        m_root = buildDefaultRoot();
    }
    emit configFilePathChanged();
    emit settingsChanged();
}

bool SettingsManager::isLoaded() const noexcept
{
    QMutexLocker locker(&m_mutex);
    return m_loaded;
}

QString SettingsManager::lastError() const
{
    QMutexLocker locker(&m_mutex);
    return m_lastError;
}

bool SettingsManager::load()
{
    bool ok = true;
    {
        QMutexLocker locker(&m_mutex);
        m_lastError.clear();
        const QString path = resolveConfigPathLocked();
        QFile f(path);
        if (!f.exists()) {
            m_root = buildDefaultRoot();
            m_loaded = true;
            ok = saveLocked();
        } else if (!f.open(QIODevice::ReadOnly)) {
            m_lastError = QString("Failed to open settings file '%1': %2")
                              .arg(path, f.errorString());
            m_root = buildDefaultRoot();
            m_loaded = true;
            ok = false;
        } else {
            QJsonParseError parseErr{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
            f.close();
            if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
                m_lastError = QString("Invalid settings JSON in '%1': %2")
                                  .arg(path, parseErr.errorString());
                m_root = buildDefaultRoot();
                m_loaded = true;
                ok = false;
            } else {
                QJsonObject loadedObj = doc.object();
                const int ver = loadedObj.value(QStringLiteral("format_version")).toInt(0);
                if (ver <= 0 || ver > kSettingsFormatVersion) {
                    m_lastError = QString("Unsupported settings format_version %1 in '%2'")
                                      .arg(ver)
                                      .arg(path);
                    m_root = buildDefaultRoot();
                    m_loaded = true;
                    ok = false;
                } else {
                    const QJsonObject defaults = buildDefaultRoot();
                    mergeMissingDefaults(loadedObj, defaults);
                    loadedObj.insert(QStringLiteral("format_version"), kSettingsFormatVersion);
                    m_root = loadedObj;
                    m_loaded = true;
                    ok = true;
                }
            }
        }
    }
    emit settingsChanged();
    return ok;
}

bool SettingsManager::saveLocked() const
{
    const QString path = resolveConfigPathLocked();
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)) {
        return false;
    }
    QSaveFile saveFile(path);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray json = QJsonDocument(m_root).toJson(QJsonDocument::Indented);
    if (saveFile.write(json) != json.size()) {
        saveFile.cancelWriting();
        return false;
    }
    return saveFile.commit();
}

bool SettingsManager::save() const
{
    QMutexLocker locker(&m_mutex);
    return saveLocked();
}

void SettingsManager::resetToDefaults(bool autoSave)
{
    {
        QMutexLocker locker(&m_mutex);
        m_root = buildDefaultRoot();
        m_lastError.clear();
        if (autoSave && m_loaded) {
            saveLocked();
        }
    }
    emit settingsChanged();
}

QVariant SettingsManager::value(const QString& dottedKey, const QVariant& defaultValue) const
{
    const QStringList parts = dottedKey.split('.', Qt::SkipEmptyParts);
    QMutexLocker locker(&m_mutex);
    const QVariant v = readDotted(m_root, parts);
    return v.isValid() ? v : defaultValue;
}

void SettingsManager::setValue(const QString& dottedKey, const QVariant& val, bool autoSave)
{
    const QStringList parts = dottedKey.split('.', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        writeDotted(m_root, parts, QJsonValue::fromVariant(val));
        if (autoSave && m_loaded) {
            saveLocked();
        }
    }
    emit settingChanged(dottedKey);
    emit settingsChanged();
}

QVariantMap SettingsManager::section(const QString& sectionName) const
{
    QMutexLocker locker(&m_mutex);
    const QJsonValue v = m_root.value(sectionName);
    return v.isObject() ? v.toObject().toVariantMap() : QVariantMap{};
}

void SettingsManager::setSection(const QString& sectionName, const QVariantMap& map, bool autoSave)
{
    if (sectionName.isEmpty() || sectionName == QStringLiteral("format_version")) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        m_root.insert(sectionName, QJsonObject::fromVariantMap(map));
        if (autoSave && m_loaded) {
            saveLocked();
        }
    }
    emit settingChanged(sectionName);
    emit settingsChanged();
}

void SettingsManager::setDottedAndPersist(const QString& dottedKey, const QJsonValue& val)
{
    const QStringList parts = dottedKey.split('.', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return;
    }
    bool changed = false;
    {
        QMutexLocker locker(&m_mutex);
        const QVariant prev = readDotted(m_root, parts);
        if (!prev.isValid() || prev != val.toVariant()) {
            writeDotted(m_root, parts, val);
            changed = true;
            if (m_loaded) {
                saveLocked();
            }
        }
    }
    if (changed) {
        emit settingChanged(dottedKey);
        emit settingsChanged();
    }
}

QString SettingsManager::languagePreference() const
{
    return value(QStringLiteral("general.language"), QStringLiteral("auto")).toString();
}

bool SettingsManager::fullscreenOnStartup() const
{
    return value(QStringLiteral("general.fullscreen_on_startup"), true).toBool();
}

bool SettingsManager::restoreSessionOnStartup() const
{
    return value(QStringLiteral("general.restore_session_on_startup"), false).toBool();
}

QString SettingsManager::audioBackend() const
{
    return value(QStringLiteral("audio.backend"), QStringLiteral("PipeWire")).toString();
}

QString SettingsManager::audioTargetDevice() const
{
    return value(QStringLiteral("audio.target_device"), QString()).toString();
}

QString SettingsManager::audioPcmFormat() const
{
    return value(QStringLiteral("audio.pcm_format"), QStringLiteral("F32LE")).toString();
}

int SettingsManager::audioBufferFrames() const
{
    return value(QStringLiteral("audio.buffer_frames"), 256).toInt();
}

double SettingsManager::audioFixedSampleRate() const
{
    return value(QStringLiteral("audio.fixed_sample_rate"), 48000.0).toDouble();
}

bool SettingsManager::directModeDefault() const
{
    return value(QStringLiteral("audio.direct_mode_default"), false).toBool();
}

bool SettingsManager::liveDitherEnabled() const
{
    return value(QStringLiteral("audio.live_dither_enabled"), true).toBool();
}

double SettingsManager::reverbMaxTailSeconds() const
{
    return value(QStringLiteral("dsp.reverb_max_tail_seconds"), 5.0).toDouble();
}

double SettingsManager::reverbMaxProcessingMs() const
{
    return value(QStringLiteral("dsp.reverb_max_processing_ms"), 0.0).toDouble();
}

QString SettingsManager::listeningPreset() const
{
    return value(QStringLiteral("dsp.listening_preset"), QStringLiteral("Monitor Flat")).toString();
}

int SettingsManager::externalInsertPosition() const
{
    return value(QStringLiteral("external_insert.insert_position"), 4).toInt();
}

int SettingsManager::externalInsertDisconnectPolicy() const
{
    return value(QStringLiteral("external_insert.disconnect_policy"), 0).toInt();
}

void SettingsManager::setLanguagePreference(const QString& lang)
{
    const QString norm = (lang == QStringLiteral("ja") || lang == QStringLiteral("en"))
                             ? lang
                             : QStringLiteral("auto");
    setDottedAndPersist(QStringLiteral("general.language"), norm);
}

void SettingsManager::setFullscreenOnStartup(bool fullscreen)
{
    setDottedAndPersist(QStringLiteral("general.fullscreen_on_startup"), fullscreen);
}

void SettingsManager::setRestoreSessionOnStartup(bool restore)
{
    setDottedAndPersist(QStringLiteral("general.restore_session_on_startup"), restore);
}

void SettingsManager::setAudioBackend(const QString& backend)
{
    if (!backend.trimmed().isEmpty()) {
        setDottedAndPersist(QStringLiteral("audio.backend"), backend.trimmed());
    }
}

void SettingsManager::setAudioTargetDevice(const QString& deviceIdOrName)
{
    const QString normalized = (deviceIdOrName == QStringLiteral("default"))
                                   ? QString()
                                   : deviceIdOrName;
    setDottedAndPersist(QStringLiteral("audio.target_device"), normalized);
}

void SettingsManager::setAudioPcmFormat(const QString& formatName)
{
    setDottedAndPersist(QStringLiteral("audio.pcm_format"), formatName.trimmed().toUpper());
}

void SettingsManager::setAudioBufferFrames(int frames)
{
    if (frames > 0 && frames <= 16384) {
        setDottedAndPersist(QStringLiteral("audio.buffer_frames"), frames);
    }
}

void SettingsManager::setAudioFixedSampleRate(double sampleRate)
{
    if (sampleRate >= 8000.0 && sampleRate <= 768000.0) {
        setDottedAndPersist(QStringLiteral("audio.fixed_sample_rate"), sampleRate);
    }
}

void SettingsManager::setDirectModeDefault(bool enabled)
{
    setDottedAndPersist(QStringLiteral("audio.direct_mode_default"), enabled);
}

void SettingsManager::setLiveDitherEnabled(bool enabled)
{
    setDottedAndPersist(QStringLiteral("audio.live_dither_enabled"), enabled);
}

void SettingsManager::setReverbMaxTailSeconds(double seconds)
{
    setDottedAndPersist(QStringLiteral("dsp.reverb_max_tail_seconds"), seconds);
}

void SettingsManager::setReverbMaxProcessingMs(double ms)
{
    setDottedAndPersist(QStringLiteral("dsp.reverb_max_processing_ms"), ms);
}

void SettingsManager::setListeningPreset(const QString& presetName)
{
    if (!presetName.trimmed().isEmpty()) {
        setDottedAndPersist(QStringLiteral("dsp.listening_preset"), presetName.trimmed());
    }
}

void SettingsManager::setExternalInsertPosition(int positionIndex)
{
    setDottedAndPersist(QStringLiteral("external_insert.insert_position"), positionIndex);
}

void SettingsManager::setExternalInsertDisconnectPolicy(int policy)
{
    setDottedAndPersist(QStringLiteral("external_insert.disconnect_policy"), policy);
}

} // namespace desk32::core
