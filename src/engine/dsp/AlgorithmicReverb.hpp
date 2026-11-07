#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// Standard Schroeder Algorithmic Reverb in 64-bit float
class AlgorithmicReverb
{
public:
    struct CombFilter {
        std::vector<double> buffer;
        size_t index = 0;
        double feedback = 0.8;
        double damp = 0.2;
        double filterStore = 0.0;

        void init(size_t size, double fb, double d) {
            buffer.assign(std::max<size_t>(size, 16), 0.0);
            index = 0;
            feedback = fb;
            damp = d;
            filterStore = 0.0;
        }

        inline double process(double input) {
            if (buffer.empty()) return input;
            double output = buffer[index];
            filterStore = (output * (1.0 - damp)) + (filterStore * damp);
            buffer[index] = input + (filterStore * feedback);
            index = (index + 1) % buffer.size();
            return output;
        }

        void reset() {
            std::fill(buffer.begin(), buffer.end(), 0.0);
            filterStore = 0.0;
        }
    };

    struct AllPassFilter {
        std::vector<double> buffer;
        size_t index = 0;
        double feedback = 0.5;

        void init(size_t size, double fb = 0.5) {
            buffer.assign(std::max<size_t>(size, 16), 0.0);
            index = 0;
            feedback = fb;
        }

        inline double process(double input) {
            if (buffer.empty()) return input;
            double bufOut = buffer[index];
            double output = -input + bufOut;
            buffer[index] = input + (bufOut * feedback);
            index = (index + 1) % buffer.size();
            return output;
        }

        void reset() {
            std::fill(buffer.begin(), buffer.end(), 0.0);
        }
    };

    AlgorithmicReverb()
    {
        configure(48000.0);
    }

    void configure(double sampleRate, double roomSize = 0.7, double damp = 0.3, double wetLevel = 0.2)
    {
        m_sampleRate = sampleRate;
        m_wet = wetLevel;
        m_dry = 1.0 - wetLevel;

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
            m_combsLeft[i].init(combLengths[i], roomSize, damp);
            m_combsRight[i].init(combLengths[i] + static_cast<size_t>(23 * scale), roomSize, damp);
        }

        for (size_t i = 0; i < 2; ++i) {
            m_allPassLeft[i].init(allPassLengths[i], 0.5);
            m_allPassRight[i].init(allPassLengths[i] + static_cast<size_t>(17 * scale), 0.5);
        }
    }

    void reset()
    {
        for (auto& c : m_combsLeft) c.reset();
        for (auto& c : m_combsRight) c.reset();
        for (auto& a : m_allPassLeft) a.reset();
        for (auto& a : m_allPassRight) a.reset();
    }

    inline void process(double& left, double& right)
    {
        if (!m_enabled) return;

        double inMono = (left + right) * 0.5;

        // Parallel Comb Filters
        double outCombL = 0.0;
        double outCombR = 0.0;
        for (size_t i = 0; i < 4; ++i) {
            outCombL += m_combsLeft[i].process(inMono);
            outCombR += m_combsRight[i].process(inMono);
        }

        // Series All-Pass Filters
        for (size_t i = 0; i < 2; ++i) {
            outCombL = m_allPassLeft[i].process(outCombL);
            outCombR = m_allPassRight[i].process(outCombR);
        }

        left = left * m_dry + outCombL * m_wet;
        right = right * m_dry + outCombR * m_wet;
    }

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

private:
    double m_sampleRate = 48000.0;
    double m_dry = 0.8;
    double m_wet = 0.2;
    bool m_enabled = true;

    CombFilter m_combsLeft[4];
    CombFilter m_combsRight[4];
    AllPassFilter m_allPassLeft[2];
    AllPassFilter m_allPassRight[2];
};
