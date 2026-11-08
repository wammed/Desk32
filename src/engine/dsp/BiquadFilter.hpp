#pragma once

#include <cmath>
#include <numbers>

enum class BiquadFilterType {
    Peak,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    Notch
};

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;

    static constexpr BiquadCoefficients identity() noexcept {
        return { 1.0, 0.0, 0.0, 0.0, 0.0 };
    }
};

namespace desk32::engine::dsp {
    using BiquadCoefficients = ::BiquadCoefficients;
    using PreparedBiquadCoefficients = ::BiquadCoefficients;
}

// 64-bit double precision Direct Form II Transposed Biquad Filter
class BiquadFilter
{
public:
    BiquadFilter() = default;

    /// Calculate filter coefficients strictly outside Audio Thread (Control / Worker Thread, P0-27)
    static BiquadCoefficients calculateCoefficients(BiquadFilterType type, double sampleRate, double frequency, double Q, double gainDb) noexcept
    {
        if (sampleRate <= 0.0) return BiquadCoefficients::identity();
        const double w0 = 2.0 * std::numbers::pi * frequency / sampleRate;
        const double cos_w0 = std::cos(w0);
        const double sin_w0 = std::sin(w0);
        const double alpha = sin_w0 / (2.0 * std::max(Q, 0.001));
        const double A = std::pow(10.0, gainDb / 40.0);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type) {
        case BiquadFilterType::Peak:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cos_w0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha / A;
            break;
        case BiquadFilterType::LowShelf: {
            const double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrtA * alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrtA * alpha);
            a0 = (A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrtA * alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
            a2 = (A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrtA * alpha;
            break;
        }
        case BiquadFilterType::HighShelf: {
            const double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + 2.0 * sqrtA * alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - 2.0 * sqrtA * alpha);
            a0 = (A + 1.0) - (A - 1.0) * cos_w0 + 2.0 * sqrtA * alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
            a2 = (A + 1.0) - (A - 1.0) * cos_w0 - 2.0 * sqrtA * alpha;
            break;
        }
        case BiquadFilterType::LowPass:
            b0 = (1.0 - cos_w0) / 2.0;
            b1 = 1.0 - cos_w0;
            b2 = (1.0 - cos_w0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case BiquadFilterType::HighPass:
            b0 = (1.0 + cos_w0) / 2.0;
            b1 = -(1.0 + cos_w0);
            b2 = (1.0 + cos_w0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        case BiquadFilterType::Notch:
            b0 = 1.0;
            b1 = -2.0 * cos_w0;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cos_w0;
            a2 = 1.0 - alpha;
            break;
        }

        BiquadCoefficients c;
        if (std::abs(a0) > 1e-15) {
            c.b0 = b0 / a0;
            c.b1 = b1 / a0;
            c.b2 = b2 / a0;
            c.a1 = a1 / a0;
            c.a2 = a2 / a0;
        }
        return c;
    }

    /// Fast, real-time safe assignment of precalculated coefficients (Audio Thread execution, P0-27)
    /// Zero allocations, zero trigonometric functions, zero locks, bounded O(1) execution.
    inline void setCoefficients(const BiquadCoefficients& c) noexcept
    {
        m_b0 = c.b0;
        m_b1 = c.b1;
        m_b2 = c.b2;
        m_a1 = c.a1;
        m_a2 = c.a2;
    }

    [[nodiscard]] BiquadCoefficients coefficients() const noexcept
    {
        return { m_b0, m_b1, m_b2, m_a1, m_a2 };
    }

    void configure(BiquadFilterType type, double sampleRate, double frequency, double Q, double gainDb)
    {
        m_sampleRate = sampleRate;
        m_frequency = frequency;
        m_Q = Q;
        m_gainDb = gainDb;
        setCoefficients(calculateCoefficients(type, sampleRate, frequency, Q, gainDb));
    }

    void reset()
    {
        m_z1 = 0.0;
        m_z2 = 0.0;
    }

    inline double process(double input)
    {
        // Direct Form II Transposed
        double output = m_b0 * input + m_z1;
        m_z1 = m_b1 * input - m_a1 * output + m_z2;
        m_z2 = m_b2 * input - m_a2 * output;
        return output;
    }

    /// Calculate magnitude response in dB at a given frequency
    [[nodiscard]] double magnitudeResponseDb(double frequency) const noexcept
    {
        if (m_sampleRate <= 0.0 || frequency <= 0.0) return 0.0;
        const double w = 2.0 * std::numbers::pi * frequency / m_sampleRate;
        const double cos_w = std::cos(w);
        const double sin_w = std::sin(w);
        const double cos_2w = std::cos(2.0 * w);
        const double sin_2w = std::sin(2.0 * w);

        const double numReal = m_b0 + m_b1 * cos_w + m_b2 * cos_2w;
        const double numImag = -m_b1 * sin_w - m_b2 * sin_2w;
        const double denReal = 1.0 + m_a1 * cos_w + m_a2 * cos_2w;
        const double denImag = -m_a1 * sin_w - m_a2 * sin_2w;

        const double numSq = numReal * numReal + numImag * numImag;
        const double denSq = denReal * denReal + denImag * denImag;

        if (denSq < 1e-15) return 0.0;
        const double magSq = numSq / denSq;
        if (magSq < 1e-15) return -120.0;

        return 10.0 * std::log10(magSq);
    }

private:
    double m_sampleRate = 48000.0;
    double m_frequency = 1000.0;
    double m_Q = 0.7071;
    double m_gainDb = 0.0;

    double m_b0 = 1.0;
    double m_b1 = 0.0;
    double m_b2 = 0.0;
    double m_a1 = 0.0;
    double m_a2 = 0.0;

    double m_z1 = 0.0;
    double m_z2 = 0.0;
};

