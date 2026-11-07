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
    SetNodeBypass
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
    float param4 = 0.0f;     // ReleaseMs / MakeupGainDb / PredelayMs
    float param5 = 0.0f;     // KneeDb
};

/// Lock-free Single-Producer Single-Consumer Parameter Command Queue.
/// Producer (Control / UI Thread) pushes validated parameter change commands.
/// Consumer (Audio Thread) drains commands at audio block boundaries.
/// Strictly RT-Safe: zero dynamic allocation, zero locks, non-blocking.
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

    /// Control Thread: Push command into queue. Returns true if queued, false if full.
    bool push(const DspCommand& cmd) noexcept {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        if (writeIdx - readIdx >= CAPACITY) {
            return false; // Queue full
        }
        m_buffer[writeIdx & MASK] = cmd;
        m_writeIndex.store(writeIdx + 1, std::memory_order_release);
        return true;
    }

    /// Audio Thread: Pop single command. Returns true if command retrieved, false if empty.
    bool pop(DspCommand& outCmd) noexcept {
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        if (readIdx == writeIdx) {
            return false; // Queue empty
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

private:
    std::array<DspCommand, CAPACITY> m_buffer{};
    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};
};

} // namespace desk32::engine::dsp
