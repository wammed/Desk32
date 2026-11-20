#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace desk32::engine::backend {

enum class StreamState {
    Unconnected,
    Connecting,
    Paused,
    Streaming,
    Error
};

enum class DirectModeStatus {
    Unknown = 0,    ///< Unmeasured / Not yet queried or unconnected
    Disabled = 1,   ///< Operating through PipeWire resampler/graph mixer
    Active = 2      ///< Direct Mode confirmed (1:1 rate match, bit-perfect direct output)
};

enum class OutputPcmFormat {
    F32LE = 0,
    S24_32LE = 1,
    S16LE = 2,
    S32LE = 3
};

[[nodiscard]] inline constexpr const char* outputPcmFormatName(OutputPcmFormat fmt) noexcept {
    switch (fmt) {
    case OutputPcmFormat::F32LE: return "F32LE";
    case OutputPcmFormat::S24_32LE: return "S24_32LE";
    case OutputPcmFormat::S16LE: return "S16LE";
    case OutputPcmFormat::S32LE: return "S32LE";
    }
    return "F32LE";
}

[[nodiscard]] inline constexpr uint32_t outputPcmFormatBitDepth(OutputPcmFormat fmt) noexcept {
    switch (fmt) {
    case OutputPcmFormat::F32LE: return 32;
    case OutputPcmFormat::S24_32LE: return 24;
    case OutputPcmFormat::S16LE: return 16;
    case OutputPcmFormat::S32LE: return 32;
    }
    return 32;
}

[[nodiscard]] inline constexpr uint32_t outputPcmFormatBytesPerSample(OutputPcmFormat fmt) noexcept {
    switch (fmt) {
    case OutputPcmFormat::S16LE: return 2;
    case OutputPcmFormat::F32LE:
    case OutputPcmFormat::S24_32LE:
    case OutputPcmFormat::S32LE:
        return 4;
    }
    return 4;
}

[[nodiscard]] inline constexpr bool outputPcmFormatIsLosslessForSourceBits(
    OutputPcmFormat fmt, uint32_t sourceBitDepth = 24) noexcept {
    if (fmt == OutputPcmFormat::S16LE) {
        return sourceBitDepth > 0 && sourceBitDepth <= 16;
    }
    return true;
}

/// Spec §8.2 4-State Capability Availability Model
enum class CapabilityAvailability {
    Supported = 0,
    Unsupported = 1,
    Unavailable = 2,
    Unknown = 3
};

[[nodiscard]] inline constexpr const char* capabilityAvailabilityName(CapabilityAvailability cap) noexcept {
    switch (cap) {
    case CapabilityAvailability::Supported: return "Supported";
    case CapabilityAvailability::Unsupported: return "Unsupported";
    case CapabilityAvailability::Unavailable: return "Unavailable";
    case CapabilityAvailability::Unknown: return "Unknown";
    }
    return "Unknown";
}

struct AudioOutputDeviceInfo {
    uint32_t nodeId = 0;
    std::string name;
    std::string description;
    std::string mediaClass{"Audio/Sink"};
    bool isDefault = false;
    uint32_t sampleRate = 0;
    uint32_t channels = 2;
    OutputPcmFormat nativeFormat = OutputPcmFormat::F32LE;
};

struct StreamDiagnostics {
    uint32_t sampleRate = 48000;
    uint32_t requestedSampleRate = 48000;
    uint32_t channels = 2;
    uint32_t bufferFrames = 256;
    uint32_t negotiatedQuantum = 0;
    std::string formatName = "F32LE";
    std::string deviceName = "default";
    std::string targetDeviceId;
    OutputPcmFormat preferredFormat = OutputPcmFormat::F32LE;
    OutputPcmFormat negotiatedFormat = OutputPcmFormat::F32LE;
    uint32_t outputBitDepth = 32;
    bool outputFormatLossless = true;
    bool ditherActive = true;
    uint64_t xrunCount = 0;
    uint64_t totalFramesProcessed = 0;
    bool isDirectMode = false;
    DirectModeStatus directModeStatus = DirectModeStatus::Unknown;
    uint64_t lastCallbackDurationNs = 0;
    uint64_t maxCallbackDurationNs = 0;
    uint64_t lastCallbackIntervalNs = 0;
    uint64_t expectedPeriodNs = 0;
    int64_t lastSchedJitterNs = 0;
    uint64_t maxAbsSchedJitterNs = 0;
    uint64_t callbackCount = 0;
};

struct BackendCapabilitiesReport {
    std::string backendName{"PipeWire"};
    CapabilityAvailability directMode = CapabilityAvailability::Unknown;
    CapabilityAvailability sampleRateFollowing = CapabilityAvailability::Unknown;
    CapabilityAvailability exclusiveAccess = CapabilityAvailability::Unknown;
    CapabilityAvailability hardwareBitPerfect = CapabilityAvailability::Unknown;
    CapabilityAvailability deviceSwitching = CapabilityAvailability::Supported;
    CapabilityAvailability latencyControl = CapabilityAvailability::Supported;
    CapabilityAvailability formatSelection = CapabilityAvailability::Supported;
    CapabilityAvailability externalInsert = CapabilityAvailability::Supported;
    CapabilityAvailability offlineRender = CapabilityAvailability::Supported;
};

/// Spec §8.1 Backend-specific Audio Control Adapter interface.
/// Separates Audio Control Manager policy from PipeWire / (future) ALSA / JACK mechanisms.
class AudioControlAdapter {
public:
    virtual ~AudioControlAdapter() = default;

    [[nodiscard]] virtual std::string backendName() const = 0;
    [[nodiscard]] virtual std::vector<AudioOutputDeviceInfo> enumerateOutputDevices() const = 0;
    [[nodiscard]] virtual std::string currentTargetDevice() const = 0;
    virtual bool setTargetDevice(const std::string& deviceIdOrName, std::string* outError = nullptr) = 0;
    virtual bool applyLatencySetting(uint32_t bufferFrames, double sampleRate) = 0;
    virtual bool renegotiateSampleRate(uint32_t targetRate, double& outSilenceMs) = 0;
    virtual bool setPreferredPcmFormat(OutputPcmFormat format) = 0;
    [[nodiscard]] virtual OutputPcmFormat preferredPcmFormat() const = 0;
    [[nodiscard]] virtual StreamDiagnostics diagnostics() const = 0;
    [[nodiscard]] virtual StreamState streamState() const = 0;
    [[nodiscard]] virtual BackendCapabilitiesReport queryCapabilities(bool offlineSimulation,
                                                                     bool streamInitialized) const = 0;
};

} // namespace desk32::engine::backend
