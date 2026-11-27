#pragma once

#include <memory>
#include <vector>
#include <array>
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include "engine/pipeline/AudioNode.hpp"

namespace desk32::engine::render {

/// Immutable snapshot of DSP parameters captured at Render Job enqueue time (P0-03).
/// Guarantees that running and enqueued render jobs are completely isolated from
/// subsequent GUI tweaks, live console edits, and subsequent render job registrations.
struct RenderDspSnapshot {
    bool enabled = true;

    // PEQ
    bool peqEnabled = true;
    double peqPreampDb = 0.0;
    std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> peqLeftBands{};
    std::array<ParametricEQ::BandConfig, ParametricEQ::BANDS_PER_CHANNEL> peqRightBands{};

    // GEQ
    bool geqEnabled = true;
    std::array<double, 32> geqGains{};
    std::array<bool, 32> geqBandEnabled = [] {
        std::array<bool, 32> a{};
        a.fill(true);
        return a;
    }();

    // Filter
    bool hpfEnabled = false;
    double hpfFrequency = 20.0;
    HighLowPassFilter::FilterSlope hpfSlope = HighLowPassFilter::FilterSlope::Slope12dB;
    double hpfQ = 0.7071067811865475;
    bool lpfEnabled = false;
    double lpfFrequency = 20000.0;
    HighLowPassFilter::FilterSlope lpfSlope = HighLowPassFilter::FilterSlope::Slope12dB;
    double lpfQ = 0.7071067811865475;

    // Compressor
    bool compEnabled = false;
    double compThresholdDb = -12.0;
    double compRatio = 4.0;
    double compAttackMs = 10.0;
    double compReleaseMs = 100.0;
    double compMakeupGainDb = 0.0;
    double compKneeDb = 4.0;

    // Reverb
    bool reverbEnabled = false;
    double reverbRoomSize = 0.7;
    double reverbDamp = 0.3;
    double reverbWetLevel = 0.2;
    double reverbPredelayMs = 0.0;
    bool reverbTailPreserving = true;
    double reverbDecaySeconds = -1.0;

    // Limiter
    bool limiterEnabled = true;
    double limiterCeilingDb = -0.1;
    double limiterReleaseMs = 50.0;
    double limiterLookaheadMs = 1.0;

    // Per-DSP Input / Output Trims (dB, excluded from bypass & presets)
    double peqInputTrimDb = 0.0;
    double peqOutputTrimDb = 0.0;
    double geqInputTrimDb = 0.0;
    double geqOutputTrimDb = 0.0;
    double hpfInputTrimDb = 0.0;
    double hpfOutputTrimDb = 0.0;
    double lpfInputTrimDb = 0.0;
    double lpfOutputTrimDb = 0.0;
    double compInputTrimDb = 0.0;
    double compOutputTrimDb = 0.0;
    double limiterInputTrimDb = 0.0;
    double limiterOutputTrimDb = 0.0;
    double reverbInputTrimDb = 0.0;
    double reverbOutputTrimDb = 0.0;

    // External Insert live-session exclusion notice (§6.6)
    bool externalInsertWasActiveInLive = false;
    std::string externalInsertNodeName;
};

/// Offline DSP Render Chain for File Output (Specification Section 5.11.3 & 20.1).
/// Operates exclusively on the background Render Worker Thread (NEVER on Audio Thread).
/// Architectural Guarantees:
/// - Strictly EXCLUDES Listening Correction (0.000000 leakage into rendered files)
/// - Strictly EXCLUDES Listening Volume and Render Monitor Volume
/// - Completely decoupled from real-time live playback state
class RenderChain {
public:
    RenderChain() = default;
    ~RenderChain() = default;

    RenderChain(const RenderChain&) = delete;
    RenderChain& operator=(const RenderChain&) = delete;

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled; }
    void setEnabled(bool enabled) noexcept { m_enabled = enabled; }

    void configure(double sampleRate) {
        m_sampleRate = sampleRate;
        m_peq.configure(sampleRate);
        m_geq.configure(sampleRate);
        m_filter.configure(sampleRate);
        m_comp.configure(sampleRate);
        m_limiter.configure(sampleRate);
        m_reverb.configure(sampleRate);
    }

    void reset() {
        m_peq.reset();
        m_geq.reset();
        m_filter.reset();
        m_comp.reset();
        m_limiter.reset();
        m_reverb.reset();
    }

    void setExternalInsertLiveStatus(bool active, std::string nodeName = {}) {
        m_externalInsertWasActiveInLive = active;
        m_externalInsertNodeName = std::move(nodeName);
    }

