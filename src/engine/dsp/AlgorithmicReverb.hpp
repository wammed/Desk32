#pragma once

#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include "engine/memory/EpochReclaimer.hpp"

// Clean, natural Schroeder Algorithmic Reverb with Tail-Preserving Bypass & Pre-delay (Section 5.6, P0-14)
// Control Thread: Calculates all comb/all-pass lengths, dry/wet balances, pre-delay sizing, and buffer initializations.
// Audio Thread: Reads precalculated parameters via Lock-Free EpochReclaimer with strictly zero heap allocation/deallocation,
// zero mutex waits, zero heavy configure math, and continuous preservation of reverb tail decay without truncation.
class AlgorithmicReverb
{
public:
    static constexpr size_t NUM_COMBS = 8;
    static constexpr size_t NUM_ALLPASSES = 4;
    static constexpr size_t MAX_COMB_SIZE = 8192;
    static constexpr size_t MAX_ALLPASS_SIZE = 4096;
    static constexpr size_t MAX_PREDELAY_SIZE = 19200; // 100 ms at 192 kHz

    // Schroeder / Freeverb-style gain staging and parameter mapping:
    // - Map user roomSize [0, 1] -> combFeedback [0.38, 0.985] with a perceptual curve
    //   instead of passing raw roomSize.
    // - Scale damping [0, 1] -> [0, 0.45] so 100% damping warms the tail rather than muting feedback.
    // - Normalize 8 parallel combs by 1/NUM_COMBS and attenuate comb bank input by
    //   COMB_INPUT_DRIVE * sqrt(1 - fb^2) so steady-state RMS stays balanced without clipping.
    static constexpr double MIN_COMB_FEEDBACK = 0.38;
    static constexpr double MAX_COMB_FEEDBACK = 0.985;
    static constexpr double MAX_COMB_DAMP = 0.45;
    static constexpr double COMB_INPUT_DRIVE = 1.4;
    static constexpr double INV_NUM_COMBS = 1.0 / static_cast<double>(NUM_COMBS);

    [[nodiscard]] static constexpr double mapRoomSizeToFeedback(double roomSize) noexcept {
        const double r = std::clamp(roomSize, 0.0, 1.0);
        const double shaped = (0.35 * r) + (0.65 * r * r);
        return MIN_COMB_FEEDBACK + (MAX_COMB_FEEDBACK - MIN_COMB_FEEDBACK) * shaped;
    }

    [[nodiscard]] static constexpr double mapDampingCoefficient(double damp) noexcept {
        return std::clamp(damp, 0.0, 1.0) * MAX_COMB_DAMP;
    }

    [[nodiscard]] static inline double computeCombInputGain(double combFeedback) noexcept {
        const double fb = std::clamp(combFeedback, 0.0, MAX_COMB_FEEDBACK);
        return COMB_INPUT_DRIVE * std::sqrt(std::max(1e-6, 1.0 - (fb * fb)));
    }

    struct CombFilter {
        alignas(64) std::array<double, MAX_COMB_SIZE> buffer{};
        size_t size = 1116;
        size_t index = 0;
        double feedback = 0.7945;
        double damp = 0.135;
        double filterStore = 0.0;
        double energy = 0.0; // Strictly O(1) buffer energy tracking

        void init(size_t len, double fb, double d) {
            size = std::clamp(len, size_t(16), MAX_COMB_SIZE);
            index = 0;
            feedback = std::clamp(fb, 0.0, MAX_COMB_FEEDBACK);
            damp = std::clamp(d, 0.0, 1.0);
            filterStore = 0.0;
            energy = 0.0;
            buffer.fill(0.0);
        }

        inline double process(double input) noexcept {
            const double output = buffer[index];
            filterStore = (output * (1.0 - damp)) + (filterStore * damp);
            const double newVal = input + (filterStore * feedback);
            const double diff = (newVal * newVal) - (output * output);
            energy += diff;
            if (energy < 1e-15 || !std::isfinite(energy)) {
                energy = 0.0;
            }
            buffer[index] = newVal;
            if (++index >= size) index = 0;
            return output;
        }

