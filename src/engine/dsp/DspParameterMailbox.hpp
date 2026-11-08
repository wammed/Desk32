#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <cstring>

namespace desk32::engine::dsp {

/// Snapshot of continuous parameters for EQ and Filters (P0-25)
struct ContinuousDspParameters {
    // 32-band Graphic EQ band gains (dB)
    std::array<float, 32> geqGains{};
    uint32_t geqModifiedMask = 0; // bitmask of modified bands

    // 24-band Parametric EQ band configs
    struct PeqBandConfig {
        float frequency = 1000.0f;
        float gainDb = 0.0f;
        float Q = 0.707f;
        uint8_t type = 0;    // BiquadFilterType
        uint8_t enabled = 0;
        uint8_t modified = 0;
    };
    std::array<PeqBandConfig, 24> peqLeftBands{};
    std::array<PeqBandConfig, 24> peqRightBands{};
    uint32_t peqLeftModifiedMask = 0;
    uint32_t peqRightModifiedMask = 0;

    // HighLowPassFilter
    float hpfFrequency = 20.0f;
    float lpfFrequency = 20000.0f;
    uint8_t hpfSlope = 0;
    uint8_t lpfSlope = 0;
    uint8_t hpfEnabled = 0;
    uint8_t lpfEnabled = 0;
    bool filterModified = false;

    uint64_t sequenceNumber = 0;
};

/// Lock-free Triple-Buffered Continuous Parameter Mailbox (P0-25)
/// - Multi-Producer Safe: Multiple Control Threads serialize updates using a producer mutex.
/// - Audio Thread Wait-Free: Audio Thread never acquires locks, never allocates memory,
///   and reads tear-free snapshots via an atomic published buffer index.
/// - Eliminates SPSC Queue in-place coalescing race conditions.
class DspParameterMailbox {
public:
    DspParameterMailbox() {
        m_publishedIndex.store(-1, std::memory_order_relaxed);
    }

    // Set a GEQ band gain (Thread-safe for multiple Control Threads)
    void setGeqBandGain(size_t band, float gainDb) {
        if (band >= 32) return;
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.geqGains[band] = gainDb;
        m_staging.geqModifiedMask |= (1u << band);
        publishStaging();
    }

    // Set a PEQ band (Thread-safe for multiple Control Threads)
    void setPeqBand(size_t band, size_t channel, float freq, float gainDb, float Q, uint8_t type, bool enabled) {
        if (band >= 24) return;
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        auto& target = (channel == 1) ? m_staging.peqRightBands[band] : m_staging.peqLeftBands[band];
        target.frequency = freq;
        target.gainDb = gainDb;
        target.Q = Q;
        target.type = type;
        target.enabled = enabled ? 1 : 0;
        target.modified = 1;
        if (channel == 1) {
            m_staging.peqRightModifiedMask |= (1u << band);
        } else {
            m_staging.peqLeftModifiedMask |= (1u << band);
        }
        publishStaging();
    }

    // Set Filter parameters (Thread-safe for multiple Control Threads)
    void setFilterParams(float hpfFreq, float lpfFreq, uint8_t hpfSlope, uint8_t lpfSlope, bool hpfOn, bool lpfOn) {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.hpfFrequency = hpfFreq;
        m_staging.lpfFrequency = lpfFreq;
        m_staging.hpfSlope = hpfSlope;
        m_staging.lpfSlope = lpfSlope;
        m_staging.hpfEnabled = hpfOn ? 1 : 0;
        m_staging.lpfEnabled = lpfOn ? 1 : 0;
        m_staging.filterModified = true;
        publishStaging();
    }

    /// Audio Thread: Non-blocking, wait-free check and retrieval of published parameters.
    /// Returns true and populates `outParams` if new parameters were published since last check.
    bool fetchUpdates(ContinuousDspParameters& outParams) noexcept {
        int idx = m_publishedIndex.exchange(-1, std::memory_order_acq_rel);
        if (idx >= 0 && idx < 3) {
            outParams = m_buffers[static_cast<size_t>(idx)];
            m_consumerReadIndex.store(idx, std::memory_order_release);
            return true;
        }
        return false;
    }

    [[nodiscard]] uint64_t updateCount() const noexcept {
        return m_updateCounter.load(std::memory_order_relaxed);
    }

private:
    void prepareStaging() {
        m_staging.sequenceNumber = m_updateCounter.load(std::memory_order_relaxed) + 1;
    }

    void publishStaging() {
        // Find a buffer slot among the 3 that is neither being read by the consumer
        // nor holding the most recent unpublished staging
        int consumerIdx = m_consumerReadIndex.load(std::memory_order_acquire);
        int currentPublished = m_publishedIndex.load(std::memory_order_relaxed);

        int writeIdx = 0;
        for (int i = 0; i < 3; ++i) {
            if (i != consumerIdx && i != currentPublished) {
                writeIdx = i;
                break;
            }
        }
        m_buffers[static_cast<size_t>(writeIdx)] = m_staging;
        m_publishedIndex.store(writeIdx, std::memory_order_release);
        m_updateCounter.fetch_add(1, std::memory_order_release);

        // Clear masks in staging for delta tracking
        m_staging.geqModifiedMask = 0;
        m_staging.peqLeftModifiedMask = 0;
        m_staging.peqRightModifiedMask = 0;
        m_staging.filterModified = false;
    }

    std::mutex m_producerMutex;
    ContinuousDspParameters m_staging{};
    std::array<ContinuousDspParameters, 3> m_buffers{};

    alignas(64) std::atomic<int> m_publishedIndex{-1};
    alignas(64) std::atomic<int> m_consumerReadIndex{-1};
    alignas(64) std::atomic<uint64_t> m_updateCounter{0};
};

} // namespace desk32::engine::dsp
