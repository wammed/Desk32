#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <random>

enum class DitherType {
    None,
    Rectangular,
    Triangular // TPDF (standard recommended)
};

// High-precision output format converter with TPDF dither and anti-clipping
class FormatConverter
{
public:
    explicit FormatConverter(uint32_t seed = 12345)
        : m_rng(seed), m_dist(-1.0, 1.0)
    {
    }

    // Convert float64 [-1.0, 1.0] to float32
    static inline float toFloat32(double sample)
    {
        double clamped = std::clamp(sample, -1.0, 1.0);
        return static_cast<float>(clamped);
    }

    // Convert float64 to 32-bit signed integer PCM
    static inline int32_t toInt32(double sample)
    {
        double clamped = std::clamp(sample, -1.0, 1.0);
        double scaled = clamped * 2147483647.0;
        return static_cast<int32_t>(std::lrint(scaled));
    }

    // Convert float64 to 24-bit signed integer (stored in 32-bit int)
    inline int32_t toInt24(double sample, DitherType dither = DitherType::Triangular)
    {
        double ditherVal = 0.0;
        if (dither == DitherType::Triangular) {
            ditherVal = (m_dist(m_rng) + m_dist(m_rng)) * 0.5; // TPDF
        }

        constexpr double maxVal = 8388607.0; // 2^23 - 1
        double scaled = sample * maxVal + ditherVal;
        double clamped = std::clamp(scaled, -8388608.0, maxVal);
        return static_cast<int32_t>(std::lrint(clamped));
    }

    // Convert float64 to 16-bit signed integer PCM
    inline int16_t toInt16(double sample, DitherType dither = DitherType::Triangular)
    {
        double ditherVal = 0.0;
        if (dither == DitherType::Triangular) {
            ditherVal = (m_dist(m_rng) + m_dist(m_rng)) * 0.5; // TPDF
        }

        constexpr double maxVal = 32767.0; // 2^15 - 1
        double scaled = sample * maxVal + ditherVal;
        double clamped = std::clamp(scaled, -32768.0, maxVal);
        return static_cast<int16_t>(std::lrint(clamped));
    }

private:
    std::mt19937 m_rng;
    std::uniform_real_distribution<double> m_dist;
};