        inline double process(double input, double fb, double d) noexcept {
            const double output = buffer[index];
            filterStore = (output * (1.0 - d)) + (filterStore * d);
            const double newVal = input + (filterStore * fb);
            const double diff = (newVal * newVal) - (output * output);
            energy += diff;
            if (energy < 1e-15 || !std::isfinite(energy)) {
                energy = 0.0;
            }
            buffer[index] = newVal;
            if (++index >= size) index = 0;
            return output;
        }

        void reset() {
            buffer.fill(0.0);
            filterStore = 0.0;
            energy = 0.0;
        }

        [[nodiscard]] double totalEnergy() const noexcept {
            if (!std::isfinite(filterStore) || !std::isfinite(energy)) return std::numeric_limits<double>::quiet_NaN();
            return energy + (filterStore * filterStore);
        }

        [[nodiscard]] double scanBufferEnergy() const noexcept {
            if (!std::isfinite(filterStore)) return std::numeric_limits<double>::quiet_NaN();
            double sum = filterStore * filterStore;
            for (size_t i = 0; i < size; ++i) {
                const double v = buffer[i];
                if (!std::isfinite(v)) return std::numeric_limits<double>::quiet_NaN();
                sum += v * v;
            }
            return sum;
        }
    };

    struct AllPassFilter {
        alignas(64) std::array<double, MAX_ALLPASS_SIZE> buffer{};
        size_t size = 225;
        size_t index = 0;
        double feedback = 0.5;
        double energy = 0.0; // Strictly O(1) buffer energy tracking

        void init(size_t len, double fb = 0.5) {
            size = std::clamp(len, size_t(16), MAX_ALLPASS_SIZE);
            index = 0;
            feedback = std::clamp(fb, 0.0, 0.98);
            energy = 0.0;
            buffer.fill(0.0);
        }

        inline double process(double input) noexcept {
            const double bufOut = buffer[index];
            const double newVal = input + (bufOut * feedback);
            // True unity-gain Schroeder allpass: |H(e^jw)| == 1 across all frequencies
            const double output = bufOut - (newVal * feedback);
            const double diff = (newVal * newVal) - (bufOut * bufOut);
            energy += diff;
            if (energy < 1e-15 || !std::isfinite(energy)) {
                energy = 0.0;
            }
            buffer[index] = newVal;
            if (++index >= size) index = 0;
            return output;
        }

        void reset() {
            buffer.fill(0.0);
            energy = 0.0;
        }

        [[nodiscard]] double totalEnergy() const noexcept {
            if (!std::isfinite(energy)) return std::numeric_limits<double>::quiet_NaN();
            return energy;
        }

        [[nodiscard]] double scanBufferEnergy() const noexcept {
            double sum = 0.0;
            for (size_t i = 0; i < size; ++i) {
                const double v = buffer[i];
                if (!std::isfinite(v)) return std::numeric_limits<double>::quiet_NaN();
                sum += v * v;
            }
            return sum;
        }
    };

    struct Params {
        double sampleRate = 48000.0;
        double roomSize = 0.7;
        double damp = 0.3;
        double combFeedback = mapRoomSizeToFeedback(0.7);
        double combDamp = mapDampingCoefficient(0.3);
        double combInputGain = computeCombInputGain(mapRoomSizeToFeedback(0.7));
        double wet = 0.2;
        double dry = 0.8;
        double predelayMs = 0.0;
        size_t predelaySamples = 1;
        bool tailPreserving = true;
        bool enabled = false;
    };

    AlgorithmicReverb()
    {
        configure(48000.0);
    }

    AlgorithmicReverb(const AlgorithmicReverb& other)
    {
        *this = other;
    }

