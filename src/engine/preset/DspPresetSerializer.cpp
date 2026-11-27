#include "DspPresetSerializer.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace desk32::engine::preset {

namespace {

inline bool approxEq(double a, double b, double eps) noexcept {
    return std::abs(a - b) <= eps;
}

double readClampedDouble(const QJsonObject& obj,
                         const QString& key,
                         double defaultVal,
                         double minVal,
                         double maxVal,
                         const QString& fieldPath,
                         PresetValidationReport& report,
                         const QString& legacyKey = QString()) {
    QString actualKey;
    if (obj.contains(key)) {
        actualKey = key;
    } else if (!legacyKey.isEmpty() && obj.contains(legacyKey)) {
        actualKey = legacyKey;
    } else {
        report.missingDefaultedFields.append(fieldPath);
        return defaultVal;
    }

    const QJsonValue val = obj.value(actualKey);
    if (!val.isDouble()) {
        report.warnings.append(QStringLiteral("Field '%1' is not a number; defaulted to %2")
                                   .arg(fieldPath)
                                   .arg(defaultVal));
        report.missingDefaultedFields.append(fieldPath);
        return defaultVal;
    }

    const double raw = val.toDouble(defaultVal);
    if (!std::isfinite(raw)) {
        report.warnings.append(QStringLiteral("Field '%1' is non-finite; defaulted to %2")
                                   .arg(fieldPath)
                                   .arg(defaultVal));
        report.missingDefaultedFields.append(fieldPath);
        return defaultVal;
    }

    if (raw < minVal || raw > maxVal) {
        const double clamped = std::clamp(raw, minVal, maxVal);
        report.clampedFields.append(QStringLiteral("%1 (%2 -> %3)")
                                        .arg(fieldPath)
                                        .arg(raw)
                                        .arg(clamped));
        report.warnings.append(QStringLiteral("Clamped '%1' from %2 to %3 [%4, %5]")
                                   .arg(fieldPath)
                                   .arg(raw)
                                   .arg(clamped)
                                   .arg(minVal)
                                   .arg(maxVal));
        return clamped;
    }
    return raw;
}

int readSlopeDbOct(const QJsonObject& obj,
                   const QString& key,
                   int defaultVal,
                   const QString& fieldPath,
                   PresetValidationReport& report,
                   const QString& legacyIndexKey = QString()) {
    if (obj.contains(key)) {
        const QJsonValue v = obj.value(key);
        if (v.isDouble()) {
            const double dv = v.toDouble(-1.0);
            const int iv = static_cast<int>(std::llround(dv));
            if (std::abs(dv - static_cast<double>(iv)) < 1e-6 &&
                (iv == 12 || iv == 18 || iv == 24)) {
                return iv;
            }
        } else if (v.isString()) {
            const QString s = v.toString().trimmed().toLower();
            if (s == QLatin1String("12") || s == QLatin1String("12db") || s == QLatin1String("12db/oct")) return 12;
            if (s == QLatin1String("18") || s == QLatin1String("18db") || s == QLatin1String("18db/oct")) return 18;
            if (s == QLatin1String("24") || s == QLatin1String("24db") || s == QLatin1String("24db/oct")) return 24;
        }
        report.unknownEnumFields.append(QStringLiteral("%1=%2 -> %3")
                                            .arg(fieldPath, v.toVariant().toString())
                                            .arg(defaultVal));
        report.warnings.append(QStringLiteral("Unknown slope enum at '%1'; replaced with default %2 dB/oct")
                                   .arg(fieldPath)
                                   .arg(defaultVal));
        return defaultVal;
    }

    if (!legacyIndexKey.isEmpty() && obj.contains(legacyIndexKey)) {
        const int idx = obj.value(legacyIndexKey).toInt(0);
        if (idx == 0) return 12;
        if (idx == 1) return 24;
        if (idx == 2) return 18;
        report.unknownEnumFields.append(QStringLiteral("%1=%2 -> %3")
                                            .arg(fieldPath)
                                            .arg(idx)
                                            .arg(defaultVal));
        return defaultVal;
    }

    report.missingDefaultedFields.append(fieldPath);
    return defaultVal;
}

void collectUnknownKeys(const QJsonObject& obj,
                        const QSet<QString>& knownKeys,
                        const QString& prefix,
                        QJsonObject& outUnknown,
                        PresetValidationReport& report) {
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (!knownKeys.contains(it.key())) {
            outUnknown.insert(it.key(), it.value());
            const QString path = prefix.isEmpty() ? it.key() : (prefix + QLatin1Char('.') + it.key());
            report.unknownKeys.append(path);
            report.warnings.append(QStringLiteral("Preserved unknown key '%1'").arg(path));
        }
    }
}

void mergeUnknownKeys(QJsonObject& target, const QJsonObject& unknown) {
    for (auto it = unknown.constBegin(); it != unknown.constEnd(); ++it) {
        if (!target.contains(it.key())) {
            target.insert(it.key(), it.value());
        }
    }
}

QJsonObject peqBandToJson(const PeqBandPreset& b) {
    QJsonObject obj;
    obj.insert(QStringLiteral("freq_hz"), b.freqHz);
    obj.insert(QStringLiteral("gain_db"), b.gainDb);
    obj.insert(QStringLiteral("q"), b.q);
    obj.insert(QStringLiteral("type"), filterTypeToString(b.type));
    obj.insert(QStringLiteral("enabled"), b.enabled);
    mergeUnknownKeys(obj, b.unknownFields);
    return obj;
}

PeqBandPreset parsePeqBand(const QJsonObject& obj,
                           size_t index,
                           const QString& pathPrefix,
                           PresetValidationReport& report) {
    PeqBandPreset b;
    const double defaultFreq = (index < ParametricEQ::BANDS_PER_CHANNEL)
                                   ? kPeqIsoDefaultFreqs[index]
                                   : 1000.0;
    b.freqHz = readClampedDouble(obj, QStringLiteral("freq_hz"), defaultFreq, 10.0, 24000.0,
                                 pathPrefix + QStringLiteral(".freq_hz"), report,
                                 QStringLiteral("frequency"));
    b.gainDb = readClampedDouble(obj, QStringLiteral("gain_db"), 0.0, -24.0, 24.0,
                                 pathPrefix + QStringLiteral(".gain_db"), report,
                                 QStringLiteral("gainDb"));
    b.q = readClampedDouble(obj, QStringLiteral("q"), 0.707, 0.1, 20.0,
                            pathPrefix + QStringLiteral(".q"), report,
                            QStringLiteral("Q"));

    if (obj.contains(QStringLiteral("type"))) {
        const QJsonValue tv = obj.value(QStringLiteral("type"));
        if (tv.isString()) {
            BiquadFilterType parsed = BiquadFilterType::Peak;
            if (filterTypeFromString(tv.toString(), parsed)) {
                b.type = parsed;
            } else {
                b.type = BiquadFilterType::Peak;
                report.unknownEnumFields.append(
                    QStringLiteral("%1.type=%2 -> peak").arg(pathPrefix, tv.toString()));
                report.warnings.append(
                    QStringLiteral("Unknown PEQ filter type '%1' at '%2.type'; replaced with 'peak'")
                        .arg(tv.toString(), pathPrefix));
            }
        } else if (tv.isDouble()) {
            const int ti = tv.toInt(-1);
            if (ti >= 0 && ti <= 5) {
                b.type = static_cast<BiquadFilterType>(ti);
            } else {
                b.type = BiquadFilterType::Peak;
                report.unknownEnumFields.append(
                    QStringLiteral("%1.type=%2 -> peak").arg(pathPrefix).arg(ti));
                report.warnings.append(
                    QStringLiteral("Unknown numeric PEQ filter type %1 at '%2.type'; replaced with 'peak'")
                        .arg(ti)
                        .arg(pathPrefix));
            }
        } else {
            b.type = BiquadFilterType::Peak;
            report.unknownEnumFields.append(QStringLiteral("%1.type -> peak").arg(pathPrefix));
        }
    } else {
        b.type = BiquadFilterType::Peak;
        report.missingDefaultedFields.append(pathPrefix + QStringLiteral(".type"));
    }

    if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
        b.enabled = obj.value(QStringLiteral("enabled")).toBool(false);
    } else {
        b.enabled = false;
        report.missingDefaultedFields.append(pathPrefix + QStringLiteral(".enabled"));
    }

    static const QSet<QString> kKnownBandKeys = {
        QStringLiteral("freq_hz"), QStringLiteral("frequency"),
        QStringLiteral("gain_db"), QStringLiteral("gainDb"),
        QStringLiteral("q"), QStringLiteral("Q"),
        QStringLiteral("type"), QStringLiteral("enabled"),
        QStringLiteral("index")
    };
    collectUnknownKeys(obj, kKnownBandKeys, pathPrefix, b.unknownFields, report);
    return b;
}

