#pragma once

#include <cmath>
#include <algorithm>

namespace desk32::engine::dsp {

/// Real-Time Safe Parameter Smoother using single-pole exponential low-pass filter
/// Eliminates zipper noise and audio clicks during parameter updates (Section 5.9).
class ParameterSmoother {
public:
    explicit ParameterSmoother(double initialValue = 0.0, double glideTimeMs = 20.0, double sampleRate = 48000.0)
        : m_current(initialValue)
        , m_target(initialValue)
        , m_sampleRate(sampleRate)
        , m_glideTimeMs(glideTimeMs)
    {
        updateAlpha();
    }

    void configure(double sampleRate, double glideTimeMs = 20.0) noexcept {
        m_sampleRate = sampleRate;
        m_glideTimeMs = glideTimeMs;
        updateAlpha();
    }

    void reset(double value) noexcept {
        m_current = value;
        m_target = value;
    }

    void setTarget(double target) noexcept {
        m_target = target;
    }

    [[nodiscard]] double target() const noexcept { return m_target; }
    [[nodiscard]] double current() const noexcept { return m_current; }

    [[nodiscard]] bool isSmoothing() const noexcept {
        return std::abs(m_target - m_current) > 1e-6;
    }

    /// Advance smoother by one sample and return smoothed value
    [[nodiscard]] inline double next() noexcept {
        if (std::abs(m_target - m_current) < 1e-4) {
            m_current = m_target; // Snap to avoid floating-point denormals and long asymptotic tail
            return m_current;
        }
        m_current = (1.0 - m_alpha) * m_target + m_alpha * m_current;
        return m_current;
    }

    /// Process an array of samples, applying smoothed gain
    inline void process(double* inOut, size_t frames) noexcept {
        if (!isSmoothing()) {
            if (m_target != 1.0) {
                for (size_t i = 0; i < frames; ++i) {
                    inOut[i] *= m_target;
                }
            }
            return;
        }

        for (size_t i = 0; i < frames; ++i) {
            inOut[i] *= next();
        }
    }

private:
    void updateAlpha() noexcept {
        if (m_glideTimeMs <= 0.0 || m_sampleRate <= 0.0) {
            m_alpha = 0.0;
        } else {
            m_alpha = std::exp(-1.0 / (m_sampleRate * (m_glideTimeMs * 0.001)));
        }
    }

    double m_current = 0.0;
    double m_target = 0.0;
    double m_sampleRate = 48000.0;
    double m_glideTimeMs = 20.0;
    double m_alpha = 0.0;
};

} // namespace desk32::engine::dsp
