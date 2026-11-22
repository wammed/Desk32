#pragma once

#include "AudioControlTypes.hpp"
#include "AudioControlManager.hpp"
#include "JackAudioOutput.hpp"

namespace desk32::engine::backend {

/// JACK (PipeWire-JACK compatibility layer) implementation of AudioControlAdapter (Spec §8.1 / §8.2 / §8.3).
class JackControlAdapter final : public AudioControlAdapter {
public:
    explicit JackControlAdapter(JackAudioOutput* output) noexcept
        : m_output(output)
    {}

    [[nodiscard]] std::string backendName() const override { return "JACK"; }

    [[nodiscard]] std::vector<AudioOutputDeviceInfo> enumerateOutputDevices() const override;
    [[nodiscard]] std::string currentTargetDevice() const override;
    bool setTargetDevice(const std::string& deviceIdOrName, std::string* outError = nullptr) override;
    bool applyLatencySetting(uint32_t bufferFrames, double sampleRate) override;
    bool renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs) override;
    bool setPreferredPcmFormat(OutputPcmFormat format) override;
    [[nodiscard]] OutputPcmFormat preferredPcmFormat() const override;
    [[nodiscard]] StreamDiagnostics diagnostics() const override;
    [[nodiscard]] StreamState streamState() const override;
    [[nodiscard]] BackendCapabilitiesReport queryCapabilities(bool offlineSimulation,
                                                             bool streamInitialized) const override;

private:
    JackAudioOutput* m_output = nullptr;
};

} // namespace desk32::engine::backend
