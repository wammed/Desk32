#pragma once

#include "AudioNode.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include <memory>
#include <mutex>

namespace desk32::engine::pipeline {

/// Node 1: Parametric Equalizer (Master or Listening Correction)
class ParametricEqNode : public AudioNode {
public:
    explicit ParametricEqNode(std::string name = "Parametric EQ", NodeAffiliation affiliation = NodeAffiliation::RenderCommon)
        : AudioNode(std::move(name), affiliation)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_eq.configure(sampleRate);
    }

    void reset() override {
        m_eq.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_eq.process(left[i], right[i]);
        }
    }

    [[nodiscard]] ParametricEQ& dsp() noexcept { return m_eq; }
    [[nodiscard]] const ParametricEQ& dsp() const noexcept { return m_eq; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<ParametricEqNode>(m_name, m_affiliation);
        node->setBypassed(isBypassed());
        node->m_eq = m_eq;
        return node;
    }

private:
    ParametricEQ m_eq;
};

/// Node 2: 32-band Graphic Equalizer (Console-style GEQ)
class GraphicEqNode : public AudioNode {
public:
    explicit GraphicEqNode(std::string name = "Graphic EQ")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_geq.configure(sampleRate);
    }

    void reset() override {
        m_geq.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_geq.process(left[i], right[i]);
        }
    }

    [[nodiscard]] GraphicEQ& dsp() noexcept { return m_geq; }
    [[nodiscard]] const GraphicEQ& dsp() const noexcept { return m_geq; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<GraphicEqNode>(m_name);
        node->setBypassed(isBypassed());
        node->m_geq = m_geq;
        return node;
    }

private:
    GraphicEQ m_geq;
};

/// Node 3: High-Pass / Low-Pass Filter
class HighLowPassNode : public AudioNode {
public:
    explicit HighLowPassNode(std::string name = "HPF / LPF")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_filter.configure(sampleRate);
    }

    void reset() override {
        m_filter.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_filter.process(left[i], right[i]);
        }
    }

    [[nodiscard]] HighLowPassFilter& dsp() noexcept { return m_filter; }
    [[nodiscard]] const HighLowPassFilter& dsp() const noexcept { return m_filter; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<HighLowPassNode>(m_name);
        node->setBypassed(isBypassed());
        node->m_filter = m_filter;
        return node;
    }

private:
    HighLowPassFilter m_filter;
};

/// Node 4: Stereo Compressor (Section 5.4, P0-21)
/// Prepared Instance Exchange:
/// - Control Thread builds and pre-configures new Compressor instances.
/// - Audio Thread swaps prepared instance at block boundary, preserving envelope follower state.
/// - Old instances are retired to an intrusive lock-free stack and reclaimed strictly outside Audio Thread.
class CompressorNode : public AudioNode {
public:
    struct Config {
        double sampleRate{48000.0};
        double thresholdDb{-12.0};
        double ratio{4.0};
        double attackMs{10.0};
        double releaseMs{100.0};
        double makeupDb{0.0};
        double kneeDb{4.0};
        bool enabled{false};
    };

    explicit CompressorNode(std::string name = "Compressor")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {
        auto comp = std::make_unique<Compressor>();
        comp->configure(48000.0);
        m_active = comp.release();
    }

