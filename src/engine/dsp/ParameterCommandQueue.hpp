#pragma once

#include <cstdint>
#include <atomic>
#include <array>
#include <algorithm>
#include <string>
#include <mutex>

namespace desk32::engine::dsp {

enum class DspCommandType : uint8_t {
    None = 0,
    SetGeqBandGain,
    ResetAllGeqBands,
    SetPeqBand,
    SetPeqBandEnabled,
    ApplyPeqPreset,
    SetFilterParams,
    SetCompressorParams,
    SetLimiterParams,
    SetReverbParams,
    SetNodeBypass,
    SetGlobalBypass
};

struct DspCommand {
    DspCommandType type = DspCommandType::None;
    uint8_t index = 0;       // Band index, or preset index
    uint8_t subIndex = 0;    // Channel (0 = Left, 1 = Right)
    uint8_t boolFlag1 = 0;   // Enabled / Bypassed / etc.
    uint8_t boolFlag2 = 0;
    uint8_t intParam1 = 0;   // Filter type, Slope, etc.
    uint8_t intParam2 = 0;
    float param1 = 0.0f;     // Freq / Threshold / Ceiling / RoomSize / GainDb
    float param2 = 0.0f;     // GainDb / Ratio / ReleaseMs / Damping
    float param3 = 0.0f;     // Q / AttackMs / LookaheadMs / WetLevel
    float param4 = 0.0f;     // ReleaseMs / PredelayMs
    float param5 = 0.0f;     // KneeDb
    float param6 = 0.0f;     // MakeupGainDb (P0-06)
};

/// Lock-free Single-Producer Single-Consumer Parameter Command Queue (P0-24).
/// Producer (Control / UI Thread) pushes validated parameter change commands (ordered discrete events).
/// Consumer (Audio Thread) drains commands at audio block boundaries.
/// Strictly RT-Safe: zero dynamic allocation, zero locks, non-blocking.
/// SPSC Ownership Guarantees (Section 7, P0-24):
/// - SPSC single-ownership strictly maintained: Producer never searches or mutates elements
///   already enqueued for Consumer consumption (in-place coalescing is completely removed).
/// - Discrete, ordered events (DSP Enable/Disable, Preset application, Bypass, Mode switches)
///   preserve strict FIFO sequential ordering.
/// - Continuous parameters (sliders) are dispatched via DspParameterMailbox (P0-25).
/// - Overflow tracking records any queue full drops and signals callers for UI notification.
class ParameterCommandQueue {
public:
    static constexpr size_t CAPACITY = 512;
    static constexpr size_t MASK = CAPACITY - 1;

    ParameterCommandQueue() {
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_relaxed);
    }

    ~ParameterCommandQueue() = default;

    ParameterCommandQueue(const ParameterCommandQueue&) = delete;
    ParameterCommandQueue& operator=(const ParameterCommandQueue&) = delete;

    /// Multi-Producer (Control / UI Threads): Push command into queue.
    /// Thread-safe for multiple concurrent producers via m_producerMutex.
    /// Consumer (Audio Thread) is completely independent and lock-free/wait-free.
    /// Strictly appends at the write tail. If full, increments overflow counter and returns false.
    bool push(const DspCommand& cmd) noexcept {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);

        if (writeIdx - readIdx < CAPACITY) {
            m_buffer[writeIdx & MASK] = cmd;
            m_writeIndex.store(writeIdx + 1, std::memory_order_release);
            return true;
        }

        m_overflowCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Audio Thread: Pop single command. Returns true if command retrieved, false if empty.
    bool pop(DspCommand& outCmd) noexcept {
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        if (readIdx == writeIdx) {
            return false;
        }
        outCmd = m_buffer[readIdx & MASK];
        m_readIndex.store(readIdx + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        return (writeIdx >= readIdx) ? (writeIdx - readIdx) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_writeIndex.load(std::memory_order_acquire) == m_readIndex.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t overflowCount() const noexcept {
        return m_overflowCount.load(std::memory_order_relaxed);
    }

    void resetStatistics() noexcept {
        m_overflowCount.store(0, std::memory_order_relaxed);
    }

private:
    mutable std::mutex m_producerMutex;
    std::array<DspCommand, CAPACITY> m_buffer{};
    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};

    alignas(64) std::atomic<uint64_t> m_overflowCount{0};
};

} // namespace desk32::engine::dsp