void parsePeqChannelBands(const QJsonArray& arr,
                          std::array<PeqBandPreset, ParametricEQ::BANDS_PER_CHANNEL>& outBands,
                          std::vector<PeqBandPreset>& outOverflow,
                          const QString& channelPrefix,
                          bool isRight,
                          PresetValidationReport& report) {
    outOverflow.clear();
    const size_t total = static_cast<size_t>(arr.size());
    const size_t active = std::min(total, ParametricEQ::BANDS_PER_CHANNEL);
    for (size_t i = 0; i < active; ++i) {
        if (arr.at(static_cast<qsizetype>(i)).isObject()) {
            outBands[i] = parsePeqBand(arr.at(static_cast<qsizetype>(i)).toObject(),
                                       i,
                                       QStringLiteral("%1[%2]").arg(channelPrefix).arg(i),
                                       report);
        } else {
            report.warnings.append(QStringLiteral("Non-object PEQ band at '%1[%2]'; defaulted to OFF")
                                       .arg(channelPrefix)
                                       .arg(i));
        }
    }
    if (total > ParametricEQ::BANDS_PER_CHANNEL) {
        outOverflow.reserve(total - ParametricEQ::BANDS_PER_CHANNEL);
        for (size_t i = ParametricEQ::BANDS_PER_CHANNEL; i < total; ++i) {
            if (arr.at(static_cast<qsizetype>(i)).isObject()) {
                outOverflow.push_back(parsePeqBand(arr.at(static_cast<qsizetype>(i)).toObject(),
                                                   i,
                                                   QStringLiteral("%1[%2]").arg(channelPrefix).arg(i),
                                                   report));
            }
        }
        if (isRight) {
            report.peqOverflowRight = outOverflow.size();
        } else {
            report.peqOverflowLeft = outOverflow.size();
        }
        report.warnings.append(
            QStringLiteral("PEQ %1 channel exceeded 16 bands (%2 total); preserved %3 overflow band(s)")
                .arg(isRight ? QStringLiteral("right") : QStringLiteral("left"))
                .arg(total)
                .arg(outOverflow.size()));
    }
}

