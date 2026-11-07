#pragma once

#include <vector>
#include <atomic>
#include <cstddef>
#include <algorithm>
#include <cstring>

namespace desk32::engine::buffer {

struct StereoFrame {
    double left = 0.0;
    double right = 0.0;
};

/// Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer for planar/stereo PCM audio frames.
/// Producer (DecoderWorker) writes decoded/resampled PCM frames.
/// Consumer (Audio Thread) reads PCM frames with strictly ZERO locks, ZERO heap allocations, and ZERO blocking.
class PcmRingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY_FRAMES = 65536; // ~1.36s at 48kHz, ~0.34s at 192kHz

    explicit PcmRingBuffer(size_t capacityFrames = DEFAULT_CAPACITY_FRAMES)
        : m_capacity(nextPowerOfTwo(std::max<size_t>(1024, capacityFrames))),
          m_mask(m_capacity - 1),
          m_buffer(m_capacity)
    {
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_relaxed);
    }

    ~PcmRingBuffer() = default;

    // Non-copyable, non-movable to ensure pointer stability
    PcmRingBuffer(const PcmRingBuffer&) = delete;
    PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;
    PcmRingBuffer(PcmRingBuffer&&) = delete;
    PcmRingBuffer& operator=(PcmRingBuffer&&) = delete;

    [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }

    /// Number of available frames to read (can be called by consumer or producer)
    [[nodiscard]] size_t availableRead() const noexcept {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        return (writeIdx >= readIdx) ? (writeIdx - readIdx) : 0;
    }

    /// Number of frames that can be written before buffer is full
    [[nodiscard]] size_t availableWrite() const noexcept {
        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t used = writeIdx - readIdx;
        return (used < m_capacity) ? (m_capacity - used) : 0;
    }

    /// PRODUCER ONLY: Write interleaved stereo frames into ring buffer.
    /// Returns number of frames actually written.
    size_t write(const StereoFrame* data, size_t frames) noexcept {
        if (!data || frames == 0) return 0;

        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t used = writeIdx - readIdx;
        size_t available = (used < m_capacity) ? (m_capacity - used) : 0;
        size_t toWrite = std::min(frames, available);

        if (toWrite == 0) return 0;

        size_t part1Start = writeIdx & m_mask;
        size_t part1Len = std::min(toWrite, m_capacity - part1Start);
        std::copy_n(data, part1Len, m_buffer.data() + part1Start);

        if (toWrite > part1Len) {
            size_t part2Len = toWrite - part1Len;
            std::copy_n(data + part1Len, part2Len, m_buffer.data());
        }

        m_writeIndex.store(writeIdx + toWrite, std::memory_order_release);
        return toWrite;
    }

    /// PRODUCER ONLY: Write planar left and right channels into ring buffer.
    size_t writePlanar(const double* left, const double* right, size_t frames) noexcept {
        if (!left || !right || frames == 0) return 0;

        size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        size_t used = writeIdx - readIdx;
        size_t available = (used < m_capacity) ? (m_capacity - used) : 0;
        size_t toWrite = std::min(frames, available);

        if (toWrite == 0) return 0;

        for (size_t i = 0; i < toWrite; ++i) {
            size_t pos = (writeIdx + i) & m_mask;
            m_buffer[pos].left = left[i];
            m_buffer[pos].right = right[i];
        }

        m_writeIndex.store(writeIdx + toWrite, std::memory_order_release);
        return toWrite;
    }

    /// CONSUMER (AUDIO THREAD) ONLY: Read planar left and right channels.
    /// Strictly RT-safe: zero heap allocations, zero locks, zero blocking.
    /// Returns number of frames actually read.
    size_t readPlanar(double* outLeft, double* outRight, size_t frames) noexcept {
        if (!outLeft || !outRight || frames == 0) return 0;

        size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t available = (writeIdx >= readIdx) ? (writeIdx - readIdx) : 0;
        size_t toRead = std::min(frames, available);

        if (toRead == 0) return 0;

        for (size_t i = 0; i < toRead; ++i) {
            size_t pos = (readIdx + i) & m_mask;
            outLeft[i] = m_buffer[pos].left;
            outRight[i] = m_buffer[pos].right;
        }

        m_readIndex.store(readIdx + toRead, std::memory_order_release);
        return toRead;
    }

    /// Clear buffer (reset both indices).
    void clear() noexcept {
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_release);
    }

private:
    static size_t nextPowerOfTwo(size_t v) noexcept {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const size_t m_capacity;
    const size_t m_mask;
    std::vector<StereoFrame> m_buffer;

    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};
};

} // namespace desk32::engine::buffer
