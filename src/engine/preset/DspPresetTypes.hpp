#pragma once

#include "engine/dsp/BiquadFilter.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/ParametricEQ.hpp"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace desk32::engine::preset {

inline constexpr const char* kDspPresetFormat = "desk32-dsp-preset";
inline constexpr int kDspPresetCurrentVersion = 2;

enum class DspPresetKind : uint8_t {
    Global = 0,
    PerDsp = 1
};

enum class DspPresetScope : uint8_t {
    Chain = 0,
    MasteringChain = 1,
    ListeningCorrection = 2 // Reserved per Spec §11.1; rejected for Mastering Chain presets
};

enum class DspPresetTarget : size_t {
    Peq = 0,
    Geq = 1,
    Hpf = 2,
    Lpf = 3,
    Compressor = 4,
    Limiter = 5,
    Reverb = 6,
    Count = 7
};

inline constexpr size_t kDspPresetTargetCount = static_cast<size_t>(DspPresetTarget::Count);

[[nodiscard]] inline QString dspTargetToString(DspPresetTarget target) {
    switch (target) {
    case DspPresetTarget::Peq: return QStringLiteral("peq");
    case DspPresetTarget::Geq: return QStringLiteral("geq");
    case DspPresetTarget::Hpf: return QStringLiteral("hpf");
    case DspPresetTarget::Lpf: return QStringLiteral("lpf");
    case DspPresetTarget::Compressor: return QStringLiteral("compressor");
    case DspPresetTarget::Limiter: return QStringLiteral("limiter");
    case DspPresetTarget::Reverb: return QStringLiteral("reverb");
    case DspPresetTarget::Count: break;
    }
    return QString();
}

[[nodiscard]] inline bool dspTargetFromString(const QString& str, DspPresetTarget& outTarget) {
    const QString lower = str.trimmed().toLower();
    if (lower == QLatin1String("peq")) { outTarget = DspPresetTarget::Peq; return true; }
    if (lower == QLatin1String("geq")) { outTarget = DspPresetTarget::Geq; return true; }
    if (lower == QLatin1String("hpf")) { outTarget = DspPresetTarget::Hpf; return true; }
    if (lower == QLatin1String("lpf")) { outTarget = DspPresetTarget::Lpf; return true; }
    if (lower == QLatin1String("compressor")) { outTarget = DspPresetTarget::Compressor; return true; }
    if (lower == QLatin1String("limiter")) { outTarget = DspPresetTarget::Limiter; return true; }
    if (lower == QLatin1String("reverb")) { outTarget = DspPresetTarget::Reverb; return true; }
    return false;
}

[[nodiscard]] inline QString dspScopeToString(DspPresetScope scope) {
    switch (scope) {
    case DspPresetScope::Chain: return QStringLiteral("chain");
    case DspPresetScope::MasteringChain: return QStringLiteral("mastering-chain");
    case DspPresetScope::ListeningCorrection: return QStringLiteral("listening-correction");
    }
    return QStringLiteral("chain");
}

[[nodiscard]] inline bool dspScopeFromString(const QString& str, DspPresetScope& outScope) {
    const QString lower = str.trimmed().toLower();
    if (lower == QLatin1String("chain")) { outScope = DspPresetScope::Chain; return true; }
    if (lower == QLatin1String("mastering-chain")) { outScope = DspPresetScope::MasteringChain; return true; }
    if (lower == QLatin1String("listening-correction")) { outScope = DspPresetScope::ListeningCorrection; return true; }
    return false;
}

[[nodiscard]] inline QString dspKindToString(DspPresetKind kind) {
    return (kind == DspPresetKind::Global) ? QStringLiteral("global") : QStringLiteral("per-dsp");
}

[[nodiscard]] inline QString filterTypeToString(BiquadFilterType type) {
    switch (type) {
    case BiquadFilterType::Peak: return QStringLiteral("peak");
    case BiquadFilterType::LowShelf: return QStringLiteral("low_shelf");
    case BiquadFilterType::HighShelf: return QStringLiteral("high_shelf");
    case BiquadFilterType::LowPass: return QStringLiteral("low_pass");
    case BiquadFilterType::HighPass: return QStringLiteral("high_pass");
    case BiquadFilterType::Notch: return QStringLiteral("notch");
    }
    return QStringLiteral("peak");
}

