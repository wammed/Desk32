#pragma once

#include "AudioControlTypes.hpp"
#include "PipeWireAudioOutput.hpp"

namespace desk32::engine::backend {

/// PipeWire implementation of AudioControlAdapter (Spec §8.1 / §8.2).
class PipeWireControlAdapter final : public AudioControlAdapter {
public:
    explicit PipeWireControlAdapter(PipeWireAudioOutput* output) noexcept
        : m_output(output)
    {}

    [[nodiscard]] std::string backendName() const override { return "PipeWire"; }

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
    PipeWireAudioOutput* m_output = nullptr;
};

} // namespace desk32::engine::backend
