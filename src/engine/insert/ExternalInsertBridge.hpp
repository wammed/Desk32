#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <functional>
#include <memory>

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

enum class LatencySource {
    Uncalibrated,
    ReportedStatic,
    SimulatedMock,
    HardwareMeasured
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
    using MockProcessorFunc = std::function<void(double*, double*, size_t)>;

    struct MockHolder {
        MockProcessorFunc func;
        MockHolder* nextRetired = nullptr;
    };

    ExternalInsertBridge() = default;

    ~ExternalInsertBridge() {
        reclaimRetiredMocks();
        delete m_preparedMock.exchange(nullptr, std::memory_order_acquire);
        delete m_activeMock;
    }

    ExternalInsertBridge(const ExternalInsertBridge&) = delete;
    ExternalInsertBridge& operator=(const ExternalInsertBridge&) = delete;

    [[nodiscard]] InsertState state() const noexcept { return m_state.load(std::memory_order_acquire); }
    [[nodiscard]] bool isBypassed() const noexcept { return m_isBypassed.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t reportedLatencyFrames() const noexcept { return m_reportedLatencyFrames.load(std::memory_order_acquire); }
    [[nodiscard]] double measuredLatencyMs() const noexcept { return m_measuredLatencyMs.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t measuredLatencyFrames() const noexcept { return m_measuredLatencyFrames.load(std::memory_order_acquire); }
    [[nodiscard]] LatencySource latencySource() const noexcept { return m_latencySource.load(std::memory_order_acquire); }
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

    static const char* latencySourceToString(LatencySource src) noexcept {
        switch (src) {
            case LatencySource::Uncalibrated: return "Uncalibrated";
            case LatencySource::ReportedStatic: return "Host Reported (Static)";
            case LatencySource::SimulatedMock: return "Simulated Mock";
            case LatencySource::HardwareMeasured: return "Hardware Measured (Physical)";
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

    /// Set mock processing function for testing or simulated plugins (lock-free Prepared Instance Exchange, P0-21/P0-27)
    /// Strictly zero shared_ptr atomic spinlocks and zero Audio Thread deallocations.
    void setMockProcessor(MockProcessorFunc processor) {
        auto next = std::make_unique<MockHolder>();
        next->func = std::move(processor);
        MockHolder* unconsumed = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            unconsumed = m_preparedMock.exchange(next.release(), std::memory_order_acq_rel);
        }
        delete unconsumed;
        reclaimRetiredMocks();
    }

    /// Reclaim retired MockHolder instances strictly on Control Thread (P0-21/P0-27)
    void reclaimRetiredMocks() noexcept {
        MockHolder* cur = m_retiredMockHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            MockHolder* next = cur->nextRetired;
            delete cur;
            cur = next;
        }
    }

    void setDelayCompensationFrames(size_t frames) noexcept {
        m_delayCompensationFrames.store(static_cast<int64_t>(frames), std::memory_order_release);
    }

    [[nodiscard]] int64_t delayCompensationFrames() const noexcept {
        return m_delayCompensationFrames.load(std::memory_order_acquire);
    }

    /// Configure external insert connection ports
    bool configure(const ExternalPortInfo& config, double engineSampleRate, size_t engineBufferSize) {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;
        m_engineSampleRate.store(engineSampleRate, std::memory_order_release);
        m_engineBufferSize.store(engineBufferSize, std::memory_order_release);
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
        if (std::abs(m_config.sampleRate - engineSampleRate) > 0.01) {
            m_state.store(InsertState::RateMismatch, std::memory_order_release);
            m_lastError = "Sample rate mismatch between Engine and External Host";
            m_isBypassed.store(true, std::memory_order_release);
            return false;
        }

        if (m_config.bufferSize != engineBufferSize) {
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
        const double sr = m_engineSampleRate.load(std::memory_order_acquire);
        const size_t bs = m_engineBufferSize.load(std::memory_order_acquire);
        // Verify parameters
        if (std::abs(m_config.sampleRate - sr) < 0.01 &&
            m_config.bufferSize == bs) {
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
    /// Audio Thread execution: strictly zero dynamic allocations, zero deallocations, zero blocking locks (P0-21/P0-23/P0-27).
    void process(double* inOutLeft, double* inOutRight, size_t frames) noexcept {
        // 1. Activate prepared mock processor at block boundary if present
        if (auto* prepared = m_preparedMock.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_activeMock) {
                retireMock(m_activeMock);
            }
            m_activeMock = prepared;
        }

        const InsertState curState = m_state.load(std::memory_order_acquire);
        if (m_isBypassed.load(std::memory_order_acquire) ||
            (curState != InsertState::Connected && curState != InsertState::Active)) {
            // Passthrough (dry)
            return;
        }

        // Transition Connected -> Active on first active block
        if (curState == InsertState::Connected) {
            m_state.store(InsertState::Active, std::memory_order_release);
        }

        // Execute active mock processor without any locks or refcounting on Audio Thread
        if (m_activeMock && m_activeMock->func) {
            m_activeMock->func(inOutLeft, inOutRight, frames);
        }

        // In active send/return, check for impulse detection during latency measurement
        const uint64_t baseFrame = m_totalFramesProcessed.load(std::memory_order_relaxed);
        if (m_measuringLatency.load(std::memory_order_acquire)) {
            const uint64_t targetFrame = m_impulseSendFrame.load(std::memory_order_relaxed);
            for (size_t i = 0; i < frames; ++i) {
                if (baseFrame + i == targetFrame) {
                    inOutLeft[i] += 1.0;
                    inOutRight[i] += 1.0;
                }
            }
        }

        m_totalFramesProcessed.store(baseFrame + frames, std::memory_order_release);
    }

    /// Initiate round-trip latency measurement using an impulse signal
    void triggerLatencyMeasurement() noexcept {
        m_measuringLatency.store(true, std::memory_order_release);
        const uint64_t curFrames = m_totalFramesProcessed.load(std::memory_order_acquire);
        m_impulseSendFrame.store(curFrames + 64, std::memory_order_release); // Send in 64 frames
        m_measuredLatencyFrames.store(-1, std::memory_order_release);
        m_measuredLatencyMs.store(-1.0, std::memory_order_release);
    }

    /// Record received impulse frame and calculate measured roundtrip latency
    void recordImpulseReturn(uint64_t returnFrame, LatencySource source = LatencySource::SimulatedMock) noexcept {
        uint64_t sendFrame = m_impulseSendFrame.load(std::memory_order_acquire);
        if (returnFrame >= sendFrame) {
            int64_t latencyFrames = static_cast<int64_t>(returnFrame - sendFrame);
            const double sr = std::max(1.0, m_engineSampleRate.load(std::memory_order_acquire));
            m_measuredLatencyFrames.store(latencyFrames, std::memory_order_release);
            m_measuredLatencyMs.store((static_cast<double>(latencyFrames) / sr) * 1000.0, std::memory_order_release);
            m_latencySource.store(source, std::memory_order_release);
            m_measuringLatency.store(false, std::memory_order_release);
        }
    }

private:
    void retireMock(MockHolder* old) noexcept {
        old->nextRetired = m_retiredMockHead.load(std::memory_order_relaxed);
        while (!m_retiredMockHead.compare_exchange_weak(old->nextRetired, old,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed));
    }

    std::atomic<InsertState> m_state{InsertState::NotConfigured};
    std::atomic<bool> m_isBypassed{true};
    mutable std::mutex m_configMutex;
    ExternalPortInfo m_config;
    std::atomic<double> m_engineSampleRate{48000.0};
    std::atomic<size_t> m_engineBufferSize{256};
    std::string m_lastError;

    MockHolder* m_activeMock{nullptr};
    alignas(64) std::atomic<MockHolder*> m_preparedMock{nullptr};
    alignas(64) std::atomic<MockHolder*> m_retiredMockHead{nullptr};

    std::atomic<uint32_t> m_xrunCount{0};
    std::atomic<bool> m_measuringLatency{false};
    std::atomic<uint64_t> m_impulseSendFrame{0};
    std::atomic<uint64_t> m_totalFramesProcessed{0};
    std::atomic<int64_t> m_measuredLatencyFrames{-1};
    std::atomic<double> m_measuredLatencyMs{-1.0};
    std::atomic<uint32_t> m_reportedLatencyFrames{0};
    std::atomic<int64_t> m_delayCompensationFrames{-1};
    std::atomic<LatencySource> m_latencySource{LatencySource::Uncalibrated};
};

} // namespace desk32::engine::insert
