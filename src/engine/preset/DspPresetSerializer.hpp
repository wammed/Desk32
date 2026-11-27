#pragma once

#include "DspPresetTypes.hpp"

#include <QByteArray>
#include <QJsonObject>

namespace desk32::engine::preset {

class DspPresetSerializer {
public:
    static constexpr double kDefaultCompareTolerance = 1e-3;

    [[nodiscard]] static QJsonObject toJson(const DspPresetDocument& doc);
    [[nodiscard]] static QByteArray toJsonBytes(const DspPresetDocument& doc);

    [[nodiscard]] static bool fromJson(const QJsonObject& obj,
                                       DspPresetDocument& outDoc,
                                       PresetValidationReport& outReport);
    [[nodiscard]] static bool fromJsonBytes(const QByteArray& bytes,
                                            DspPresetDocument& outDoc,
                                            PresetValidationReport& outReport);

    // Individual DSP payload serialization helpers (shared between Global and Per-DSP)
    [[nodiscard]] static QJsonObject peqToJson(const PeqPresetData& d, const bool* nodeEnabled = nullptr);
    [[nodiscard]] static QJsonObject geqToJson(const GeqPresetData& d, const bool* nodeEnabled = nullptr);
    [[nodiscard]] static QJsonObject hpfToJson(const HpfPresetData& d, const bool* nodeEnabled = nullptr);
    [[nodiscard]] static QJsonObject lpfToJson(const LpfPresetData& d, const bool* nodeEnabled = nullptr);
    [[nodiscard]] static QJsonObject compressorToJson(const CompressorPresetData& d, const bool* nodeEnabled = nullptr);
    [[nodiscard]] static QJsonObject limiterToJson(const LimiterPresetData& d, const bool* nodeEnabled = nullptr);
    [[nodiscard]] static QJsonObject reverbToJson(const ReverbPresetData& d, const bool* nodeEnabled = nullptr);

    // Canonical state equality for Spec §11.1 Global `modified` comparison
    // Compares all 7 DSP parameters, band enabled states, and 7 DSP node ON/OFF flags.
    // Excludes Trim, Master, External Insert, GEQ global bypass, Listening Correction, and unknown metadata.
    [[nodiscard]] static bool isGlobalStateEqual(const GlobalDspPresetData& a,
                                                 const GlobalDspPresetData& b,
                                                 double eps = kDefaultCompareTolerance) noexcept;

    [[nodiscard]] static bool isPeqEqual(const PeqPresetData& a, const PeqPresetData& b, double eps = kDefaultCompareTolerance) noexcept;
    [[nodiscard]] static bool isGeqEqual(const GeqPresetData& a, const GeqPresetData& b, double eps = kDefaultCompareTolerance) noexcept;
    [[nodiscard]] static bool isHpfEqual(const HpfPresetData& a, const HpfPresetData& b, double eps = kDefaultCompareTolerance) noexcept;
    [[nodiscard]] static bool isLpfEqual(const LpfPresetData& a, const LpfPresetData& b, double eps = kDefaultCompareTolerance) noexcept;
    [[nodiscard]] static bool isCompressorEqual(const CompressorPresetData& a, const CompressorPresetData& b, double eps = kDefaultCompareTolerance) noexcept;
    [[nodiscard]] static bool isLimiterEqual(const LimiterPresetData& a, const LimiterPresetData& b, double eps = kDefaultCompareTolerance) noexcept;
    [[nodiscard]] static bool isReverbEqual(const ReverbPresetData& a, const ReverbPresetData& b, double eps = kDefaultCompareTolerance) noexcept;

    // Validation helper ensuring all values in `doc` lie strictly within Appendix A.3-A.9 bounds
    [[nodiscard]] static bool validateStrictInRange(const DspPresetDocument& doc, QStringList& outViolations);
};

} // namespace desk32::engine::preset
