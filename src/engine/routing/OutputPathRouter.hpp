#pragma once

#include "engine/dsp/ParametricEQ.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace desk32::engine::routing {

enum class OutputPathMode {
    ListeningPath,       // Through Listening Correction -> Listening Volume -> Device
    RenderMonitorPath    // Logical Render Tap -> Render Monitor Volume -> Device (No Listening Correction)
};

class OutputPathRouter {
public:
    OutputPathRouter() {
        m_listeningCorrection.configure(48000.0);
    }

    void setSampleRate(double sampleRate) {
        m_sampleRate = sampleRate;
        m_listeningCorrection.configure(m_sampleRate);
    }

    [[nodiscard]] OutputPathMode activeMode() const noexcept { return m_activeMode; }
    void setMode(OutputPathMode mode) noexcept { m_activeMode = mode; }

    [[nodiscard]] double listeningVolume() const noexcept { return m_listeningVolume; }
    void setListeningVolume(double volLinear) noexcept { m_listeningVolume = std::clamp(volLinear, 0.0, 2.0); }

    [[nodiscard]] double renderMonitorVolume() const noexcept { return m_renderMonitorVolume; }
    void setRenderMonitorVolume(double volLinear) noexcept { m_renderMonitorVolume = std::clamp(volLinear, 0.0, 2.0); }

    ParametricEQ& listeningCorrection() noexcept { return m_listeningCorrection; }
    const ParametricEQ& listeningCorrection() const noexcept { return m_listeningCorrection; }

    /// Process audio block through selected Output Path.
    /// Input: mastered audio block from DSP Chain (Render Bus equivalent).
    /// Output: processed block delivered to target physical device.
    void routeBlock(const double* inInterleaved, double* outInterleaved, size_t frames) {
        if (m_activeMode == OutputPathMode::RenderMonitorPath) {
            // Render Monitor Path:
            // Strictly bypasses Listening Correction (Zero leakage)
            for (size_t i = 0; i < frames; ++i) {
                outInterleaved[i * 2]     = inInterleaved[i * 2] * m_renderMonitorVolume;
                outInterleaved[i * 2 + 1] = inInterleaved[i * 2 + 1] * m_renderMonitorVolume;
            }
        } else {
            // Listening Path:
            // Applies Listening Correction (headphone/speaker EQ) followed by Listening Volume
            for (size_t i = 0; i < frames; ++i) {
                double l = inInterleaved[i * 2];
                double r = inInterleaved[i * 2 + 1];
                m_listeningCorrection.process(l, r);
                outInterleaved[i * 2]     = l * m_listeningVolume;
                outInterleaved[i * 2 + 1] = r * m_listeningVolume;
            }
        }
    }

private:
    OutputPathMode m_activeMode = OutputPathMode::ListeningPath;
    double m_sampleRate = 48000.0;
    double m_listeningVolume = 1.0;
    double m_renderMonitorVolume = 1.0;
    ParametricEQ m_listeningCorrection;
};

} // namespace desk32::engine::routing
