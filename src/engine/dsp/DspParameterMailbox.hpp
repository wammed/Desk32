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
/// - Multi-Producer Safe: Multiple Control Threads serialize staging updates using m_producerMutex.
/// - Linearization Point: An atomic exchange on m_sharedState simultaneously transfers ownership
///   of the newly written buffer to the shared slot and receives the previously shared buffer.
/// - Audio Thread Wait-Free: Audio Thread performs exactly one atomic exchange to acquire the published
///   buffer and release its previous buffer. No locks, no loops, no heap allocations.
/// - Strict Ownership Invariant:
///   { m_writeIndex, m_consumerIndex, m_sharedState & INDEX_MASK } is strictly a permutation of {0, 1, 2}.
///   Producer can NEVER overwrite the buffer currently held or being read by Consumer.
class DspParameterMailbox {
public:
    static constexpr uint32_t DIRTY_FLAG = 0x4;
    static constexpr uint32_t INDEX_MASK = 0x3;

    DspParameterMailbox() {
        // Initially:
        // Slot 0 is owned by Producer (m_writeIndex = 0)
        // Slot 1 is held in m_sharedState (DIRTY_FLAG = 0)
        // Slot 2 is owned by Consumer (m_consumerIndex = 2)
        // All 3 buffers are strictly separated and valid.
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

    // Reset all 32 GEQ bands to 0 dB (Thread-safe)
    void resetAllGeqBands() {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.geqGains.fill(0.0f);
        m_staging.geqModifiedMask = 0xFFFFFFFF;
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

    // Set PEQ band enabled flag (Thread-safe)
    void setPeqBandEnabled(size_t band, size_t channel, bool enabled) {
        if (band >= 24) return;
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        auto& target = (channel == 1) ? m_staging.peqRightBands[band] : m_staging.peqLeftBands[band];
        target.enabled = enabled ? 1 : 0;
        target.modified = 1;
        if (channel == 1) {
            m_staging.peqRightModifiedMask |= (1u << band);
        } else {
            m_staging.peqLeftModifiedMask |= (1u << band);
        }
        publishStaging();
    }

    // Apply entire PEQ preset (Thread-safe)
    void applyPeqPreset(const std::array<ContinuousDspParameters::PeqBandConfig, 24>& leftBands,
                        const std::array<ContinuousDspParameters::PeqBandConfig, 24>& rightBands) {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.peqLeftBands = leftBands;
        m_staging.peqRightBands = rightBands;
        m_staging.peqLeftModifiedMask = 0x00FFFFFF;
        m_staging.peqRightModifiedMask = 0x00FFFFFF;
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

    /// Audio Thread: Non-blocking, wait-free retrieval of published parameters.
    /// Strictly O(1): at most one atomic load and one atomic exchange.
    /// Returns true and populates `outParams` if new parameters were published since last fetch.
    bool fetchUpdates(ContinuousDspParameters& outParams) noexcept {
        uint32_t cur = m_sharedState.load(std::memory_order_acquire);
        if (!(cur & DIRTY_FLAG)) {
            return false;
        }

        // Swap consumer buffer with published shared buffer, clearing DIRTY_FLAG
        uint32_t toExchange = m_consumerIndex & INDEX_MASK;
        uint32_t oldShared = m_sharedState.exchange(toExchange, std::memory_order_acq_rel);
        m_consumerIndex = oldShared & INDEX_MASK;

        // Safely copy from privately-owned consumer buffer
        outParams = m_buffers[m_consumerIndex];
        return true;
    }

    [[nodiscard]] uint64_t updateCount() const noexcept {
        return m_updateCounter.load(std::memory_order_relaxed);
    }

    /// Thread-safe read of current staged parameters for Control Thread / UI display (P0-26)
    [[nodiscard]] ContinuousDspParameters currentParameters() const {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        return m_staging;
    }

private:
    void prepareStaging() {
        // P0-25: If consumer has already fetched the previous snapshot (DIRTY_FLAG cleared),
        // we can safely start a new delta tracking cycle.
        // If consumer has not fetched yet, preserve and accumulate masks so no updates are lost.
        uint32_t curShared = m_sharedState.load(std::memory_order_acquire);
        if (!(curShared & DIRTY_FLAG)) {
            m_staging.geqModifiedMask = 0;
            m_staging.peqLeftModifiedMask = 0;
            m_staging.peqRightModifiedMask = 0;
            m_staging.filterModified = false;
        }
        m_staging.sequenceNumber = m_updateCounter.load(std::memory_order_relaxed) + 1;
    }

    void publishStaging() {
        // 1. Write staged parameters into producer's private buffer
        m_buffers[m_writeIndex] = m_staging;

        // 2. Linearization point: Atomically publish write buffer to shared state
        uint32_t newState = (m_writeIndex & INDEX_MASK) | DIRTY_FLAG;
        uint32_t oldShared = m_sharedState.exchange(newState, std::memory_order_acq_rel);

        // 3. Take previously shared buffer as the new private write buffer
        m_writeIndex = oldShared & INDEX_MASK;
        m_updateCounter.fetch_add(1, std::memory_order_release);
    }

    mutable std::mutex m_producerMutex;
    ContinuousDspParameters m_staging{};
    std::array<ContinuousDspParameters, 3> m_buffers{};

    alignas(64) std::atomic<uint32_t> m_sharedState{1};
    uint32_t m_writeIndex{0};
    uint32_t m_consumerIndex{2};
    alignas(64) std::atomic<uint64_t> m_updateCounter{0};
};

} // namespace desk32::engine::dsp