[[nodiscard]] inline bool filterTypeFromString(const QString& str, BiquadFilterType& outType) {
    const QString lower = str.trimmed().toLower();
    if (lower == QLatin1String("peak") || lower == QLatin1String("bell")) {
        outType = BiquadFilterType::Peak; return true;
    }
    if (lower == QLatin1String("low_shelf") || lower == QLatin1String("lowshelf")) {
        outType = BiquadFilterType::LowShelf; return true;
    }
    if (lower == QLatin1String("high_shelf") || lower == QLatin1String("highshelf")) {
        outType = BiquadFilterType::HighShelf; return true;
    }
    if (lower == QLatin1String("low_pass") || lower == QLatin1String("lowpass")) {
        outType = BiquadFilterType::LowPass; return true;
    }
    if (lower == QLatin1String("high_pass") || lower == QLatin1String("highpass")) {
        outType = BiquadFilterType::HighPass; return true;
    }
    if (lower == QLatin1String("notch")) {
        outType = BiquadFilterType::Notch; return true;
    }
    return false;
}

inline constexpr std::array<double, ParametricEQ::BANDS_PER_CHANNEL> kPeqIsoDefaultFreqs = {
    25.0, 40.0, 63.0, 100.0, 160.0, 250.0, 400.0, 630.0,
    1000.0, 1600.0, 2500.0, 4000.0, 6300.0, 10000.0, 14000.0, 18000.0
};

inline constexpr std::array<double, GraphicEQ::NUM_BANDS> kGeqIsoFreqs = {
    16.0, 20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0,
    100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0,
    630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
    4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0
};

// Appendix A.3: PEQ (Mastering Chain, L/R independent 16 bands + common preamp)
struct PeqBandPreset {
    double freqHz = 1000.0;                  // 10.0 .. 24000.0
    double gainDb = 0.0;                     // -24.0 .. +24.0
    double q = 0.707;                        // 0.1 .. 20.0
    BiquadFilterType type = BiquadFilterType::Peak;
    bool enabled = false;                    // Unity default: false
    QJsonObject unknownFields;
};

struct PeqPresetData {
    std::array<PeqBandPreset, ParametricEQ::BANDS_PER_CHANNEL> leftBands{};
    std::array<PeqBandPreset, ParametricEQ::BANDS_PER_CHANNEL> rightBands{};
    std::vector<PeqBandPreset> overflowLeft;
    std::vector<PeqBandPreset> overflowRight;
    double preampDb = 0.0;                   // -24.0 .. +24.0
    QJsonObject unknownFields;

    static PeqPresetData unity() {
        PeqPresetData d;
        for (size_t i = 0; i < ParametricEQ::BANDS_PER_CHANNEL; ++i) {
            PeqBandPreset b;
            b.freqHz = kPeqIsoDefaultFreqs[i];
            b.gainDb = 0.0;
            b.q = 0.707;
            b.type = BiquadFilterType::Peak;
            b.enabled = false;
            d.leftBands[i] = b;
            d.rightBands[i] = b;
        }
        d.preampDb = 0.0;
        return d;
    }
};

// Appendix A.4: 32-band GEQ
struct GeqPresetData {
    std::array<double, GraphicEQ::NUM_BANDS> gainsDb{};      // -12.0 .. +12.0
    std::array<bool, GraphicEQ::NUM_BANDS> bandEnabled{};    // Unity default: all true
    QJsonObject unknownFields;

    static GeqPresetData unity() {
        GeqPresetData d;
        d.gainsDb.fill(0.0);
        d.bandEnabled.fill(true);
        return d;
    }
};

// Appendix A.5: HPF
struct HpfPresetData {
    double cutoffHz = 20.0;                  // 10.0 .. 1000.0
    double q = 0.71;                         // 0.1 .. 10.0
    int slopeDbOct = 12;                     // {12, 18, 24}
    QJsonObject unknownFields;

    static HpfPresetData unity() { return HpfPresetData{}; }
};

// Appendix A.6: LPF
struct LpfPresetData {
    double cutoffHz = 20000.0;               // 1000.0 .. 22000.0
    double q = 0.71;                         // 0.1 .. 10.0
    int slopeDbOct = 12;                     // {12, 18, 24}
    QJsonObject unknownFields;

    static LpfPresetData unity() { return LpfPresetData{}; }
};

// Appendix A.7: Compressor
struct CompressorPresetData {
    double thresholdDb = 0.0;                // -60.0 .. 0.0
    double ratio = 1.0;                      // 1.0 .. 20.0
    double attackMs = 10.0;                  // 0.1 .. 500.0
    double releaseMs = 100.0;                // 1.0 .. 1000.0
    double kneeDb = 0.0;                     // 0.0 .. 20.0
    double makeupDb = 0.0;                   // -12.0 .. +24.0
    QJsonObject unknownFields;

    static CompressorPresetData unity() { return CompressorPresetData{}; }
};

// Appendix A.8: Limiter (Sample-Peak; Lookahead excluded from presets)
struct LimiterPresetData {
    double ceilingDbfs = 0.0;                // -12.0 .. 0.0
    double releaseMs = 50.0;                 // 10.0 .. 500.0
    QJsonObject unknownFields;

