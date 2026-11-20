#pragma once

#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/ParameterSmoother.hpp"
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
        m_listeningCorrection.setEnabled(true);
        m_listeningVolSmoother.configure(48000.0, 10.0);
        m_renderMonVolSmoother.configure(48000.0, 10.0);
        m_listeningVolSmoother.reset(m_listeningVolume);
        m_renderMonVolSmoother.reset(m_renderMonitorVolume);
    }

    void setSampleRate(double sampleRate) {
        m_sampleRate = sampleRate;
        m_listeningCorrection.configure(m_sampleRate);
        m_listeningVolSmoother.configure(m_sampleRate, 10.0);
        m_renderMonVolSmoother.configure(m_sampleRate, 10.0);
    }

    [[nodiscard]] OutputPathMode activeMode() const noexcept { return m_activeMode; }
    void setMode(OutputPathMode mode) noexcept { m_activeMode = mode; }

    [[nodiscard]] double listeningVolume() const noexcept { return m_listeningVolume; }
    void setListeningVolume(double volLinear) noexcept {
        m_listeningVolume = std::clamp(volLinear, 0.0, 2.0);
        const double target = m_listeningMuted ? 0.0 : m_listeningVolume;
        if (!m_smootherPrimed) {
            m_listeningVolSmoother.reset(target);
        } else {
            m_listeningVolSmoother.setTarget(target);
        }
    }

    [[nodiscard]] bool listeningMuted() const noexcept { return m_listeningMuted; }
    void setListeningMuted(bool muted) noexcept {
        m_listeningMuted = muted;
        const double target = m_listeningMuted ? 0.0 : m_listeningVolume;
        if (!m_smootherPrimed) {
            m_listeningVolSmoother.reset(target);
        } else {
            m_listeningVolSmoother.setTarget(target);
        }
    }

    [[nodiscard]] double renderMonitorVolume() const noexcept { return m_renderMonitorVolume; }
    void setRenderMonitorVolume(double volLinear) noexcept {
        m_renderMonitorVolume = std::clamp(volLinear, 0.0, 2.0);
        const double target = m_renderMonitorMuted ? 0.0 : m_renderMonitorVolume;
        if (!m_smootherPrimed) {
            m_renderMonVolSmoother.reset(target);
        } else {
            m_renderMonVolSmoother.setTarget(target);
        }
    }

    [[nodiscard]] bool renderMonitorMuted() const noexcept { return m_renderMonitorMuted; }
    void setRenderMonitorMuted(bool muted) noexcept {
        m_renderMonitorMuted = muted;
        const double target = m_renderMonitorMuted ? 0.0 : m_renderMonitorVolume;
        if (!m_smootherPrimed) {
            m_renderMonVolSmoother.reset(target);
        } else {
            m_renderMonVolSmoother.setTarget(target);
        }
    }

    [[nodiscard]] bool isSmoothingVolume() const noexcept {
        return m_listeningVolSmoother.isSmoothing() || m_renderMonVolSmoother.isSmoothing();
    }

    ParametricEQ& listeningCorrection() noexcept { return m_listeningCorrection; }
    const ParametricEQ& listeningCorrection() const noexcept { return m_listeningCorrection; }

    /// Process audio block through selected Output Path.
    /// Input: mastered audio block from DSP Chain (Render Bus equivalent).
    /// Output: processed block delivered to target physical device.
    void routeBlock(const double* inInterleaved, double* outInterleaved, size_t frames) {
        m_smootherPrimed = true;
        if (m_activeMode == OutputPathMode::RenderMonitorPath) {
            // Render Monitor Path:
            // Strictly bypasses Listening Correction (Zero leakage)
            for (size_t i = 0; i < frames; ++i) {
                (void)m_listeningVolSmoother.next();
                const double rv = m_renderMonVolSmoother.next();
                outInterleaved[i * 2]     = inInterleaved[i * 2] * rv;
                outInterleaved[i * 2 + 1] = inInterleaved[i * 2 + 1] * rv;
            }
        } else {
            // Listening Path:
            // Applies Listening Correction (headphone/speaker EQ) followed by Listening Volume
            for (size_t i = 0; i < frames; ++i) {
                const double lv = m_listeningVolSmoother.next();
                (void)m_renderMonVolSmoother.next();
                double l = inInterleaved[i * 2];
                double r = inInterleaved[i * 2 + 1];
                m_listeningCorrection.process(l, r);
                outInterleaved[i * 2]     = l * lv;
                outInterleaved[i * 2 + 1] = r * lv;
            }
        }
    }

private:
    OutputPathMode m_activeMode = OutputPathMode::ListeningPath;
    double m_sampleRate = 48000.0;
    double m_listeningVolume = 1.0;
    double m_renderMonitorVolume = 1.0;
    bool m_listeningMuted = false;
    bool m_renderMonitorMuted = false;
    bool m_smootherPrimed = false;
    dsp::ParameterSmoother m_listeningVolSmoother{1.0, 10.0, 48000.0};
    dsp::ParameterSmoother m_renderMonVolSmoother{1.0, 10.0, 48000.0};
    ParametricEQ m_listeningCorrection;
};

} // namespace desk32::engine::routing