    // Direct access to cloned DSP processors
    [[nodiscard]] ParametricEQ& peq() noexcept { return m_peq; }
    [[nodiscard]] const ParametricEQ& peq() const noexcept { return m_peq; }
    [[nodiscard]] GraphicEQ& geq() noexcept { return m_geq; }
    [[nodiscard]] const GraphicEQ& geq() const noexcept { return m_geq; }
    [[nodiscard]] HighLowPassFilter& filter() noexcept { return m_filter; }
    [[nodiscard]] const HighLowPassFilter& filter() const noexcept { return m_filter; }
    [[nodiscard]] Compressor& compressor() noexcept { return m_comp; }
    [[nodiscard]] const Compressor& compressor() const noexcept { return m_comp; }
    [[nodiscard]] PeakLimiter& limiter() noexcept { return m_limiter; }
    [[nodiscard]] const PeakLimiter& limiter() const noexcept { return m_limiter; }
    [[nodiscard]] AlgorithmicReverb& reverb() noexcept { return m_reverb; }
    [[nodiscard]] const AlgorithmicReverb& reverb() const noexcept { return m_reverb; }

    void setTrims(const std::array<double, 7>& inDb, const std::array<double, 7>& outDb) noexcept {
        for (size_t i = 0; i < 7; ++i) {
            const double ci = std::clamp(inDb[i], -12.0, 12.0);
            const double co = std::clamp(outDb[i], -12.0, 12.0);
            m_inputTrimDb[i] = (std::abs(ci) <= 1e-9) ? 0.0 : ci;
            m_outputTrimDb[i] = (std::abs(co) <= 1e-9) ? 0.0 : co;
        }
        m_filter.setHpfTrimDb(m_inputTrimDb[2], m_outputTrimDb[2], 0);
        m_filter.setLpfTrimDb(m_inputTrimDb[3], m_outputTrimDb[3], 0);
    }

    [[nodiscard]] double inputTrimDb(size_t idx) const noexcept {
        return (idx < 7) ? m_inputTrimDb[idx] : 0.0;
    }
    [[nodiscard]] double outputTrimDb(size_t idx) const noexcept {
        return (idx < 7) ? m_outputTrimDb[idx] : 0.0;
    }

    /// Synchronize parameters from live console Master Chain
    void copyParametersFrom(const ParametricEQ& peq,
                            const GraphicEQ& geq,
                            const HighLowPassFilter& flt,
                            const Compressor& comp,
                            const AlgorithmicReverb& rev,
                            const PeakLimiter& lim)
    {
        // 1. Parametric EQ
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            m_peq.setLeftBand(b, peq.leftBand(b));
            m_peq.setRightBand(b, peq.rightBand(b));
        }
        m_peq.setPreampDb(peq.preampDb());
        m_peq.setEnabled(peq.isEnabled());

        // 2. Graphic EQ
        for (size_t b = 0; b < 32; ++b) {
            m_geq.setBandEnabled(b, geq.isBandEnabled(b));
            m_geq.setBandGain(b, geq.bandGain(b));
        }
        m_geq.setEnabled(geq.isEnabled());

        // 3. Filter
        m_filter.setHpfFrequency(flt.hpfFrequency());
        m_filter.setHpfSlope(flt.hpfSlope());
        m_filter.setHpfQ(flt.hpfQ());
        m_filter.setHpfEnabled(flt.isHpfEnabled());
        m_filter.setLpfFrequency(flt.lpfFrequency());
        m_filter.setLpfSlope(flt.lpfSlope());
        m_filter.setLpfQ(flt.lpfQ());
        m_filter.setLpfEnabled(flt.isLpfEnabled());
        m_inputTrimDb[2] = flt.hpfInputTrimDb();
        m_outputTrimDb[2] = flt.hpfOutputTrimDb();
        m_inputTrimDb[3] = flt.lpfInputTrimDb();
        m_outputTrimDb[3] = flt.lpfOutputTrimDb();
        m_filter.setHpfTrimDb(m_inputTrimDb[2], m_outputTrimDb[2], 0);
        m_filter.setLpfTrimDb(m_inputTrimDb[3], m_outputTrimDb[3], 0);

        // 4. Compressor
        m_comp.configure(m_sampleRate, comp.thresholdDb(), comp.ratio(), comp.attackMs(), comp.releaseMs(), comp.makeupGainDb(), comp.kneeDb());
        m_comp.setEnabled(comp.isEnabled());

        // 5. Reverb
        m_reverb.configure(m_sampleRate, rev.roomSize(), rev.damping(), rev.wetLevel(), rev.predelayMs(),
                           rev.isTailPreserving(), true, rev.usesExplicitDecay() ? rev.decaySeconds() : -1.0);
        m_reverb.setEnabled(rev.isEnabled());

