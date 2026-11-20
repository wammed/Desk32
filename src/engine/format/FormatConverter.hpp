#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <random>

namespace desk32::engine::format {

enum class DitherType {
    None,
    Rectangular,
    Triangular ///< TPDF (Triangular Probability Density Function, audiophile standard)
};

enum class SampleFormat {
    Float32,   ///< Stereo 32-bit float interleaved (Standard PipeWire/JACK graph format)
    Int24_32,  ///< Stereo 24-bit PCM in 32-bit int interleaved
    Int32,     ///< Stereo 32-bit signed integer PCM interleaved
    Int16,     ///< Stereo 16-bit integer PCM interleaved
    Float64    ///< Stereo 64-bit float interleaved
};

/// High-precision output format converter with TPDF dither and anti-clipping
class FormatConverter {
public:
    explicit FormatConverter(uint32_t seed = 12345)
        : m_rng(seed), m_dist(-1.0, 1.0)
    {}

    // Convert float64 [-1.0, 1.0] to float32
    static inline float toFloat32(double sample) {
        double clamped = std::clamp(sample, -1.0, 1.0);
        return static_cast<float>(clamped);
    }

    // Convert float64 to 32-bit signed integer PCM
    static inline int32_t toInt32(double sample) {
        double clamped = std::clamp(sample, -1.0, 1.0);
        double scaled = clamped * 2147483647.0;
        return static_cast<int32_t>(std::lrint(scaled));
    }

    // Convert float64 to 24-bit signed integer (stored in 32-bit int)
    inline int32_t toInt24(double sample, DitherType dither = DitherType::Triangular) {
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
    inline int16_t toInt16(double sample, DitherType dither = DitherType::Triangular) {
        double ditherVal = 0.0;
        if (dither == DitherType::Triangular) {
            ditherVal = (m_dist(m_rng) + m_dist(m_rng)) * 0.5; // TPDF
        }

        constexpr double maxVal = 32767.0; // 2^15 - 1
        double scaled = sample * maxVal + ditherVal;
        double clamped = std::clamp(scaled, -32768.0, maxVal);
        return static_cast<int16_t>(std::lrint(clamped));
    }

    /// Convert planar float64 stereo buffers [left, right] to target interleaved PCM buffer.
    /// Real-time safe: Zero heap allocation.
    static void convertPlanarFloat64ToPcm(const double* left, const double* right, size_t frames,
                                         void* outInterleavedBuffer, SampleFormat targetFormat,
                                         bool applyDither = true)
    {
        // Thread-local fast generator for TPDF dither
        thread_local uint32_t s_prngState = 0x853c49e6;
        auto fastTpdf = []() noexcept -> double {
            // Xorshift32 PRNG
            s_prngState ^= s_prngState << 13;
            s_prngState ^= s_prngState >> 17;
            s_prngState ^= s_prngState << 5;
            double r1 = static_cast<double>(s_prngState) * (1.0 / 4294967296.0) - 0.5;
            s_prngState ^= s_prngState << 13;
            s_prngState ^= s_prngState >> 17;
            s_prngState ^= s_prngState << 5;
            double r2 = static_cast<double>(s_prngState) * (1.0 / 4294967296.0) - 0.5;
            return r1 + r2; // TPDF [-1.0, 1.0]
        };

        switch (targetFormat) {
            case SampleFormat::Float32: {
                auto* out = static_cast<float*>(outInterleavedBuffer);
                for (size_t i = 0; i < frames; ++i) {
                    out[i * 2]     = static_cast<float>(std::clamp(left[i], -1.0, 1.0));
                    out[i * 2 + 1] = static_cast<float>(std::clamp(right[i], -1.0, 1.0));
                }
                break;
            }
            case SampleFormat::Int24_32: {
                auto* out = static_cast<int32_t*>(outInterleavedBuffer);
                constexpr double scale = 8388607.0;
                for (size_t i = 0; i < frames; ++i) {
                    double ditherL = applyDither ? fastTpdf() : 0.0;
                    double ditherR = applyDither ? fastTpdf() : 0.0;
                    out[i * 2]     = static_cast<int32_t>(std::lrint(std::clamp(left[i] * scale + ditherL, -8388608.0, 8388607.0)));
                    out[i * 2 + 1] = static_cast<int32_t>(std::lrint(std::clamp(right[i] * scale + ditherR, -8388608.0, 8388607.0)));
                }
                break;
            }
            case SampleFormat::Int32: {
                auto* out = static_cast<int32_t*>(outInterleavedBuffer);
                constexpr double scale = 2147483647.0;
                for (size_t i = 0; i < frames; ++i) {
                    out[i * 2]     = static_cast<int32_t>(std::lrint(std::clamp(left[i], -1.0, 1.0) * scale));
                    out[i * 2 + 1] = static_cast<int32_t>(std::lrint(std::clamp(right[i], -1.0, 1.0) * scale));
                }
                break;
            }
            case SampleFormat::Int16: {
                auto* out = static_cast<int16_t*>(outInterleavedBuffer);
                constexpr double scale = 32767.0;
                for (size_t i = 0; i < frames; ++i) {
                    double ditherL = applyDither ? fastTpdf() : 0.0;
                    double ditherR = applyDither ? fastTpdf() : 0.0;
                    out[i * 2]     = static_cast<int16_t>(std::lrint(std::clamp(left[i] * scale + ditherL, -32768.0, 32767.0)));
                    out[i * 2 + 1] = static_cast<int16_t>(std::lrint(std::clamp(right[i] * scale + ditherR, -32768.0, 32767.0)));
                }
                break;
            }
            case SampleFormat::Float64: {
                auto* out = static_cast<double*>(outInterleavedBuffer);
                for (size_t i = 0; i < frames; ++i) {
                    out[i * 2]     = std::clamp(left[i], -1.0, 1.0);
                    out[i * 2 + 1] = std::clamp(right[i], -1.0, 1.0);
                }
                break;
            }
        }
    }

private:
    std::mt19937 m_rng;
    std::uniform_real_distribution<double> m_dist;
};

} // namespace desk32::engine::format

// Global aliases for backward compatibility
using desk32::engine::format::FormatConverter;
using desk32::engine::format::DitherType;
using desk32::engine::format::SampleFormat;
