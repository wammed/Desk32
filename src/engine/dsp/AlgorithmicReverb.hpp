#pragma once

#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>

// Clean, natural Schroeder Algorithmic Reverb with Tail-Preserving Bypass & Pre-delay (Section 5.6)
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

    AlgorithmicReverb()
    {
        configure(48000.0);
    }

    void configure(double sampleRate, double roomSize = 0.7, double damp = 0.3,
                   double wetLevel = 0.2, double predelayMs = 0.0, bool tailPreserving = true)
    {
        m_sampleRate = sampleRate;
        m_roomSize = std::clamp(roomSize, 0.1, 0.98);
        m_damp = std::clamp(damp, 0.0, 1.0);
        m_wet = std::clamp(wetLevel, 0.0, 1.0);
        m_dry = 1.0 - m_wet;
        m_predelayMs = std::clamp(predelayMs, 0.0, 100.0);
        m_tailPreserving = tailPreserving;

        m_predelaySamples = static_cast<size_t>(std::ceil(sampleRate * (m_predelayMs * 0.001)));
        m_predelaySamples = std::clamp(m_predelaySamples, size_t(1), MAX_PREDELAY_SIZE);

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
            m_combsLeft[i].init(combLengths[i], m_roomSize, m_damp);
            m_combsRight[i].init(combLengths[i] + static_cast<size_t>(23 * scale), m_roomSize, m_damp);
        }

        for (size_t i = 0; i < 2; ++i) {
            m_allPassLeft[i].init(allPassLengths[i], 0.5);
            m_allPassRight[i].init(allPassLengths[i] + static_cast<size_t>(17 * scale), 0.5);
        }
    }

    [[nodiscard]] double roomSize() const noexcept { return m_roomSize; }
    [[nodiscard]] double damping() const noexcept { return m_damp; }
    [[nodiscard]] double wetLevel() const noexcept { return m_wet; }
    [[nodiscard]] double predelayMs() const noexcept { return m_predelayMs; }
    [[nodiscard]] bool isTailPreserving() const noexcept { return m_tailPreserving; }

    void setRoomSize(double size) { configure(m_sampleRate, size, m_damp, m_wet, m_predelayMs, m_tailPreserving); }
    void setDamping(double d) { configure(m_sampleRate, m_roomSize, d, m_wet, m_predelayMs, m_tailPreserving); }
    void setWetLevel(double w) { configure(m_sampleRate, m_roomSize, m_damp, w, m_predelayMs, m_tailPreserving); }
    void setPredelayMs(double ms) { configure(m_sampleRate, m_roomSize, m_damp, m_wet, ms, m_tailPreserving); }
    void setTailPreserving(bool tail) noexcept { m_tailPreserving = tail; }

    void reset()
    {
        for (auto& c : m_combsLeft) c.reset();
        for (auto& c : m_combsRight) c.reset();
        for (auto& a : m_allPassLeft) a.reset();
        for (auto& a : m_allPassRight) a.reset();
        m_predelayBuffer.fill(0.0);
        m_predelayWritePos = 0;
    }

    inline void process(double& left, double& right)
    {
        const bool active = m_enabled.load(std::memory_order_relaxed);

        if (!active && !m_tailPreserving) {
            // Immediate Bypass: instantly cut wet signal
            return;
        }

        // Pre-delay ring buffer
        double inMono = (left + right) * 0.5;
        double delayedInput = 0.0;

        if (active) {
            m_predelayBuffer[m_predelayWritePos] = inMono;
            size_t readPos = (m_predelayWritePos + MAX_PREDELAY_SIZE - m_predelaySamples) % MAX_PREDELAY_SIZE;
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
            outCombL += m_combsLeft[i].process(delayedInput);
            outCombR += m_combsRight[i].process(delayedInput);
        }

        // Series All-Pass Filters
        for (size_t i = 0; i < 2; ++i) {
            outCombL = m_allPassLeft[i].process(outCombL);
            outCombR = m_allPassRight[i].process(outCombR);
        }

        // Mix
        if (active) {
            left = left * m_dry + outCombL * m_wet;
            right = right * m_dry + outCombR * m_wet;
        } else {
            // In tail-preserving bypass, add decaying wet tail to dry signal
            left += outCombL * m_wet;
            right += outCombR * m_wet;
        }
    }

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

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
};