    static LimiterPresetData unity() { return LimiterPresetData{}; }
};

// Appendix A.9: Reverb (Tail mode excluded from presets)
struct ReverbPresetData {
    double decayS = 0.2;                     // 0.1 .. 10.0
    double predelayMs = 0.0;                 // 0.0 .. 200.0
    double size = 0.10;                      // 0.00 .. 1.00
    double damping = 0.50;                   // 0.00 .. 1.00
    double wetDryPct = 0.0;                  // 0.0 .. 100.0
    QJsonObject unknownFields;

    static ReverbPresetData unity() { return ReverbPresetData{}; }
};

// Appendix A.2: Global snapshot across all 7 DSPs + 7 node ON/OFF flags
struct GlobalDspPresetData {
    PeqPresetData peq = PeqPresetData::unity();
    GeqPresetData geq = GeqPresetData::unity();
    HpfPresetData hpf = HpfPresetData::unity();
    LpfPresetData lpf = LpfPresetData::unity();
    CompressorPresetData compressor = CompressorPresetData::unity();
    LimiterPresetData limiter = LimiterPresetData::unity();
    ReverbPresetData reverb = ReverbPresetData::unity();

    std::array<bool, kDspPresetTargetCount> nodeEnabled{}; // All false in Unity
    QJsonObject unknownDspKeys;

    [[nodiscard]] bool isNodeEnabled(DspPresetTarget t) const noexcept {
        return nodeEnabled[static_cast<size_t>(t)];
    }

    void setNodeEnabled(DspPresetTarget t, bool on) noexcept {
        nodeEnabled[static_cast<size_t>(t)] = on;
    }

    static GlobalDspPresetData unity() {
        GlobalDspPresetData g;
        g.peq = PeqPresetData::unity();
        g.geq = GeqPresetData::unity();
        g.hpf = HpfPresetData::unity();
        g.lpf = LpfPresetData::unity();
        g.compressor = CompressorPresetData::unity();
        g.limiter = LimiterPresetData::unity();
        g.reverb = ReverbPresetData::unity();
        g.nodeEnabled.fill(false);
        return g;
    }
};

struct DspPresetDocument {
    QString format = QString::fromLatin1(kDspPresetFormat);
    int version = kDspPresetCurrentVersion;
    DspPresetKind kind = DspPresetKind::Global;
    QString id;
    QString name;
    DspPresetScope scope = DspPresetScope::Chain;
    DspPresetTarget target = DspPresetTarget::Peq; // Only meaningful when kind == PerDsp
    QString createdAt;
    QString modifiedAt;
    bool isFactory = false;

    GlobalDspPresetData global = GlobalDspPresetData::unity();
    // For PerDsp documents, the active DSP payload is stored in the matching field of `global`.

    QJsonObject unknownTopLevelKeys;
};

struct PresetValidationReport {
    bool ok = true;
    bool migratedFromLegacy = false;
    int sourceVersion = kDspPresetCurrentVersion;
    QString errorMessage;

    QStringList warnings;
    QStringList clampedFields;
    QStringList missingDefaultedFields;
    QStringList unknownKeys;
    QStringList unknownEnumFields;
    size_t peqOverflowLeft = 0;
    size_t peqOverflowRight = 0;

    [[nodiscard]] bool hasWarnings() const noexcept {
        return !warnings.isEmpty() || !clampedFields.isEmpty() ||
               !unknownKeys.isEmpty() || !unknownEnumFields.isEmpty() ||
               peqOverflowLeft > 0 || peqOverflowRight > 0;
    }

    [[nodiscard]] QVariantMap toVariantMap() const {
        QVariantMap m;
        m.insert(QStringLiteral("ok"), ok);
        m.insert(QStringLiteral("migratedFromLegacy"), migratedFromLegacy);
        m.insert(QStringLiteral("sourceVersion"), sourceVersion);
        m.insert(QStringLiteral("errorMessage"), errorMessage);
        m.insert(QStringLiteral("hasWarnings"), hasWarnings());
        m.insert(QStringLiteral("warnings"), warnings);
        m.insert(QStringLiteral("clampedFields"), clampedFields);
        m.insert(QStringLiteral("missingDefaultedFields"), missingDefaultedFields);
        m.insert(QStringLiteral("unknownKeys"), unknownKeys);
        m.insert(QStringLiteral("unknownEnumFields"), unknownEnumFields);
        m.insert(QStringLiteral("peqOverflowLeft"), static_cast<int>(peqOverflowLeft));
        m.insert(QStringLiteral("peqOverflowRight"), static_cast<int>(peqOverflowRight));
        return m;
    }
};

} // namespace desk32::engine::preset
