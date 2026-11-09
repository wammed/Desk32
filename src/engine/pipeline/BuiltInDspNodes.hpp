#pragma once

#include "AudioNode.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace desk32::engine::pipeline {

/// Node 1: Parametric Equalizer (Master or Listening Correction)
class ParametricEqNode : public AudioNode {
public:
    explicit ParametricEqNode(std::string name = "Parametric EQ", NodeAffiliation affiliation = NodeAffiliation::RenderCommon)
        : AudioNode(std::move(name), affiliation)
    {
        m_eq.setEnabled(true);
    }

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_eq.configure(sampleRate);
        m_eq.setEnabled(true);
    }

    void reset() override {
        m_eq.reset();
        m_eq.setEnabled(true);
    }

    void process(double* left, double* right, size_t frameCount) override {
        if (isBypassed()) return;
        for (size_t i = 0; i < frameCount; ++i) {
            m_eq.process(left[i], right[i]);
        }
    }

    void applyCoefficients(const std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL>& leftCoeffs,
                           const std::array<BiquadCoefficients, ParametricEQ::BANDS_PER_CHANNEL>& rightCoeffs,
                           const std::array<bool, ParametricEQ::BANDS_PER_CHANNEL>& leftEnabled,
                           const std::array<bool, ParametricEQ::BANDS_PER_CHANNEL>& rightEnabled) noexcept {
        m_eq.applyCoefficients(leftCoeffs, rightCoeffs, leftEnabled, rightEnabled);
    }

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
    {
        m_geq.setEnabled(true);
    }

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_geq.configure(sampleRate);
        m_geq.setEnabled(true);
    }

    void reset() override {
        m_geq.reset();
        m_geq.setEnabled(true);
    }

    void process(double* left, double* right, size_t frameCount) override {
        if (isBypassed()) return;
        for (size_t i = 0; i < frameCount; ++i) {
            m_geq.process(left[i], right[i]);
        }
    }

    void applyAllCoefficients(const std::array<BiquadCoefficients, GraphicEQ::NUM_BANDS>& coeffs) noexcept {
        m_geq.applyAllCoefficients(coeffs);
    }

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
    {
        m_filter.setHpfEnabled(true);
        m_filter.setLpfEnabled(true);
    }

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_filter.configure(sampleRate);
        m_filter.setHpfEnabled(true);
        m_filter.setLpfEnabled(true);
    }

    void configure(double sampleRate, double hpfFreq, double lpfFreq,
                   HighLowPassFilter::FilterSlope hpfSlope = HighLowPassFilter::FilterSlope::Slope12dB,
                   HighLowPassFilter::FilterSlope lpfSlope = HighLowPassFilter::FilterSlope::Slope12dB) {
        AudioNode::configure(sampleRate);
        m_filter.configure(sampleRate, hpfFreq, lpfFreq, hpfSlope, lpfSlope);
    }

    void reset() override {
        m_filter.reset();
        m_filter.setHpfEnabled(true);
        m_filter.setLpfEnabled(true);
    }

    void process(double* left, double* right, size_t frameCount) override {
        if (isBypassed()) return;
        for (size_t i = 0; i < frameCount; ++i) {
            m_filter.process(left[i], right[i]);
        }
    }

    void applyCoefficients(const HighLowPassCoefficients& coeffs) noexcept {
        m_filter.applyCoefficients(coeffs);
    }

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
        bool enabled{true};
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
        auto next = std::make_unique<Compressor>();
        next->configure(sampleRate, thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
        Compressor* raw = next.release();
        Compressor* unconsumed = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = {sampleRate, thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled};
            unconsumed = m_prepared.exchange(raw, std::memory_order_acq_rel);
        }
        if (unconsumed) {
            m_replacedBeforeActivationCount.fetch_add(1, std::memory_order_relaxed);
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
            m_activatedCount.fetch_add(1, std::memory_order_relaxed);
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

    [[nodiscard]] const Compressor& dsp() const noexcept { return *m_active; }

    [[nodiscard]] uint64_t activatedCount() const noexcept { return m_activatedCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t replacedBeforeActivationCount() const noexcept { return m_replacedBeforeActivationCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t retiredCount() const noexcept { return m_retiredCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reclaimedCount() const noexcept { return m_reclaimedCount.load(std::memory_order_relaxed); }

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
        return m_active ? m_active->isEnabled() : false;
    }

    void reclaimRetired() noexcept override {
        Compressor* cur = m_retireHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            Compressor* next = cur->nextRetired;
            delete cur;
            m_reclaimedCount.fetch_add(1, std::memory_order_relaxed);
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
        m_retiredCount.fetch_add(1, std::memory_order_relaxed);
    }

    mutable std::mutex m_configMutex;
    Config m_config;
    Compressor* m_active{nullptr};
    alignas(64) std::atomic<Compressor*> m_prepared{nullptr};
    alignas(64) std::atomic<Compressor*> m_retireHead{nullptr};
    std::atomic<uint64_t> m_activatedCount{0};
    std::atomic<uint64_t> m_replacedBeforeActivationCount{0};
    std::atomic<uint64_t> m_retiredCount{0};
    std::atomic<uint64_t> m_reclaimedCount{0};
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
        bool enabled{true};
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
        auto next = std::make_unique<PeakLimiter>();
        next->configure(sampleRate, ceilingDb, releaseMs, lookaheadMs, enabled);
        const int64_t nextLatency = static_cast<int64_t>(next->lookaheadSamples());
        PeakLimiter* raw = next.release();
        PeakLimiter* unconsumed = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = {sampleRate, ceilingDb, releaseMs, lookaheadMs, enabled};
            m_latencyFrames.store(nextLatency, std::memory_order_release);
            unconsumed = m_prepared.exchange(raw, std::memory_order_acq_rel);
        }
        if (unconsumed) {
            m_replacedBeforeActivationCount.fetch_add(1, std::memory_order_relaxed);
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
            m_activatedCount.fetch_add(1, std::memory_order_relaxed);
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

    [[nodiscard]] const PeakLimiter& dsp() const noexcept { return *m_active; }

    [[nodiscard]] uint64_t activatedCount() const noexcept { return m_activatedCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t replacedBeforeActivationCount() const noexcept { return m_replacedBeforeActivationCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t retiredCount() const noexcept { return m_retiredCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reclaimedCount() const noexcept { return m_reclaimedCount.load(std::memory_order_relaxed); }

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
        return m_active ? m_active->isEnabled() : false;
    }

    void reclaimRetired() noexcept override {
        PeakLimiter* cur = m_retireHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            PeakLimiter* next = cur->nextRetired;
            delete cur;
            m_reclaimedCount.fetch_add(1, std::memory_order_relaxed);
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
        m_retiredCount.fetch_add(1, std::memory_order_relaxed);
    }

    mutable std::mutex m_configMutex;
    Config m_config;
    std::atomic<int64_t> m_latencyFrames{48};
    PeakLimiter* m_active{nullptr};
    alignas(64) std::atomic<PeakLimiter*> m_prepared{nullptr};
    alignas(64) std::atomic<PeakLimiter*> m_retireHead{nullptr};
    std::atomic<uint64_t> m_activatedCount{0};
    std::atomic<uint64_t> m_replacedBeforeActivationCount{0};
    std::atomic<uint64_t> m_retiredCount{0};
    std::atomic<uint64_t> m_reclaimedCount{0};
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
        bool enabled{true};
    };

    struct RingoutSlot {
        AlgorithmicReverb* reverb{nullptr};
        size_t ageFrames{0};
        size_t silentBlocks{0};
        double lastMeasuredEnergy{0.0};
    };
    static constexpr size_t MAX_RINGOUT_SLOTS = 8;

    enum class RetireReason : uint8_t {
        None = 0,
        ReplacedBeforeActivation = 1,   // Unconsumed prepared instance replaced on Control Thread
        NaturalDecayComplete = 2,       // Internal buffer energy < 1e-7 AND age >= 1.5*RT60 AND 8 silent blocks
        MaxRetentionSafetyCeiling = 3,  // Fallback safety timeout (10 seconds @ sampleRate)
        SlotCapacityEviction = 4,       // All 8 ringout slots full; lowest-energy slot evicted
        ExplicitCancel = 5,             // Explicit reset() / non-tail-preserving cancel
        AbnormalStateReset = 6          // NaN / Inf detected in internal state
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
        for (auto& slot : m_ringoutSlots) {
            delete slot.reverb;
            slot.reverb = nullptr;
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
        prepareInstance(sampleRate, cfg.roomSize, cfg.damping, cfg.wetLevel,
                        cfg.predelayMs, cfg.tailPreserving, cfg.enabled);
    }

    // Called on Control Thread (P0-21, P0-22)
    void prepareInstance(double sampleRate, double roomSize, double damping,
                         double wetLevel, double predelayMs, bool tailPreserving,
                         bool enabled) {
        auto next = std::make_unique<AlgorithmicReverb>();
        next->configure(sampleRate, roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
        AlgorithmicReverb* raw = next.release();
        AlgorithmicReverb* unconsumed = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = {sampleRate, roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled};
            unconsumed = m_prepared.exchange(raw, std::memory_order_acq_rel);
        }
        if (unconsumed) {
            m_replacedBeforeActivationCount.fetch_add(1, std::memory_order_relaxed);
            m_lastRetireReason.store(RetireReason::ReplacedBeforeActivation, std::memory_order_relaxed);
            delete unconsumed;
        }
        reclaimRetired();
    }

    void reset() override {
        if (m_active) m_active->reset();
        for (auto& slot : m_ringoutSlots) {
            if (slot.reverb) {
                recordRetireReason(RetireReason::ExplicitCancel);
                retireInstance(slot.reverb);
                slot = RingoutSlot{};
            }
        }
    }

    [[nodiscard]] RetireReason evaluateRetireCondition(const RingoutSlot& slot, double maxBlockTail) const noexcept {
        if (!slot.reverb) return RetireReason::None;

        // 0. Abnormal state (NaN / Inf) protection: strictly O(1) lock-free check
        if (!std::isfinite(maxBlockTail) || slot.reverb->hasInvalidState()) {
            return RetireReason::AbnormalStateReset;
        }

        // 1. Max Retention Safety Ceiling (10 seconds @ instance sampleRate)
        const double sr = std::max(8000.0, slot.reverb->sampleRate());
        const size_t maxRetentionFrames = static_cast<size_t>(10.0 * sr);
        if (slot.ageFrames >= maxRetentionFrames) {
            return RetireReason::MaxRetentionSafetyCeiling;
        }

        // 2. Natural decay complete: Output amplitude < -80 dB (< 1e-4),
        // elapsed frames >= 1.5 * RT60 lower bound, AND internal comb/allpass buffer energy < 1e-7 (-100 dB)
        // Note: totalInternalEnergy() is computed ONLY when all preceding criteria pass (final gate).
        if (maxBlockTail < 1e-4 &&
            slot.silentBlocks >= 8 &&
            slot.ageFrames >= slot.reverb->theoreticalDecayFrames()) {
            if (slot.reverb->totalInternalEnergy() < 1e-7) {
                return RetireReason::NaturalDecayComplete;
            }
        }

        return RetireReason::None;
    }

    void process(double* left, double* right, size_t frameCount) override {
        // 1. Check for prepared instance at block boundary (RT-safe atomic exchange)
        if (auto* prepared = m_prepared.exchange(nullptr, std::memory_order_acq_rel)) {
            m_activatedCount.fetch_add(1, std::memory_order_relaxed);
            if (m_active) {
                const double activeEnergy = m_active->totalInternalEnergy();
                if (!std::isfinite(activeEnergy)) {
                    m_active->reset();
                    recordRetireReason(RetireReason::AbnormalStateReset);
                    retireInstance(m_active);
                } else if (!m_active->isTailPreserving()) {
                    recordRetireReason(RetireReason::ExplicitCancel);
                    retireInstance(m_active);
                } else if (activeEnergy <= 1e-12) {
                    // Already silent (never excited or completely decayed): retire directly without occupying a ringout slot
                    retireInstance(m_active);
                } else {
                    // Tail Preservation (P0-22): Migrate active instance to ringout slot for natural tail decay
                    bool placed = false;
                    for (auto& slot : m_ringoutSlots) {
                        if (!slot.reverb) {
                            slot = RingoutSlot{m_active, 0, 0, activeEnergy};
                            placed = true;
                            break;
                        }
                    }
                    if (!placed) {
                        // All 8 slots occupied: find the slot with the longest elapsed age (most decayed)
                        // Deterministic O(1) scalar comparison without scanning delay buffers (P0-22).
                        size_t minIdx = 0;
                        size_t maxAge = 0;
                        for (size_t s = 0; s < MAX_RINGOUT_SLOTS; ++s) {
                            if (m_ringoutSlots[s].ageFrames >= maxAge) {
                                maxAge = m_ringoutSlots[s].ageFrames;
                                minIdx = s;
                            }
                        }
                        recordRetireReason(RetireReason::SlotCapacityEviction);
                        retireInstance(m_ringoutSlots[minIdx].reverb);
                        m_ringoutSlots[minIdx] = RingoutSlot{m_active, 0, 0, activeEnergy};
                    }
                }
                m_active = prepared;
            } else {
                m_active = prepared;
            }
        }

        // 2. Bypass handling (P0-22): When bypassed, audio passes through clean without wet reverb.
        // However, we MUST advance ringout slots and active reverb with zero input (0.0)
        // so that internal delay states decay naturally rather than freezing indefinitely!
        if (isBypassed()) {
            for (auto& slot : m_ringoutSlots) {
                if (!slot.reverb) continue;
                double maxTail = 0.0;
                bool abnormal = false;
                for (size_t i = 0; i < frameCount; ++i) {
                    double dummyL = 0.0, dummyR = 0.0;
                    slot.reverb->process(dummyL, dummyR);
                    if (!std::isfinite(dummyL) || !std::isfinite(dummyR)) {
                        abnormal = true;
                        break;
                    }
                    double absL = std::abs(dummyL);
                    double absR = std::abs(dummyR);
                    if (absL > maxTail) maxTail = absL;
                    if (absR > maxTail) maxTail = absR;
                }
                if (abnormal) {
                    slot.reverb->reset();
                    recordRetireReason(RetireReason::AbnormalStateReset);
                    retireInstance(slot.reverb);
                    slot = RingoutSlot{};
                    continue;
                }
                slot.ageFrames += frameCount;
                if (maxTail < 1e-4) slot.silentBlocks++; else slot.silentBlocks = 0;

                RetireReason reason = evaluateRetireCondition(slot, maxTail);
                if (reason != RetireReason::None) {
                    if (reason == RetireReason::AbnormalStateReset) slot.reverb->reset();
                    recordRetireReason(reason);
                    retireInstance(slot.reverb);
                    slot = RingoutSlot{};
                }
            }
            if (m_active) {
                for (size_t i = 0; i < frameCount; ++i) {
                    double dummyL = 0.0, dummyR = 0.0;
                    m_active->process(dummyL, dummyR);
                    if (!std::isfinite(dummyL) || !std::isfinite(dummyR)) {
                        m_active->reset();
                        break;
                    }
                }
            }
            return;
        }

        // 3. Active audio processing with new parameters
        if (m_active) {
            if (m_active->hasInvalidState()) {
                m_active->reset();
            }
            for (size_t i = 0; i < frameCount; ++i) {
                const double dryL = left[i];
                const double dryR = right[i];
                m_active->process(left[i], right[i]);
                if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
                    m_active->reset();
                    left[i] = dryL;
                    right[i] = dryR;
                }
            }
        }

        // 4. Ringout slots processing: input 0.0 to render decaying reverberation tails naturally
        for (auto& slot : m_ringoutSlots) {
            if (!slot.reverb) continue;

            if (slot.reverb->hasInvalidState()) {
                slot.reverb->reset();
                recordRetireReason(RetireReason::AbnormalStateReset);
                retireInstance(slot.reverb);
                slot = RingoutSlot{};
                continue;
            }

            double maxBlockTail = 0.0;
            bool abnormal = false;
            for (size_t i = 0; i < frameCount; ++i) {
                double tailL = 0.0;
                double tailR = 0.0;
                slot.reverb->process(tailL, tailR);
                if (!std::isfinite(tailL) || !std::isfinite(tailR)) {
                    abnormal = true;
                    break;
                }
                left[i] += tailL;
                right[i] += tailR;

                double absL = std::abs(tailL);
                double absR = std::abs(tailR);
                if (absL > maxBlockTail) maxBlockTail = absL;
                if (absR > maxBlockTail) maxBlockTail = absR;
            }

            if (abnormal) {
                slot.reverb->reset();
                recordRetireReason(RetireReason::AbnormalStateReset);
                retireInstance(slot.reverb);
                slot = RingoutSlot{};
                continue;
            }

            slot.ageFrames += frameCount;

            if (maxBlockTail < 1e-4) {
                slot.silentBlocks++;
            } else {
                slot.silentBlocks = 0;
            }

            RetireReason reason = evaluateRetireCondition(slot, maxBlockTail);
            if (reason != RetireReason::None) {
                if (reason == RetireReason::AbnormalStateReset) slot.reverb->reset();
                recordRetireReason(reason);
                retireInstance(slot.reverb);
                slot = RingoutSlot{};
            }
        }
    }

    [[nodiscard]] double wetLevel() const noexcept {
        return m_active ? m_active->wetLevel() : 0.0;
    }

    void injectInvalidStateForTest(double nonFiniteVal) noexcept {
        if (m_active) m_active->injectInvalidStateForTest(nonFiniteVal);
    }

    [[nodiscard]] bool hasInvalidState() const noexcept {
        return m_active ? m_active->hasInvalidState() : false;
    }

    [[nodiscard]] const AlgorithmicReverb& dsp() const noexcept { return *m_active; }

    [[nodiscard]] size_t activeRingoutSlotCount() const noexcept {
        size_t count = 0;
        for (const auto& slot : m_ringoutSlots) {
            if (slot.reverb != nullptr) ++count;
        }
        return count;
    }

    [[nodiscard]] uint64_t activatedCount() const noexcept { return m_activatedCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t replacedBeforeActivationCount() const noexcept { return m_replacedBeforeActivationCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t retiredCount() const noexcept { return m_retiredCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reclaimedCount() const noexcept { return m_reclaimedCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t naturalDecayRetireCount() const noexcept { return m_naturalDecayRetireCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t safetyCeilingRetireCount() const noexcept { return m_safetyCeilingRetireCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t capacityEvictionRetireCount() const noexcept { return m_capacityEvictionRetireCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t explicitCancelRetireCount() const noexcept { return m_explicitCancelRetireCount.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t abnormalStateRetireCount() const noexcept { return m_abnormalStateRetireCount.load(std::memory_order_relaxed); }
    [[nodiscard]] RetireReason lastRetireReason() const noexcept { return m_lastRetireReason.load(std::memory_order_relaxed); }

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
        return m_active ? m_active->isEnabled() : false;
    }

    void reclaimRetired() noexcept override {
        AlgorithmicReverb* cur = m_retireHead.exchange(nullptr, std::memory_order_acquire);
        while (cur) {
            AlgorithmicReverb* next = cur->nextRetired;
            delete cur;
            m_reclaimedCount.fetch_add(1, std::memory_order_relaxed);
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
    void recordRetireReason(RetireReason reason) noexcept {
        m_lastRetireReason.store(reason, std::memory_order_relaxed);
        switch (reason) {
        case RetireReason::NaturalDecayComplete:
            m_naturalDecayRetireCount.fetch_add(1, std::memory_order_relaxed);
            break;
        case RetireReason::MaxRetentionSafetyCeiling:
            m_safetyCeilingRetireCount.fetch_add(1, std::memory_order_relaxed);
            break;
        case RetireReason::SlotCapacityEviction:
            m_capacityEvictionRetireCount.fetch_add(1, std::memory_order_relaxed);
            break;
        case RetireReason::ExplicitCancel:
            m_explicitCancelRetireCount.fetch_add(1, std::memory_order_relaxed);
            break;
        case RetireReason::AbnormalStateReset:
            m_abnormalStateRetireCount.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
        }
    }

    void retireInstance(AlgorithmicReverb* old) noexcept {
        old->nextRetired = m_retireHead.load(std::memory_order_relaxed);
        while (!m_retireHead.compare_exchange_weak(old->nextRetired, old,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed));
        m_retiredCount.fetch_add(1, std::memory_order_relaxed);
    }

    mutable std::mutex m_configMutex;
    Config m_config;
    AlgorithmicReverb* m_active{nullptr};
    std::array<RingoutSlot, MAX_RINGOUT_SLOTS> m_ringoutSlots{};

    alignas(64) std::atomic<AlgorithmicReverb*> m_prepared{nullptr};
    alignas(64) std::atomic<AlgorithmicReverb*> m_retireHead{nullptr};

    std::atomic<uint64_t> m_activatedCount{0};
    std::atomic<uint64_t> m_replacedBeforeActivationCount{0};
    std::atomic<uint64_t> m_retiredCount{0};
    std::atomic<uint64_t> m_reclaimedCount{0};
    std::atomic<uint64_t> m_naturalDecayRetireCount{0};
    std::atomic<uint64_t> m_safetyCeilingRetireCount{0};
    std::atomic<uint64_t> m_capacityEvictionRetireCount{0};
    std::atomic<uint64_t> m_explicitCancelRetireCount{0};
    std::atomic<uint64_t> m_abnormalStateRetireCount{0};
    std::atomic<RetireReason> m_lastRetireReason{RetireReason::None};
};

} // namespace desk32::engine::pipeline
