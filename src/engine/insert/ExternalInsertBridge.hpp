#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <chrono>
#include <algorithm>

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

    [[nodiscard]] InsertState state() const noexcept { return m_state; }
    [[nodiscard]] bool isBypassed() const noexcept { return m_isBypassed; }
    [[nodiscard]] uint32_t reportedLatencyFrames() const noexcept { return m_config.reportedLatencyFrames; }
    [[nodiscard]] double measuredLatencyMs() const noexcept { return m_measuredLatencyMs; }
    [[nodiscard]] int64_t measuredLatencyFrames() const noexcept { return m_measuredLatencyFrames; }
    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }

    void setBypass(bool bypass) noexcept {
        m_isBypassed = bypass;
    }

    /// Configure external insert connection ports
    bool configure(const ExternalPortInfo& config, double engineSampleRate, size_t engineBufferSize) {
        m_config = config;
        m_engineSampleRate = engineSampleRate;
        m_engineBufferSize = engineBufferSize;

        if (m_config.clientName.empty()) {
            m_state = InsertState::NotConfigured;
            return false;
        }

        // 1. Feedback Loop Detection & Prevention:
        // Check if send port is identical to return port or self-referential
        if (m_config.sendPortL == m_config.returnPortL || m_config.sendPortR == m_config.returnPortR) {
            m_state = InsertState::Error;
            m_lastError = "Feedback loop detected: Send and Return ports are identical!";
            m_isBypassed = true;
            return false;
        }

        // 2. Sample Rate & Buffer Matching Check
        if (std::abs(m_config.sampleRate - m_engineSampleRate) > 0.01) {
            m_state = InsertState::RateMismatch;
            m_lastError = "Sample rate mismatch between Engine and External Host";
            m_isBypassed = true;
            return false;
        }

        if (m_config.bufferSize != m_engineBufferSize) {
            m_state = InsertState::BufferMismatch;
            m_lastError = "Buffer size mismatch between Engine and External Host";
            m_isBypassed = true;
            return false;
        }

        m_state = InsertState::Connected;
        m_isBypassed = false;
        m_lastError.clear();
        return true;
    }

    /// Simulate disconnect event (e.g. external host crashed or port closed)
    void handleDisconnect() {
        m_state = InsertState::Disconnected;
        m_lastError = "External host disconnected or port lost";
        // Smoothly activate hardware bypass to protect playback
        m_isBypassed = true;
    }

    /// Simulate port recovery event
    void handleRecovery() {
        m_state = InsertState::Recovering;
        // Verify parameters
        if (std::abs(m_config.sampleRate - m_engineSampleRate) < 0.01 &&
            m_config.bufferSize == m_engineBufferSize) {
            m_state = InsertState::Connected;
            m_isBypassed = false;
            m_lastError.clear();
        } else {
            m_state = InsertState::Error;
            m_lastError = "Recovery failed: incompatible configuration";
        }
    }

    /// Audio block processing through insert
    /// If bypassed or disconnected, performs dry pass-through with no clicks.
    void process(double* inOutLeft, double* inOutRight, size_t frames) {
        if (m_isBypassed || m_state != InsertState::Connected) {
            // Passthrough (dry)
            return;
        }

        // Active state
        m_state = InsertState::Active;

        // In active send/return, signal is sent to external host and received.
        // If testing latency measurement, check for impulse detection.
        if (m_measuringLatency) {
            for (size_t i = 0; i < frames; ++i) {
                // If impulse was sent at m_impulseSendFrame
                if (m_totalFramesProcessed + i == m_impulseSendFrame) {
                    inOutLeft[i] += 1.0;
                    inOutRight[i] += 1.0;
                }
            }
        }

        m_totalFramesProcessed += frames;
    }

    /// Initiate round-trip latency measurement using an impulse signal
    void triggerLatencyMeasurement() {
        m_measuringLatency = true;
        m_impulseSendFrame = m_totalFramesProcessed + 64; // Send in 64 frames
        m_measuredLatencyFrames = -1;
        m_measuredLatencyMs = -1.0;
    }

    /// Record received impulse frame and calculate measured roundtrip latency
    void recordImpulseReturn(uint64_t returnFrame) {
        if (returnFrame >= m_impulseSendFrame) {
            m_measuredLatencyFrames = static_cast<int64_t>(returnFrame - m_impulseSendFrame);
            m_measuredLatencyMs = (static_cast<double>(m_measuredLatencyFrames) / m_engineSampleRate) * 1000.0;
            m_measuringLatency = false;
        }
    }

private:
    InsertState m_state = InsertState::NotConfigured;
    bool m_isBypassed = true;
    ExternalPortInfo m_config;
    double m_engineSampleRate = 48000.0;
    size_t m_engineBufferSize = 256;
    std::string m_lastError;

    bool m_measuringLatency = false;
    uint64_t m_impulseSendFrame = 0;
    uint64_t m_totalFramesProcessed = 0;
    int64_t m_measuredLatencyFrames = -1;
    double m_measuredLatencyMs = -1.0;
};

} // namespace desk32::engine::insert

