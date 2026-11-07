#pragma once

#include <vector>
#include <atomic>
#include <cstddef>
#include <algorithm>
#include <cstring>

namespace desk32::engine::insert {

/// Real-time safe delay line for Plugin Delay Compensation (PDC).
/// Used to align parallel paths or compensate for external insert latency.
/// Operates on planar float64 buffers with strictly ZERO heap allocations in RT path.
class DelayCompensationBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 65536; // ~1.36s at 48kHz, ~0.34s at 192kHz
    static constexpr size_t MASK = DEFAULT_CAPACITY - 1;

    explicit DelayCompensationBuffer(size_t capacity = DEFAULT_CAPACITY)
        : m_capacity(DEFAULT_CAPACITY),
          m_bufL(DEFAULT_CAPACITY, 0.0),
          m_bufR(DEFAULT_CAPACITY, 0.0)
    {
        (void)capacity; // Fixed power of two
    }

    ~DelayCompensationBuffer() = default;

    DelayCompensationBuffer(const DelayCompensationBuffer&) = delete;
    DelayCompensationBuffer& operator=(const DelayCompensationBuffer&) = delete;

    [[nodiscard]] size_t delayFrames() const noexcept {
        return m_delayFrames.load(std::memory_order_relaxed);
    }

    void setDelayFrames(size_t frames) noexcept {
        m_delayFrames.store(std::min(frames, m_capacity - 1), std::memory_order_relaxed);
    }

    void reset() noexcept {
        std::fill(m_bufL.begin(), m_bufL.end(), 0.0);
        std::fill(m_bufR.begin(), m_bufR.end(), 0.0);
        m_writeIndex = 0;
    }

    /// Process planar audio block through delay line.
    /// Strictly RT-Safe: Zero dynamic allocation, zero locks, zero blocking.
    void process(double* left, double* right, size_t frameCount) noexcept {
        size_t delay = m_delayFrames.load(std::memory_order_relaxed);
        if (delay == 0 || !left || !right || frameCount == 0) {
            return; // 0 delay -> straight pass-through
        }

        for (size_t i = 0; i < frameCount; ++i) {
            size_t readPos = (m_writeIndex + m_capacity - delay) & MASK;
            double inL = left[i];
            double inR = right[i];

            left[i]  = m_bufL[readPos];
            right[i] = m_bufR[readPos];

            m_bufL[m_writeIndex & MASK] = inL;
            m_bufR[m_writeIndex & MASK] = inR;

            m_writeIndex = (m_writeIndex + 1) & MASK;
        }
    }

private:
    const size_t m_capacity;
    std::atomic<size_t> m_delayFrames{0};
    size_t m_writeIndex{0};

    std::vector<double> m_bufL;
    std::vector<double> m_bufR;
};

} // namespace desk32::engine::insert
