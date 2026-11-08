#pragma once

#include <cmath>
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include "engine/memory/EpochReclaimer.hpp"

// Sample-Peak Brickwall Limiter with Lookahead & GR Telemetry (Explicitly NOT a True Peak Limiter, Section 5.5, P0-14)
// Control Thread: Calculates all ceiling conversions, release alphas, lookahead sizing, and buffer initializations.
// Audio Thread: Reads precalculated parameters via Lock-Free EpochReclaimer with strictly zero heap allocation/deallocation,
// zero mutex waits, and zero heavy configure math.
class PeakLimiter
{
public:
    static constexpr size_t MAX_LOOKAHEAD = 1024; // Up to 5.33 ms at 192 kHz

    struct Params {
        double sampleRate = 48000.0;
        double ceilingDb = -0.1;
        double ceiling = 0.9885530946569388; // -0.1 dB
        double releaseMs = 50.0;
        double lookaheadMs = 1.0;
        double releaseAlpha = 0.0;
        size_t lookaheadSamples = 48;
        bool enabled = false;
    };

    PeakLimiter()
    {
        configure(48000.0);
    }

    PeakLimiter(const PeakLimiter& other)
    {
        *this = other;
    }

    PeakLimiter& operator=(const PeakLimiter& other)
    {
        if (this != &other) {
            auto guard = other.m_paramsReclaimer.enterRead();
            if (guard) {
                configure(guard->sampleRate, guard->ceilingDb, guard->releaseMs,
                          guard->lookaheadMs, guard->enabled);
            } else {
                configure(48000.0);
            }
            m_writePos = other.m_writePos;
            m_currentGain = other.m_currentGain;
            m_bufferLeft = other.m_bufferLeft;
            m_bufferRight = other.m_bufferRight;
            m_gainReductionDb.store(other.m_gainReductionDb.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    /// Executed on Control Thread: Precalculates alphas, ceiling multipliers, and handles lookahead buffer sizing
    void configure(double sampleRate, double ceilingDb = -0.1, double releaseMs = 50.0,
                   double lookaheadMs = 1.0, bool enabled = true)
    {
        bool rateChanged = std::abs(m_sampleRate - sampleRate) > 0.01;
        m_sampleRate = sampleRate;

        auto p = std::make_unique<Params>();
        p->sampleRate = sampleRate;
        p->ceilingDb = std::clamp(ceilingDb, -12.0, 0.0);
        p->ceiling = std::pow(10.0, p->ceilingDb / 20.0);
        p->releaseMs = std::clamp(releaseMs, 10.0, 500.0);
        p->lookaheadMs = std::clamp(lookaheadMs, 0.1, 5.0);
        p->releaseAlpha = std::exp(-1.0 / (sampleRate * (p->releaseMs * 0.001)));

        size_t samples = static_cast<size_t>(std::ceil(sampleRate * (p->lookaheadMs * 0.001)));
        p->lookaheadSamples = std::clamp(samples, size_t(8), MAX_LOOKAHEAD);
        p->enabled = enabled;

        m_ceilingDb = p->ceilingDb;
        m_ceiling = p->ceiling;
        m_releaseMs = p->releaseMs;
        m_lookaheadMs = p->lookaheadMs;
        m_releaseAlpha = p->releaseAlpha;
        m_enabled.store(enabled, std::memory_order_relaxed);

        if (rateChanged || p->lookaheadSamples != m_lookaheadSamples) {
            m_lookaheadSamples = p->lookaheadSamples;
            reset();
        }

        m_paramsReclaimer.swapChain(std::move(p));
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] double ceilingDb() const noexcept { return m_ceilingDb; }
    [[nodiscard]] double ceiling() const noexcept { return m_ceiling; }
    [[nodiscard]] double releaseMs() const noexcept { return m_releaseMs; }
    [[nodiscard]] double lookaheadMs() const noexcept { return m_lookaheadMs; }
    [[nodiscard]] size_t lookaheadSamples() const noexcept { return m_lookaheadSamples; }
    [[nodiscard]] int64_t latencyFrames() const noexcept {
        return m_enabled.load(std::memory_order_relaxed) ? static_cast<int64_t>(m_lookaheadSamples) : 0;
    }
    [[nodiscard]] double gainReductionDb() const noexcept { return m_gainReductionDb.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    void setCeilingDb(double db) { configure(m_sampleRate, db, m_releaseMs, m_lookaheadMs, m_enabled.load(std::memory_order_relaxed)); }
    void setReleaseMs(double ms) { configure(m_sampleRate, m_ceilingDb, ms, m_lookaheadMs, m_enabled.load(std::memory_order_relaxed)); }
    void setLookaheadMs(double ms) { configure(m_sampleRate, m_ceilingDb, m_releaseMs, ms, m_enabled.load(std::memory_order_relaxed)); }
    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }

    PeakLimiter* nextRetired = nullptr;

    void inheritStateFrom(const PeakLimiter& other) noexcept
    {
        m_currentGain = other.m_currentGain;
        m_gainReductionDb.store(other.m_gainReductionDb.load(std::memory_order_relaxed), std::memory_order_relaxed);

        // Inherit lookahead ring buffer samples (P0-23)
        // Copies unread lookahead samples chronologically so no samples are dropped or duplicated
        const size_t otherLookahead = other.m_lookaheadSamples;
        const size_t myLookahead = m_lookaheadSamples;

        m_bufferLeft.fill(0.0);
        m_bufferRight.fill(0.0);

        if (otherLookahead > 0 && myLookahead > 0) {
            const size_t count = std::min(otherLookahead, myLookahead);
            for (size_t i = 0; i < count; ++i) {
                size_t srcOffset = (other.m_writePos + otherLookahead - count + i) % otherLookahead;
                m_bufferLeft[i] = other.m_bufferLeft[srcOffset];
                m_bufferRight[i] = other.m_bufferRight[srcOffset];
            }
            m_writePos = count % myLookahead;
        } else {
            m_writePos = 0;
        }
    }

    void reset()
    {
        m_bufferLeft.fill(0.0);
        m_bufferRight.fill(0.0);
        m_writePos = 0;
        m_currentGain = 1.0;
        m_gainReductionDb.store(0.0, std::memory_order_relaxed);
    }

    /// Real-time audio rendering: Lock-free, zero heap allocation, bounded execution time.
    inline void process(double& left, double& right)
    {
        auto guard = m_paramsReclaimer.enterRead();
        const Params* p = guard.get();
        if (!p || !m_enabled.load(std::memory_order_relaxed)) {
            m_gainReductionDb.store(0.0, std::memory_order_relaxed);
            return;
        }

        // Sample peak of incoming frame
        const double peak = std::max(std::abs(left), std::abs(right));

        double targetGain = 1.0;
        if (peak > p->ceiling) {
            targetGain = p->ceiling / peak;
        }

        // Instantaneous attack (brickwall lookahead), exponential release
        if (targetGain < m_currentGain) {
            m_currentGain = targetGain;
        } else {
            m_currentGain = p->releaseAlpha * m_currentGain + (1.0 - p->releaseAlpha) * targetGain;
        }

        // Record gain reduction in dB
        double grDb = (m_currentGain < 0.9999) ? -20.0 * std::log10(m_currentGain) : 0.0;
        m_gainReductionDb.store(grDb, std::memory_order_relaxed);

        const size_t lookahead = p->lookaheadSamples;
        const size_t pos = m_writePos % lookahead;

        // Retrieve delayed sample from preallocated ring buffer
        const double delayedLeft = m_bufferLeft[pos];
        const double delayedRight = m_bufferRight[pos];

        // Store new sample into ring buffer
        m_bufferLeft[pos] = left;
        m_bufferRight[pos] = right;
        m_writePos = (pos + 1) % lookahead;

        // Output delayed sample scaled by currentGain
        left = delayedLeft * m_currentGain;
        right = delayedRight * m_currentGain;

        // Final safety ceiling clamp against numerical transients
        if (std::abs(left) > p->ceiling) {
            left = (left > 0.0) ? p->ceiling : -p->ceiling;
        }
        if (std::abs(right) > p->ceiling) {
            right = (right > 0.0) ? p->ceiling : -p->ceiling;
        }
    }

private:
    double m_sampleRate = 48000.0;
    double m_ceilingDb = -0.1;
    double m_ceiling = 0.9885530946569388;
    double m_releaseMs = 50.0;
    double m_lookaheadMs = 1.0;
    double m_releaseAlpha = 0.0;
    size_t m_lookaheadSamples = 48;
    size_t m_writePos = 0;
    double m_currentGain = 1.0;

    std::atomic<bool> m_enabled{false};
    std::atomic<double> m_gainReductionDb{0.0};

    // Preallocated fixed ring buffer (strictly zero dynamic allocation)
    alignas(64) std::array<double, MAX_LOOKAHEAD> m_bufferLeft{};
    alignas(64) std::array<double, MAX_LOOKAHEAD> m_bufferRight{};

    mutable EpochReclaimer<Params> m_paramsReclaimer;
};
