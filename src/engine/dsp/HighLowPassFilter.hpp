#pragma once

#include "BiquadFilter.hpp"

// 2nd-order Butterworth HPF & LPF
class HighLowPassFilter
{
public:
    HighLowPassFilter() = default;

    void configure(double sampleRate, double hpfFreq = 20.0, double lpfFreq = 20000.0)
    {
        m_sampleRate = sampleRate;
        m_hpfFreq = hpfFreq;
        m_lpfFreq = lpfFreq;

        const double Q = 0.7071067811865475; // Butterworth Q

        m_hpfLeft.configure(BiquadFilterType::HighPass, sampleRate, hpfFreq, Q, 0.0);
        m_hpfRight.configure(BiquadFilterType::HighPass, sampleRate, hpfFreq, Q, 0.0);

        double safeLpf = std::min(lpfFreq, sampleRate * 0.48);
        m_lpfLeft.configure(BiquadFilterType::LowPass, sampleRate, safeLpf, Q, 0.0);
        m_lpfRight.configure(BiquadFilterType::LowPass, sampleRate, safeLpf, Q, 0.0);
    }

    void reset()
    {
        m_hpfLeft.reset();
        m_hpfRight.reset();
        m_lpfLeft.reset();
        m_lpfRight.reset();
    }

    inline void process(double& left, double& right)
    {
        if (m_hpfEnabled) {
            left = m_hpfLeft.process(left);
            right = m_hpfRight.process(right);
        }
        if (m_lpfEnabled) {
            left = m_lpfLeft.process(left);
            right = m_lpfRight.process(right);
        }
    }

    void setHpfEnabled(bool enabled) { m_hpfEnabled = enabled; }
    void setLpfEnabled(bool enabled) { m_lpfEnabled = enabled; }

private:
    double m_sampleRate = 48000.0;
    double m_hpfFreq = 20.0;
    double m_lpfFreq = 20000.0;
    bool m_hpfEnabled = true;
    bool m_lpfEnabled = true;

    BiquadFilter m_hpfLeft;
    BiquadFilter m_hpfRight;
    BiquadFilter m_lpfLeft;
    BiquadFilter m_lpfRight;
};

