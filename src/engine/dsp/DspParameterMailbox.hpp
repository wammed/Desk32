#pragma once

#include "BiquadFilter.hpp"
#include "HighLowPassFilter.hpp"
#include "ParametricEQ.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>

namespace desk32::engine::dsp {

// P0-24, P0-25: Compile-time verification that atomics used in the Triple Buffer are always lock-free
static_assert(std::atomic<uint32_t>::is_always_lock_free, "std::atomic<uint32_t> must be lock-free for DspParameterMailbox");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "std::atomic<uint64_t> must be lock-free for DspParameterMailbox");

/// Snapshot of continuous parameters and precomputed coefficients for EQ and Filters (P0-25, P0-27)
/// All heavy mathematical coefficient calculations (trigonometry, exp, sqrt) are performed strictly
/// on the Control Thread prior to publishing. Audio Thread applies precomputed coefficients in O(1).
struct ContinuousDspParameters {
    // 32-band Graphic EQ band gains (dB) and precomputed coefficients
    std::array<float, 32> geqGains{};
    uint32_t geqModifiedMask = 0; // bitmask of modified bands
    std::array<BiquadCoefficients, 32> geqCoefficients{};

    // 24-band Parametric EQ band configs and precomputed coefficients
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
    std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL> peqLeftCoefficients{};
    std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL> peqRightCoefficients{};
    std::array<bool, ParametricEQ::BANDS_PER_CHANNEL> peqLeftEnabled{};
    std::array<bool, ParametricEQ::BANDS_PER_CHANNEL> peqRightEnabled{};

    // HighLowPassFilter
    float hpfFrequency = 20.0f;
    float lpfFrequency = 20000.0f;
    uint8_t hpfSlope = 0;
    uint8_t lpfSlope = 0;
    uint8_t hpfEnabled = 0;
    uint8_t lpfEnabled = 0;
    bool filterModified = false;
    HighLowPassCoefficients filterCoefficients{};

    // Listening Correction PEQ (P0-27)
    std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL> listeningLeftCoefficients{};
    std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL> listeningRightCoefficients{};
    std::array<bool, ParametricEQ::BANDS_PER_CHANNEL> listeningLeftEnabled{};
    std::array<bool, ParametricEQ::BANDS_PER_CHANNEL> listeningRightEnabled{};
    bool listeningModified = false;

    uint64_t epoch = 1;
    uint64_t sequenceNumber = 0;
};

