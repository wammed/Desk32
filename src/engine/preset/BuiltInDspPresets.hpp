#pragma once

#include "DspPresetTypes.hpp"

#include <QString>
#include <vector>

namespace desk32::engine::preset {

/// Built-in 48 Read-Only DSP Presets per Specification v3.8.3 Appendix A.12
/// (6 Global + 7 DSPs x 6 Per-DSP, each with immutable fixed `id`).
class BuiltInDspPresets {
public:
    [[nodiscard]] static const std::vector<DspPresetDocument>& allPresets();
    [[nodiscard]] static std::vector<DspPresetDocument> presetsFor(
        DspPresetKind kind,
        DspPresetTarget target = DspPresetTarget::Peq);

    [[nodiscard]] static bool findPreset(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& idOrName,
        DspPresetDocument& outDoc);

    [[nodiscard]] static bool isBuiltIn(
        DspPresetKind kind,
        DspPresetTarget target,
        const QString& idOrName);

    [[nodiscard]] static DspPresetDocument globalUnity();
    [[nodiscard]] static DspPresetDocument perDspUnity(DspPresetTarget target);
};

} // namespace desk32::engine::preset
