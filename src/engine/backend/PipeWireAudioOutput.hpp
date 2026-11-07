#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>

// Forward declarations for PipeWire / SPA structures to keep header clean
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
struct spa_pod;

namespace desk32::engine::backend {

enum class StreamState {
    Unconnected,
    Connecting,
    Paused,
    Streaming,
    Error
};

struct StreamDiagnostics {
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    uint32_t bufferFrames = 256;
    std::string formatName = "F32LE";
    uint64_t xrunCount = 0;
    uint64_t totalFramesProcessed = 0;
    bool isDirectMode = false;
};

/// Native low-latency stereo PCM audio output powered by PipeWire (pw_stream / SPA)
class PipeWireAudioOutput {
public:
    /// Audio render callback called on the real-time PipeWire audio processing thread.
    /// Parameters:
    /// - outputInterleaved: buffer to fill with interleaved stereo Float32 samples
    /// - frameCount: number of frames to render
    using AudioCallback = std::function<void(float* outputInterleaved, size_t frameCount)>;

    explicit PipeWireAudioOutput();
    ~PipeWireAudioOutput();

    /// Initialize PipeWire context and thread loop.
    /// Safe in both active audio environments and minimal CI environments (graceful fallback).
    bool initialize(uint32_t sampleRate = 48000, uint32_t bufferFrames = 256);

    /// Connect and start playback stream
    bool start();

    /// Pause stream
    void pause();

    /// Stop stream and disconnect
    void stop();

    /// Terminate and cleanup all PipeWire resources
    void shutdown();

    [[nodiscard]] StreamState state() const noexcept { return m_state.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isRunning() const noexcept { return m_state.load(std::memory_order_relaxed) == StreamState::Streaming; }

    [[nodiscard]] StreamDiagnostics diagnostics() const noexcept;

    void setAudioCallback(AudioCallback callback) {
        m_callback = std::move(callback);
    }

    /// Set simulated offline processing for headless CI or fallback verification
    void setOfflineSimulationMode(bool enabled) {
        m_offlineSimulation = enabled;
    }

    /// Render a simulated block directly (useful for tests and non-PipeWire verification)
    void renderOfflineBlock(float* outInterleaved, size_t frames);

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

    std::atomic<StreamState> m_state{StreamState::Unconnected};
    std::atomic<uint64_t> m_xrunCount{0};
    std::atomic<uint64_t> m_totalFramesProcessed{0};
    std::atomic<bool> m_isDirectMode{false};
    bool m_offlineSimulation = false;

    AudioCallback m_callback;
};

} // namespace desk32::engine::backend