/// Mutex-Free / Non-Blocking Lock-Free Triple-Buffered Continuous Parameter Mailbox (P0-24, P0-25, P0-27, P0-29)
/// Design & Concurrency Contract (v1.7 Unified):
/// - Mutex-Free & Lock-Free: The atomic operations on m_sharedState are compile-time verified
///   to be natively lock-free. Consumer fetchUpdates() operates strictly in bounded O(1) non-blocking
///   steps without mutexes, conditions, or loops.
/// - Multi-Producer Safe: Control Threads serialize staging updates using m_producerMutex.
/// - Single-Consumer Audio Thread: Strictly Mutex-Free, Non-Blocking Lock-Free Triple Buffer retrieval.
/// - Linearization Point: An atomic exchange on m_sharedState simultaneously transfers ownership
///   of the latest written buffer and clears the dirty flag.
/// - Bounded Execution: Copying the fixed-size ContinuousDspParameters snapshot (~3.8 KB) takes
///   sub-microsecond execution time (< 300 ns), well within real-time budget.
/// - Audio Thread Non-Blocking: Audio Thread performs exactly one atomic load and at most one atomic exchange
///   to acquire the published buffer. Zero locks, zero loops, zero heap allocations.
/// - Strict Ownership Invariant:
///   { m_writeIndex, m_consumerIndex, m_sharedState & INDEX_MASK } is strictly a permutation of {0, 1, 2}.
///   Producer can NEVER overwrite the buffer currently held or being read by Consumer.
class DspParameterMailbox {
public:
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "Mailbox shared state must be natively lock-free on this platform");

    static constexpr uint32_t DIRTY_FLAG = 0x4;
    static constexpr uint32_t INDEX_MASK = 0x3;

    static constexpr std::array<double, 32> GEQ_ISO_FREQS = {
        16.0, 20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0,
        100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0,
        630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0,
        4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0
    };
    static constexpr double GEQ_Q = 4.318;

    DspParameterMailbox() {
        initDefaultStaging();
        for (auto& buf : m_buffers) {
            buf = m_staging;
        }
    }

    /// Re-initialize mailbox to default state (Control Thread when Audio Thread is stopped, P0-24)
    /// Monotonic sequenceNumber is preserved across resets, while epoch is incremented.
    void reset() {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        m_epoch.fetch_add(1, std::memory_order_release);
        m_listeningPresetConfigured = false;
        initDefaultStaging();
        for (auto& buf : m_buffers) {
            buf = m_staging;
        }
        m_writeIndex = 0;
        m_consumerIndex = 2;
        m_sharedState.store(1, std::memory_order_release);
    }

    [[nodiscard]] uint64_t currentEpoch() const noexcept {
        return m_epoch.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t currentSequenceNumber() const noexcept {
        return m_updateCounter.load(std::memory_order_acquire);
    }

    void setSampleRate(double sampleRate) {
        if (sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> lock(m_producerMutex);
        if (sampleRate == m_sampleRate) return;
        m_sampleRate = sampleRate;
        prepareStaging();
        recalculateAllCoefficients();
        m_staging.geqModifiedMask = 0xFFFFFFFF;
        m_staging.peqLeftModifiedMask = 0x00FFFFFF;
        m_staging.peqRightModifiedMask = 0x00FFFFFF;
        m_staging.filterModified = true;
        if (m_listeningPresetConfigured) {
            m_staging.listeningModified = true;
        }
        publishStaging();
    }

    // Set a GEQ band gain (Thread-safe for multiple Control Threads, P0-27 precomputed)
    void setGeqBandGain(size_t band, float gainDb) {
        if (band >= 32) return;
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.geqGains[band] = gainDb;
        m_staging.geqModifiedMask |= (1u << band);

        const double clampedGain = std::clamp(static_cast<double>(gainDb), -12.0, 12.0);
        const double freq = std::min(GEQ_ISO_FREQS[band], m_sampleRate * 0.48);
        m_staging.geqCoefficients[band] = BiquadFilter::calculateCoefficients(
            BiquadFilterType::Peak, m_sampleRate, freq, GEQ_Q, clampedGain
        );

        publishStaging();
    }

    // Reset all 32 GEQ bands to 0 dB (Thread-safe, P0-27 precomputed)
    void resetAllGeqBands() {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.geqGains.fill(0.0f);
        m_staging.geqModifiedMask = 0xFFFFFFFF;
        for (size_t b = 0; b < 32; ++b) {
            const double freq = std::min(GEQ_ISO_FREQS[b], m_sampleRate * 0.48);
            m_staging.geqCoefficients[b] = BiquadFilter::calculateCoefficients(
                BiquadFilterType::Peak, m_sampleRate, freq, GEQ_Q, 0.0
            );
        }
        publishStaging();
    }

    // Set all 32 GEQ band gains in a single atomic snapshot (Thread-safe, P0-24, P0-25, P0-27)
    void setAllGeqBandGains(const std::array<float, 32>& gains) {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.geqGains = gains;
        m_staging.geqModifiedMask = 0xFFFFFFFF;
        for (size_t b = 0; b < 32; ++b) {
            const double clampedGain = std::clamp(static_cast<double>(gains[b]), -12.0, 12.0);
            const double freq = std::min(GEQ_ISO_FREQS[b], m_sampleRate * 0.48);
            m_staging.geqCoefficients[b] = BiquadFilter::calculateCoefficients(
                BiquadFilterType::Peak, m_sampleRate, freq, GEQ_Q, clampedGain
            );
        }
        publishStaging();
    }

    // Set a PEQ band (Thread-safe for multiple Control Threads, P0-27 precomputed)
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

        const double clampedFreq = std::clamp(static_cast<double>(freq), 10.0, std::min(24000.0, m_sampleRate * 0.48));
        const double clampedGain = std::clamp(static_cast<double>(gainDb), -24.0, 24.0);
        const double clampedQ = std::clamp(static_cast<double>(Q), 0.1, 20.0);
        const auto filterType = static_cast<BiquadFilterType>(std::clamp<int>(type, 0, 5));

        auto coeff = BiquadFilter::calculateCoefficients(filterType, m_sampleRate, clampedFreq, clampedQ, clampedGain);

        if (channel == 1) {
            m_staging.peqRightModifiedMask |= (1u << band);
            if (band < ParametricEQ::BANDS_PER_CHANNEL) {
                m_staging.peqRightCoefficients[band] = coeff;
                m_staging.peqRightEnabled[band] = enabled;
            }
        } else {
            m_staging.peqLeftModifiedMask |= (1u << band);
            if (band < ParametricEQ::BANDS_PER_CHANNEL) {
                m_staging.peqLeftCoefficients[band] = coeff;
                m_staging.peqLeftEnabled[band] = enabled;
            }
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
            if (band < ParametricEQ::BANDS_PER_CHANNEL) {
                m_staging.peqRightEnabled[band] = enabled;
            }
        } else {
            m_staging.peqLeftModifiedMask |= (1u << band);
            if (band < ParametricEQ::BANDS_PER_CHANNEL) {
                m_staging.peqLeftEnabled[band] = enabled;
            }
        }
        publishStaging();
    }

    // Apply entire PEQ preset (Thread-safe, P0-27 precomputed)
    void applyPeqPreset(const std::array<ContinuousDspParameters::PeqBandConfig, 24>& leftBands,
                        const std::array<ContinuousDspParameters::PeqBandConfig, 24>& rightBands) {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        m_staging.peqLeftBands = leftBands;
        m_staging.peqRightBands = rightBands;
        m_staging.peqLeftModifiedMask = 0x00FFFFFF;
        m_staging.peqRightModifiedMask = 0x00FFFFFF;

        for (size_t b = 0; b < 24; ++b) {
            const auto& l = leftBands[b];
            const double lFreq = std::clamp(static_cast<double>(l.frequency), 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double lGain = std::clamp(static_cast<double>(l.gainDb), -24.0, 24.0);
            const double lQ = std::clamp(static_cast<double>(l.Q), 0.1, 20.0);
            const auto lType = static_cast<BiquadFilterType>(std::clamp<int>(l.type, 0, 5));
            auto lCoeff = BiquadFilter::calculateCoefficients(lType, m_sampleRate, lFreq, lQ, lGain);

            const auto& r = rightBands[b];
            const double rFreq = std::clamp(static_cast<double>(r.frequency), 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double rGain = std::clamp(static_cast<double>(r.gainDb), -24.0, 24.0);
            const double rQ = std::clamp(static_cast<double>(r.Q), 0.1, 20.0);
            const auto rType = static_cast<BiquadFilterType>(std::clamp<int>(r.type, 0, 5));
            auto rCoeff = BiquadFilter::calculateCoefficients(rType, m_sampleRate, rFreq, rQ, rGain);

            if (b < ParametricEQ::BANDS_PER_CHANNEL) {
                m_staging.peqLeftCoefficients[b] = lCoeff;
                m_staging.peqLeftEnabled[b] = (l.enabled != 0);
                m_staging.peqRightCoefficients[b] = rCoeff;
                m_staging.peqRightEnabled[b] = (r.enabled != 0);
            }
        }

        publishStaging();
    }

    // Set Filter parameters (Thread-safe for multiple Control Threads, P0-27 precomputed)
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

        m_staging.filterCoefficients = HighLowPassFilter::calculateCoefficients(
            m_sampleRate,
            static_cast<double>(hpfFreq),
            static_cast<double>(lpfFreq),
            hpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB,
            lpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB,
            hpfOn,
            lpfOn
        );

        publishStaging();
    }

    // Apply Listening Correction preset (Thread-safe, P0-27 precomputed)
    bool setListeningPreset(const std::string& presetName) {
        std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> lBands{};
        std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> rBands{};
        if (!ParametricEQ::getPresetBandConfigs(presetName, lBands, rBands)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();

        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            const auto& l = lBands[b];
            const double lFreq = std::clamp(l.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double lGain = std::clamp(l.gainDb, -24.0, 24.0);
            const double lQ = std::clamp(l.Q, 0.1, 20.0);
            m_staging.listeningLeftCoefficients[b] = BiquadFilter::calculateCoefficients(l.type, m_sampleRate, lFreq, lQ, lGain);
            m_staging.listeningLeftEnabled[b] = l.enabled;

            const auto& r = rBands[b];
            const double rFreq = std::clamp(r.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double rGain = std::clamp(r.gainDb, -24.0, 24.0);
            const double rQ = std::clamp(r.Q, 0.1, 20.0);
            m_staging.listeningRightCoefficients[b] = BiquadFilter::calculateCoefficients(r.type, m_sampleRate, rFreq, rQ, rGain);
            m_staging.listeningRightEnabled[b] = r.enabled;
        }

        m_listeningPresetConfigured = true;
        m_staging.listeningModified = true;
        publishStaging();
        return true;
    }

    void setListeningBand(size_t index, const ParametricEQ::BandConfig& config) {
        if (index >= ParametricEQ::BANDS_PER_CHANNEL) return;
        std::lock_guard<std::mutex> lock(m_producerMutex);
        prepareStaging();
        const double freq = std::clamp(config.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
        const double gain = std::clamp(config.gainDb, -24.0, 24.0);
        const double q = std::clamp(config.Q, 0.1, 20.0);
        auto coeff = BiquadFilter::calculateCoefficients(config.type, m_sampleRate, freq, q, gain);
        m_staging.listeningLeftCoefficients[index] = coeff;
        m_staging.listeningLeftEnabled[index] = config.enabled;
        m_staging.listeningRightCoefficients[index] = coeff;
        m_staging.listeningRightEnabled[index] = config.enabled;
        m_staging.listeningModified = true;
        publishStaging();
    }

    /// Audio Thread: Mutex-free, non-blocking lock-free retrieval of published parameters (P0-29).
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

    void resetStatistics() noexcept {
        m_updateCounter.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isLockFree() const noexcept {
        return m_sharedState.is_lock_free();
    }

    /// Thread-safe read of current staged parameters for Control Thread / UI display (P0-26)
    [[nodiscard]] ContinuousDspParameters currentParameters() const {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        return m_staging;
    }

private:
    void initDefaultStaging() {
        m_staging = ContinuousDspParameters{};
        m_sampleRate = 48000.0;
        recalculateAllCoefficients();
    }

    void recalculateAllCoefficients() {
        // GEQ
        for (size_t b = 0; b < 32; ++b) {
            const double clampedGain = std::clamp(static_cast<double>(m_staging.geqGains[b]), -12.0, 12.0);
            const double freq = std::min(GEQ_ISO_FREQS[b], m_sampleRate * 0.48);
            m_staging.geqCoefficients[b] = BiquadFilter::calculateCoefficients(
                BiquadFilterType::Peak, m_sampleRate, freq, GEQ_Q, clampedGain
            );
        }

        // PEQ
        for (size_t b = 0; b < 24; ++b) {
            const auto& l = m_staging.peqLeftBands[b];
            const double lFreq = std::clamp(static_cast<double>(l.frequency), 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double lGain = std::clamp(static_cast<double>(l.gainDb), -24.0, 24.0);
            const double lQ = std::clamp(static_cast<double>(l.Q), 0.1, 20.0);
            const auto lType = static_cast<BiquadFilterType>(std::clamp<int>(l.type, 0, 5));
            auto lCoeff = BiquadFilter::calculateCoefficients(lType, m_sampleRate, lFreq, lQ, lGain);

            const auto& r = m_staging.peqRightBands[b];
            const double rFreq = std::clamp(static_cast<double>(r.frequency), 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double rGain = std::clamp(static_cast<double>(r.gainDb), -24.0, 24.0);
            const double rQ = std::clamp(static_cast<double>(r.Q), 0.1, 20.0);
            const auto rType = static_cast<BiquadFilterType>(std::clamp<int>(r.type, 0, 5));
            auto rCoeff = BiquadFilter::calculateCoefficients(rType, m_sampleRate, rFreq, rQ, rGain);

            if (b < ParametricEQ::BANDS_PER_CHANNEL) {
                m_staging.peqLeftCoefficients[b] = lCoeff;
                m_staging.peqLeftEnabled[b] = (l.enabled != 0);
                m_staging.peqRightCoefficients[b] = rCoeff;
                m_staging.peqRightEnabled[b] = (r.enabled != 0);
            }
        }

        // Filter
        m_staging.filterCoefficients = HighLowPassFilter::calculateCoefficients(
            m_sampleRate,
            static_cast<double>(m_staging.hpfFrequency),
            static_cast<double>(m_staging.lpfFrequency),
            m_staging.hpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB,
            m_staging.lpfSlope == 1 ? HighLowPassFilter::FilterSlope::Slope24dB : HighLowPassFilter::FilterSlope::Slope12dB,
            m_staging.hpfEnabled != 0,
            m_staging.lpfEnabled != 0
        );

        // Listening
        std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> lBands{};
        std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> rBands{};
        ParametricEQ::getPresetBandConfigs("Monitor Flat", lBands, rBands);
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            const auto& l = lBands[b];
            const double lFreq = std::clamp(l.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double lGain = std::clamp(l.gainDb, -24.0, 24.0);
            const double lQ = std::clamp(l.Q, 0.1, 20.0);
            m_staging.listeningLeftCoefficients[b] = BiquadFilter::calculateCoefficients(l.type, m_sampleRate, lFreq, lQ, lGain);
            m_staging.listeningLeftEnabled[b] = l.enabled;

            const auto& r = rBands[b];
            const double rFreq = std::clamp(r.frequency, 10.0, std::min(24000.0, m_sampleRate * 0.48));
            const double rGain = std::clamp(r.gainDb, -24.0, 24.0);
            const double rQ = std::clamp(r.Q, 0.1, 20.0);
            m_staging.listeningRightCoefficients[b] = BiquadFilter::calculateCoefficients(r.type, m_sampleRate, rFreq, rQ, rGain);
            m_staging.listeningRightEnabled[b] = r.enabled;
        }
    }

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
            m_staging.listeningModified = false;
        }
        m_staging.epoch = m_epoch.load(std::memory_order_relaxed);
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
    double m_sampleRate{48000.0};
    bool m_listeningPresetConfigured{false};
    ContinuousDspParameters m_staging{};
    std::array<ContinuousDspParameters, 3> m_buffers{};

    alignas(64) std::atomic<uint64_t> m_epoch{1};
    alignas(64) std::atomic<uint32_t> m_sharedState{1};
    uint32_t m_writeIndex{0};
    uint32_t m_consumerIndex{2};
    alignas(64) std::atomic<uint64_t> m_updateCounter{0};
};

} // namespace desk32::engine::dsp
