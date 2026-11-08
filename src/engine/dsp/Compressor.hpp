#pragma once

#include <cmath>
#include <algorithm>
#include <atomic>
#include <memory>
#include "engine/memory/EpochReclaimer.hpp"

// Clean, neutral 64-bit float Feedforward Compressor with Soft-Knee and GR reporting (Section 5.4, P0-14)
// Control Thread: Calculates all coefficients, exponents, power conversions, and clamps in configure().
// Audio Thread: Reads precalculated parameters via Lock-Free EpochReclaimer with strictly zero heap allocation/deallocation,
// zero mutex waits, zero heavy configure math, and continuous envelope follower preservation across parameter updates.
class Compressor
{
public:
    struct Params {
        double sampleRate = 48000.0;
        double thresholdDb = -12.0;
        double ratio = 4.0;
        double attackMs = 10.0;
        double releaseMs = 100.0;
        double makeupGainDb = 0.0;
        double makeupGain = 1.0;
        double kneeDb = 4.0;
        double attackAlpha = 0.0;
        double releaseAlpha = 0.0;
        bool enabled = false;
    };

    Compressor()
    {
        configure(48000.0);
    }

    Compressor(const Compressor& other)
    {
        *this = other;
    }

    Compressor& operator=(const Compressor& other)
    {
        if (this != &other) {
            auto guard = other.m_paramsReclaimer.enterRead();
            if (guard) {
                configure(guard->sampleRate, guard->thresholdDb, guard->ratio, guard->attackMs,
                          guard->releaseMs, guard->makeupGainDb, guard->kneeDb, guard->enabled);
            } else {
                configure(48000.0);
            }
            m_envelope = other.m_envelope;
            m_gainReductionDb.store(other.m_gainReductionDb.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    /// Executed on Control Thread: Computes math and publishes new immutable parameter snapshot
    void configure(double sampleRate, double thresholdDb = -12.0, double ratio = 4.0,
                   double attackMs = 10.0, double releaseMs = 100.0, double makeupGainDb = 0.0,
                   double kneeDb = 4.0, bool enabled = true)
    {
        auto p = std::make_unique<Params>();
        p->sampleRate = sampleRate;
        p->thresholdDb = std::clamp(thresholdDb, -60.0, 0.0);
        p->ratio = std::clamp(ratio, 1.0, 20.0);
        p->attackMs = std::max(0.1, attackMs);
        p->releaseMs = std::max(1.0, releaseMs);
        p->makeupGainDb = std::clamp(makeupGainDb, -12.0, 24.0);
        p->makeupGain = std::pow(10.0, p->makeupGainDb / 20.0);
        p->kneeDb = std::clamp(kneeDb, 0.0, 20.0);

        p->attackAlpha = std::exp(-1.0 / (sampleRate * std::max(0.0001, p->attackMs * 0.001)));
        p->releaseAlpha = std::exp(-1.0 / (sampleRate * std::max(0.001, p->releaseMs * 0.001)));
        p->enabled = enabled;

        m_sampleRate = sampleRate;
        m_thresholdDb = p->thresholdDb;
        m_ratio = p->ratio;
        m_attackMs = p->attackMs;
        m_releaseMs = p->releaseMs;
        m_makeupGainDb = p->makeupGainDb;
        m_makeupGain = p->makeupGain;
        m_kneeDb = p->kneeDb;
        m_attackAlpha = p->attackAlpha;
        m_releaseAlpha = p->releaseAlpha;
        m_enabled.store(enabled, std::memory_order_relaxed);

        m_paramsReclaimer.swapChain(std::move(p));
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] double thresholdDb() const noexcept { return m_thresholdDb; }
    [[nodiscard]] double ratio() const noexcept { return m_ratio; }
    [[nodiscard]] double attackMs() const noexcept { return m_attackMs; }
    [[nodiscard]] double releaseMs() const noexcept { return m_releaseMs; }
    [[nodiscard]] double makeupGainDb() const noexcept { return m_makeupGainDb; }
    [[nodiscard]] double kneeDb() const noexcept { return m_kneeDb; }
    [[nodiscard]] double gainReductionDb() const noexcept { return m_gainReductionDb.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    void setThresholdDb(double db) { configure(m_sampleRate, db, m_ratio, m_attackMs, m_releaseMs, m_makeupGainDb, m_kneeDb, m_enabled.load(std::memory_order_relaxed)); }
    void setRatio(double r) { configure(m_sampleRate, m_thresholdDb, r, m_attackMs, m_releaseMs, m_makeupGainDb, m_kneeDb, m_enabled.load(std::memory_order_relaxed)); }
    void setAttackMs(double ms) { configure(m_sampleRate, m_thresholdDb, m_ratio, ms, m_releaseMs, m_makeupGainDb, m_kneeDb, m_enabled.load(std::memory_order_relaxed)); }
    void setReleaseMs(double ms) { configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, ms, m_makeupGainDb, m_kneeDb, m_enabled.load(std::memory_order_relaxed)); }
    void setMakeupGainDb(double db) { configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, m_releaseMs, db, m_kneeDb, m_enabled.load(std::memory_order_relaxed)); }
    void setKneeDb(double db) { configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, m_releaseMs, m_makeupGainDb, db, m_enabled.load(std::memory_order_relaxed)); }
    void setEnabled(bool enabled) noexcept { configure(m_sampleRate, m_thresholdDb, m_ratio, m_attackMs, m_releaseMs, m_makeupGainDb, m_kneeDb, enabled); }

    void reset()
    {
        m_envelope = 0.0;
        m_gainReductionDb.store(0.0, std::memory_order_relaxed);
    }

    /// Real-time audio rendering: Lock-free, zero heap allocation, bounded execution time.
    inline void process(double& left, double& right)
    {
        auto guard = m_paramsReclaimer.enterRead();
        const Params* p = guard.get();
        if (!p || !p->enabled) {
            m_gainReductionDb.store(0.0, std::memory_order_relaxed);
            return;
        }

        // Peak detector (max of absolute stereo channels)
        const double peak = std::max(std::abs(left), std::abs(right));

        // Envelope follower with decoupled attack / release filtering
        if (peak > m_envelope) {
            m_envelope = (1.0 - p->attackAlpha) * peak + p->attackAlpha * m_envelope;
        } else {
            m_envelope = (1.0 - p->releaseAlpha) * peak + p->releaseAlpha * m_envelope;
        }

        const double envDb = (m_envelope > 1e-9) ? 20.0 * std::log10(m_envelope) : -180.0;

        // Dynamic curve with soft-knee interpolation
        double grDb = 0.0;
        const double delta = envDb - p->thresholdDb;
        const double halfKnee = p->kneeDb * 0.5;

        if (p->kneeDb > 0.1 && delta > -halfKnee && delta < halfKnee) {
            // Soft-knee quadratic transition
            const double normalizedKnee = (delta + halfKnee) / p->kneeDb;
            grDb = (normalizedKnee * normalizedKnee * halfKnee) * (1.0 - 1.0 / p->ratio);
        } else if (delta >= halfKnee) {
            // Full compression region
            grDb = delta * (1.0 - 1.0 / p->ratio);
        }

        m_gainReductionDb.store(grDb, std::memory_order_relaxed);

        const double gainLinear = std::pow(10.0, -grDb / 20.0) * p->makeupGain;
        left *= gainLinear;
        right *= gainLinear;
    }

private:
    double m_sampleRate = 48000.0;
    double m_thresholdDb = -12.0;
    double m_ratio = 4.0;
    double m_attackMs = 10.0;
    double m_releaseMs = 100.0;
    double m_makeupGainDb = 0.0;
    double m_makeupGain = 1.0;
    double m_kneeDb = 4.0;
    double m_attackAlpha = 0.0;
    double m_releaseAlpha = 0.0;
    double m_envelope = 0.0;

    std::atomic<bool> m_enabled{false};
    std::atomic<double> m_gainReductionDb{0.0};

    mutable EpochReclaimer<Params> m_paramsReclaimer;
};
