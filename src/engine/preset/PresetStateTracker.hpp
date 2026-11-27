#pragma once

#include "BuiltInDspPresets.hpp"
#include "DspPresetSerializer.hpp"
#include "DspPresetTypes.hpp"

#include <QString>
#include <QVariantMap>

#include <array>

namespace desk32::engine::preset {

/// Canonical Preset State Manager (Specification v3.8.3 §11.1, Phase P5).
/// Owns:
/// - Active Global preset metadata (`id`, `name`) and saved snapshot (`GlobalDspPresetData`)
/// - Per-DSP preset-applied indicator flags (7 independent booleans, decoupled from node ON/OFF and values)
/// - `globalModified` computed strictly by comparing current 7-DSP + 7-node-ON/OFF chain state
///   against the active Global snapshot (NEVER inferred from indicator LEDs).
class PresetStateTracker {
public:
    PresetStateTracker() {
        resetToUnity();
    }

    void resetToUnity() {
        const DspPresetDocument unity = BuiltInDspPresets::globalUnity();
        m_activeGlobalId = unity.id;
        m_activeGlobalName = unity.name;
        m_activeGlobalSnapshot = unity.global;
        m_dspPresetApplied.fill(true); // Unity Global applied at startup
        m_globalModified = false;
    }

    /// Spec §11.1: Global apply turns ALL 7 preset-applied indicators ON,
    /// updates active Global name/id/snapshot, and sets modified=false (verified against currentState).
    void onGlobalPresetApplied(const QString& presetId,
                               const QString& presetName,
                               const GlobalDspPresetData& appliedSnapshot,
                               const GlobalDspPresetData& currentChainState) {
        m_activeGlobalId = presetId;
        m_activeGlobalName = presetName;
        m_activeGlobalSnapshot = appliedSnapshot;
        m_dspPresetApplied.fill(true);
        recomputeModified(currentChainState);
    }

    /// Spec §11.1: Saving current state as a Global preset promotes that snapshot as active Global.
    void onGlobalPresetSaved(const QString& presetId,
                             const QString& presetName,
                             const GlobalDspPresetData& savedSnapshot,
                             const GlobalDspPresetData& currentChainState) {
        m_activeGlobalId = presetId;
        m_activeGlobalName = presetName;
        m_activeGlobalSnapshot = savedSnapshot;
        m_dspPresetApplied.fill(true);
        recomputeModified(currentChainState);
    }

    /// Spec §11.1: Renaming the currently active Global user preset updates display name while keeping id/snapshot.
    void onGlobalPresetRenamed(const QString& presetId, const QString& newName) {
        if (m_activeGlobalId == presetId && !newName.trimmed().isEmpty()) {
            m_activeGlobalName = newName.trimmed();
        }
    }

    /// Spec §11.1: Manual parameter edit on ANY of the 7 Mastering Chain DSPs turns ALL 7 indicators OFF.
    /// Does NOT alter DSP values or node ON/OFF; retains active Global name; recomputes `modified`.
    void onManualParameterChanged(const GlobalDspPresetData& currentChainState) {
        m_dspPresetApplied.fill(false);
        recomputeModified(currentChainState);
    }

    /// Spec §11.1: Per-DSP preset apply turns ONLY target DSP indicator ON and the other 6 OFF.
    /// Retains active Global name and snapshot; recomputes `modified` from snapshot diff.
    void onPerDspPresetApplied(DspPresetTarget target,
                               const GlobalDspPresetData& currentChainState) {
        m_dspPresetApplied.fill(false);
        const size_t idx = static_cast<size_t>(target);
        if (idx < kDspPresetTargetCount) {
            m_dspPresetApplied[idx] = true;
        }
        recomputeModified(currentChainState);
    }

    /// Spec §11.1: DSP node ON/OFF toggle does NOT change preset-applied indicators,
    /// but DOES update `modified` because node ON/OFF is part of the Global snapshot.
    void onDspNodeBypassToggled(const GlobalDspPresetData& currentChainState) {
        recomputeModified(currentChainState);
    }

    /// Re-evaluate `modified` without altering indicators.
    void recomputeModified(const GlobalDspPresetData& currentChainState) {
        m_globalModified = !DspPresetSerializer::isGlobalStateEqual(currentChainState, m_activeGlobalSnapshot);
    }

    [[nodiscard]] const QString& activeGlobalId() const noexcept { return m_activeGlobalId; }
    [[nodiscard]] const QString& activeGlobalName() const noexcept { return m_activeGlobalName; }
    [[nodiscard]] const GlobalDspPresetData& activeGlobalSnapshot() const noexcept { return m_activeGlobalSnapshot; }
    [[nodiscard]] bool isGlobalModified() const noexcept { return m_globalModified; }

    [[nodiscard]] bool isDspPresetApplied(DspPresetTarget target) const noexcept {
        const size_t idx = static_cast<size_t>(target);
        return (idx < kDspPresetTargetCount) ? m_dspPresetApplied[idx] : false;
    }

    [[nodiscard]] const std::array<bool, kDspPresetTargetCount>& dspPresetIndicators() const noexcept {
        return m_dspPresetApplied;
    }

    [[nodiscard]] QVariantMap dspPresetIndicatorsMap() const {
        QVariantMap m;
        for (size_t i = 0; i < kDspPresetTargetCount; ++i) {
            const auto t = static_cast<DspPresetTarget>(i);
            m.insert(dspTargetToString(t), m_dspPresetApplied[i]);
        }
        return m;
    }

private:
    QString m_activeGlobalId;
    QString m_activeGlobalName;
    GlobalDspPresetData m_activeGlobalSnapshot = GlobalDspPresetData::unity();
    std::array<bool, kDspPresetTargetCount> m_dspPresetApplied{};
    bool m_globalModified = false;
};

} // namespace desk32::engine::preset