    ~CompressorNode() override {
        reclaimRetired();
        if (auto* unconsumed = m_prepared.exchange(nullptr)) {
            delete unconsumed;
        }
        delete m_active;
    }

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config.sampleRate = sampleRate;
            cfg = m_config;
        }
        prepareInstance(sampleRate, cfg.thresholdDb, cfg.ratio, cfg.attackMs,
                        cfg.releaseMs, cfg.makeupDb, cfg.kneeDb, cfg.enabled);
    }

    // Called on Control Thread (P0-21)
    void prepareInstance(double sampleRate, double thresholdDb, double ratio,
                         double attackMs, double releaseMs, double makeupDb,
                         double kneeDb, bool enabled) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = {sampleRate, thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled};
        }
        auto next = std::make_unique<Compressor>();
        next->configure(sampleRate, thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
        Compressor* raw = next.release();
        Compressor* unconsumed = m_prepared.exchange(raw, std::memory_order_acq_rel);
        if (unconsumed) {
            delete unconsumed;
        }
        reclaimRetired();
    }

    void reset() override {
        if (m_active) m_active->reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        // 1. Check for prepared instance at block boundary
        if (auto* prepared = m_prepared.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_active) {
                prepared->inheritStateFrom(*m_active);
                retireInstance(m_active);
            }
            m_active = prepared;
        }

        // 2. Audio processing with active instance
        if (m_active && !isBypassed()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_active->process(left[i], right[i]);
            }
        }
    }

    [[nodiscard]] double gainReductionDb() const noexcept {
        return m_active ? m_active->gainReductionDb() : 0.0;
    }

    [[nodiscard]] Compressor& dsp() noexcept { return *m_active; }
    [[nodiscard]] const Compressor& dsp() const noexcept { return *m_active; }

    [[nodiscard]] double thresholdDb() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.thresholdDb;
    }
    [[nodiscard]] double ratio() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.ratio;
    }
    [[nodiscard]] double attackMs() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.attackMs;
    }
    [[nodiscard]] double releaseMs() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.releaseMs;
    }
    [[nodiscard]] double makeupGainDb() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.makeupDb;
    }
    [[nodiscard]] double kneeDb() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.kneeDb;
    }
    [[nodiscard]] bool isEnabled() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.enabled;
    }

    void reclaimRetired() noexcept override {
        Compressor* cur = m_retireHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            Compressor* next = cur->nextRetired;
            delete cur;
            cur = next;
        }
    }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<CompressorNode>(m_name);
        node->setBypassed(isBypassed());
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            cfg = m_config;
        }
        node->prepareInstance(cfg.sampleRate, cfg.thresholdDb, cfg.ratio,
                              cfg.attackMs, cfg.releaseMs, cfg.makeupDb,
                              cfg.kneeDb, cfg.enabled);
        return node;
    }

private:
    void retireInstance(Compressor* old) noexcept {
        old->nextRetired = m_retireHead.load(std::memory_order_relaxed);
        while (!m_retireHead.compare_exchange_weak(old->nextRetired, old,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
    }

    mutable std::mutex m_configMutex;
    Config m_config;
    Compressor* m_active{nullptr};
    alignas(64) std::atomic<Compressor*> m_prepared{nullptr};
    alignas(64) std::atomic<Compressor*> m_retireHead{nullptr};
};

/// Node 5: Sample Peak Limiter (Section 5.5, P0-21, P0-23)
/// Prepared Instance Exchange with State & Lookahead Preservation:
/// - Control Thread builds and sizes lookahead ring buffers in new Limiter instances.
/// - Audio Thread swaps prepared instance at block boundary, seamlessly transferring existing
///   lookahead samples and current gain envelope to eliminate transients, clicks, or duplicate frames.
/// - Old instances are retired to an intrusive lock-free stack and reclaimed outside Audio Thread.
class PeakLimiterNode : public AudioNode {
public:
    struct Config {
        double sampleRate{48000.0};
        double ceilingDb{-0.1};
        double releaseMs{50.0};
        double lookaheadMs{1.0};
        bool enabled{false};
    };

    explicit PeakLimiterNode(std::string name = "Peak Limiter")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {
        auto lim = std::make_unique<PeakLimiter>();
        lim->configure(48000.0);
        m_active = lim.release();
    }

    ~PeakLimiterNode() override {
        reclaimRetired();
        if (auto* unconsumed = m_prepared.exchange(nullptr)) {
            delete unconsumed;
        }
        delete m_active;
    }

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config.sampleRate = sampleRate;
            cfg = m_config;
        }
        prepareInstance(sampleRate, cfg.ceilingDb, cfg.releaseMs, cfg.lookaheadMs, cfg.enabled);
    }

    // Called on Control Thread (P0-21, P0-23)
    void prepareInstance(double sampleRate, double ceilingDb, double releaseMs,
                         double lookaheadMs, bool enabled) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = {sampleRate, ceilingDb, releaseMs, lookaheadMs, enabled};
        }
        auto next = std::make_unique<PeakLimiter>();
        next->configure(sampleRate, ceilingDb, releaseMs, lookaheadMs, enabled);
        m_latencyFrames.store(static_cast<int64_t>(next->lookaheadSamples()), std::memory_order_release);
        PeakLimiter* raw = next.release();
        PeakLimiter* unconsumed = m_prepared.exchange(raw, std::memory_order_acq_rel);
        if (unconsumed) {
            delete unconsumed;
        }
        reclaimRetired();
    }

    [[nodiscard]] int64_t latencyFrames() const noexcept override {
        return m_latencyFrames.load(std::memory_order_acquire);
    }

    void reset() override {
        if (m_active) m_active->reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        // 1. Check for prepared instance at block boundary
        if (auto* prepared = m_prepared.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_active) {
                // State transition (P0-23): inherit gain and lookahead delay buffer
                prepared->inheritStateFrom(*m_active);
                retireInstance(m_active);
            }
            m_active = prepared;
        }

        // 2. Audio processing with active instance
        if (m_active && !isBypassed()) {
            for (size_t i = 0; i < frameCount; ++i) {
                m_active->process(left[i], right[i]);
            }
        }
    }

    [[nodiscard]] double gainReductionDb() const noexcept {
        return m_active ? m_active->gainReductionDb() : 0.0;
    }

    [[nodiscard]] PeakLimiter& dsp() noexcept { return *m_active; }
    [[nodiscard]] const PeakLimiter& dsp() const noexcept { return *m_active; }

    [[nodiscard]] double ceilingDb() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.ceilingDb;
    }
    [[nodiscard]] double releaseMs() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.releaseMs;
    }
    [[nodiscard]] double lookaheadMs() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.lookaheadMs;
    }
    [[nodiscard]] bool isEnabled() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.enabled;
    }

    void reclaimRetired() noexcept override {
        PeakLimiter* cur = m_retireHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            PeakLimiter* next = cur->nextRetired;
            delete cur;
            cur = next;
        }
    }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<PeakLimiterNode>(m_name);
        node->setBypassed(isBypassed());
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            cfg = m_config;
        }
        node->prepareInstance(cfg.sampleRate, cfg.ceilingDb, cfg.releaseMs,
                              cfg.lookaheadMs, cfg.enabled);
        return node;
    }

