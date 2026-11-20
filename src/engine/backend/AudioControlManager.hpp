#pragma once

#include "AudioControlTypes.hpp"
#include <memory>
#include <string>
#include <vector>

namespace desk32::engine::backend {

/// Spec §8.1 Audio Control Manager: holds a backend-specific AudioControlAdapter and exposes
/// backend-independent control policy to EngineController.
class AudioControlManager {
public:
    AudioControlManager() = default;
    explicit AudioControlManager(std::unique_ptr<AudioControlAdapter> adapter)
        : m_adapter(std::move(adapter))
    {}

    void setAdapter(std::unique_ptr<AudioControlAdapter> adapter) {
        m_adapter = std::move(adapter);
    }

    [[nodiscard]] AudioControlAdapter* adapter() const noexcept {
        return m_adapter.get();
    }

    [[nodiscard]] std::string backendName() const {
        return m_adapter ? m_adapter->backendName() : std::string("None");
    }

    [[nodiscard]] std::vector<AudioOutputDeviceInfo> enumerateOutputDevices() const {
        return m_adapter ? m_adapter->enumerateOutputDevices() : std::vector<AudioOutputDeviceInfo>{};
    }

    [[nodiscard]] std::string currentTargetDevice() const {
        return m_adapter ? m_adapter->currentTargetDevice() : std::string("default");
    }

    bool selectOutputDevice(const std::string& deviceIdOrName, std::string* outError = nullptr) {
        if (!m_adapter) {
            if (outError) *outError = "No AudioControlAdapter configured";
            return false;
        }
        return m_adapter->setTargetDevice(deviceIdOrName, outError);
    }

    bool applyLatencySetting(uint32_t bufferFrames, double sampleRate) {
        return m_adapter ? m_adapter->applyLatencySetting(bufferFrames, sampleRate) : false;
    }

    bool renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs) {
        if (!m_adapter) {
            outSilenceMs = 0.0;
            return false;
        }
        return m_adapter->renegotiateSampleRate(targetRate, outSilenceMs);
    }

    bool setPreferredPcmFormat(OutputPcmFormat format) {
        return m_adapter ? m_adapter->setPreferredPcmFormat(format) : false;
    }

    [[nodiscard]] OutputPcmFormat preferredPcmFormat() const {
        return m_adapter ? m_adapter->preferredPcmFormat() : OutputPcmFormat::F32LE;
    }

    [[nodiscard]] StreamDiagnostics diagnostics() const {
        return m_adapter ? m_adapter->diagnostics() : StreamDiagnostics{};
    }

    [[nodiscard]] BackendCapabilitiesReport queryCapabilities(bool offlineSimulation,
                                                            bool streamInitialized) const {
        if (!m_adapter) {
            BackendCapabilitiesReport r;
            r.backendName = "None";
            r.directMode = CapabilityAvailability::Unsupported;
            r.sampleRateFollowing = CapabilityAvailability::Unsupported;
            r.exclusiveAccess = CapabilityAvailability::Unsupported;
            r.hardwareBitPerfect = CapabilityAvailability::Unsupported;
            r.deviceSwitching = CapabilityAvailability::Unsupported;
            r.latencyControl = CapabilityAvailability::Unsupported;
            r.formatSelection = CapabilityAvailability::Unsupported;
            return r;
        }
        return m_adapter->queryCapabilities(offlineSimulation, streamInitialized);
    }

private:
    std::unique_ptr<AudioControlAdapter> m_adapter;
};

} // namespace desk32::engine::backend