PeqPresetData parsePeq(const QJsonObject& obj,
                       const QString& prefix,
                       PresetValidationReport& report,
                       bool* outNodeEnabled) {
    PeqPresetData d = PeqPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else if (obj.contains(QStringLiteral("bypassed")) && obj.value(QStringLiteral("bypassed")).isBool()) {
            *outNodeEnabled = !obj.value(QStringLiteral("bypassed")).toBool(true);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }

    d.preampDb = readClampedDouble(obj, QStringLiteral("preamp_db"), 0.0, -24.0, 24.0,
                                   prefix + QStringLiteral(".preamp_db"), report,
                                   QStringLiteral("preampDb"));

    // Standard v2 shape: left: { bands: [...] }, right: { bands: [...] }
    // Also supports left: [...] / left_bands: [...] for compatibility
    auto extractChannelArray = [&](const QString& chKey, const QString& legacyKey, bool& found) -> QJsonArray {
        found = false;
        if (obj.contains(chKey)) {
            const QJsonValue v = obj.value(chKey);
            if (v.isObject() && v.toObject().contains(QStringLiteral("bands")) &&
                v.toObject().value(QStringLiteral("bands")).isArray()) {
                found = true;
                return v.toObject().value(QStringLiteral("bands")).toArray();
            }
            if (v.isArray()) {
                found = true;
                return v.toArray();
            }
        }
        if (obj.contains(legacyKey) && obj.value(legacyKey).isArray()) {
            found = true;
            return obj.value(legacyKey).toArray();
        }
        return {};
    };

    bool foundL = false;
    bool foundR = false;
    const QJsonArray leftArr = extractChannelArray(QStringLiteral("left"), QStringLiteral("left_bands"), foundL);
    const QJsonArray rightArr = extractChannelArray(QStringLiteral("right"), QStringLiteral("right_bands"), foundR);

    if (foundL) {
        parsePeqChannelBands(leftArr, d.leftBands, d.overflowLeft, prefix + QStringLiteral(".left.bands"), false, report);
    } else {
        report.missingDefaultedFields.append(prefix + QStringLiteral(".left.bands"));
    }
    if (foundR) {
        parsePeqChannelBands(rightArr, d.rightBands, d.overflowRight, prefix + QStringLiteral(".right.bands"), true, report);
    } else {
        report.missingDefaultedFields.append(prefix + QStringLiteral(".right.bands"));
    }

    static const QSet<QString> kKnown = {
        QStringLiteral("left"), QStringLiteral("right"),
        QStringLiteral("left_bands"), QStringLiteral("right_bands"),
        QStringLiteral("preamp_db"), QStringLiteral("preampDb"), QStringLiteral("preamp"),
        QStringLiteral("enabled"), QStringLiteral("bypassed")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

GeqPresetData parseGeq(const QJsonObject& obj,
                       const QString& prefix,
                       PresetValidationReport& report,
                       bool* outNodeEnabled) {
    GeqPresetData d = GeqPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else if (obj.contains(QStringLiteral("bypassed")) && obj.value(QStringLiteral("bypassed")).isBool()) {
            *outNodeEnabled = !obj.value(QStringLiteral("bypassed")).toBool(true);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }

    QString gainsKey;
    if (obj.contains(QStringLiteral("gains_db")) && obj.value(QStringLiteral("gains_db")).isArray()) {
        gainsKey = QStringLiteral("gains_db");
    } else if (obj.contains(QStringLiteral("gains")) && obj.value(QStringLiteral("gains")).isArray()) {
        gainsKey = QStringLiteral("gains");
    }

    if (!gainsKey.isEmpty()) {
        const QJsonArray arr = obj.value(gainsKey).toArray();
        const size_t n = std::min<size_t>(static_cast<size_t>(arr.size()), GraphicEQ::NUM_BANDS);
        for (size_t i = 0; i < n; ++i) {
            const QJsonValue v = arr.at(static_cast<qsizetype>(i));
            const QString fp = QStringLiteral("%1.gains_db[%2]").arg(prefix).arg(i);
            if (!v.isDouble() || !std::isfinite(v.toDouble(0.0))) {
                report.warnings.append(QStringLiteral("Non-numeric GEQ gain at '%1'; defaulted to 0.0").arg(fp));
                d.gainsDb[i] = 0.0;
                continue;
            }
            const double raw = v.toDouble(0.0);
            if (raw < -12.0 || raw > 12.0) {
                const double clamped = std::clamp(raw, -12.0, 12.0);
                report.clampedFields.append(QStringLiteral("%1 (%2 -> %3)").arg(fp).arg(raw).arg(clamped));
                report.warnings.append(QStringLiteral("Clamped '%1' from %2 to %3 [-12, 12]").arg(fp).arg(raw).arg(clamped));
                d.gainsDb[i] = clamped;
            } else {
                d.gainsDb[i] = raw;
            }
        }
        if (static_cast<size_t>(arr.size()) > GraphicEQ::NUM_BANDS) {
            report.warnings.append(
                QStringLiteral("GEQ gains_db exceeded 32 bands (%1); active DSP uses first 32").arg(arr.size()));
        }
    } else {
        report.missingDefaultedFields.append(prefix + QStringLiteral(".gains_db"));
    }

    if (obj.contains(QStringLiteral("band_enabled")) && obj.value(QStringLiteral("band_enabled")).isArray()) {
        const QJsonArray arr = obj.value(QStringLiteral("band_enabled")).toArray();
        const size_t n = std::min<size_t>(static_cast<size_t>(arr.size()), GraphicEQ::NUM_BANDS);
        for (size_t i = 0; i < n; ++i) {
            const QJsonValue v = arr.at(static_cast<qsizetype>(i));
            d.bandEnabled[i] = v.isBool() ? v.toBool(true) : true;
        }
    } else {
        report.missingDefaultedFields.append(prefix + QStringLiteral(".band_enabled"));
    }

    static const QSet<QString> kKnown = {
        QStringLiteral("gains_db"), QStringLiteral("gains"),
        QStringLiteral("band_enabled"), QStringLiteral("enabled"), QStringLiteral("bypassed")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

HpfPresetData parseHpf(const QJsonObject& obj,
                       const QString& prefix,
                       PresetValidationReport& report,
                       bool* outNodeEnabled) {
    HpfPresetData d = HpfPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }
    d.cutoffHz = readClampedDouble(obj, QStringLiteral("cutoff_hz"), 20.0, 10.0, 1000.0,
                                   prefix + QStringLiteral(".cutoff_hz"), report,
                                   QStringLiteral("hpf_frequency"));
    d.q = readClampedDouble(obj, QStringLiteral("q"), 0.71, 0.1, 10.0,
                            prefix + QStringLiteral(".q"), report,
                            QStringLiteral("hpf_q"));
    d.slopeDbOct = readSlopeDbOct(obj, QStringLiteral("slope_db_oct"), 12,
                                  prefix + QStringLiteral(".slope_db_oct"), report,
                                  QStringLiteral("hpf_slope"));

    static const QSet<QString> kKnown = {
        QStringLiteral("cutoff_hz"), QStringLiteral("hpf_frequency"),
        QStringLiteral("q"), QStringLiteral("hpf_q"),
        QStringLiteral("slope_db_oct"), QStringLiteral("hpf_slope"),
        QStringLiteral("enabled")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

LpfPresetData parseLpf(const QJsonObject& obj,
                       const QString& prefix,
                       PresetValidationReport& report,
                       bool* outNodeEnabled) {
    LpfPresetData d = LpfPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }
    d.cutoffHz = readClampedDouble(obj, QStringLiteral("cutoff_hz"), 20000.0, 1000.0, 22000.0,
                                   prefix + QStringLiteral(".cutoff_hz"), report,
                                   QStringLiteral("lpf_frequency"));
    d.q = readClampedDouble(obj, QStringLiteral("q"), 0.71, 0.1, 10.0,
                            prefix + QStringLiteral(".q"), report,
                            QStringLiteral("lpf_q"));
    d.slopeDbOct = readSlopeDbOct(obj, QStringLiteral("slope_db_oct"), 12,
                                  prefix + QStringLiteral(".slope_db_oct"), report,
                                  QStringLiteral("lpf_slope"));

    static const QSet<QString> kKnown = {
        QStringLiteral("cutoff_hz"), QStringLiteral("lpf_frequency"),
        QStringLiteral("q"), QStringLiteral("lpf_q"),
        QStringLiteral("slope_db_oct"), QStringLiteral("lpf_slope"),
        QStringLiteral("enabled")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

CompressorPresetData parseCompressor(const QJsonObject& obj,
                                     const QString& prefix,
                                     PresetValidationReport& report,
                                     bool* outNodeEnabled) {
    CompressorPresetData d = CompressorPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else if (obj.contains(QStringLiteral("bypassed")) && obj.value(QStringLiteral("bypassed")).isBool()) {
            *outNodeEnabled = !obj.value(QStringLiteral("bypassed")).toBool(true);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }
    d.thresholdDb = readClampedDouble(obj, QStringLiteral("threshold_db"), 0.0, -60.0, 0.0,
                                      prefix + QStringLiteral(".threshold_db"), report,
                                      QStringLiteral("thresholdDb"));
    d.ratio = readClampedDouble(obj, QStringLiteral("ratio"), 1.0, 1.0, 20.0,
                                prefix + QStringLiteral(".ratio"), report);
    d.attackMs = readClampedDouble(obj, QStringLiteral("attack_ms"), 10.0, 0.1, 500.0,
                                   prefix + QStringLiteral(".attack_ms"), report,
                                   QStringLiteral("attackMs"));
    d.releaseMs = readClampedDouble(obj, QStringLiteral("release_ms"), 100.0, 1.0, 1000.0,
                                    prefix + QStringLiteral(".release_ms"), report,
                                    QStringLiteral("releaseMs"));
    d.kneeDb = readClampedDouble(obj, QStringLiteral("knee_db"), 0.0, 0.0, 20.0,
                                 prefix + QStringLiteral(".knee_db"), report,
                                 QStringLiteral("kneeDb"));
    d.makeupDb = readClampedDouble(obj, QStringLiteral("makeup_db"), 0.0, -12.0, 24.0,
                                   prefix + QStringLiteral(".makeup_db"), report,
                                   QStringLiteral("makeupGainDb"));

    static const QSet<QString> kKnown = {
        QStringLiteral("threshold_db"), QStringLiteral("thresholdDb"),
        QStringLiteral("ratio"),
        QStringLiteral("attack_ms"), QStringLiteral("attackMs"),
        QStringLiteral("release_ms"), QStringLiteral("releaseMs"),
        QStringLiteral("knee_db"), QStringLiteral("kneeDb"),
        QStringLiteral("makeup_db"), QStringLiteral("makeupGainDb"),
        QStringLiteral("enabled"), QStringLiteral("bypassed")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

LimiterPresetData parseLimiter(const QJsonObject& obj,
                               const QString& prefix,
                               PresetValidationReport& report,
                               bool* outNodeEnabled) {
    LimiterPresetData d = LimiterPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else if (obj.contains(QStringLiteral("bypassed")) && obj.value(QStringLiteral("bypassed")).isBool()) {
            *outNodeEnabled = !obj.value(QStringLiteral("bypassed")).toBool(true);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }
    d.ceilingDbfs = readClampedDouble(obj, QStringLiteral("ceiling_dbfs"), 0.0, -12.0, 0.0,
                                      prefix + QStringLiteral(".ceiling_dbfs"), report,
                                      QStringLiteral("ceiling_db"));
    d.releaseMs = readClampedDouble(obj, QStringLiteral("release_ms"), 50.0, 10.0, 500.0,
                                    prefix + QStringLiteral(".release_ms"), report,
                                    QStringLiteral("releaseMs"));

    static const QSet<QString> kKnown = {
        QStringLiteral("ceiling_dbfs"), QStringLiteral("ceiling_db"), QStringLiteral("ceilingDb"),
        QStringLiteral("release_ms"), QStringLiteral("releaseMs"),
        QStringLiteral("enabled"), QStringLiteral("bypassed")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

ReverbPresetData parseReverb(const QJsonObject& obj,
                             const QString& prefix,
                             PresetValidationReport& report,
                             bool* outNodeEnabled) {
    ReverbPresetData d = ReverbPresetData::unity();
    if (outNodeEnabled) {
        if (obj.contains(QStringLiteral("enabled")) && obj.value(QStringLiteral("enabled")).isBool()) {
            *outNodeEnabled = obj.value(QStringLiteral("enabled")).toBool(false);
        } else if (obj.contains(QStringLiteral("bypassed")) && obj.value(QStringLiteral("bypassed")).isBool()) {
            *outNodeEnabled = !obj.value(QStringLiteral("bypassed")).toBool(true);
        } else {
            *outNodeEnabled = false;
            report.missingDefaultedFields.append(prefix + QStringLiteral(".enabled"));
        }
    }

    d.size = readClampedDouble(obj, QStringLiteral("size"), 0.10, 0.0, 1.0,
                               prefix + QStringLiteral(".size"), report,
                               QStringLiteral("room_size"));

    if (obj.contains(QStringLiteral("decay_s"))) {
        d.decayS = readClampedDouble(obj, QStringLiteral("decay_s"), 0.2, 0.1, 10.0,
                                     prefix + QStringLiteral(".decay_s"), report);
    } else if (obj.contains(QStringLiteral("room_size"))) {
        // Legacy v1 migration: approximate decay_s from room_size
        d.decayS = std::clamp(0.2 + d.size * 3.0, 0.1, 10.0);
        report.warnings.append(QStringLiteral("Migrated legacy reverb room_size=%1 -> decay_s=%2")
                                   .arg(d.size)
                                   .arg(d.decayS));
    } else {
        report.missingDefaultedFields.append(prefix + QStringLiteral(".decay_s"));
        d.decayS = 0.2;
    }

    d.predelayMs = readClampedDouble(obj, QStringLiteral("predelay_ms"), 0.0, 0.0, 200.0,
                                     prefix + QStringLiteral(".predelay_ms"), report,
                                     QStringLiteral("predelayMs"));
    d.damping = readClampedDouble(obj, QStringLiteral("damping"), 0.50, 0.0, 1.0,
                                  prefix + QStringLiteral(".damping"), report);

    if (obj.contains(QStringLiteral("wet_dry_pct"))) {
        d.wetDryPct = readClampedDouble(obj, QStringLiteral("wet_dry_pct"), 0.0, 0.0, 100.0,
                                        prefix + QStringLiteral(".wet_dry_pct"), report);
    } else if (obj.contains(QStringLiteral("wet_level")) && obj.value(QStringLiteral("wet_level")).isDouble()) {
        const double rawWet = obj.value(QStringLiteral("wet_level")).toDouble(0.0);
        d.wetDryPct = std::clamp(rawWet * 100.0, 0.0, 100.0);
    } else {
        report.missingDefaultedFields.append(prefix + QStringLiteral(".wet_dry_pct"));
        d.wetDryPct = 0.0;
    }

    static const QSet<QString> kKnown = {
        QStringLiteral("decay_s"),
        QStringLiteral("predelay_ms"), QStringLiteral("predelayMs"),
        QStringLiteral("size"), QStringLiteral("room_size"), QStringLiteral("roomSize"),
        QStringLiteral("damping"),
        QStringLiteral("wet_dry_pct"), QStringLiteral("wet_level"), QStringLiteral("wetLevel"),
        QStringLiteral("enabled"), QStringLiteral("bypassed")
    };
    collectUnknownKeys(obj, kKnown, prefix, d.unknownFields, report);
    return d;
}

bool parseLegacyV1MasterDsp(const QJsonObject& obj,
                            DspPresetDocument& outDoc,
                            PresetValidationReport& outReport) {
    outReport.migratedFromLegacy = true;
    outReport.sourceVersion = 1;
    outDoc = DspPresetDocument{};
    outDoc.format = QString::fromLatin1(kDspPresetFormat);
    outDoc.version = kDspPresetCurrentVersion;
    outDoc.kind = DspPresetKind::Global;
    outDoc.scope = DspPresetScope::Chain;
    outDoc.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Migrated Preset"));
    outDoc.id = obj.value(QStringLiteral("id")).toString();
    if (outDoc.id.isEmpty()) {
        outDoc.id = QStringLiteral("migrated.v1.") + outDoc.name.toLower().replace(QLatin1Char(' '), QLatin1Char('_'));
    }
    outDoc.createdAt = obj.value(QStringLiteral("created_at")).toString(
        obj.value(QStringLiteral("saved_at")).toString());
    outDoc.modifiedAt = obj.value(QStringLiteral("modified_at")).toString(
        obj.value(QStringLiteral("saved_at")).toString(outDoc.createdAt));
    outDoc.isFactory = obj.value(QStringLiteral("is_factory")).toBool(false);
    outDoc.global = GlobalDspPresetData::unity();

    if (obj.contains(QStringLiteral("peq")) && obj.value(QStringLiteral("peq")).isObject()) {
        bool en = false;
        outDoc.global.peq = parsePeq(obj.value(QStringLiteral("peq")).toObject(),
                                     QStringLiteral("dsp.peq"), outReport, &en);
        outDoc.global.setNodeEnabled(DspPresetTarget::Peq, en);
    }
    if (obj.contains(QStringLiteral("geq")) && obj.value(QStringLiteral("geq")).isObject()) {
        bool en = false;
        outDoc.global.geq = parseGeq(obj.value(QStringLiteral("geq")).toObject(),
                                     QStringLiteral("dsp.geq"), outReport, &en);
        outDoc.global.setNodeEnabled(DspPresetTarget::Geq, en);
    }
    if (obj.contains(QStringLiteral("filter")) && obj.value(QStringLiteral("filter")).isObject()) {
        const QJsonObject flt = obj.value(QStringLiteral("filter")).toObject();
        const bool fltNodeOn = flt.contains(QStringLiteral("enabled"))
                                   ? flt.value(QStringLiteral("enabled")).toBool(true)
                                   : !flt.value(QStringLiteral("bypassed")).toBool(false);
        const bool hpfOn = fltNodeOn && flt.value(QStringLiteral("hpf_enabled")).toBool(false);
        const bool lpfOn = fltNodeOn && flt.value(QStringLiteral("lpf_enabled")).toBool(false);

        QJsonObject hpfObj;
        hpfObj.insert(QStringLiteral("enabled"), hpfOn);
        if (flt.contains(QStringLiteral("hpf_frequency"))) hpfObj.insert(QStringLiteral("hpf_frequency"), flt.value(QStringLiteral("hpf_frequency")));
        if (flt.contains(QStringLiteral("hpf_q"))) hpfObj.insert(QStringLiteral("hpf_q"), flt.value(QStringLiteral("hpf_q")));
        if (flt.contains(QStringLiteral("hpf_slope"))) hpfObj.insert(QStringLiteral("hpf_slope"), flt.value(QStringLiteral("hpf_slope")));
        bool he = false;
        outDoc.global.hpf = parseHpf(hpfObj, QStringLiteral("dsp.hpf"), outReport, &he);
        outDoc.global.setNodeEnabled(DspPresetTarget::Hpf, he);

        QJsonObject lpfObj;
        lpfObj.insert(QStringLiteral("enabled"), lpfOn);
        if (flt.contains(QStringLiteral("lpf_frequency"))) lpfObj.insert(QStringLiteral("lpf_frequency"), flt.value(QStringLiteral("lpf_frequency")));
        if (flt.contains(QStringLiteral("lpf_q"))) lpfObj.insert(QStringLiteral("lpf_q"), flt.value(QStringLiteral("lpf_q")));
        if (flt.contains(QStringLiteral("lpf_slope"))) lpfObj.insert(QStringLiteral("lpf_slope"), flt.value(QStringLiteral("lpf_slope")));
        bool le = false;
        outDoc.global.lpf = parseLpf(lpfObj, QStringLiteral("dsp.lpf"), outReport, &le);
        outDoc.global.setNodeEnabled(DspPresetTarget::Lpf, le);
    }
    if (obj.contains(QStringLiteral("compressor")) && obj.value(QStringLiteral("compressor")).isObject()) {
        bool en = false;
        outDoc.global.compressor = parseCompressor(obj.value(QStringLiteral("compressor")).toObject(),
                                                   QStringLiteral("dsp.compressor"), outReport, &en);
        outDoc.global.setNodeEnabled(DspPresetTarget::Compressor, en);
    }
    if (obj.contains(QStringLiteral("limiter")) && obj.value(QStringLiteral("limiter")).isObject()) {
        bool en = false;
        outDoc.global.limiter = parseLimiter(obj.value(QStringLiteral("limiter")).toObject(),
                                             QStringLiteral("dsp.limiter"), outReport, &en);
        outDoc.global.setNodeEnabled(DspPresetTarget::Limiter, en);
    }
    if (obj.contains(QStringLiteral("reverb")) && obj.value(QStringLiteral("reverb")).isObject()) {
        bool en = false;
        outDoc.global.reverb = parseReverb(obj.value(QStringLiteral("reverb")).toObject(),
                                           QStringLiteral("dsp.reverb"), outReport, &en);
        outDoc.global.setNodeEnabled(DspPresetTarget::Reverb, en);
    }

    outReport.warnings.append(QStringLiteral("Migrated legacy v1 master_dsp preset format to v2"));
    outReport.ok = true;
    return true;
}

} // namespace

QJsonObject DspPresetSerializer::peqToJson(const PeqPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    obj.insert(QStringLiteral("preamp_db"), d.preampDb);

    auto buildSide = [](const std::array<PeqBandPreset, ParametricEQ::BANDS_PER_CHANNEL>& bands,
                        const std::vector<PeqBandPreset>& overflow) {
        QJsonArray arr;
        for (const auto& b : bands) {
            arr.append(peqBandToJson(b));
        }
        for (const auto& b : overflow) {
            arr.append(peqBandToJson(b));
        }
        QJsonObject side;
        side.insert(QStringLiteral("bands"), arr);
        return side;
    };

    obj.insert(QStringLiteral("left"), buildSide(d.leftBands, d.overflowLeft));
    obj.insert(QStringLiteral("right"), buildSide(d.rightBands, d.overflowRight));
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::geqToJson(const GeqPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    QJsonArray gains;
    QJsonArray enabled;
    for (size_t i = 0; i < GraphicEQ::NUM_BANDS; ++i) {
        gains.append(d.gainsDb[i]);
        enabled.append(d.bandEnabled[i]);
    }
    obj.insert(QStringLiteral("gains_db"), gains);
    obj.insert(QStringLiteral("band_enabled"), enabled);
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::hpfToJson(const HpfPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    obj.insert(QStringLiteral("cutoff_hz"), d.cutoffHz);
    obj.insert(QStringLiteral("q"), d.q);
    obj.insert(QStringLiteral("slope_db_oct"), d.slopeDbOct);
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::lpfToJson(const LpfPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    obj.insert(QStringLiteral("cutoff_hz"), d.cutoffHz);
    obj.insert(QStringLiteral("q"), d.q);
    obj.insert(QStringLiteral("slope_db_oct"), d.slopeDbOct);
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::compressorToJson(const CompressorPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    obj.insert(QStringLiteral("threshold_db"), d.thresholdDb);
    obj.insert(QStringLiteral("ratio"), d.ratio);
    obj.insert(QStringLiteral("attack_ms"), d.attackMs);
    obj.insert(QStringLiteral("release_ms"), d.releaseMs);
    obj.insert(QStringLiteral("knee_db"), d.kneeDb);
    obj.insert(QStringLiteral("makeup_db"), d.makeupDb);
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::limiterToJson(const LimiterPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    obj.insert(QStringLiteral("ceiling_dbfs"), d.ceilingDbfs);
    obj.insert(QStringLiteral("release_ms"), d.releaseMs);
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::reverbToJson(const ReverbPresetData& d, const bool* nodeEnabled) {
    QJsonObject obj;
    if (nodeEnabled != nullptr) {
        obj.insert(QStringLiteral("enabled"), *nodeEnabled);
    }
    obj.insert(QStringLiteral("decay_s"), d.decayS);
    obj.insert(QStringLiteral("predelay_ms"), d.predelayMs);
    obj.insert(QStringLiteral("size"), d.size);
    obj.insert(QStringLiteral("damping"), d.damping);
    obj.insert(QStringLiteral("wet_dry_pct"), d.wetDryPct);
    mergeUnknownKeys(obj, d.unknownFields);
    return obj;
}

QJsonObject DspPresetSerializer::toJson(const DspPresetDocument& doc) {
    QJsonObject root;
    root.insert(QStringLiteral("format"),
                doc.format.isEmpty() ? QString::fromLatin1(kDspPresetFormat) : doc.format);
    root.insert(QStringLiteral("version"), doc.version > 0 ? doc.version : kDspPresetCurrentVersion);
    root.insert(QStringLiteral("kind"), dspKindToString(doc.kind));
    root.insert(QStringLiteral("id"), doc.id);
    root.insert(QStringLiteral("name"), doc.name);

    const DspPresetScope effectiveScope =
        (doc.kind == DspPresetKind::PerDsp && doc.target == DspPresetTarget::Peq)
            ? DspPresetScope::MasteringChain
            : doc.scope;
    root.insert(QStringLiteral("scope"), dspScopeToString(effectiveScope));
    root.insert(QStringLiteral("target"),
                doc.kind == DspPresetKind::Global ? QStringLiteral("all") : dspTargetToString(doc.target));

    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root.insert(QStringLiteral("created_at"), doc.createdAt.isEmpty() ? nowIso : doc.createdAt);
    root.insert(QStringLiteral("modified_at"), doc.modifiedAt.isEmpty() ? nowIso : doc.modifiedAt);
    root.insert(QStringLiteral("is_factory"), doc.isFactory);

    if (doc.kind == DspPresetKind::Global) {
        QJsonObject dspObj;
        const bool peqOn = doc.global.isNodeEnabled(DspPresetTarget::Peq);
        const bool geqOn = doc.global.isNodeEnabled(DspPresetTarget::Geq);
        const bool hpfOn = doc.global.isNodeEnabled(DspPresetTarget::Hpf);
        const bool lpfOn = doc.global.isNodeEnabled(DspPresetTarget::Lpf);
        const bool compOn = doc.global.isNodeEnabled(DspPresetTarget::Compressor);
        const bool limOn = doc.global.isNodeEnabled(DspPresetTarget::Limiter);
        const bool revOn = doc.global.isNodeEnabled(DspPresetTarget::Reverb);

        dspObj.insert(QStringLiteral("peq"), peqToJson(doc.global.peq, &peqOn));
        dspObj.insert(QStringLiteral("geq"), geqToJson(doc.global.geq, &geqOn));
        dspObj.insert(QStringLiteral("hpf"), hpfToJson(doc.global.hpf, &hpfOn));
        dspObj.insert(QStringLiteral("lpf"), lpfToJson(doc.global.lpf, &lpfOn));
        dspObj.insert(QStringLiteral("compressor"), compressorToJson(doc.global.compressor, &compOn));
        dspObj.insert(QStringLiteral("limiter"), limiterToJson(doc.global.limiter, &limOn));
        dspObj.insert(QStringLiteral("reverb"), reverbToJson(doc.global.reverb, &revOn));
        mergeUnknownKeys(dspObj, doc.global.unknownDspKeys);
        root.insert(QStringLiteral("dsp"), dspObj);
    } else {
        QJsonObject dspObj;
        switch (doc.target) {
        case DspPresetTarget::Peq: dspObj = peqToJson(doc.global.peq, nullptr); break;
        case DspPresetTarget::Geq: dspObj = geqToJson(doc.global.geq, nullptr); break;
        case DspPresetTarget::Hpf: dspObj = hpfToJson(doc.global.hpf, nullptr); break;
        case DspPresetTarget::Lpf: dspObj = lpfToJson(doc.global.lpf, nullptr); break;
        case DspPresetTarget::Compressor: dspObj = compressorToJson(doc.global.compressor, nullptr); break;
        case DspPresetTarget::Limiter: dspObj = limiterToJson(doc.global.limiter, nullptr); break;
        case DspPresetTarget::Reverb: dspObj = reverbToJson(doc.global.reverb, nullptr); break;
        case DspPresetTarget::Count: break;
        }
        root.insert(QStringLiteral("dsp"), dspObj);
    }

    mergeUnknownKeys(root, doc.unknownTopLevelKeys);
    return root;
}

QByteArray DspPresetSerializer::toJsonBytes(const DspPresetDocument& doc) {
    return QJsonDocument(toJson(doc)).toJson(QJsonDocument::Indented);
}

bool DspPresetSerializer::fromJsonBytes(const QByteArray& bytes,
                                        DspPresetDocument& outDoc,
                                        PresetValidationReport& outReport) {
    outReport = PresetValidationReport{};
    QJsonParseError err{};
    const QJsonDocument jdoc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !jdoc.isObject()) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Invalid preset JSON: %1").arg(err.errorString());
        return false;
    }
    return fromJson(jdoc.object(), outDoc, outReport);
}

bool DspPresetSerializer::fromJson(const QJsonObject& obj,
                                   DspPresetDocument& outDoc,
                                   PresetValidationReport& outReport) {
    outReport = PresetValidationReport{};
    outDoc = DspPresetDocument{};

    // Legacy v1 master_dsp detection (category == "master_dsp" without v2 format)
    const QString fmt = obj.value(QStringLiteral("format")).toString();
    const bool looksLegacyV1 =
        fmt.isEmpty() && obj.value(QStringLiteral("category")).toString() == QLatin1String("master_dsp");
    if (looksLegacyV1) {
        const int v1Ver = obj.contains(QStringLiteral("format_version"))
                              ? obj.value(QStringLiteral("format_version")).toInt(-1)
                              : obj.value(QStringLiteral("version")).toInt(1);
        if (v1Ver != 1) {
            outReport.ok = false;
            outReport.sourceVersion = v1Ver;
            outReport.errorMessage = QStringLiteral("Unsupported legacy preset version: %1").arg(v1Ver);
            return false;
        }
        return parseLegacyV1MasterDsp(obj, outDoc, outReport);
    }

    // Format validation
    if (fmt != QLatin1String(kDspPresetFormat)) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Unsupported preset format: '%1' (expected '%2')")
                                     .arg(fmt, QLatin1String(kDspPresetFormat));
        return false;
    }

    // Version validation
    if (!obj.contains(QStringLiteral("version")) || !obj.value(QStringLiteral("version")).isDouble()) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Missing or non-numeric preset 'version' field");
        return false;
    }
    const int ver = obj.value(QStringLiteral("version")).toInt(-1);
    outReport.sourceVersion = ver;
    if (ver != kDspPresetCurrentVersion) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Unsupported preset version %1 (supported: %2)")
                                     .arg(ver)
                                     .arg(kDspPresetCurrentVersion);
        return false;
    }

    // Kind validation
    const QString kindStr = obj.value(QStringLiteral("kind")).toString().trimmed().toLower();
    if (kindStr == QLatin1String("global")) {
        outDoc.kind = DspPresetKind::Global;
    } else if (kindStr == QLatin1String("per-dsp")) {
        outDoc.kind = DspPresetKind::PerDsp;
    } else {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Invalid preset kind: '%1' (expected 'global' or 'per-dsp')")
                                     .arg(kindStr);
        return false;
    }

    // Scope validation
    DspPresetScope parsedScope = DspPresetScope::Chain;
    const QString scopeStr = obj.value(QStringLiteral("scope")).toString();
    if (!dspScopeFromString(scopeStr, parsedScope)) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Invalid preset scope: '%1'").arg(scopeStr);
        return false;
    }
    if (parsedScope == DspPresetScope::ListeningCorrection) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral(
            "Preset scope 'listening-correction' cannot be loaded as a Mastering Chain DSP preset (§11.1)");
        return false;
    }
    outDoc.scope = parsedScope;

    // Target validation
    const QString targetStr = obj.value(QStringLiteral("target")).toString().trimmed().toLower();
    if (outDoc.kind == DspPresetKind::Global) {
        if (targetStr != QLatin1String("all")) {
            outReport.ok = false;
            outReport.errorMessage = QStringLiteral("Global preset must have target 'all', got '%1'").arg(targetStr);
            return false;
        }
        if (outDoc.scope != DspPresetScope::Chain) {
            outReport.ok = false;
            outReport.errorMessage = QStringLiteral("Global preset must have scope 'chain', got '%1'").arg(scopeStr);
            return false;
        }
    } else {
        DspPresetTarget t = DspPresetTarget::Peq;
        if (!dspTargetFromString(targetStr, t)) {
            outReport.ok = false;
            outReport.errorMessage = QStringLiteral("Unknown per-dsp target: '%1'").arg(targetStr);
            return false;
        }
        outDoc.target = t;
        if (t == DspPresetTarget::Peq && outDoc.scope != DspPresetScope::MasteringChain) {
            outReport.ok = false;
            outReport.errorMessage = QStringLiteral(
                "Per-DSP PEQ preset must have scope 'mastering-chain', got '%1'").arg(scopeStr);
            return false;
        }
        if (t != DspPresetTarget::Peq && outDoc.scope != DspPresetScope::Chain) {
            outReport.ok = false;
            outReport.errorMessage = QStringLiteral(
                "Per-DSP '%1' preset must have scope 'chain', got '%2'").arg(targetStr, scopeStr);
            return false;
        }
    }

    // Metadata
    outDoc.format = fmt;
    outDoc.version = ver;
    outDoc.name = obj.value(QStringLiteral("name")).toString();
    outDoc.id = obj.value(QStringLiteral("id")).toString();
    if (outDoc.id.isEmpty()) {
        outDoc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        outReport.missingDefaultedFields.append(QStringLiteral("id"));
    }
    // Spec v3.8.3 writes created_at / modified_at; accept legacy created / modified fallback on read
    if (obj.contains(QStringLiteral("created_at"))) {
        outDoc.createdAt = obj.value(QStringLiteral("created_at")).toString();
    } else if (obj.contains(QStringLiteral("created"))) {
        outDoc.createdAt = obj.value(QStringLiteral("created")).toString();
    }
    if (obj.contains(QStringLiteral("modified_at"))) {
        outDoc.modifiedAt = obj.value(QStringLiteral("modified_at")).toString();
    } else if (obj.contains(QStringLiteral("modified"))) {
        outDoc.modifiedAt = obj.value(QStringLiteral("modified")).toString();
    }
    outDoc.isFactory = obj.value(QStringLiteral("is_factory")).toBool(false);

    // DSP payload validation
    if (!obj.contains(QStringLiteral("dsp")) || !obj.value(QStringLiteral("dsp")).isObject()) {
        outReport.ok = false;
        outReport.errorMessage = QStringLiteral("Missing or non-object 'dsp' payload");
        return false;
    }
    const QJsonObject dspObj = obj.value(QStringLiteral("dsp")).toObject();
    outDoc.global = GlobalDspPresetData::unity();

    if (outDoc.kind == DspPresetKind::Global) {
        auto parseSub = [&](const QString& key, DspPresetTarget target, auto&& fn) {
            if (dspObj.contains(key) && dspObj.value(key).isObject()) {
                bool en = false;
                fn(dspObj.value(key).toObject(), QStringLiteral("dsp.") + key, outReport, &en);
                outDoc.global.setNodeEnabled(target, en);
            } else {
                outReport.missingDefaultedFields.append(QStringLiteral("dsp.") + key);
                outDoc.global.setNodeEnabled(target, false);
            }
        };

        parseSub(QStringLiteral("peq"), DspPresetTarget::Peq,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.peq = parsePeq(o, p, r, en);
                 });
        parseSub(QStringLiteral("geq"), DspPresetTarget::Geq,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.geq = parseGeq(o, p, r, en);
                 });
        parseSub(QStringLiteral("hpf"), DspPresetTarget::Hpf,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.hpf = parseHpf(o, p, r, en);
                 });
        parseSub(QStringLiteral("lpf"), DspPresetTarget::Lpf,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.lpf = parseLpf(o, p, r, en);
                 });
        parseSub(QStringLiteral("compressor"), DspPresetTarget::Compressor,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.compressor = parseCompressor(o, p, r, en);
                 });
        parseSub(QStringLiteral("limiter"), DspPresetTarget::Limiter,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.limiter = parseLimiter(o, p, r, en);
                 });
        parseSub(QStringLiteral("reverb"), DspPresetTarget::Reverb,
                 [&](const QJsonObject& o, const QString& p, PresetValidationReport& r, bool* en) {
                     outDoc.global.reverb = parseReverb(o, p, r, en);
                 });

        static const QSet<QString> kKnownGlobalDspKeys = {
            QStringLiteral("peq"), QStringLiteral("geq"), QStringLiteral("hpf"),
            QStringLiteral("lpf"), QStringLiteral("compressor"), QStringLiteral("limiter"),
            QStringLiteral("reverb")
        };
        collectUnknownKeys(dspObj, kKnownGlobalDspKeys, QStringLiteral("dsp"),
                           outDoc.global.unknownDspKeys, outReport);
    } else {
        const QString pfx = QStringLiteral("dsp");
        switch (outDoc.target) {
        case DspPresetTarget::Peq:
            outDoc.global.peq = parsePeq(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Geq:
            outDoc.global.geq = parseGeq(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Hpf:
            outDoc.global.hpf = parseHpf(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Lpf:
            outDoc.global.lpf = parseLpf(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Compressor:
            outDoc.global.compressor = parseCompressor(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Limiter:
            outDoc.global.limiter = parseLimiter(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Reverb:
            outDoc.global.reverb = parseReverb(dspObj, pfx, outReport, nullptr);
            break;
        case DspPresetTarget::Count:
            break;
        }
    }

    static const QSet<QString> kKnownTopKeys = {
        QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("kind"),
        QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("scope"),
        QStringLiteral("target"), QStringLiteral("created_at"), QStringLiteral("modified_at"),
        QStringLiteral("created"), QStringLiteral("modified"), QStringLiteral("is_factory"),
        QStringLiteral("dsp")
    };
    collectUnknownKeys(obj, kKnownTopKeys, QString(), outDoc.unknownTopLevelKeys, outReport);

    outReport.ok = true;
    return true;
}

bool DspPresetSerializer::isPeqEqual(const PeqPresetData& a, const PeqPresetData& b, double eps) noexcept {
    if (!approxEq(a.preampDb, b.preampDb, eps)) return false;
    for (size_t i = 0; i < ParametricEQ::BANDS_PER_CHANNEL; ++i) {
        const auto& la = a.leftBands[i];
        const auto& lb = b.leftBands[i];
        if (la.enabled != lb.enabled || la.type != lb.type ||
            !approxEq(la.freqHz, lb.freqHz, std::max(eps, 0.05)) ||
            !approxEq(la.gainDb, lb.gainDb, eps) ||
            !approxEq(la.q, lb.q, eps)) {
            return false;
        }
        const auto& ra = a.rightBands[i];
        const auto& rb = b.rightBands[i];
        if (ra.enabled != rb.enabled || ra.type != rb.type ||
            !approxEq(ra.freqHz, rb.freqHz, std::max(eps, 0.05)) ||
            !approxEq(ra.gainDb, rb.gainDb, eps) ||
            !approxEq(ra.q, rb.q, eps)) {
            return false;
        }
    }
    return true;
}

bool DspPresetSerializer::isGeqEqual(const GeqPresetData& a, const GeqPresetData& b, double eps) noexcept {
    for (size_t i = 0; i < GraphicEQ::NUM_BANDS; ++i) {
        if (a.bandEnabled[i] != b.bandEnabled[i] || !approxEq(a.gainsDb[i], b.gainsDb[i], eps)) {
            return false;
        }
    }
    return true;
}

bool DspPresetSerializer::isHpfEqual(const HpfPresetData& a, const HpfPresetData& b, double eps) noexcept {
    return a.slopeDbOct == b.slopeDbOct &&
           approxEq(a.cutoffHz, b.cutoffHz, std::max(eps, 0.05)) &&
           approxEq(a.q, b.q, std::max(eps, 0.005));
}

bool DspPresetSerializer::isLpfEqual(const LpfPresetData& a, const LpfPresetData& b, double eps) noexcept {
    return a.slopeDbOct == b.slopeDbOct &&
           approxEq(a.cutoffHz, b.cutoffHz, std::max(eps, 0.5)) &&
           approxEq(a.q, b.q, std::max(eps, 0.005));
}

bool DspPresetSerializer::isCompressorEqual(const CompressorPresetData& a, const CompressorPresetData& b, double eps) noexcept {
    return approxEq(a.thresholdDb, b.thresholdDb, eps) &&
           approxEq(a.ratio, b.ratio, eps) &&
           approxEq(a.attackMs, b.attackMs, std::max(eps, 0.05)) &&
           approxEq(a.releaseMs, b.releaseMs, std::max(eps, 0.05)) &&
           approxEq(a.kneeDb, b.kneeDb, eps) &&
           approxEq(a.makeupDb, b.makeupDb, eps);
}

bool DspPresetSerializer::isLimiterEqual(const LimiterPresetData& a, const LimiterPresetData& b, double eps) noexcept {
    return approxEq(a.ceilingDbfs, b.ceilingDbfs, eps) &&
           approxEq(a.releaseMs, b.releaseMs, std::max(eps, 0.05));
}

bool DspPresetSerializer::isReverbEqual(const ReverbPresetData& a, const ReverbPresetData& b, double eps) noexcept {
    return approxEq(a.decayS, b.decayS, std::max(eps, 0.01)) &&
           approxEq(a.predelayMs, b.predelayMs, std::max(eps, 0.05)) &&
           approxEq(a.size, b.size, std::max(eps, 0.005)) &&
           approxEq(a.damping, b.damping, std::max(eps, 0.005)) &&
           approxEq(a.wetDryPct, b.wetDryPct, std::max(eps, 0.05));
}

bool DspPresetSerializer::isGlobalStateEqual(const GlobalDspPresetData& a,
                                             const GlobalDspPresetData& b,
                                             double eps) noexcept {
    if (a.nodeEnabled != b.nodeEnabled) return false;
    return isPeqEqual(a.peq, b.peq, eps) &&
           isGeqEqual(a.geq, b.geq, eps) &&
           isHpfEqual(a.hpf, b.hpf, eps) &&
           isLpfEqual(a.lpf, b.lpf, eps) &&
           isCompressorEqual(a.compressor, b.compressor, eps) &&
           isLimiterEqual(a.limiter, b.limiter, eps) &&
           isReverbEqual(a.reverb, b.reverb, eps);
}

bool DspPresetSerializer::validateStrictInRange(const DspPresetDocument& doc, QStringList& outViolations) {
    outViolations.clear();
    auto checkRange = [&](const QString& label, double v, double lo, double hi) {
        if (!std::isfinite(v) || v < lo || v > hi) {
            outViolations.append(QStringLiteral("%1=%2 out of [%3, %4]").arg(label).arg(v).arg(lo).arg(hi));
        }
    };
    auto checkSlope = [&](const QString& label, int s) {
        if (s != 12 && s != 18 && s != 24) {
            outViolations.append(QStringLiteral("%1=%2 not in {12, 18, 24}").arg(label).arg(s));
        }
    };

    if (doc.id.trimmed().isEmpty()) {
        outViolations.append(QStringLiteral("id is empty"));
    }
    if (doc.name.trimmed().isEmpty()) {
        outViolations.append(QStringLiteral("name is empty"));
    }

    auto checkPeq = [&]() {
        checkRange(QStringLiteral("peq.preamp_db"), doc.global.peq.preampDb, -24.0, 24.0);
        for (size_t i = 0; i < ParametricEQ::BANDS_PER_CHANNEL; ++i) {
            const auto& lb = doc.global.peq.leftBands[i];
            checkRange(QStringLiteral("peq.left[%1].freq_hz").arg(i), lb.freqHz, 10.0, 24000.0);
            checkRange(QStringLiteral("peq.left[%1].gain_db").arg(i), lb.gainDb, -24.0, 24.0);
            checkRange(QStringLiteral("peq.left[%1].q").arg(i), lb.q, 0.1, 20.0);
            const auto& rb = doc.global.peq.rightBands[i];
            checkRange(QStringLiteral("peq.right[%1].freq_hz").arg(i), rb.freqHz, 10.0, 24000.0);
            checkRange(QStringLiteral("peq.right[%1].gain_db").arg(i), rb.gainDb, -24.0, 24.0);
            checkRange(QStringLiteral("peq.right[%1].q").arg(i), rb.q, 0.1, 20.0);
        }
    };
    auto checkGeq = [&]() {
        for (size_t i = 0; i < GraphicEQ::NUM_BANDS; ++i) {
            checkRange(QStringLiteral("geq.gains_db[%1]").arg(i), doc.global.geq.gainsDb[i], -12.0, 12.0);
        }
    };
    auto checkHpf = [&]() {
        checkRange(QStringLiteral("hpf.cutoff_hz"), doc.global.hpf.cutoffHz, 10.0, 1000.0);
        checkRange(QStringLiteral("hpf.q"), doc.global.hpf.q, 0.1, 10.0);
        checkSlope(QStringLiteral("hpf.slope_db_oct"), doc.global.hpf.slopeDbOct);
    };
    auto checkLpf = [&]() {
        checkRange(QStringLiteral("lpf.cutoff_hz"), doc.global.lpf.cutoffHz, 1000.0, 22000.0);
        checkRange(QStringLiteral("lpf.q"), doc.global.lpf.q, 0.1, 10.0);
        checkSlope(QStringLiteral("lpf.slope_db_oct"), doc.global.lpf.slopeDbOct);
    };
    auto checkComp = [&]() {
        checkRange(QStringLiteral("compressor.threshold_db"), doc.global.compressor.thresholdDb, -60.0, 0.0);
        checkRange(QStringLiteral("compressor.ratio"), doc.global.compressor.ratio, 1.0, 20.0);
        checkRange(QStringLiteral("compressor.attack_ms"), doc.global.compressor.attackMs, 0.1, 500.0);
        checkRange(QStringLiteral("compressor.release_ms"), doc.global.compressor.releaseMs, 1.0, 1000.0);
        checkRange(QStringLiteral("compressor.knee_db"), doc.global.compressor.kneeDb, 0.0, 20.0);
        checkRange(QStringLiteral("compressor.makeup_db"), doc.global.compressor.makeupDb, -12.0, 24.0);
    };
    auto checkLim = [&]() {
        checkRange(QStringLiteral("limiter.ceiling_dbfs"), doc.global.limiter.ceilingDbfs, -12.0, 0.0);
        checkRange(QStringLiteral("limiter.release_ms"), doc.global.limiter.releaseMs, 10.0, 500.0);
    };
    auto checkRev = [&]() {
        checkRange(QStringLiteral("reverb.decay_s"), doc.global.reverb.decayS, 0.1, 10.0);
        checkRange(QStringLiteral("reverb.predelay_ms"), doc.global.reverb.predelayMs, 0.0, 200.0);
        checkRange(QStringLiteral("reverb.size"), doc.global.reverb.size, 0.0, 1.0);
        checkRange(QStringLiteral("reverb.damping"), doc.global.reverb.damping, 0.0, 1.0);
        checkRange(QStringLiteral("reverb.wet_dry_pct"), doc.global.reverb.wetDryPct, 0.0, 100.0);
    };

    if (doc.kind == DspPresetKind::Global) {
        if (doc.scope != DspPresetScope::Chain) {
            outViolations.append(QStringLiteral("Global preset must have scope=chain"));
        }
        checkPeq();
        checkGeq();
        checkHpf();
        checkLpf();
        checkComp();
        checkLim();
        checkRev();
    } else {
        switch (doc.target) {
        case DspPresetTarget::Peq:
            if (doc.scope != DspPresetScope::MasteringChain) {
                outViolations.append(QStringLiteral("Per-DSP PEQ preset must have scope=mastering-chain"));
            }
            checkPeq();
            break;
        case DspPresetTarget::Geq: checkGeq(); break;
        case DspPresetTarget::Hpf: checkHpf(); break;
        case DspPresetTarget::Lpf: checkLpf(); break;
        case DspPresetTarget::Compressor: checkComp(); break;
        case DspPresetTarget::Limiter: checkLim(); break;
        case DspPresetTarget::Reverb: checkRev(); break;
        case DspPresetTarget::Count:
            outViolations.append(QStringLiteral("Invalid target Count"));
            break;
        }
    }
    return outViolations.isEmpty();
}

} // namespace desk32::engine::preset