        // 6. Peak Limiter
        m_limiter.configure(m_sampleRate, lim.ceilingDb(), lim.releaseMs(), lim.lookaheadMs());
        m_limiter.setEnabled(lim.isEnabled());
    }

    /// Capture an immutable parameter snapshot (P0-03)
    [[nodiscard]] RenderDspSnapshot createSnapshot() const {
        RenderDspSnapshot s;
        s.enabled = m_enabled;

        // 1. PEQ
        s.peqEnabled = m_peq.isEnabled();
        s.peqPreampDb = m_peq.preampDb();
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            s.peqLeftBands[b] = m_peq.leftBand(b);
            s.peqRightBands[b] = m_peq.rightBand(b);
        }

        // 2. GEQ
        s.geqEnabled = m_geq.isEnabled();
        for (size_t b = 0; b < 32; ++b) {
            s.geqGains[b] = m_geq.bandGain(b);
            s.geqBandEnabled[b] = m_geq.isBandEnabled(b);
        }

        // 3. Filter
        s.hpfEnabled = m_filter.isHpfEnabled();
        s.hpfFrequency = m_filter.hpfFrequency();
        s.hpfSlope = m_filter.hpfSlope();
        s.hpfQ = m_filter.hpfQ();
        s.lpfEnabled = m_filter.isLpfEnabled();
        s.lpfFrequency = m_filter.lpfFrequency();
        s.lpfSlope = m_filter.lpfSlope();
        s.lpfQ = m_filter.lpfQ();

        // 4. Compressor
        s.compEnabled = m_comp.isEnabled();
        s.compThresholdDb = m_comp.thresholdDb();
        s.compRatio = m_comp.ratio();
        s.compAttackMs = m_comp.attackMs();
        s.compReleaseMs = m_comp.releaseMs();
        s.compMakeupGainDb = m_comp.makeupGainDb();
        s.compKneeDb = m_comp.kneeDb();

        // 5. Reverb
        s.reverbEnabled = m_reverb.isEnabled();
        s.reverbRoomSize = m_reverb.roomSize();
        s.reverbDamp = m_reverb.damping();
        s.reverbWetLevel = m_reverb.wetLevel();
        s.reverbPredelayMs = m_reverb.predelayMs();
        s.reverbTailPreserving = m_reverb.isTailPreserving();
        s.reverbDecaySeconds = m_reverb.usesExplicitDecay() ? m_reverb.decaySeconds() : -1.0;

        // 6. Peak Limiter
        s.limiterEnabled = m_limiter.isEnabled();
        s.limiterCeilingDb = m_limiter.ceilingDb();
        s.limiterReleaseMs = m_limiter.releaseMs();
        s.limiterLookaheadMs = m_limiter.lookaheadMs();

        s.externalInsertWasActiveInLive = m_externalInsertWasActiveInLive;
        s.externalInsertNodeName = m_externalInsertNodeName;

        // 7. Trims
        s.peqInputTrimDb = m_inputTrimDb[0];
        s.peqOutputTrimDb = m_outputTrimDb[0];
        s.geqInputTrimDb = m_inputTrimDb[1];
        s.geqOutputTrimDb = m_outputTrimDb[1];
        s.hpfInputTrimDb = m_inputTrimDb[2];
        s.hpfOutputTrimDb = m_outputTrimDb[2];
        s.lpfInputTrimDb = m_inputTrimDb[3];
        s.lpfOutputTrimDb = m_outputTrimDb[3];
        s.compInputTrimDb = m_inputTrimDb[4];
        s.compOutputTrimDb = m_outputTrimDb[4];
        s.limiterInputTrimDb = m_inputTrimDb[5];
        s.limiterOutputTrimDb = m_outputTrimDb[5];
        s.reverbInputTrimDb = m_inputTrimDb[6];
        s.reverbOutputTrimDb = m_outputTrimDb[6];

        return s;
    }

    /// Apply an immutable parameter snapshot configured for target sample rate (P0-03)
    void applySnapshot(const RenderDspSnapshot& s, double sampleRate) {
        configure(sampleRate);
        m_enabled = s.enabled;

        // 1. PEQ
        m_peq.setEnabled(s.peqEnabled);
        m_peq.setPreampDb(s.peqPreampDb);
        for (size_t b = 0; b < ParametricEQ::BANDS_PER_CHANNEL; ++b) {
            m_peq.setLeftBand(b, s.peqLeftBands[b]);
            m_peq.setRightBand(b, s.peqRightBands[b]);
        }

        // 2. GEQ
        m_geq.setEnabled(s.geqEnabled);
        for (size_t b = 0; b < 32; ++b) {
            m_geq.setBandEnabled(b, s.geqBandEnabled[b]);
            m_geq.setBandGain(b, s.geqGains[b]);
        }

        // 3. Filter
        m_filter.setHpfFrequency(s.hpfFrequency);
        m_filter.setHpfSlope(s.hpfSlope);
        m_filter.setHpfQ(s.hpfQ);
        m_filter.setHpfEnabled(s.hpfEnabled);
        m_filter.setLpfFrequency(s.lpfFrequency);
        m_filter.setLpfSlope(s.lpfSlope);
        m_filter.setLpfQ(s.lpfQ);
        m_filter.setLpfEnabled(s.lpfEnabled);

        // 4. Compressor
        m_comp.configure(sampleRate, s.compThresholdDb, s.compRatio, s.compAttackMs, s.compReleaseMs, s.compMakeupGainDb, s.compKneeDb);
        m_comp.setEnabled(s.compEnabled);

        // 5. Reverb
        m_reverb.configure(sampleRate, s.reverbRoomSize, s.reverbDamp, s.reverbWetLevel, s.reverbPredelayMs,
                           s.reverbTailPreserving, true, s.reverbDecaySeconds);
        m_reverb.setEnabled(s.reverbEnabled);

        // 6. Peak Limiter
        m_limiter.configure(sampleRate, s.limiterCeilingDb, s.limiterReleaseMs, s.limiterLookaheadMs);
        m_limiter.setEnabled(s.limiterEnabled);

        // 7. Trims
        setTrims(
            {s.peqInputTrimDb, s.geqInputTrimDb, s.hpfInputTrimDb, s.lpfInputTrimDb,
             s.compInputTrimDb, s.limiterInputTrimDb, s.reverbInputTrimDb},
            {s.peqOutputTrimDb, s.geqOutputTrimDb, s.hpfOutputTrimDb, s.lpfOutputTrimDb,
             s.compOutputTrimDb, s.limiterOutputTrimDb, s.reverbOutputTrimDb});
    }

    /// Process planar double stereo block through offline Render Chain.
    /// Note: DSP input/output trims remain in the signal path even when individual DSPs are disabled.
    void process(double* left, double* right, size_t frameCount) {
        if (!m_enabled || !left || !right || frameCount == 0) return;

        auto applyTrim = [](double* l, double* r, size_t n, double db) noexcept {
            const double g = pipeline::AudioNode::dbToLinearExact(db);
            if (g == 1.0) return;
            for (size_t i = 0; i < n; ++i) {
                l[i] *= g;
                r[i] *= g;
            }
        };

        // Upstream DSP Chain in Mastering Order:
        // 1. PEQ
        applyTrim(left, right, frameCount, m_inputTrimDb[0]);
        if (m_peq.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_peq.process(left[i], right[i]);
            }
        }
        applyTrim(left, right, frameCount, m_outputTrimDb[0]);

        // 2. GEQ
        applyTrim(left, right, frameCount, m_inputTrimDb[1]);
        if (m_geq.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_geq.process(left[i], right[i]);
            }
        }
        applyTrim(left, right, frameCount, m_outputTrimDb[1]);

        // 3. HPF / LPF (includes HPF in/out trims & LPF in/out trims)
        const bool fltActive = m_filter.isHpfEnabled() || m_filter.isLpfEnabled();
        const bool anyFilterTrim = (m_inputTrimDb[2] != 0.0 || m_outputTrimDb[2] != 0.0 ||
                                    m_inputTrimDb[3] != 0.0 || m_outputTrimDb[3] != 0.0);
        if (fltActive || anyFilterTrim) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_filter.processWithBypass(left[i], right[i], !fltActive);
            }
        }

        // 4. Compressor
        applyTrim(left, right, frameCount, m_inputTrimDb[4]);
        if (m_comp.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_comp.process(left[i], right[i]);
            }
        }
        applyTrim(left, right, frameCount, m_outputTrimDb[4]);

        // 5. Algorithmic Reverb
        applyTrim(left, right, frameCount, m_inputTrimDb[6]);
        if (m_reverb.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_reverb.process(left[i], right[i]);
            }
        }
        applyTrim(left, right, frameCount, m_outputTrimDb[6]);

        // 6. Peak Limiter (brickwall safeguard for render)
        applyTrim(left, right, frameCount, m_inputTrimDb[5]);
        if (m_limiter.isEnabled()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_limiter.process(left[i], right[i]);
            }
        }
        applyTrim(left, right, frameCount, m_outputTrimDb[5]);

        // NOTE: Strictly NO Listening Correction here!
        // Listening Correction is isolated to Listening Path only.
    }

private:
    double m_sampleRate = 48000.0;
    bool m_enabled = true;
    bool m_externalInsertWasActiveInLive = false;
    std::string m_externalInsertNodeName;
    std::array<double, 7> m_inputTrimDb{};
    std::array<double, 7> m_outputTrimDb{};

    ParametricEQ m_peq;
    GraphicEQ m_geq;
    HighLowPassFilter m_filter;
    Compressor m_comp;
    PeakLimiter m_limiter;
    AlgorithmicReverb m_reverb;
};

} // namespace desk32::engine::render
