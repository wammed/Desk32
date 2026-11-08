#pragma once

#include <soxr.h>
#include <vector>
#include <cstddef>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace desk32::engine::dsp {

enum class ResamplerQuality {
    Quick,
    Low,
    Medium,
    High,
    VeryHigh
};

class Resampler {
public:
    Resampler() = default;

    ~Resampler() {
        destroy();
    }

    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    Resampler(Resampler&& other) noexcept
        : m_soxr(other.m_soxr)
        , m_inputRate(other.m_inputRate)
        , m_outputRate(other.m_outputRate)
        , m_channels(other.m_channels)
    {
        other.m_soxr = nullptr;
    }

    Resampler& operator=(Resampler&& other) noexcept {
        if (this != &other) {
            destroy();
            m_soxr = other.m_soxr;
            m_inputRate = other.m_inputRate;
            m_outputRate = other.m_outputRate;
            m_channels = other.m_channels;
            other.m_soxr = nullptr;
        }
        return *this;
    }

    bool configure(double inputRate, double outputRate, unsigned int channels = 2, ResamplerQuality quality = ResamplerQuality::VeryHigh) {
        destroy();
        m_inputRate = inputRate;
        m_outputRate = outputRate;
        m_channels = channels;

        if (isBypass()) {
            return true;
        }

        unsigned long qRecipe = SOXR_VHQ;
        switch (quality) {
            case ResamplerQuality::Quick: qRecipe = SOXR_QQ; break;
            case ResamplerQuality::Low: qRecipe = SOXR_LQ; break;
            case ResamplerQuality::Medium: qRecipe = SOXR_MQ; break;
            case ResamplerQuality::High: qRecipe = SOXR_HQ; break;
            case ResamplerQuality::VeryHigh: qRecipe = SOXR_VHQ; break;
        }

        soxr_error_t error;
        soxr_io_spec_t ioSpec = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
        soxr_quality_spec_t qSpec = soxr_quality_spec(qRecipe, 0);

        m_soxr = soxr_create(m_inputRate, m_outputRate, m_channels, &error, &ioSpec, &qSpec, nullptr);
        return (error == nullptr && m_soxr != nullptr);
    }

    [[nodiscard]] bool isBypass() const noexcept {
        return std::abs(m_inputRate - m_outputRate) < 0.001;
    }

    [[nodiscard]] double inputRate() const noexcept { return m_inputRate; }
    [[nodiscard]] double outputRate() const noexcept { return m_outputRate; }
    [[nodiscard]] unsigned int channels() const noexcept { return m_channels; }

    [[nodiscard]] double latencyInInputSamples() const noexcept {
        if (isBypass() || !m_soxr) return 0.0;
        return soxr_delay(m_soxr);
    }

    void reset() {
        if (m_soxr) {
            soxr_clear(m_soxr);
        }
    }

    /// Process interleaved audio frames.
    /// @param inInterleaved Input audio frames [inFrames * channels]
    /// @param inFrames Number of frames in input
    /// @param inDone Output actual input frames consumed
    /// @param outInterleaved Output buffer [outMaxFrames * channels]
    /// @param outMaxFrames Capacity of output buffer in frames
    /// @param outDone Output actual frames generated
    bool process(const double* inInterleaved, size_t inFrames, size_t& inDone,
                 double* outInterleaved, size_t outMaxFrames, size_t& outDone) {
        if (isBypass()) {
            size_t copyFrames = std::min(inFrames, outMaxFrames);
            std::copy_n(inInterleaved, copyFrames * m_channels, outInterleaved);
            inDone = copyFrames;
            outDone = copyFrames;
            return true;
        }

        if (!m_soxr) {
            inDone = 0;
            outDone = 0;
            return false;
        }

        soxr_error_t err = soxr_process(m_soxr,
                                        inInterleaved, inFrames, &inDone,
                                        outInterleaved, outMaxFrames, &outDone);
        return (err == nullptr);
    }

    /// Flush remaining delayed samples in SoXR at EOF (P0-11).
    /// Pass outInterleaved buffer with outMaxFrames capacity.
    /// Call repeatedly until outDone == 0.
    bool flush(double* outInterleaved, size_t outMaxFrames, size_t& outDone) {
        if (isBypass()) {
            outDone = 0;
            return true;
        }

        if (!m_soxr) {
            outDone = 0;
            return false;
        }

        soxr_error_t err = soxr_process(m_soxr,
                                        nullptr, 0, nullptr,
                                        outInterleaved, outMaxFrames, &outDone);
        return (err == nullptr);
    }

private:
    void destroy() noexcept {
        if (m_soxr) {
            soxr_delete(m_soxr);
            m_soxr = nullptr;
        }
    }

    soxr_t m_soxr = nullptr;
    double m_inputRate = 48000.0;
    double m_outputRate = 48000.0;
    unsigned int m_channels = 2;
};

} // namespace desk32::engine::dsp

