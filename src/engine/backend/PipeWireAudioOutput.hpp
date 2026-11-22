#pragma once

#include "AudioControlTypes.hpp"
#include "AudioOutput.hpp"

#include <string>
#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// Forward declarations for PipeWire / SPA structures to keep header clean
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
struct spa_pod;

namespace desk32::engine::backend {

/// Native low-latency stereo PCM audio output powered by PipeWire (pw_stream / SPA)
class PipeWireAudioOutput final : public AudioOutput {
public:
    explicit PipeWireAudioOutput();
    ~PipeWireAudioOutput() override;

    /// Initialize PipeWire context and thread loop.
    /// Safe in both active audio environments and minimal CI environments (graceful fallback).
    bool initialize(uint32_t sampleRate = 48000, uint32_t bufferFrames = 256) override;

    /// Connect and start playback stream
    bool start() override;

    /// Pause stream
    void pause() override;

    /// Stop stream and disconnect
    void stop() override;

    /// Re-negotiate hardware/graph sample rate by cleanly tearing down and restarting stream (Spec §2.6 Direct Mode).
    /// Measures interruption/silence duration in milliseconds (`outSilenceMs`).
    bool renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs) override;

    /// Spec §8.1 Enumerate available PipeWire Audio/Sink devices (returns simulated devices when offline).
    [[nodiscard]] std::vector<AudioOutputDeviceInfo> enumerateOutputDevices() const override;

    /// Set simulated output devices for deterministic offline/CI testing.
    void setSimulatedOutputDevices(std::vector<AudioOutputDeviceInfo> devices) override;

    /// Switch target PipeWire Sink object (`PW_KEY_TARGET_OBJECT` / node.name / node.id string).
    bool setTargetDevice(const std::string& deviceIdOrName, std::string* outError = nullptr) override;
    [[nodiscard]] std::string targetDeviceId() const override;

    /// Apply requested buffer latency (`PW_KEY_NODE_LATENCY` = "<frames>/<rate>").
    bool setBufferFrames(uint32_t bufferFrames) override;

    /// Select preferred PipeWire stream PCM format (`F32LE`, `S24_32LE`, `S16LE`, `S32LE`).
    bool setPreferredFormat(OutputPcmFormat format) override;
    [[nodiscard]] OutputPcmFormat preferredFormat() const noexcept override {
        return m_preferredFormat.load(std::memory_order_relaxed);
    }
    [[nodiscard]] OutputPcmFormat negotiatedFormat() const noexcept override {
        return m_negotiatedFormat.load(std::memory_order_relaxed);
    }

    void setDitherActiveState(bool active) noexcept override {
        m_ditherActive.store(active, std::memory_order_relaxed);
    }
    void setSourceBitDepth(uint32_t bitDepth) noexcept override {
        m_sourceBitDepth.store(bitDepth, std::memory_order_relaxed);
    }

    /// Terminate and cleanup all PipeWire resources
    void shutdown() override;

    [[nodiscard]] StreamState state() const noexcept override { return m_state.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isRunning() const noexcept override { return m_state.load(std::memory_order_relaxed) == StreamState::Streaming; }
    [[nodiscard]] bool isOfflineSimulationMode() const noexcept override { return m_offlineSimulation; }
    [[nodiscard]] bool isInitialized() const noexcept override { return m_initialized.load(std::memory_order_relaxed); }
    [[nodiscard]] double lastRenegotiationSilenceMs() const noexcept override { return m_lastRenegotiationSilenceMs.load(std::memory_order_relaxed); }

    [[nodiscard]] StreamDiagnostics diagnostics() const noexcept override;
    [[nodiscard]] DirectModeStatus directModeStatus() const noexcept override { return m_directModeStatus.load(std::memory_order_relaxed); }

    void setAudioCallback(AudioCallback callback) override {
        m_callback = std::move(callback);
    }

    void setRawPcmAudioCallback(RawPcmAudioCallback callback) override {
        m_rawCallback = std::move(callback);
    }

    /// Set simulated offline processing for headless CI or fallback verification
    void setOfflineSimulationMode(bool enabled) override {
        m_offlineSimulation = enabled;
    }

    /// Render a simulated F32LE block directly (useful for tests and non-PipeWire verification)
    void renderOfflineBlock(float* outInterleaved, size_t frames) override;

    /// Render a simulated block directly in the currently negotiated OutputPcmFormat
    void renderOfflineRawBlock(void* outInterleaved, size_t frames, OutputPcmFormat* outUsedFormat = nullptr) override;

    // PipeWire event handlers (public for C callback bindings)
    static void onStreamStateChanged(void* data, int oldState, int newState, const char* error);
    static void onStreamParamChanged(void* data, uint32_t id, const struct ::spa_pod* param);
    static void onStreamProcess(void* data);

private:
    struct pw_thread_loop* m_threadLoop = nullptr;
    struct pw_context* m_context = nullptr;
    struct pw_core* m_core = nullptr;
    struct pw_stream* m_stream = nullptr;
    std::unique_ptr<struct spa_hook> m_streamListener;

    uint32_t m_requestedSampleRate = 48000;
    uint32_t m_requestedBufferFrames = 256;

    std::atomic<bool> m_initialized{false};
    std::atomic<StreamState> m_state{StreamState::Unconnected};
    std::atomic<uint32_t> m_negotiatedSampleRate{0};
    std::atomic<OutputPcmFormat> m_preferredFormat{OutputPcmFormat::F32LE};
    std::atomic<OutputPcmFormat> m_negotiatedFormat{OutputPcmFormat::F32LE};
    std::atomic<bool> m_ditherActive{false};
    std::atomic<uint32_t> m_sourceBitDepth{24};
    std::atomic<double> m_lastRenegotiationSilenceMs{0.0};
    std::atomic<uint64_t> m_xrunCount{0};
    std::atomic<uint64_t> m_totalFramesProcessed{0};
    std::atomic<uint32_t> m_negotiatedQuantum{0};
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

    mutable std::mutex m_deviceMutex;
    std::string m_deviceName{"default"};
    std::string m_targetDeviceId;
    std::vector<AudioOutputDeviceInfo> m_simulatedDevices;
    std::vector<float> m_fallbackFloatBuffer;

    AudioCallback m_callback;
    RawPcmAudioCallback m_rawCallback;
};

} // namespace desk32::engine::backend