private:
    void retireInstance(PeakLimiter* old) noexcept {
        old->nextRetired = m_retireHead.load(std::memory_order_relaxed);
        while (!m_retireHead.compare_exchange_weak(old->nextRetired, old,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
    }

    mutable std::mutex m_configMutex;
    Config m_config;
    std::atomic<int64_t> m_latencyFrames{48};
    PeakLimiter* m_active{nullptr};
    alignas(64) std::atomic<PeakLimiter*> m_prepared{nullptr};
    alignas(64) std::atomic<PeakLimiter*> m_retireHead{nullptr};
};

/// Node 6: Algorithmic Reverb (Section 5.6, P0-21, P0-22)
/// Prepared Instance Exchange with Tail-Preserving Crossfade:
/// - Control Thread builds and pre-configures comb and all-pass delay buffers.
/// - Audio Thread swaps prepared instance at block boundary.
/// - Tail Preservation (P0-22): An active reverb undergoing a parameter update enters a smooth
///   CROSSFADE_FRAMES (512-frame, ~10.6ms) crossfade with the outgoing instance. The decaying reverberation
///   tail is naturally preserved without abrupt cutoff, clicks, or pops.
/// - Finished fading instances are retired to an intrusive lock-free stack and reclaimed outside Audio Thread.
class ReverbNode : public AudioNode {
public:
    static constexpr size_t CROSSFADE_FRAMES = 512;

    struct Config {
        double sampleRate{48000.0};
        double roomSize{0.7};
        double damping{0.3};
        double wetLevel{0.2};
        double predelayMs{0.0};
        bool tailPreserving{true};
        bool enabled{false};
    };

    explicit ReverbNode(std::string name = "Reverb")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {
        auto rev = std::make_unique<AlgorithmicReverb>();
        rev->configure(48000.0);
        m_active = rev.release();
    }

    ~ReverbNode() override {
        reclaimRetired();
        if (auto* unconsumed = m_prepared.exchange(nullptr)) {
            delete unconsumed;
        }
        delete m_fade;
        delete m_active;
    }

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config.sampleRate = sampleRate;
            cfg = m_config;
        }
        prepareInstance(sampleRate, cfg.roomSize, cfg.damping, cfg.wetLevel,
                        cfg.predelayMs, cfg.tailPreserving, cfg.enabled);
    }

    // Called on Control Thread (P0-21, P0-22)
    void prepareInstance(double sampleRate, double roomSize, double damping,
                         double wetLevel, double predelayMs, bool tailPreserving,
                         bool enabled) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = {sampleRate, roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled};
        }
        auto next = std::make_unique<AlgorithmicReverb>();
        next->configure(sampleRate, roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
        AlgorithmicReverb* raw = next.release();
        AlgorithmicReverb* unconsumed = m_prepared.exchange(raw, std::memory_order_acq_rel);
        if (unconsumed) {
            delete unconsumed;
        }
        reclaimRetired();
    }

    void reset() override {
        if (m_active) m_active->reset();
        if (m_fade) m_fade->reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        // 1. Check for prepared instance at block boundary
        if (auto* prepared = m_prepared.exchange(nullptr, std::memory_order_acq_rel)) {
            if (m_active) {
                // Tail Preservation (P0-22, v1.6): No buffer copying on Audio Thread!
                // Instead, m_fade naturally continues rendering existing reverberation tail,
                // while m_active renders incoming audio with new parameters, crossfaded smoothly over CROSSFADE_FRAMES.
                if (m_fade) {
                    retireInstance(m_fade);
                }
                m_fade = m_active;
                m_active = prepared;
                m_crossfadeRemaining = CROSSFADE_FRAMES;
            } else {
                m_active = prepared;
            }
        }

        if (isBypassed()) return;

        // 2. Audio processing with Tail-Preserving Crossfade (P0-22)
        if (m_crossfadeRemaining > 0 && m_fade) {
            for (size_t i = 0; i < frameCount; ++i) {
                double l_act = left[i];
                double r_act = right[i];
                if (m_active) m_active->process(l_act, r_act);

                double l_fade = left[i];
                double r_fade = right[i];
                m_fade->process(l_fade, r_fade);

                double activeWeight = 1.0 - static_cast<double>(m_crossfadeRemaining) / static_cast<double>(CROSSFADE_FRAMES);
                double fadeWeight = 1.0 - activeWeight;

                left[i] = activeWeight * l_act + fadeWeight * l_fade;
                right[i] = activeWeight * r_act + fadeWeight * r_fade;

                if (--m_crossfadeRemaining == 0) {
                    retireInstance(m_fade);
                    m_fade = nullptr;
                    // Finish remaining frames in this block with m_active only
                    for (size_t j = i + 1; j < frameCount; ++j) {
                        if (m_active) m_active->process(left[j], right[j]);
                    }
                    break;
                }
            }
        } else {
            if (m_active) {
                for (size_t i = 0; i < frameCount; ++i) {
                    m_active->process(left[i], right[i]);
                }
            }
        }
    }

    [[nodiscard]] double wetLevel() const noexcept {
        return m_active ? m_active->wetLevel() : 0.0;
    }

    [[nodiscard]] AlgorithmicReverb& dsp() noexcept { return *m_active; }
    [[nodiscard]] const AlgorithmicReverb& dsp() const noexcept { return *m_active; }

    [[nodiscard]] double roomSize() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.roomSize;
    }
    [[nodiscard]] double damping() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.damping;
    }
    [[nodiscard]] double predelayMs() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.predelayMs;
    }
    [[nodiscard]] bool isTailPreserving() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.tailPreserving;
    }
    [[nodiscard]] bool isEnabled() const noexcept {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config.enabled;
    }

    void reclaimRetired() noexcept override {
        AlgorithmicReverb* cur = m_retireHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            AlgorithmicReverb* next = cur->nextRetired;
            delete cur;
            cur = next;
        }
    }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<ReverbNode>(m_name);
        node->setBypassed(isBypassed());
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            cfg = m_config;
        }
        node->prepareInstance(cfg.sampleRate, cfg.roomSize, cfg.damping,
                              cfg.wetLevel, cfg.predelayMs, cfg.tailPreserving,
                              cfg.enabled);
        return node;
    }

private:
    void retireInstance(AlgorithmicReverb* old) noexcept {
        old->nextRetired = m_retireHead.load(std::memory_order_relaxed);
        while (!m_retireHead.compare_exchange_weak(old->nextRetired, old,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
    }

    mutable std::mutex m_configMutex;
    Config m_config;
    AlgorithmicReverb* m_active{nullptr};
    AlgorithmicReverb* m_fade{nullptr};
    size_t m_crossfadeRemaining{0};

    alignas(64) std::atomic<AlgorithmicReverb*> m_prepared{nullptr};
    alignas(64) std::atomic<AlgorithmicReverb*> m_retireHead{nullptr};
};

} // namespace desk32::engine::pipeline
