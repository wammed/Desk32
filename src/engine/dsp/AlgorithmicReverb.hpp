#pragma once

#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <memory>
#include "engine/memory/EpochReclaimer.hpp"

// Clean, natural Schroeder Algorithmic Reverb with Tail-Preserving Bypass & Pre-delay (Section 5.6, P0-14)
// Control Thread: Calculates all comb/all-pass lengths, dry/wet balances, pre-delay sizing, and buffer initializations.
// Audio Thread: Reads precalculated parameters via Lock-Free EpochReclaimer with strictly zero heap allocation/deallocation,
// zero mutex waits, zero heavy configure math, and continuous preservation of reverb tail decay without truncation.
class AlgorithmicReverb
{
public:
    static constexpr size_t MAX_COMB_SIZE = 8192;
    static constexpr size_t MAX_ALLPASS_SIZE = 4096;
    static constexpr size_t MAX_PREDELAY_SIZE = 19200; // 100 ms at 192 kHz

    struct CombFilter {
        alignas(64) std::array<double, MAX_COMB_SIZE> buffer{};
        size_t size = 1116;
        size_t index = 0;
        double feedback = 0.8;
        double damp = 0.2;
        double filterStore = 0.0;

        void init(size_t len, double fb, double d) {
            size = std::clamp(len, size_t(16), MAX_COMB_SIZE);
            index = 0;
            feedback = std::clamp(fb, 0.0, 0.98);
            damp = std::clamp(d, 0.0, 1.0);
            filterStore = 0.0;
            buffer.fill(0.0);
        }

        inline double process(double input) {
            double output = buffer[index];
            filterStore = (output * (1.0 - damp)) + (filterStore * damp);
            buffer[index] = input + (filterStore * feedback);
            index = (index + 1) % size;
            return output;
        }

        inline double process(double input, double fb, double d) {
            double output = buffer[index];
            filterStore = (output * (1.0 - d)) + (filterStore * d);
            buffer[index] = input + (filterStore * fb);
            index = (index + 1) % size;
            return output;
        }

        void reset() {
            buffer.fill(0.0);
            filterStore = 0.0;
        }
    };

    struct AllPassFilter {
        alignas(64) std::array<double, MAX_ALLPASS_SIZE> buffer{};
        size_t size = 225;
        size_t index = 0;
        double feedback = 0.5;

        void init(size_t len, double fb = 0.5) {
            size = std::clamp(len, size_t(16), MAX_ALLPASS_SIZE);
            index = 0;
            feedback = std::clamp(fb, 0.0, 0.98);
            buffer.fill(0.0);
        }

        inline double process(double input) {
            double bufOut = buffer[index];
            double output = -input + bufOut;
            buffer[index] = input + (bufOut * feedback);
            index = (index + 1) % size;
            return output;
        }

        void reset() {
            buffer.fill(0.0);
        }
    };

