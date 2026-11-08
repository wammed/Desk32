#pragma once

#include <cstdint>
#include <atomic>
#include <array>
#include <algorithm>
#include <string>

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

/// Lock-free Single-Producer Single-Consumer Parameter Command Queue (P0-16).
/// Producer (Control / UI Thread) pushes validated parameter change commands.
/// Consumer (Audio Thread) drains commands at audio block boundaries.
/// Strictly RT-Safe: zero dynamic allocation, zero locks, non-blocking.
/// Capacity & Coalescing Guarantees:
/// - Producer-side coalescing merges rapid redundant continuous updates (e.g., slider drags)
///   when the queue approaches capacity, preventing unneeded queue overflow without losing the latest state.
/// - Overflow tracking records any dropped commands and signals callers for UI notification.
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

    /// Checks if a command type represents continuous idempotent state that can safely be coalesced
    static constexpr bool isCoalesceable(DspCommandType type) noexcept {
        return type == DspCommandType::SetGeqBandGain ||
               type == DspCommandType::SetPeqBand ||
               type == DspCommandType::SetFilterParams ||
               type == DspCommandType::SetNodeBypass;
    }

    /// Control Thread: Push command into queue.
    /// If full, attempts to coalesce with existing unread matching command.
    /// If full and cannot coalesce, records overflow and returns false.
    bool push(const DspCommand& cmd) noexcept {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);

        // Standard case: queue has room
        if (writeIdx - readIdx < CAPACITY) {
            m_buffer[writeIdx & MASK] = cmd;
            m_writeIndex.store(writeIdx + 1, std::memory_order_release);
            return true;
        }

        // Queue full: Attempt coalescing into newest unread matching command
        if (isCoalesceable(cmd.type)) {
            for (size_t i = writeIdx; i > readIdx; --i) {
                size_t idx = (i - 1) & MASK;
                if (m_buffer[idx].type == cmd.type &&
                    m_buffer[idx].index == cmd.index &&
                    m_buffer[idx].subIndex == cmd.subIndex) {
                    m_buffer[idx] = cmd;
                    m_coalesceCount.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
            }
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

    [[nodiscard]] uint64_t coalesceCount() const noexcept {
        return m_coalesceCount.load(std::memory_order_relaxed);
    }

    void resetStatistics() noexcept {
        m_overflowCount.store(0, std::memory_order_relaxed);
        m_coalesceCount.store(0, std::memory_order_relaxed);
    }

private:
    std::array<DspCommand, CAPACITY> m_buffer{};
    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};

    alignas(64) std::atomic<uint64_t> m_overflowCount{0};
    alignas(64) std::atomic<uint64_t> m_coalesceCount{0};
};

} // namespace desk32::engine::dsp
