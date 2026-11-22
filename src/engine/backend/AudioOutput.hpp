#pragma once

#include "AudioControlTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace desk32::engine::backend {

/// Spec §8.1 Abstract PCM audio output stream contract.
/// Implemented by PipeWireAudioOutput and JackAudioOutput (PipeWire-JACK compatibility layer).
class AudioOutput {
public:
    using AudioCallback = std::function<void(float* outputInterleaved, size_t frameCount)>;
    using RawPcmAudioCallback = std::function<void(void* outputInterleaved, size_t frameCount, OutputPcmFormat format)>;

    virtual ~AudioOutput() = default;

    virtual bool initialize(uint32_t sampleRate = 48000, uint32_t bufferFrames = 256) = 0;
    virtual bool start() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;

    virtual bool renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs) = 0;

    [[nodiscard]] virtual std::vector<AudioOutputDeviceInfo> enumerateOutputDevices() const = 0;
    virtual void setSimulatedOutputDevices(std::vector<AudioOutputDeviceInfo> devices) = 0;

    virtual bool setTargetDevice(const std::string& deviceIdOrName, std::string* outError = nullptr) = 0;
    [[nodiscard]] virtual std::string targetDeviceId() const = 0;

    virtual bool setBufferFrames(uint32_t bufferFrames) = 0;
    virtual bool setPreferredFormat(OutputPcmFormat format) = 0;

    [[nodiscard]] virtual OutputPcmFormat preferredFormat() const noexcept = 0;
    [[nodiscard]] virtual OutputPcmFormat negotiatedFormat() const noexcept = 0;

    virtual void setDitherActiveState(bool active) noexcept = 0;
    virtual void setSourceBitDepth(uint32_t bitDepth) noexcept = 0;

    [[nodiscard]] virtual StreamState state() const noexcept = 0;
    [[nodiscard]] virtual bool isRunning() const noexcept = 0;
    [[nodiscard]] virtual bool isOfflineSimulationMode() const noexcept = 0;
    [[nodiscard]] virtual bool isInitialized() const noexcept = 0;
    [[nodiscard]] virtual double lastRenegotiationSilenceMs() const noexcept = 0;

    [[nodiscard]] virtual StreamDiagnostics diagnostics() const noexcept = 0;
    [[nodiscard]] virtual DirectModeStatus directModeStatus() const noexcept = 0;

    virtual void setAudioCallback(AudioCallback callback) = 0;
    virtual void setRawPcmAudioCallback(RawPcmAudioCallback callback) = 0;

    virtual void setOfflineSimulationMode(bool enabled) = 0;
    virtual void renderOfflineBlock(float* outInterleaved, size_t frames) = 0;
    virtual void renderOfflineRawBlock(void* outInterleaved, size_t frames, OutputPcmFormat* outUsedFormat = nullptr) = 0;
};

} // namespace desk32::engine::backend
