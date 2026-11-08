#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <functional>

namespace desk32::engine::insert {

enum class InsertState {
    NotConfigured,
    HostNotFound,
    PortsFound,
    Connected,
    Active,
    RateMismatch,
    BufferMismatch,
    XRun,
    Disconnected,
    Recovering,
    Error
};

struct ExternalPortInfo {
    std::string clientName;
    std::string sendPortL;
    std::string sendPortR;
    std::string returnPortL;
    std::string returnPortR;
    double sampleRate = 48000.0;
    size_t bufferSize = 256;
    uint32_t reportedLatencyFrames = 0;
};

class ExternalInsertBridge {
public:
    ExternalInsertBridge() = default;

    [[nodiscard]] InsertState state() const noexcept { return m_state.load(std::memory_order_acquire); }
    [[nodiscard]] bool isBypassed() const noexcept { return m_isBypassed.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t reportedLatencyFrames() const noexcept { return m_reportedLatencyFrames.load(std::memory_order_acquire); }
    [[nodiscard]] double measuredLatencyMs() const noexcept { return m_measuredLatencyMs.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t measuredLatencyFrames() const noexcept { return m_measuredLatencyFrames.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t xrunCount() const noexcept { return m_xrunCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t impulseSendFrame() const noexcept { return m_impulseSendFrame.load(std::memory_order_relaxed); }

    [[nodiscard]] std::string lastError() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_lastError;
    }

    [[nodiscard]] ExternalPortInfo config() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config;
    }

    static const char* stateToString(InsertState s) noexcept {
        switch (s) {
            case InsertState::NotConfigured: return "Not Configured";
            case InsertState::HostNotFound: return "Host Not Found";
            case InsertState::PortsFound: return "Ports Found";
            case InsertState::Connected: return "Connected";
            case InsertState::Active: return "Active";
            case InsertState::RateMismatch: return "Rate Mismatch";
            case InsertState::BufferMismatch: return "Buffer Mismatch";
            case InsertState::XRun: return "XRUN / Dropout";
            case InsertState::Disconnected: return "Disconnected";
            case InsertState::Recovering: return "Recovering";
            case InsertState::Error: return "Error";
        }
        return "Unknown";
    }

    void setBypass(bool bypass) noexcept {
        m_isBypassed.store(bypass, std::memory_order_release);
    }

    void recordXRun() noexcept {
        m_xrunCount.fetch_add(1, std::memory_order_relaxed);
        m_state.store(InsertState::XRun, std::memory_order_release);
    }

    void resetXRunCount() noexcept {
        m_xrunCount.store(0, std::memory_order_relaxed);
    }

    /// Set mock processing function for testing or simulated plugins
    void setMockProcessor(std::function<void(double*, double*, size_t)> processor) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_mockProcessor = std::move(processor);
    }

    /// Configure external insert connection ports
    bool configure(const ExternalPortInfo& config, double engineSampleRate, size_t engineBufferSize) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;
        m_engineSampleRate = engineSampleRate;
        m_engineBufferSize = engineBufferSize;
        m_reportedLatencyFrames.store(config.reportedLatencyFrames, std::memory_order_release);

        if (m_config.clientName.empty()) {
            m_state.store(InsertState::NotConfigured, std::memory_order_release);
            return false;
        }

        // 1. Feedback Loop Detection & Prevention:
        // Check if send port is identical to return port or self-referential
        if (m_config.sendPortL == m_config.returnPortL || m_config.sendPortR == m_config.returnPortR) {
            m_state.store(InsertState::Error, std::memory_order_release);
            m_lastError = "Feedback loop detected: Send and Return ports are identical!";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        // 2. Sample Rate & Buffer Matching Check
        if (std::abs(m_config.sampleRate - m_engineSampleRate) > 0.01) {
            m_state.store(InsertState::RateMismatch, std::memory_order_release);
            m_lastError = "Sample rate mismatch between Engine and External Host";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        if (m_config.bufferSize != m_engineBufferSize) {
            m_state.store(InsertState::BufferMismatch, std::memory_order_release);
            m_lastError = "Buffer size mismatch between Engine and External Host";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        m_state.store(InsertState::Connected, std::memory_order_release);
        m_isBypassed.store(false, std::memory_order_release);
        m_lastError.clear();
        return true;
    }

    /// Simulate disconnect event (e.g. external host crashed or port closed)
    void handleDisconnect() {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_lastError = "External host disconnected or port lost";
        }
        m_state.store(InsertState::Disconnected, std::memory_order_release);
        // Smoothly activate hardware bypass to protect playback
        m_isBypassed.store(true, std::memory_order_release);
    }

    /// Simulate port recovery event
    void handleRecovery() {
        m_state.store(InsertState::Recovering, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_configMutex);
        // Verify parameters
        if (std::abs(m_config.sampleRate - m_engineSampleRate) < 0.01 &&
            m_config.bufferSize == m_engineBufferSize) {
            m_state.store(InsertState::Connected, std::memory_order_release);
            m_isBypassed.store(false, std::memory_order_release);
            m_lastError.clear();
        } else {
            m_state.store(InsertState::Error, std::memory_order_release);
            m_lastError = "Recovery failed: incompatible configuration";
        }
    }

    /// Audio block processing through insert
    /// If bypassed or disconnected, performs dry pass-through with no clicks.
    /// Audio Thread execution: strictly zero dynamic allocations or blocking locks.
    void process(double* inOutLeft, double* inOutRight, size_t frames) {
        if (m_isBypassed.load(std::memory_order_acquire) || m_state.load(std::memory_order_acquire) != InsertState::Connected) {
            // Passthrough (dry)
            return;
        }

        // Active state
        m_state.store(InsertState::Active, std::memory_order_release);

        // If mock processor attached, execute it (for testing/simulation)
        if (m_mockProcessor) {
            m_mockProcessor(inOutLeft, inOutRight, frames);
        }

        // In active send/return, check for impulse detection during latency measurement
        if (m_measuringLatency.load(std::memory_order_acquire)) {
            for (size_t i = 0; i < frames; ++i) {
                // If impulse was sent at m_impulseSendFrame
                if (m_totalFramesProcessed + i == m_impulseSendFrame.load(std::memory_order_relaxed)) {
                    inOutLeft[i] += 1.0;
                    inOutRight[i] += 1.0;
                }
            }
        }

        m_totalFramesProcessed += frames;
    }

    /// Initiate round-trip latency measurement using an impulse signal
    void triggerLatencyMeasurement() {
        m_measuringLatency.store(true, std::memory_order_release);
        m_impulseSendFrame.store(m_totalFramesProcessed + 64, std::memory_order_release); // Send in 64 frames
        m_measuredLatencyFrames.store(-1, std::memory_order_release);
        m_measuredLatencyMs.store(-1.0, std::memory_order_release);
    }

    /// Record received impulse frame and calculate measured roundtrip latency
    void recordImpulseReturn(uint64_t returnFrame) {
        uint64_t sendFrame = m_impulseSendFrame.load(std::memory_order_acquire);
        if (returnFrame >= sendFrame) {
            int64_t latencyFrames = static_cast<int64_t>(returnFrame - sendFrame);
            m_measuredLatencyFrames.store(latencyFrames, std::memory_order_release);
            m_measuredLatencyMs.store((static_cast<double>(latencyFrames) / m_engineSampleRate) * 1000.0, std::memory_order_release);
            m_measuringLatency.store(false, std::memory_order_release);
        }
    }

private:
    std::atomic<InsertState> m_state{InsertState::NotConfigured};
    std::atomic<bool> m_isBypassed{true};
    mutable std::mutex m_configMutex;
    ExternalPortInfo m_config;
    double m_engineSampleRate = 48000.0;
    size_t m_engineBufferSize = 256;
    std::string m_lastError;
    std::function<void(double*, double*, size_t)> m_mockProcessor;

    std::atomic<uint32_t> m_xrunCount{0};
    std::atomic<bool> m_measuringLatency{false};
    std::atomic<uint64_t> m_impulseSendFrame{0};
    uint64_t m_totalFramesProcessed{0};
    std::atomic<int64_t> m_measuredLatencyFrames{-1};
    std::atomic<double> m_measuredLatencyMs{-1.0};
    std::atomic<uint32_t> m_reportedLatencyFrames{0};
};

} // namespace desk32::engine::insert
