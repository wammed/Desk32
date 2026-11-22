#pragma once

#include "AudioControlTypes.hpp"
#include "AudioOutput.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct _jack_client;
struct _jack_port;

namespace desk32::engine::backend {

/// JACK (PipeWire-JACK compatibility layer) audio output implementation (Spec §8.1 / §8.2 / §8.3).
/// Kept free of `<jack/jack.h>` in the public header so controller and tests compile cleanly.
class JackAudioOutput final : public AudioOutput {
public:
    static constexpr size_t SCRATCH_FRAMES = 16384;

    JackAudioOutput();
    ~JackAudioOutput() override;

    JackAudioOutput(const JackAudioOutput&) = delete;
    JackAudioOutput& operator=(const JackAudioOutput&) = delete;

    void setClientName(const std::string& name);
    [[nodiscard]] std::string clientName() const;
    void setAutoConnect(bool enabled) noexcept { m_autoConnect.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool autoConnect() const noexcept { return m_autoConnect.load(std::memory_order_relaxed); }

    bool initialize(uint32_t sampleRate = 48000, uint32_t bufferFrames = 256) override;
    bool start() override;
    void pause() override;
    void stop() override;
    void shutdown() override;

    bool renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs) override;

    [[nodiscard]] std::vector<AudioOutputDeviceInfo> enumerateOutputDevices() const override;
    void setSimulatedOutputDevices(std::vector<AudioOutputDeviceInfo> devices) override;

    bool setTargetDevice(const std::string& deviceIdOrName, std::string* outError = nullptr) override;
    [[nodiscard]] std::string targetDeviceId() const override;

    bool setBufferFrames(uint32_t bufferFrames) override;
    bool setPreferredFormat(OutputPcmFormat format) override;
    [[nodiscard]] OutputPcmFormat preferredFormat() const noexcept override { return OutputPcmFormat::F32LE; }
    [[nodiscard]] OutputPcmFormat negotiatedFormat() const noexcept override { return OutputPcmFormat::F32LE; }

    void setDitherActiveState(bool active) noexcept override {
        m_ditherActive.store(active, std::memory_order_relaxed);
    }
    void setSourceBitDepth(uint32_t bitDepth) noexcept override {
        m_sourceBitDepth.store(bitDepth, std::memory_order_relaxed);
    }

    [[nodiscard]] StreamState state() const noexcept override { return m_state.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isRunning() const noexcept override { return m_state.load(std::memory_order_relaxed) == StreamState::Streaming; }
    [[nodiscard]] bool isOfflineSimulationMode() const noexcept override { return m_offlineSimulation; }
    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized.load(std::memory_order_relaxed); }
    [[nodiscard]] double lastRenegotiationSilenceMs() const noexcept override { return m_lastRenegotiationSilenceMs.load(std::memory_order_relaxed); }

    [[nodiscard]] StreamDiagnostics diagnostics() const noexcept override;
    [[nodiscard]] DirectModeStatus directModeStatus() const noexcept override { return m_directModeStatus.load(std::memory_order_relaxed); }
    [[nodiscard]] bool latencyControlSupported() const noexcept { return m_latencyControlSupported.load(std::memory_order_relaxed); }
    [[nodiscard]] std::string lastError() const;

    void setAudioCallback(AudioCallback callback) override { m_callback = std::move(callback); }
    void setRawPcmAudioCallback(RawPcmAudioCallback callback) override { m_rawCallback = std::move(callback); }

    void setOfflineSimulationMode(bool enabled) override { m_offlineSimulation = enabled; }
    void renderOfflineBlock(float* outInterleaved, size_t frames) override;
    void renderOfflineRawBlock(void* outInterleaved, size_t frames, OutputPcmFormat* outUsedFormat = nullptr) override;

    // JACK static callbacks
    static int onProcessCallback(uint32_t nframes, void* arg);
    static int onSampleRateCallback(uint32_t nframes, void* arg);
    static int onBufferSizeCallback(uint32_t nframes, void* arg);
    static int onXrunCallback(void* arg);
    static void onShutdownCallback(void* arg);

private:
    struct TargetPortPair {
        std::string deviceName;
        std::string portL;
        std::string portR;
        bool isPhysical = false;
    };

    [[nodiscard]] std::vector<AudioOutputDeviceInfo> enumerateFromJackClient(_jack_client* client,
                                                                           std::vector<TargetPortPair>* outRoutes = nullptr) const;
    bool connectTargetLocked(std::string* outError);
    void disconnectPortsLocked();
    void updateRateTracking(uint32_t actualRate);

    _jack_client* m_client = nullptr;
    _jack_port* m_portL = nullptr;
    _jack_port* m_portR = nullptr;

    uint32_t m_requestedSampleRate = 48000;
    uint32_t m_requestedBufferFrames = 256;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_autoConnect{true};
    std::atomic<bool> m_sendRateHintOnOpen{false};
    std::atomic<bool> m_latencyControlSupported{true};
    std::atomic<StreamState> m_state{StreamState::Unconnected};
    std::atomic<uint32_t> m_negotiatedSampleRate{0};
    std::atomic<uint32_t> m_negotiatedQuantum{0};
    std::atomic<bool> m_ditherActive{false};
    std::atomic<uint32_t> m_sourceBitDepth{24};
    std::atomic<double> m_lastRenegotiationSilenceMs{0.0};
    std::atomic<uint64_t> m_xrunCount{0};
    std::atomic<uint64_t> m_totalFramesProcessed{0};
    std::atomic<bool> m_isDirectMode{false};
    std::atomic<DirectModeStatus> m_directModeStatus{DirectModeStatus::Unknown};
    std::atomic<uint64_t> m_lastCallbackDurationNs{0};
    std::atomic<uint64_t> m_maxCallbackDurationNs{0};
    std::atomic<uint64_t> m_lastCallbackIntervalNs{0};
    std::atomic<uint64_t> m_expectedPeriodNs{0};
    std::atomic<int64_t> m_lastSchedJitterNs{0};
    std::atomic<uint64_t> m_maxAbsSchedJitterNs{0};
    std::atomic<uint64_t> m_callbackCount{0};
    int64_t m_prevCallbackStartNs = 0;
    bool m_offlineSimulation = false;

    mutable std::mutex m_clientMutex;
    std::string m_clientName{"Desk32"};
    std::string m_deviceName{"jack_default"};
    std::string m_targetDeviceId;
    std::string m_lastError;
    std::vector<AudioOutputDeviceInfo> m_simulatedDevices;
    std::vector<float> m_scratchInterleaved;

    AudioCallback m_callback;
    RawPcmAudioCallback m_rawCallback;
};

} // namespace desk32::engine::backend