    AlgorithmicReverb& operator=(const AlgorithmicReverb& other)
    {
        if (this != &other) {
            auto guard = other.m_paramsReclaimer.enterRead();
            if (guard) {
                configure(guard->sampleRate, guard->roomSize, guard->damp,
                          guard->wet, guard->predelayMs, guard->tailPreserving, guard->enabled);
            } else {
                configure(48000.0);
            }
            m_predelayBuffer = other.m_predelayBuffer;
            m_predelayWritePos = other.m_predelayWritePos;
            m_predelayEnergy = other.m_predelayEnergy;
            for (size_t i = 0; i < NUM_COMBS; ++i) {
                m_combsLeft[i] = other.m_combsLeft[i];
                m_combsRight[i] = other.m_combsRight[i];
            }
            for (size_t i = 0; i < NUM_ALLPASSES; ++i) {
                m_allPassLeft[i] = other.m_allPassLeft[i];
                m_allPassRight[i] = other.m_allPassRight[i];
            }
        }
        return *this;
    }

    /// Executed on Control Thread: Handles room sizing, damping, buffer preparation, and rate changes
    void configure(double sampleRate, double roomSize = 0.7, double damp = 0.3,
                   double wetLevel = 0.2, double predelayMs = 0.0, bool tailPreserving = true,
                   bool enabled = true)
    {
        bool rateChanged = std::abs(m_sampleRate - sampleRate) > 0.01;
        m_sampleRate = sampleRate;

        auto p = std::make_unique<Params>();
        p->sampleRate = sampleRate;
        p->roomSize = std::clamp(roomSize, 0.0, 1.0);
        p->damp = std::clamp(damp, 0.0, 1.0);
        p->combFeedback = mapRoomSizeToFeedback(p->roomSize);
        p->combDamp = mapDampingCoefficient(p->damp);
        p->combInputGain = computeCombInputGain(p->combFeedback);
        p->wet = std::clamp(wetLevel, 0.0, 1.0);
        p->dry = 1.0 - p->wet;
        p->predelayMs = std::clamp(predelayMs, 0.0, 100.0);
        p->tailPreserving = tailPreserving;
        p->enabled = enabled;

        p->predelaySamples = static_cast<size_t>(std::ceil(sampleRate * (p->predelayMs * 0.001)));
        p->predelaySamples = std::clamp(p->predelaySamples, size_t(1), MAX_PREDELAY_SIZE);

        m_roomSize = p->roomSize;
        m_damp = p->damp;
        m_combFeedback = p->combFeedback;
        m_combDamp = p->combDamp;
        m_combInputGain = p->combInputGain;
        m_wet = p->wet;
        m_dry = p->dry;
        m_predelayMs = p->predelayMs;
        m_predelaySamples = p->predelaySamples;
        m_tailPreserving = tailPreserving;
        m_enabled.store(enabled, std::memory_order_relaxed);

        if (rateChanged) {
            const double scale = sampleRate / 44100.0;

            static constexpr size_t combLengths[NUM_COMBS] = {
                1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
            };
            static constexpr size_t allPassLengths[NUM_ALLPASSES] = {
                556, 441, 341, 225
            };
            const size_t stereoSpread = static_cast<size_t>(23.0 * scale);

            for (size_t i = 0; i < NUM_COMBS; ++i) {
                const size_t len = static_cast<size_t>(static_cast<double>(combLengths[i]) * scale);
                m_combsLeft[i].init(len, p->combFeedback, p->combDamp);
                m_combsRight[i].init(len + stereoSpread, p->combFeedback, p->combDamp);
            }

            for (size_t i = 0; i < NUM_ALLPASSES; ++i) {
                const size_t len = static_cast<size_t>(static_cast<double>(allPassLengths[i]) * scale);
                m_allPassLeft[i].init(len, 0.5);
                m_allPassRight[i].init(len + stereoSpread, 0.5);
            }
        } else {
            // Update parameter gains in-place without zeroing active audio delay buffers (P0-07)
            for (size_t i = 0; i < NUM_COMBS; ++i) {
                m_combsLeft[i].feedback = p->combFeedback;
                m_combsLeft[i].damp = p->combDamp;
                m_combsRight[i].feedback = p->combFeedback;
                m_combsRight[i].damp = p->combDamp;
            }
        }

        m_paramsReclaimer.swapChain(std::move(p));
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] double roomSize() const noexcept { return m_roomSize; }
    [[nodiscard]] double damping() const noexcept { return m_damp; }
    [[nodiscard]] double combFeedback() const noexcept { return m_combFeedback; }
    [[nodiscard]] double combInputGain() const noexcept { return m_combInputGain; }
    [[nodiscard]] double wetLevel() const noexcept { return m_wet; }
    [[nodiscard]] double predelayMs() const noexcept { return m_predelayMs; }
    [[nodiscard]] bool isTailPreserving() const noexcept { return m_tailPreserving; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    /// Calculate total energy accumulated in all internal comb/allpass delay buffers (P0-22).
    /// Strictly O(1) lock-free calculation on Audio Thread without buffer scanning.
    /// Returns NaN if any internal filter state contains NaN or Inf.
    [[nodiscard]] double totalInternalEnergy() const noexcept {
        if (m_hasInvalidState.load(std::memory_order_relaxed)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        double e = m_predelayEnergy;
        for (const auto& c : m_combsLeft) {
            double ce = c.totalEnergy();
            if (!std::isfinite(ce)) return std::numeric_limits<double>::quiet_NaN();
            e += ce;
        }
        for (const auto& c : m_combsRight) {
            double ce = c.totalEnergy();
            if (!std::isfinite(ce)) return std::numeric_limits<double>::quiet_NaN();
            e += ce;
        }
        for (const auto& a : m_allPassLeft) {
            double ae = a.totalEnergy();
            if (!std::isfinite(ae)) return std::numeric_limits<double>::quiet_NaN();
            e += ae;
        }
        for (const auto& a : m_allPassRight) {
            double ae = a.totalEnergy();
            if (!std::isfinite(ae)) return std::numeric_limits<double>::quiet_NaN();
            e += ae;
        }
        return e;
    }

    /// Full buffer scanning energy calculation for offline verification and testing (P0-22).
    [[nodiscard]] double debugScanTotalEnergy() const noexcept {
        double e = 0.0;
        for (const auto& c : m_combsLeft) {
            double ce = c.scanBufferEnergy();
            if (!std::isfinite(ce)) return std::numeric_limits<double>::quiet_NaN();
            e += ce;
        }
        for (const auto& c : m_combsRight) {
            double ce = c.scanBufferEnergy();
            if (!std::isfinite(ce)) return std::numeric_limits<double>::quiet_NaN();
            e += ce;
        }
        for (const auto& a : m_allPassLeft) {
            double ae = a.scanBufferEnergy();
            if (!std::isfinite(ae)) return std::numeric_limits<double>::quiet_NaN();
            e += ae;
        }
        for (const auto& a : m_allPassRight) {
            double ae = a.scanBufferEnergy();
            if (!std::isfinite(ae)) return std::numeric_limits<double>::quiet_NaN();
            e += ae;
        }
        for (size_t i = 0; i < m_predelaySamples; ++i) {
            size_t idx = (m_predelayWritePos + MAX_PREDELAY_SIZE - m_predelaySamples + i) % MAX_PREDELAY_SIZE;
            double v = m_predelayBuffer[idx];
            if (!std::isfinite(v)) return std::numeric_limits<double>::quiet_NaN();
            e += v * v;
        }
        return e;
    }

    /// Check whether any internal delay line or filter state has become NaN or Inf (P0-22).
    /// Strictly O(1) lock-free check on Audio Thread without buffer scanning.
    [[nodiscard]] bool hasInvalidState() const noexcept {
        return m_hasInvalidState.load(std::memory_order_relaxed);
    }

    /// Test helper to inject a non-finite value into internal state for safety verification (P0-22).
    void injectInvalidStateForTest(double nonFiniteVal) noexcept {
        m_combsLeft[0].filterStore = nonFiniteVal;
        m_combsLeft[0].energy = nonFiniteVal;
        if (!std::isfinite(nonFiniteVal)) {
            m_hasInvalidState.store(true, std::memory_order_relaxed);
        }
    }

    /// Acoustic physical decay lower bound (1.5 * RT60 frames) to reach -80 dB safely (P0-22).
    [[nodiscard]] size_t theoreticalDecayFrames() const noexcept {
        const double g = std::clamp(m_combFeedback, MIN_COMB_FEEDBACK, MAX_COMB_FEEDBACK);
        const double tau = 0.031; // ~31ms average comb loop across 1116..1617 @ 44.1kHz
        const double rt60Sec = -3.0 * tau / std::log10(g);
        const double decaySec = std::clamp(rt60Sec * 1.5, 0.5, 9.0);
        return static_cast<size_t>(decaySec * m_sampleRate);
    }

    /// Inherit active reverberation tail state from another instance (P0-22).
    /// STRICTLY Non-RT / Control Thread only: Never call on Audio Thread (v1.6).
    /// Bounded execution time, zero heap allocations, zero locks.
    void inheritStateFrom(const AlgorithmicReverb& other) noexcept
    {
        m_predelayBuffer = other.m_predelayBuffer;
        m_predelayWritePos = other.m_predelayWritePos;
        m_predelayEnergy = other.m_predelayEnergy;
        for (size_t i = 0; i < NUM_COMBS; ++i) {
            const size_t cLenL = std::min(m_combsLeft[i].size, other.m_combsLeft[i].size);
            for (size_t s = 0; s < cLenL; ++s) {
                m_combsLeft[i].buffer[s] = other.m_combsLeft[i].buffer[s];
            }
            m_combsLeft[i].index = other.m_combsLeft[i].index % m_combsLeft[i].size;
            m_combsLeft[i].filterStore = other.m_combsLeft[i].filterStore;
            m_combsLeft[i].energy = other.m_combsLeft[i].energy;

            const size_t cLenR = std::min(m_combsRight[i].size, other.m_combsRight[i].size);
            for (size_t s = 0; s < cLenR; ++s) {
                m_combsRight[i].buffer[s] = other.m_combsRight[i].buffer[s];
            }
            m_combsRight[i].index = other.m_combsRight[i].index % m_combsRight[i].size;
            m_combsRight[i].filterStore = other.m_combsRight[i].filterStore;
            m_combsRight[i].energy = other.m_combsRight[i].energy;
        }
        for (size_t i = 0; i < NUM_ALLPASSES; ++i) {
            const size_t aLenL = std::min(m_allPassLeft[i].size, other.m_allPassLeft[i].size);
            for (size_t s = 0; s < aLenL; ++s) {
                m_allPassLeft[i].buffer[s] = other.m_allPassLeft[i].buffer[s];
            }
            m_allPassLeft[i].index = other.m_allPassLeft[i].index % m_allPassLeft[i].size;
            m_allPassLeft[i].energy = other.m_allPassLeft[i].energy;

            const size_t aLenR = std::min(m_allPassRight[i].size, other.m_allPassRight[i].size);
            for (size_t s = 0; s < aLenR; ++s) {
                m_allPassRight[i].buffer[s] = other.m_allPassRight[i].buffer[s];
            }
            m_allPassRight[i].index = other.m_allPassRight[i].index % m_allPassRight[i].size;
            m_allPassRight[i].energy = other.m_allPassRight[i].energy;
        }
    }

    void setRoomSize(double size) { configure(m_sampleRate, size, m_damp, m_wet, m_predelayMs, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setDamping(double d) { configure(m_sampleRate, m_roomSize, d, m_wet, m_predelayMs, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setWetLevel(double w) { configure(m_sampleRate, m_roomSize, m_damp, w, m_predelayMs, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setPredelayMs(double ms) { configure(m_sampleRate, m_roomSize, m_damp, m_wet, ms, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setTailPreserving(bool tail) noexcept { configure(m_sampleRate, m_roomSize, m_damp, m_wet, m_predelayMs, tail, m_enabled.load(std::memory_order_relaxed)); }
    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }

    AlgorithmicReverb* nextRetired = nullptr;

    void reset()
    {
        for (auto& c : m_combsLeft) c.reset();
        for (auto& c : m_combsRight) c.reset();
        for (auto& a : m_allPassLeft) a.reset();
        for (auto& a : m_allPassRight) a.reset();
        m_predelayBuffer.fill(0.0);
        m_predelayWritePos = 0;
        m_predelayEnergy = 0.0;
        m_hasInvalidState.store(false, std::memory_order_relaxed);
    }

    /// Real-time audio rendering: Lock-free, zero heap allocation, bounded execution time.
    inline void process(double& left, double& right)
    {
        auto guard = m_paramsReclaimer.enterRead();
        const Params* p = guard.get();
        if (!p) return;

        const bool active = m_enabled.load(std::memory_order_relaxed);

        if (!active && !p->tailPreserving) {
            // Immediate Bypass: instantly cut wet signal
            return;
        }

        // Pre-delay ring buffer
        const size_t readPos = m_predelayWritePos >= p->predelaySamples
                                   ? (m_predelayWritePos - p->predelaySamples)
                                   : (m_predelayWritePos + MAX_PREDELAY_SIZE - p->predelaySamples);
        const double delayedInput = m_predelayBuffer[readPos];
        const double predelayWrite = active ? ((left + right) * 0.5) : 0.0;
        const double pdDiff = (predelayWrite * predelayWrite) - (delayedInput * delayedInput);
        m_predelayEnergy += pdDiff;
        if (m_predelayEnergy < 1e-15 || !std::isfinite(m_predelayEnergy)) {
            m_predelayEnergy = 0.0;
        }
        m_predelayBuffer[m_predelayWritePos] = predelayWrite;
        if (++m_predelayWritePos >= MAX_PREDELAY_SIZE) m_predelayWritePos = 0;

        // Parallel Comb Filters (attenuated input drive + 1/NUM_COMBS sum normalization)
        const double combIn = delayedInput * p->combInputGain;
        double outCombL = 0.0;
        double outCombR = 0.0;
        for (size_t i = 0; i < NUM_COMBS; ++i) {
            outCombL += m_combsLeft[i].process(combIn, p->combFeedback, p->combDamp);
            outCombR += m_combsRight[i].process(combIn, p->combFeedback, p->combDamp);
        }
        outCombL *= INV_NUM_COMBS;
        outCombR *= INV_NUM_COMBS;

        // Series All-Pass Filters
        for (size_t i = 0; i < NUM_ALLPASSES; ++i) {
            outCombL = m_allPassLeft[i].process(outCombL);
            outCombR = m_allPassRight[i].process(outCombR);
        }

        if (!std::isfinite(outCombL) || !std::isfinite(outCombR)) {
            m_hasInvalidState.store(true, std::memory_order_relaxed);
        }

        // Mix
        if (active) {
            left = left * p->dry + outCombL * p->wet;
            right = right * p->dry + outCombR * p->wet;
        } else {
            // In tail-preserving bypass, add decaying wet tail to dry signal
            left += outCombL * p->wet;
            right += outCombR * p->wet;
        }
    }

private:
    double m_sampleRate = 0.0; // Uninitialized so first configure(48000.0) always initializes delay lengths
    double m_roomSize = 0.7;
    double m_damp = 0.3;
    double m_combFeedback = mapRoomSizeToFeedback(0.7);
    double m_combDamp = mapDampingCoefficient(0.3);
    double m_combInputGain = computeCombInputGain(mapRoomSizeToFeedback(0.7));
    double m_wet = 0.2;
    double m_dry = 0.8;
    double m_predelayMs = 0.0;
    size_t m_predelaySamples = 1;
    size_t m_predelayWritePos = 0;
    double m_predelayEnergy = 0.0;
    bool m_tailPreserving = true;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_hasInvalidState{false};

    CombFilter m_combsLeft[NUM_COMBS];
    CombFilter m_combsRight[NUM_COMBS];
    AllPassFilter m_allPassLeft[NUM_ALLPASSES];
    AllPassFilter m_allPassRight[NUM_ALLPASSES];

    alignas(64) std::array<double, MAX_PREDELAY_SIZE> m_predelayBuffer{};

    mutable EpochReclaimer<Params> m_paramsReclaimer;
};