    struct Params {
        double sampleRate = 48000.0;
        double roomSize = 0.7;
        double damp = 0.3;
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
            for (size_t i = 0; i < 4; ++i) {
                m_combsLeft[i] = other.m_combsLeft[i];
                m_combsRight[i] = other.m_combsRight[i];
            }
            for (size_t i = 0; i < 2; ++i) {
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
        p->roomSize = std::clamp(roomSize, 0.1, 0.98);
        p->damp = std::clamp(damp, 0.0, 1.0);
        p->wet = std::clamp(wetLevel, 0.0, 1.0);
        p->dry = 1.0 - p->wet;
        p->predelayMs = std::clamp(predelayMs, 0.0, 100.0);
        p->tailPreserving = tailPreserving;
        p->enabled = enabled;

        p->predelaySamples = static_cast<size_t>(std::ceil(sampleRate * (p->predelayMs * 0.001)));
        p->predelaySamples = std::clamp(p->predelaySamples, size_t(1), MAX_PREDELAY_SIZE);

        m_roomSize = p->roomSize;
        m_damp = p->damp;
        m_wet = p->wet;
        m_dry = p->dry;
        m_predelayMs = p->predelayMs;
        m_predelaySamples = p->predelaySamples;
        m_tailPreserving = tailPreserving;
        m_enabled.store(enabled, std::memory_order_relaxed);

        if (rateChanged) {
            const double scale = sampleRate / 44100.0;

            const size_t combLengths[4] = {
                static_cast<size_t>(1116 * scale),
                static_cast<size_t>(1188 * scale),
                static_cast<size_t>(1277 * scale),
                static_cast<size_t>(1356 * scale)
            };
            const size_t allPassLengths[2] = {
                static_cast<size_t>(225 * scale),
                static_cast<size_t>(556 * scale)
            };

            for (size_t i = 0; i < 4; ++i) {
                m_combsLeft[i].init(combLengths[i], p->roomSize, p->damp);
                m_combsRight[i].init(combLengths[i] + static_cast<size_t>(23 * scale), p->roomSize, p->damp);
            }

            for (size_t i = 0; i < 2; ++i) {
                m_allPassLeft[i].init(allPassLengths[i], 0.5);
                m_allPassRight[i].init(allPassLengths[i] + static_cast<size_t>(17 * scale), 0.5);
            }
        } else {
            // Update parameter gains in-place without zeroing active audio delay buffers (P0-07)
            for (size_t i = 0; i < 4; ++i) {
                m_combsLeft[i].feedback = p->roomSize;
                m_combsLeft[i].damp = p->damp;
                m_combsRight[i].feedback = p->roomSize;
                m_combsRight[i].damp = p->damp;
            }
        }

        m_paramsReclaimer.swapChain(std::move(p));
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] double roomSize() const noexcept { return m_roomSize; }
    [[nodiscard]] double damping() const noexcept { return m_damp; }
    [[nodiscard]] double wetLevel() const noexcept { return m_wet; }
    [[nodiscard]] double predelayMs() const noexcept { return m_predelayMs; }
    [[nodiscard]] bool isTailPreserving() const noexcept { return m_tailPreserving; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    void setRoomSize(double size) { configure(m_sampleRate, size, m_damp, m_wet, m_predelayMs, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setDamping(double d) { configure(m_sampleRate, m_roomSize, d, m_wet, m_predelayMs, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setWetLevel(double w) { configure(m_sampleRate, m_roomSize, m_damp, w, m_predelayMs, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setPredelayMs(double ms) { configure(m_sampleRate, m_roomSize, m_damp, m_wet, ms, m_tailPreserving, m_enabled.load(std::memory_order_relaxed)); }
    void setTailPreserving(bool tail) noexcept { configure(m_sampleRate, m_roomSize, m_damp, m_wet, m_predelayMs, tail, m_enabled.load(std::memory_order_relaxed)); }
    void setEnabled(bool enabled) noexcept { configure(m_sampleRate, m_roomSize, m_damp, m_wet, m_predelayMs, m_tailPreserving, enabled); }

    AlgorithmicReverb* nextRetired = nullptr;

    void reset()
    {
        for (auto& c : m_combsLeft) c.reset();
        for (auto& c : m_combsRight) c.reset();
        for (auto& a : m_allPassLeft) a.reset();
        for (auto& a : m_allPassRight) a.reset();
        m_predelayBuffer.fill(0.0);
        m_predelayWritePos = 0;
    }

    /// Real-time audio rendering: Lock-free, zero heap allocation, bounded execution time.
    inline void process(double& left, double& right)
    {
        auto guard = m_paramsReclaimer.enterRead();
        const Params* p = guard.get();
        if (!p) return;

        const bool active = p->enabled;

        if (!active && !p->tailPreserving) {
            // Immediate Bypass: instantly cut wet signal
            return;
        }

        // Pre-delay ring buffer
        double inMono = (left + right) * 0.5;
        double delayedInput = 0.0;

        if (active) {
            m_predelayBuffer[m_predelayWritePos] = inMono;
            size_t readPos = (m_predelayWritePos + MAX_PREDELAY_SIZE - p->predelaySamples) % MAX_PREDELAY_SIZE;
            delayedInput = m_predelayBuffer[readPos];
            m_predelayWritePos = (m_predelayWritePos + 1) % MAX_PREDELAY_SIZE;
        } else {
            // Tail-preserving Bypass: zero input enters reverb network, allowing tail to ring out
            delayedInput = 0.0;
        }

        // Parallel Comb Filters
        double outCombL = 0.0;
        double outCombR = 0.0;
        for (size_t i = 0; i < 4; ++i) {
            outCombL += m_combsLeft[i].process(delayedInput, p->roomSize, p->damp);
            outCombR += m_combsRight[i].process(delayedInput, p->roomSize, p->damp);
        }

        // Series All-Pass Filters
        for (size_t i = 0; i < 2; ++i) {
            outCombL = m_allPassLeft[i].process(outCombL);
            outCombR = m_allPassRight[i].process(outCombR);
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
    double m_sampleRate = 48000.0;
    double m_roomSize = 0.7;
    double m_damp = 0.3;
    double m_wet = 0.2;
    double m_dry = 0.8;
    double m_predelayMs = 0.0;
    size_t m_predelaySamples = 1;
    size_t m_predelayWritePos = 0;
    bool m_tailPreserving = true;

    std::atomic<bool> m_enabled{false};

    CombFilter m_combsLeft[4];
    CombFilter m_combsRight[4];
    AllPassFilter m_allPassLeft[2];
    AllPassFilter m_allPassRight[2];

    alignas(64) std::array<double, MAX_PREDELAY_SIZE> m_predelayBuffer{};

    mutable EpochReclaimer<Params> m_paramsReclaimer;
};
