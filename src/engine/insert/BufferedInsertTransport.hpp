#pragma once

#include "ExternalInsertBridge.hpp"
#include "engine/buffer/PcmRingBuffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace desk32::engine::insert {

/// Shared lock-free ring-buffered transport base & deterministic simulation harness
/// for live graph External Insert transports (PipeWireInsertTransport & JackInsertTransport).
class BufferedInsertTransport : public IExternalInsertTransport {
public:
    static constexpr size_t RING_CAPACITY_FRAMES = 32768;

    BufferedInsertTransport()
        : m_simDelayL(RING_CAPACITY_FRAMES, 0.0)
        , m_simDelayR(RING_CAPACITY_FRAMES, 0.0)
    {}

    ~BufferedInsertTransport() override = default;

    [[nodiscard]] bool isConnected() const noexcept override {
        return m_connected.load(std::memory_order_acquire);
    }

    bool exchangeBlock(double* inOutLeft, double* inOutRight, size_t frames) noexcept override {
        if (!inOutLeft || !inOutRight || frames == 0 || !m_connected.load(std::memory_order_acquire)) {
            return false;
        }

        if (m_simulationMode.load(std::memory_order_acquire)) {
            const size_t delay = static_cast<size_t>(m_simLatencyFrames.load(std::memory_order_acquire));
            const size_t cap = m_simDelayL.size();
            const size_t mask = cap - 1;
            for (size_t i = 0; i < frames; ++i) {
                const size_t wIdx = (m_simWritePos + delay) & mask;
                m_simDelayL[wIdx] = inOutLeft[i];
                m_simDelayR[wIdx] = inOutRight[i];

                const size_t rIdx = m_simWritePos & mask;
                inOutLeft[i] = m_simDelayL[rIdx];
                inOutRight[i] = m_simDelayR[rIdx];
                m_simDelayL[rIdx] = 0.0;
                m_simDelayR[rIdx] = 0.0;
                ++m_simWritePos;
            }
            m_sendBlocksWritten.fetch_add(1, std::memory_order_relaxed);
            m_returnBlocksRead.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        const size_t safeFrames = std::min(frames, m_rtReadL.size());
        const size_t written = m_sendRing.writePlanar(inOutLeft, inOutRight, safeFrames);
        if (written < safeFrames) {
            m_xrunOrUnderruns.fetch_add(1, std::memory_order_relaxed);
        }
        m_sendBlocksWritten.fetch_add(1, std::memory_order_relaxed);

        const size_t read = m_returnRing.readPlanar(m_rtReadL.data(), m_rtReadR.data(), safeFrames);
        if (read < safeFrames) {
            if (m_returnBlocksRead.load(std::memory_order_relaxed) > 4) {
                m_xrunOrUnderruns.fetch_add(1, std::memory_order_relaxed);
            }
            std::fill(m_rtReadL.begin() + static_cast<std::ptrdiff_t>(read),
                      m_rtReadL.begin() + static_cast<std::ptrdiff_t>(safeFrames), 0.0);
            std::fill(m_rtReadR.begin() + static_cast<std::ptrdiff_t>(read),
                      m_rtReadR.begin() + static_cast<std::ptrdiff_t>(safeFrames), 0.0);
        }
        std::copy_n(m_rtReadL.data(), safeFrames, inOutLeft);
        std::copy_n(m_rtReadR.data(), safeFrames, inOutRight);
        m_returnBlocksRead.fetch_add(1, std::memory_order_relaxed);
        return read > 0;
    }

    [[nodiscard]] bool hasRateMismatch() const noexcept override {
        return m_rateMismatch.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool hasBufferMismatch() const noexcept override {
        return m_bufferMismatch.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool consumeGraphDisconnectEvent() noexcept override {
        return m_graphDisconnected.exchange(false, std::memory_order_acq_rel);
    }

    [[nodiscard]] InsertTransportDiagnostics diagnostics() const override {
        InsertTransportDiagnostics d;
        d.connected = m_connected.load(std::memory_order_acquire);
        d.simulationMode = m_simulationMode.load(std::memory_order_acquire);
        d.sendNodeId = m_sendNodeId.load(std::memory_order_acquire);
        d.returnNodeId = m_returnNodeId.load(std::memory_order_acquire);
        d.resolvedTargetPorts = m_resolvedTargetPorts.load(std::memory_order_acquire);
        d.activeLinksCreated = m_activeLinksCreated.load(std::memory_order_acquire);
        d.sendBlocksWritten = m_sendBlocksWritten.load(std::memory_order_relaxed);
        d.returnBlocksRead = m_returnBlocksRead.load(std::memory_order_relaxed);
        d.xrunOrUnderruns = m_xrunOrUnderruns.load(std::memory_order_relaxed);
        d.negotiatedSendRate = m_negotiatedSendRate.load(std::memory_order_acquire);
        d.negotiatedReturnRate = m_negotiatedReturnRate.load(std::memory_order_acquire);
        d.negotiatedQuantum = m_negotiatedQuantum.load(std::memory_order_acquire);
        d.rateMismatchDetected = m_rateMismatch.load(std::memory_order_acquire);
        d.bufferMismatchDetected = m_bufferMismatch.load(std::memory_order_acquire);
        d.graphDisconnected = m_graphDisconnected.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        d.errorDetail = m_errorDetail;
        return d;
    }

    // Headless / CI Simulation & Event Injection Hooks
    void setSimulationMode(bool enabled) noexcept {
        m_simulationMode.store(enabled, std::memory_order_release);
    }
    void setSimulationLoopbackLatencyFrames(uint32_t frames) noexcept {
        m_simLatencyFrames.store(
            std::min<uint32_t>(frames, static_cast<uint32_t>(RING_CAPACITY_FRAMES / 2)),
            std::memory_order_release);
    }
    void simulateGraphNodeRemoved() noexcept {
        m_graphDisconnected.store(true, std::memory_order_release);
        m_connected.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = "Simulated graph disconnect for external insert node";
    }
    void simulateRateMismatch(uint32_t actualRate) noexcept {
        m_negotiatedSendRate.store(actualRate, std::memory_order_release);
        m_rateMismatch.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = "External insert stream rate mismatch (" + std::to_string(actualRate) + " Hz)";
    }
    void simulateBufferMismatch(uint32_t actualQuantum) noexcept {
        m_negotiatedQuantum.store(actualQuantum, std::memory_order_release);
        m_bufferMismatch.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_errorDetail = "External insert stream quantum mismatch (" + std::to_string(actualQuantum) + " frames)";
    }

protected:
    void resetCommonTransportState(const ExternalPortInfo& target,
                                   double engineSampleRate,
                                   size_t engineBufferSize)
    {
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_targetConfig = target;
            m_expectedSampleRate = engineSampleRate > 0.0 ? engineSampleRate : 48000.0;
            m_expectedBufferSize = engineBufferSize > 0 ? engineBufferSize : 256;
            m_errorDetail.clear();
        }
        m_sendRing.fullReset();
        m_returnRing.fullReset();
        std::fill(m_simDelayL.begin(), m_simDelayL.end(), 0.0);
        std::fill(m_simDelayR.begin(), m_simDelayR.end(), 0.0);
        m_simWritePos = 0;

        m_rateMismatch.store(false, std::memory_order_release);
        m_bufferMismatch.store(false, std::memory_order_release);
        m_graphDisconnected.store(false, std::memory_order_release);
        m_resolvedTargetPorts.store(0, std::memory_order_release);
        m_activeLinksCreated.store(0, std::memory_order_release);
        m_sendBlocksWritten.store(0, std::memory_order_release);
        m_returnBlocksRead.store(0, std::memory_order_release);
        m_xrunOrUnderruns.store(0, std::memory_order_release);
        m_negotiatedSendRate.store(0, std::memory_order_release);
        m_negotiatedReturnRate.store(0, std::memory_order_release);
        m_negotiatedQuantum.store(0, std::memory_order_release);
        m_sendNodeId.store(0, std::memory_order_release);
        m_returnNodeId.store(0, std::memory_order_release);
    }

    bool completeSimulationConnect() noexcept {
        m_resolvedTargetPorts.store(4, std::memory_order_release);
        m_activeLinksCreated.store(4, std::memory_order_release);
        m_negotiatedSendRate.store(static_cast<uint32_t>(m_expectedSampleRate), std::memory_order_release);
        m_negotiatedReturnRate.store(static_cast<uint32_t>(m_expectedSampleRate), std::memory_order_release);
        m_negotiatedQuantum.store(static_cast<uint32_t>(m_expectedBufferSize), std::memory_order_release);
        m_connected.store(true, std::memory_order_release);
        return true;
    }

    mutable std::mutex m_stateMutex;
    ExternalPortInfo m_targetConfig;
    double m_expectedSampleRate = 48000.0;
    size_t m_expectedBufferSize = 256;
    std::string m_errorDetail;

    std::atomic<bool> m_simulationMode{false};
    std::atomic<uint32_t> m_simLatencyFrames{0};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_rateMismatch{false};
    std::atomic<bool> m_bufferMismatch{false};
    std::atomic<bool> m_graphDisconnected{false};
    std::atomic<uint32_t> m_resolvedTargetPorts{0};
    std::atomic<uint32_t> m_activeLinksCreated{0};
    std::atomic<uint64_t> m_sendBlocksWritten{0};
    std::atomic<uint64_t> m_returnBlocksRead{0};
    std::atomic<uint64_t> m_xrunOrUnderruns{0};
    std::atomic<uint32_t> m_negotiatedSendRate{0};
    std::atomic<uint32_t> m_negotiatedReturnRate{0};
    std::atomic<uint32_t> m_negotiatedQuantum{0};
    std::atomic<uint32_t> m_sendNodeId{0};
    std::atomic<uint32_t> m_returnNodeId{0};

    buffer::PcmRingBuffer m_sendRing{RING_CAPACITY_FRAMES};
    buffer::PcmRingBuffer m_returnRing{RING_CAPACITY_FRAMES};
    std::array<double, 8192> m_rtReadL{};
    std::array<double, 8192> m_rtReadR{};
    std::array<double, 8192> m_pwSendTmpL{};
    std::array<double, 8192> m_pwSendTmpR{};
    std::array<double, 8192> m_pwRetTmpL{};
    std::array<double, 8192> m_pwRetTmpR{};

    std::vector<double> m_simDelayL;
    std::vector<double> m_simDelayR;
    size_t m_simWritePos = 0;
};

} // namespace desk32::engine::insert
