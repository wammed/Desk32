#pragma once

#include "AudioChain.hpp"
#include "BuiltInDspNodes.hpp"
#include "engine/dsp/Resampler.hpp"
#include "engine/dsp/ParameterCommandQueue.hpp"
#include "engine/dsp/DspParameterMailbox.hpp"
#include "engine/dsp/ParameterSmoother.hpp"
#include "engine/format/FormatConverter.hpp"
#include "engine/memory/EpochReclaimer.hpp"
#include "engine/routing/OutputPathRouter.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

namespace desk32::engine::pipeline {

using routing::OutputPathMode;
using format::SampleFormat;
using format::FormatConverter;

/// DSP Telemetry Snapshot published lock-free from Audio Thread to UI Thread (P0-26)
struct DspTelemetry {
    double compressorGainReductionDb = 0.0;
    double limiterGainReductionDb = 0.0;
    int64_t limiterLatencyFrames = 0;
    double reverbWetLevel = 0.0;
    bool compressorEnabled = false;
    bool limiterEnabled = false;
    bool reverbEnabled = false;
    double graphSampleRate = 48000.0;
    uint64_t processedFrameCount = 0;
    double preDspPeakL = 0.0;
    double preDspPeakR = 0.0;
    double preDspRmsL = 0.0;
    double preDspRmsR = 0.0;
    double masterPeakL = 0.0;
    double masterPeakR = 0.0;
    double renderTapPeakL = 0.0;
    double renderTapPeakR = 0.0;
    double masterRmsL = 0.0;
    double masterRmsR = 0.0;
    bool masterClipped = false;
    bool level1SamplesIdentical = false;
    bool level1OutputPcmLossless = false;
    bool level1DitherActive = false;
    bool level1DirectBypassActive = false;
    double level1MaxAbsDiff = 0.0;
    double level1PostPcmMaxAbsDiff = 0.0;
    uint64_t level1InputHash = 0;
    uint64_t level1OutputHash = 0;
    uint64_t level1VerifiedBlocks = 0;
    uint64_t level1TotalBlocks = 0;
};

class AudioPipeline {
public:
    static constexpr size_t MAX_COMMANDS_PER_BLOCK = 64; ///< Upper bound on discrete commands drained per Audio Thread block (P0-29)
    static constexpr size_t MAX_LEVEL1_FRAMES = 4096;

    explicit AudioPipeline(double graphSampleRate = 48000.0)
        : m_graphSampleRate(graphSampleRate)
    {
        if (graphSampleRate != 48000.0) {
            m_paramMailbox.setSampleRate(graphSampleRate);
        }
        m_listeningVolSmoother.configure(graphSampleRate, 10.0);
        m_renderMonVolSmoother.configure(graphSampleRate, 10.0);
        m_levelMatchSmoother.configure(graphSampleRate, 10.0);
        m_cachedTelemetry.graphSampleRate = graphSampleRate;
        for (auto& buf : m_telemetryBuffers) {
            buf.graphSampleRate = graphSampleRate;
        }

        // Initialize default empty chains
        auto masterChain = std::make_unique<AudioChain>("Default Master Chain", ChainPurpose::MasteringChain);
        masterChain->configure(m_graphSampleRate);
        m_masterReclaimer.swapChain(std::move(masterChain));

        auto listeningChain = std::make_unique<AudioChain>("Default Listening Chain", ChainPurpose::ListeningCorrectionChain);
        listeningChain->configure(m_graphSampleRate);
        m_listeningReclaimer.swapChain(std::move(listeningChain));
    }

    ~AudioPipeline() {
        m_masterReclaimer.drainAll();
        m_listeningReclaimer.drainAll();
    }

    [[nodiscard]] double graphSampleRate() const noexcept { return m_graphSampleRate.load(std::memory_order_relaxed); }

    /// Reconfigure graph sample rate safely without mutating live running chains in-place.
    /// Builds fresh chains with deep-cloned nodes configured at the new rate, atomically swaps via EpochReclaimer (P0-08, P0-13).
    void setGraphSampleRate(double sampleRate) {
        m_graphSampleRate.store(sampleRate, std::memory_order_relaxed);
        m_paramMailbox.setSampleRate(sampleRate);
        m_listeningVolSmoother.configure(sampleRate, 10.0);
        m_renderMonVolSmoother.configure(sampleRate, 10.0);
        m_levelMatchSmoother.configure(sampleRate, 10.0);

        // Safely deep-clone Master Chain under ReadGuard protection, then configure and swap
        std::unique_ptr<AudioChain> newMaster;
        {
            auto guard = m_masterReclaimer.enterRead();
            if (guard) {
                newMaster = guard->clone();
            }
        }
        if (newMaster) {
            newMaster->configure(sampleRate);
            m_masterReclaimer.swapChain(std::move(newMaster));
        }

        // Safely deep-clone Listening Chain under ReadGuard protection, then configure and swap
        std::unique_ptr<AudioChain> newListening;
        {
            auto guard = m_listeningReclaimer.enterRead();
            if (guard) {
                newListening = guard->clone();
            }
        }
        if (newListening) {
            newListening->configure(sampleRate);
            m_listeningReclaimer.swapChain(std::move(newListening));
        }
    }

    [[nodiscard]] OutputPathMode outputMode() const noexcept { return m_outputMode.load(); }
    void setOutputMode(OutputPathMode mode) noexcept { m_outputMode.store(mode); }

    [[nodiscard]] double listeningVolume() const noexcept { return m_listeningVolume.load(); }
    void setListeningVolume(double vol) noexcept { m_listeningVolume.store(std::clamp(vol, 0.0, 2.0)); }

    [[nodiscard]] bool listeningMuted() const noexcept { return m_listeningMuted.load(std::memory_order_relaxed); }
    void setListeningMuted(bool muted) noexcept { m_listeningMuted.store(muted, std::memory_order_relaxed); }

    [[nodiscard]] double renderMonitorVolume() const noexcept { return m_renderMonitorVolume.load(); }
    void setRenderMonitorVolume(double vol) noexcept { m_renderMonitorVolume.store(std::clamp(vol, 0.0, 2.0)); }

    [[nodiscard]] bool renderMonitorMuted() const noexcept { return m_renderMonitorMuted.load(std::memory_order_relaxed); }
    void setRenderMonitorMuted(bool muted) noexcept { m_renderMonitorMuted.store(muted, std::memory_order_relaxed); }

    /// Internal temporary monitor mute used during hardware latency calibration (smooth 10ms ramp, preserves user mute state).
    [[nodiscard]] bool calibrationMonitorMute() const noexcept { return m_calibrationMonitorMute.load(std::memory_order_acquire); }
    void setCalibrationMonitorMute(bool muted) noexcept { m_calibrationMonitorMute.store(muted, std::memory_order_release); }

    [[nodiscard]] bool liveDitherEnabled() const noexcept { return m_liveDitherEnabled.load(std::memory_order_relaxed); }
    void setLiveDitherEnabled(bool enabled) noexcept { m_liveDitherEnabled.store(enabled, std::memory_order_relaxed); }

    [[nodiscard]] std::unique_ptr<AudioChain> cloneMasterChainForVerification() const {
        auto guard = m_masterReclaimer.enterRead();
        if (!guard) {
            return nullptr;
        }
        return guard->clone();
    }

    [[nodiscard]] std::unique_ptr<AudioChain> cloneListeningChainForVerification() const {
        auto guard = m_listeningReclaimer.enterRead();
        if (!guard) {
            return nullptr;
        }
        return guard->clone();
    }

    /// Thread-safe Telemetry Snapshot retrieval for Control / UI Thread (P0-26, P0-29).
    /// Audio Thread publishes via publishTelemetry() with zero mutexes and non-blocking lock-free buffer swap.
    /// UI / Reader threads serialize reading via m_telemetryReadMutex to safely copy published telemetry.
    [[nodiscard]] DspTelemetry currentTelemetry() const noexcept {
        std::lock_guard<std::mutex> lock(m_telemetryReadMutex);
        uint32_t cur = m_sharedTelemetryState.load(std::memory_order_acquire);
        if (cur & TELEMETRY_NEW_DATA) {
            uint32_t toExchange = static_cast<uint32_t>(m_readerTelemetryIndex & 3);
            uint32_t oldShared = m_sharedTelemetryState.exchange(toExchange, std::memory_order_acq_rel);
            m_readerTelemetryIndex = static_cast<int>(oldShared & 3);
            m_cachedTelemetry = m_telemetryBuffers[static_cast<size_t>(m_readerTelemetryIndex)];
        }
        return m_cachedTelemetry;
    }

    [[nodiscard]] double compressorGainReductionDb() const noexcept {
        return currentTelemetry().compressorGainReductionDb;
    }

    [[nodiscard]] double limiterGainReductionDb() const noexcept {
        return currentTelemetry().limiterGainReductionDb;
    }

    [[nodiscard]] int limiterLatencyFrames() const noexcept {
        return static_cast<int>(currentTelemetry().limiterLatencyFrames);
    }

    [[nodiscard]] double masterPeakL() const noexcept {
        return currentTelemetry().masterPeakL;
    }

    [[nodiscard]] double masterPeakR() const noexcept {
        return currentTelemetry().masterPeakR;
    }

    [[nodiscard]] double preDspPeakL() const noexcept {
        return currentTelemetry().preDspPeakL;
    }

    [[nodiscard]] double preDspPeakR() const noexcept {
        return currentTelemetry().preDspPeakR;
    }

    [[nodiscard]] double preDspRmsL() const noexcept {
        return currentTelemetry().preDspRmsL;
    }

    [[nodiscard]] double preDspRmsR() const noexcept {
        return currentTelemetry().preDspRmsR;
    }

    [[nodiscard]] double masterRmsL() const noexcept {
        return currentTelemetry().masterRmsL;
    }

    [[nodiscard]] double masterRmsR() const noexcept {
        return currentTelemetry().masterRmsR;
    }

    [[nodiscard]] bool masterClipped() const noexcept {
        return currentTelemetry().masterClipped;
    }

    [[nodiscard]] bool level1SamplesIdentical() const noexcept {
        return currentTelemetry().level1SamplesIdentical;
    }

    [[nodiscard]] double level1MaxAbsDiff() const noexcept {
        return currentTelemetry().level1MaxAbsDiff;
    }

    [[nodiscard]] uint64_t level1VerifiedBlocks() const noexcept {
        return m_level1VerifiedBlocks.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t level1TotalBlocks() const noexcept {
        return m_level1TotalBlocks.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t readerExhaustionDrops() const noexcept {
        return m_readerExhaustionDrops.load(std::memory_order_relaxed);
    }

    [[nodiscard]] dsp::ParameterCommandQueue& paramQueue() noexcept { return m_paramQueue; }
    [[nodiscard]] dsp::DspParameterMailbox& paramMailbox() noexcept { return m_paramMailbox; }

    [[nodiscard]] uint64_t commandsConsumed() const noexcept {
        return m_commandsConsumed.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t commandsApplied() const noexcept {
        return m_commandsApplied.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t mailboxConsumed() const noexcept {
        return m_mailboxConsumed.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t mailboxApplied() const noexcept {
        return m_mailboxApplied.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t mailboxLastConsumedSequence() const noexcept {
        return m_lastConsumedSequence.load(std::memory_order_relaxed);
    }
    void resetStatistics() noexcept {
        m_commandsConsumed.store(0, std::memory_order_relaxed);
        m_commandsApplied.store(0, std::memory_order_relaxed);
        m_mailboxConsumed.store(0, std::memory_order_relaxed);
        m_mailboxApplied.store(0, std::memory_order_relaxed);
        m_lastConsumedSequence.store(0, std::memory_order_relaxed);
        m_paramQueue.resetStatistics();
        m_paramMailbox.resetStatistics();
    }

    /// Runtime bypass status inspection (P1-1, P1-2)
    [[nodiscard]] bool isMasterGlobalBypassed() const noexcept {
        auto guard = m_masterReclaimer.enterRead();
        return guard.chain ? guard.chain->isBypassed() : false;
    }

    [[nodiscard]] bool isListeningGlobalBypassed() const noexcept {
        auto guard = m_listeningReclaimer.enterRead();
        return guard.chain ? guard.chain->isBypassed() : false;
    }

    [[nodiscard]] bool isMasterNodeBypassed(size_t index) const noexcept {
        auto guard = m_masterReclaimer.enterRead();
        return (guard.chain && index < guard.chain->nodeCount()) ? guard.chain->node(index)->isBypassed() : false;
    }

    [[nodiscard]] bool isListeningNodeBypassed(size_t index) const noexcept {
        auto guard = m_listeningReclaimer.enterRead();
        return (guard.chain && index < guard.chain->nodeCount()) ? guard.chain->node(index)->isBypassed() : false;
    }

    /// Hot-swap Mastering Chain (Control Thread execution).
    /// Safe, lock-free swap verified by EpochReclaimer.
    void swapMasterChain(std::unique_ptr<AudioChain> newChain) {
        if (!newChain || newChain->purpose() != ChainPurpose::MasteringChain) return;
        newChain->configure(m_graphSampleRate.load(std::memory_order_relaxed));
        m_masterReclaimer.swapChain(std::move(newChain));
    }

    /// Hot-swap Listening Correction Chain (Control Thread execution).
    void swapListeningChain(std::unique_ptr<AudioChain> newChain) {
        if (!newChain || newChain->purpose() != ChainPurpose::ListeningCorrectionChain) return;
        newChain->configure(m_graphSampleRate.load(std::memory_order_relaxed));
        m_listeningReclaimer.swapChain(std::move(newChain));
    }

    /// Atomically reposition External Insert within Master Chain while preserving live node parameters and bypass states
    bool repositionExternalInsert(size_t targetSlot, std::array<uint8_t, 7>& outNodeIndices) {
        std::shared_ptr<AudioNode> insertClone;
        struct TaggedNode {
            int canonicalSlot;
            std::shared_ptr<AudioNode> node;
        };
        std::vector<TaggedNode> others;
        bool chainBypassed = false;
        {
            auto guard = m_masterReclaimer.enterRead();
            if (!guard) return false;
            chainBypassed = guard->isBypassed();
            others.reserve(guard->nodeCount());
            for (size_t i = 0; i < guard->nodeCount(); ++i) {
                auto* n = guard->node(i);
                if (!n) continue;
                auto cloned = n->clone();
                if (!cloned) continue;
                const std::string& nm = n->name();
                if (nm == "External Insert") {
                    insertClone = std::move(cloned);
                } else {
                    int cs = -1;
                    if (nm == "Master PEQ") cs = 0;
                    else if (nm == "Graphic EQ") cs = 1;
                    else if (nm == "HPF / LPF") cs = 2;
                    else if (nm == "Compressor") cs = 3;
                    else if (nm == "Reverb") cs = 5;
                    else if (nm == "Peak Limiter") cs = 6;
                    others.push_back({cs, std::move(cloned)});
                }
            }
        }
        if (!insertClone) return false;

        auto newChain = std::make_unique<AudioChain>("Master Chain", ChainPurpose::MasteringChain);
        newChain->setBypassed(chainBypassed);
        const size_t clampedSlot = std::min(targetSlot, others.size());
        uint8_t outIdx = 0;
        outNodeIndices.fill(0xFF);
        for (size_t i = 0; i <= others.size(); ++i) {
            if (i == clampedSlot) {
                newChain->appendNode(insertClone);
                outNodeIndices[4] = outIdx++;
            }
            if (i < others.size()) {
                if (others[i].canonicalSlot >= 0 && others[i].canonicalSlot < 7) {
                    outNodeIndices[static_cast<size_t>(others[i].canonicalSlot)] = outIdx;
                }
                newChain->appendNode(std::move(others[i].node));
                ++outIdx;
            }
        }
        m_masterReclaimer.swapChain(std::move(newChain));
        return true;
    }

    /// Control Thread DSP configuration methods (P0-14, P0-21):
    /// Calculations, exponentiation, buffer sizing, and instance construction occur strictly on Control Thread.
    /// Audio Thread switches to prepared instances at block boundaries without mutating running DSPs.
    void updateCompressor(double thresholdDb, double ratio, double attackMs, double releaseMs,
                          double makeupDb, double kneeDb, bool enabled) {
        auto guard = m_masterReclaimer.enterRead();
        if (guard) {
            for (size_t i = 0; i < guard->nodeCount(); ++i) {
                if (auto* comp = dynamic_cast<CompressorNode*>(guard->node(i))) {
                    comp->prepareInstance(m_graphSampleRate.load(std::memory_order_relaxed),
                                          thresholdDb, ratio, attackMs, releaseMs, makeupDb, kneeDb, enabled);
                    break;
                }
            }
        }
    }

    void updatePeakLimiter(double ceilingDb, double releaseMs, double lookaheadMs, bool enabled) {
        auto guard = m_masterReclaimer.enterRead();
        if (guard) {
            for (size_t i = 0; i < guard->nodeCount(); ++i) {
                if (auto* lim = dynamic_cast<PeakLimiterNode*>(guard->node(i))) {
                    lim->prepareInstance(m_graphSampleRate.load(std::memory_order_relaxed),
                                         ceilingDb, releaseMs, lookaheadMs, enabled);
                    break;
                }
            }
        }
    }

    void updateReverb(double roomSize, double damping, double wetLevel, double predelayMs,
                      bool tailPreserving, bool enabled,
                      double maxTailRetentionSeconds = 5.0,
                      double maxProcessingTimeMs = 0.0) {
        auto guard = m_masterReclaimer.enterRead();
        if (guard) {
            for (size_t i = 0; i < guard->nodeCount(); ++i) {
                if (auto* rev = dynamic_cast<ReverbNode*>(guard->node(i))) {
                    rev->setBypassBehavior(tailPreserving ? BypassBehavior::TailPreserving
                                                          : BypassBehavior::Immediate);
                    rev->prepareInstance(m_graphSampleRate.load(std::memory_order_relaxed),
                                         roomSize, damping, wetLevel, predelayMs,
                                         tailPreserving, enabled,
                                         maxTailRetentionSeconds, maxProcessingTimeMs);
                    break;
                }
            }
        }
    }

    /// Trigger periodic epoch reclamation outside Audio Thread (Control / Worker Thread)
    void reclaimRetiredMemory() {
        m_masterReclaimer.reclaimRetired();
        m_listeningReclaimer.reclaimRetired();
        {
            auto guard = m_masterReclaimer.enterRead();
            if (guard) guard->reclaimRetiredNodes();
        }
        {
            auto guard = m_listeningReclaimer.enterRead();
            if (guard) guard->reclaimRetiredNodes();
        }
    }

    /// Real-time audio rendering call (Audio Thread execution).
    /// Guarantees:
    /// - Strictly zero heap allocation or blocking locks.
    /// - Drains DSP parameter queue at the block boundary.
    /// - Operates on planar double buffers through DSP & Listening Correction.
    /// - Branches at Logical Render Tap: Render Monitor bypasses Listening Correction with 0.000000 leakage.
    /// - Converts to target device PCM format with TPDF dither via FormatConverter at the boundary.
    void process(double* left, double* right, size_t frameCount,
                 void* outPcmBuffer, SampleFormat targetFormat, bool applyDither = true,
                 dsp::ParameterSmoother* postTapSmootherL = nullptr,
                 dsp::ParameterSmoother* postTapSmootherR = nullptr,
                 double monitorLevelMatchGain = 1.0,
                 bool directModeBypass = false)
    {
        // 1. Enter epoch to protect current active chains from destruction during Audio Thread execution
        auto masterGuard = m_masterReclaimer.enterRead();
        auto listeningGuard = m_listeningReclaimer.enterRead();

        if (!masterGuard) {
            // P0-20: Reader slot exhaustion. NEVER access unshielded pointers!
            m_readerExhaustionDrops.fetch_add(1, std::memory_order_relaxed);
            std::fill_n(left, frameCount, 0.0);
            std::fill_n(right, frameCount, 0.0);
            FormatConverter::convertPlanarFloat64ToPcm(
                left, right, frameCount, outPcmBuffer, targetFormat, false
            );
            return;
        }

        // 0. Drain pending discrete parameter commands and continuous mailbox updates at block boundary
        drainParameterCommands(masterGuard.chain, listeningGuard.chain);
        drainParameterMailbox(masterGuard.chain, listeningGuard.chain);

        // Pre-DSP Metering & Level 1 Input Snapshot (Strictly zero heap allocation)
        double prePeakL = 0.0;
        double prePeakR = 0.0;
        double preSumSqL = 0.0;
        double preSumSqR = 0.0;
        uint64_t inputHash = 14695981039346656037ULL;
        const size_t refFrames = std::min(frameCount, MAX_LEVEL1_FRAMES);
        for (size_t i = 0; i < frameCount; ++i) {
            const double lVal = left[i];
            const double rVal = right[i];
            prePeakL = std::max(prePeakL, std::abs(lVal));
            prePeakR = std::max(prePeakR, std::abs(rVal));
            preSumSqL += lVal * lVal;
            preSumSqR += rVal * rVal;
            uint64_t bitsL = 0;
            uint64_t bitsR = 0;
            std::memcpy(&bitsL, &lVal, sizeof(bitsL));
            std::memcpy(&bitsR, &rVal, sizeof(bitsR));
            inputHash ^= bitsL;
            inputHash *= 1099511628211ULL;
            inputHash ^= bitsR;
            inputHash *= 1099511628211ULL;
            if (i < refFrames) {
                m_level1RefL[i] = lVal;
                m_level1RefR[i] = rVal;
            }
        }
        const double preRmsL = frameCount > 0 ? std::sqrt(preSumSqL / static_cast<double>(frameCount)) : 0.0;
        const double preRmsR = frameCount > 0 ? std::sqrt(preSumSqR / static_cast<double>(frameCount)) : 0.0;

        double tapPeakL = 0.0;
        double tapPeakR = 0.0;

        if (directModeBypass) {
            // Spec §2.6 Direct Mode: Bypass all DSP, External Insert, Listening Correction, dither, and software gain/mute
            for (size_t i = 0; i < frameCount; ++i) {
                tapPeakL = std::max(tapPeakL, std::abs(left[i]));
                tapPeakR = std::max(tapPeakR, std::abs(right[i]));
            }
            const bool effectiveDither = false;
            FormatConverter::convertPlanarFloat64ToPcm(
                left, right, frameCount, outPcmBuffer, targetFormat, effectiveDither
            );
            const double postPcmDiff = computePostPcmMaxAbsDiff(
                outPcmBuffer, targetFormat, m_level1RefL.data(), m_level1RefR.data(), refFrames);
            const bool postPcmLossless =
                (refFrames > 0) && (outPcmBuffer != nullptr) && !effectiveDither &&
                (targetFormat != SampleFormat::Int16) && (postPcmDiff <= 1e-6);
            publishTelemetry(masterGuard.chain, frameCount, left, right, tapPeakL, tapPeakR,
                             prePeakL, prePeakR, preRmsL, preRmsR,
                             inputHash, refFrames, true, postPcmLossless, effectiveDither, postPcmDiff);
            return;
        }

        // 2. Upstream Master / DSP Chain (before Logical Render Tap)
        if (masterGuard.chain) {
            masterGuard.chain->process(left, right, frameCount);
        }

        // Measure Logical Render Tap peak (post Master Chain, pre Monitor/Fader/Listening Correction)
        for (size_t i = 0; i < frameCount; ++i) {
            tapPeakL = std::max(tapPeakL, std::abs(left[i]));
            tapPeakR = std::max(tapPeakR, std::abs(right[i]));
        }

        // 3. Logical Render Tap Branching (Section 5.11, Spec §5.2 downstream Master Gain & Level Match)
        const OutputPathMode currentMode = m_outputMode.load(std::memory_order_relaxed);
        const bool calMute = m_calibrationMonitorMute.load(std::memory_order_acquire);
        const double listenVol = (m_listeningMuted.load(std::memory_order_relaxed) || calMute)
                                     ? 0.0
                                     : m_listeningVolume.load(std::memory_order_relaxed);
        const double renderMonVol = (m_renderMonitorMuted.load(std::memory_order_relaxed) || calMute)
                                        ? 0.0
                                        : m_renderMonitorVolume.load(std::memory_order_relaxed);
        if (!m_smoothersInitialized.load(std::memory_order_relaxed)) {
            m_listeningVolSmoother.reset(listenVol);
            m_renderMonVolSmoother.reset(renderMonVol);
            m_levelMatchSmoother.reset(monitorLevelMatchGain);
            m_smoothersInitialized.store(true, std::memory_order_relaxed);
        } else {
            m_listeningVolSmoother.setTarget(listenVol);
            m_renderMonVolSmoother.setTarget(renderMonVol);
            m_levelMatchSmoother.setTarget(monitorLevelMatchGain);
        }

        if (currentMode == OutputPathMode::RenderMonitorPath) {
            // Render Monitor Path: Strictly NO Listening Correction.
            for (size_t i = 0; i < frameCount; ++i) {
                const double masterGainL = postTapSmootherL ? postTapSmootherL->next() : 1.0;
                const double masterGainR = postTapSmootherR ? postTapSmootherR->next() : 1.0;
                (void)m_listeningVolSmoother.next();
                const double smoothedRenderMon = m_renderMonVolSmoother.next();
                const double smoothedLevelMatch = m_levelMatchSmoother.next();
                left[i]  *= masterGainL * smoothedLevelMatch * smoothedRenderMon;
                right[i] *= masterGainR * smoothedLevelMatch * smoothedRenderMon;
            }
        } else {
            // Listening Path: Route through Listening Correction (PEQ headphone/speaker correction)
            if (listeningGuard.chain) {
                listeningGuard.chain->process(left, right, frameCount);
            }
            // Scale by post-tap Master Gain/Mute, comparative Level Match, and Listening Volume
            for (size_t i = 0; i < frameCount; ++i) {
                const double masterGainL = postTapSmootherL ? postTapSmootherL->next() : 1.0;
                const double masterGainR = postTapSmootherR ? postTapSmootherR->next() : 1.0;
                const double smoothedListen = m_listeningVolSmoother.next();
                (void)m_renderMonVolSmoother.next();
                const double smoothedLevelMatch = m_levelMatchSmoother.next();
                left[i]  *= masterGainL * smoothedLevelMatch * smoothedListen;
                right[i] *= masterGainR * smoothedLevelMatch * smoothedListen;
            }
        }

        // 4. Output Boundary: Interleave and convert from planar float64 to target PCM with TPDF dither
        // FormatConverter only applies TPDF dither on integer quantization formats (Int24_32 / Int16).
        const bool formatUsesDither =
            (targetFormat == SampleFormat::Int24_32 || targetFormat == SampleFormat::Int16);
        const bool effectiveDither =
            applyDither && formatUsesDither && m_liveDitherEnabled.load(std::memory_order_relaxed);
        FormatConverter::convertPlanarFloat64ToPcm(
            left, right, frameCount, outPcmBuffer, targetFormat, effectiveDither
        );
        const double postPcmDiff = computePostPcmMaxAbsDiff(
            outPcmBuffer, targetFormat, m_level1RefL.data(), m_level1RefR.data(), refFrames);
        const bool postPcmLossless =
            (refFrames > 0) && (outPcmBuffer != nullptr) && !effectiveDither &&
            (targetFormat != SampleFormat::Int16) && (postPcmDiff <= 1e-6);

        // 5. Publish Telemetry Snapshot to UI Thread (P0-26)
        m_hasProcessedAnyBlock.store(true, std::memory_order_relaxed);
        publishTelemetry(masterGuard.chain, frameCount, left, right, tapPeakL, tapPeakR,
                         prePeakL, prePeakR, preRmsL, preRmsR,
                         inputHash, refFrames, false, postPcmLossless, effectiveDither, postPcmDiff);
    }

private:
    void drainParameterCommands(AudioChain* master, AudioChain* listening) noexcept {
        dsp::DspCommand cmd;
        size_t drained = 0;
        while (drained < MAX_COMMANDS_PER_BLOCK && m_paramQueue.pop(cmd)) {
            if (applyDspCommand(cmd, master, listening)) {
                m_commandsApplied.fetch_add(1, std::memory_order_relaxed);
            }
            m_commandsConsumed.fetch_add(1, std::memory_order_relaxed);
            ++drained;
        }
    }

    bool applyDspCommand(const dsp::DspCommand& cmd, AudioChain* master, AudioChain* listening) noexcept {
        bool applied = false;
        switch (cmd.type) {
        // P0-26, P0-27: Continuous parameters (GEQ, PEQ, Filter) are updated exclusively
        // via DspParameterMailbox to prevent double-update racing and parameter rewind.
        case dsp::DspCommandType::ApplyPeqPreset:
            // P0-27: Discrete PEQ preset calculation is strictly executed on Control Thread via
            // paramMailbox().setListeningPreset() to prevent heavy coefficient computation in Audio Thread.
            break;
        case dsp::DspCommandType::SetCompressorParams:
            // P0-14, P0-21: Structural and mathematical configure is handled exclusively on Control Thread
            // via updateCompressor() to maintain strict Audio Thread real-time safety.
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* compNode = dynamic_cast<CompressorNode*>(master->node(i))) {
                        compNode->setBypassed(cmd.boolFlag1 == 0);
                        applied = true;
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetLimiterParams:
            // P0-14, P0-21: Lookahead buffer and ceiling configure handled on Control Thread via updatePeakLimiter()
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* limiterNode = dynamic_cast<PeakLimiterNode*>(master->node(i))) {
                        limiterNode->setBypassed(cmd.boolFlag1 == 0);
                        applied = true;
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetReverbParams:
            // P0-14, P0-21: Reverb comb filter & delay buffer configure handled on Control Thread via updateReverb()
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* reverbNode = dynamic_cast<ReverbNode*>(master->node(i))) {
                        reverbNode->setBypassed(cmd.boolFlag1 == 0);
                        applied = true;
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetNodeBypass:
            if (cmd.subIndex == 1) {
                if (listening && cmd.index < listening->nodeCount()) {
                    listening->node(cmd.index)->setBypassed(cmd.boolFlag1 != 0);
                    applied = true;
                }
            } else {
                if (master && cmd.index < master->nodeCount()) {
                    master->node(cmd.index)->setBypassed(cmd.boolFlag1 != 0);
                    applied = true;
                }
            }
            break;
        case dsp::DspCommandType::SetGlobalBypass:
            if (cmd.subIndex == 1) {
                if (listening) {
                    listening->setBypassed(cmd.boolFlag1 != 0);
                    applied = true;
                }
            } else {
                if (master) {
                    master->setBypassed(cmd.boolFlag1 != 0);
                    applied = true;
                }
            }
            break;
        default:
            break;
        }
        return applied;
    }

    void drainParameterMailbox(AudioChain* master, AudioChain* listening) noexcept {
        dsp::ContinuousDspParameters params;
        if (!m_paramMailbox.fetchUpdates(params)) {
            return;
        }
        m_mailboxConsumed.fetch_add(1, std::memory_order_relaxed);
        m_lastConsumedSequence.store(params.sequenceNumber, std::memory_order_relaxed);

        const size_t smoothSamples = m_hasProcessedAnyBlock.load(std::memory_order_relaxed)
            ? static_cast<size_t>(std::clamp(m_graphSampleRate.load(std::memory_order_relaxed) * 0.005, 32.0, 512.0))
            : 0;

        bool appliedAny = false;
        if (master) {
            // 1. Graphic EQ: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.geqModifiedMask != 0) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* geqNode = dynamic_cast<GraphicEqNode*>(master->node(i))) {
                        geqNode->applyAllCoefficients(params.geqCoefficients, smoothSamples);
                        geqNode->applyBandEnabled(params.geqBandEnabled);
                        appliedAny = true;
                        break;
                    }
                }
            }

            // 2. Parametric EQ: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.peqLeftModifiedMask != 0 || params.peqRightModifiedMask != 0) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* peqNode = dynamic_cast<ParametricEqNode*>(master->node(i))) {
                        peqNode->applyCoefficients(
                            params.peqLeftCoefficients,
                            params.peqRightCoefficients,
                            params.peqLeftEnabled,
                            params.peqRightEnabled,
                            smoothSamples
                        );
                        appliedAny = true;
                        break;
                    }
                }
            }

            // 3. Filter parameters: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.filterModified) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* filterNode = dynamic_cast<HighLowPassNode*>(master->node(i))) {
                        filterNode->applyCoefficients(params.filterCoefficients, smoothSamples);
                        appliedAny = true;
                        break;
                    }
                }
            }
        }

        if (listening) {
            // 4. Listening Correction: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.listeningModified) {
                for (size_t i = 0; i < listening->nodeCount(); ++i) {
                    if (auto* listenNode = dynamic_cast<ParametricEqNode*>(listening->node(i))) {
                        listenNode->applyCoefficients(
                            params.listeningLeftCoefficients,
                            params.listeningRightCoefficients,
                            params.listeningLeftEnabled,
                            params.listeningRightEnabled,
                            smoothSamples
                        );
                        appliedAny = true;
                        break;
                    }
                }
            }
        }

        if (appliedAny) {
            m_mailboxApplied.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static double computePostPcmMaxAbsDiff(const void* pcmBuf, SampleFormat fmt,
                                           const double* refL, const double* refR,
                                           size_t frames) noexcept {
        if (!pcmBuf || !refL || !refR || frames == 0) {
            return 0.0;
        }
        double maxDiff = 0.0;
        switch (fmt) {
        case SampleFormat::Float32: {
            const auto* in = static_cast<const float*>(pcmBuf);
            for (size_t i = 0; i < frames; ++i) {
                const double refClampL = std::clamp(refL[i], -1.0, 1.0);
                const double refClampR = std::clamp(refR[i], -1.0, 1.0);
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2]) - refClampL));
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2 + 1]) - refClampR));
            }
            break;
        }
        case SampleFormat::Int24_32: {
            const auto* in = static_cast<const int32_t*>(pcmBuf);
            constexpr double invScale = 1.0 / 8388607.0;
            for (size_t i = 0; i < frames; ++i) {
                const double refClampL = std::clamp(refL[i], -1.0, 1.0);
                const double refClampR = std::clamp(refR[i], -1.0, 1.0);
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2]) * invScale - refClampL));
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2 + 1]) * invScale - refClampR));
            }
            break;
        }
        case SampleFormat::Int32: {
            const auto* in = static_cast<const int32_t*>(pcmBuf);
            constexpr double invScale = 1.0 / 2147483647.0;
            for (size_t i = 0; i < frames; ++i) {
                const double refClampL = std::clamp(refL[i], -1.0, 1.0);
                const double refClampR = std::clamp(refR[i], -1.0, 1.0);
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2]) * invScale - refClampL));
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2 + 1]) * invScale - refClampR));
            }
            break;
        }
        case SampleFormat::Int16: {
            const auto* in = static_cast<const int16_t*>(pcmBuf);
            constexpr double invScale = 1.0 / 32767.0;
            for (size_t i = 0; i < frames; ++i) {
                const double refClampL = std::clamp(refL[i], -1.0, 1.0);
                const double refClampR = std::clamp(refR[i], -1.0, 1.0);
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2]) * invScale - refClampL));
                maxDiff = std::max(maxDiff, std::abs(static_cast<double>(in[i * 2 + 1]) * invScale - refClampR));
            }
            break;
        }
        case SampleFormat::Float64: {
            const auto* in = static_cast<const double*>(pcmBuf);
            for (size_t i = 0; i < frames; ++i) {
                const double refClampL = std::clamp(refL[i], -1.0, 1.0);
                const double refClampR = std::clamp(refR[i], -1.0, 1.0);
                maxDiff = std::max(maxDiff, std::abs(in[i * 2] - refClampL));
                maxDiff = std::max(maxDiff, std::abs(in[i * 2 + 1] - refClampR));
            }
            break;
        }
        }
        return maxDiff;
    }

    /// Audio Thread: Mutex-free, non-blocking lock-free Telemetry publication (P0-29).
    /// Inspects up to AudioChain::MAX_NODES (strictly bounded O(N) where N <= 16, zero dynamic allocation, zero locks)
    /// and publishes the snapshot buffer via a single O(1) atomic exchange.
    void publishTelemetry(AudioChain* master, size_t frames, const double* left = nullptr, const double* right = nullptr,
                          double renderTapPeakL = 0.0, double renderTapPeakR = 0.0,
                          double prePeakL = 0.0, double prePeakR = 0.0,
                          double preRmsL = 0.0, double preRmsR = 0.0,
                          uint64_t inputHash = 0, size_t refFrames = 0,
                          bool directBypass = false,
                          bool postPcmLossless = false,
                          bool effectiveDither = false,
                          double postPcmMaxDiff = 0.0) noexcept {
        DspTelemetry& t = m_telemetryBuffers[static_cast<size_t>(m_writerTelemetryIndex)];
        t.graphSampleRate = m_graphSampleRate.load(std::memory_order_relaxed);
        t.processedFrameCount += frames;
        t.renderTapPeakL = renderTapPeakL;
        t.renderTapPeakR = renderTapPeakR;
        t.preDspPeakL = prePeakL;
        t.preDspPeakR = prePeakR;
        t.preDspRmsL = preRmsL;
        t.preDspRmsR = preRmsR;

        double peakL = 0.0;
        double peakR = 0.0;
        double sumSqL = 0.0;
        double sumSqR = 0.0;
        double maxAbsDiff = 0.0;
        uint64_t outputHash = 14695981039346656037ULL;
        if (left && right && frames > 0) {
            for (size_t i = 0; i < frames; ++i) {
                const double lVal = left[i];
                const double rVal = right[i];
                double al = std::abs(lVal);
                double ar = std::abs(rVal);
                if (al > peakL) peakL = al;
                if (ar > peakR) peakR = ar;
                sumSqL += lVal * lVal;
                sumSqR += rVal * rVal;

                uint64_t bitsL = 0;
                uint64_t bitsR = 0;
                std::memcpy(&bitsL, &lVal, sizeof(bitsL));
                std::memcpy(&bitsR, &rVal, sizeof(bitsR));
                outputHash ^= bitsL;
                outputHash *= 1099511628211ULL;
                outputHash ^= bitsR;
                outputHash *= 1099511628211ULL;
                if (i < refFrames) {
                    maxAbsDiff = std::max(maxAbsDiff, std::abs(lVal - m_level1RefL[i]));
                    maxAbsDiff = std::max(maxAbsDiff, std::abs(rVal - m_level1RefR[i]));
                }
            }
        }
        t.masterPeakL = peakL;
        t.masterPeakR = peakR;
        t.masterRmsL = frames > 0 ? std::sqrt(sumSqL / static_cast<double>(frames)) : 0.0;
        t.masterRmsR = frames > 0 ? std::sqrt(sumSqR / static_cast<double>(frames)) : 0.0;
        t.masterClipped = (peakL >= 1.0 || peakR >= 1.0);
        t.level1InputHash = inputHash;
        t.level1OutputHash = outputHash;
        t.level1MaxAbsDiff = maxAbsDiff;
        const bool identical = (frames > 0) && (inputHash == outputHash) && (maxAbsDiff == 0.0);
        t.level1SamplesIdentical = identical;
        t.level1OutputPcmLossless = postPcmLossless;
        t.level1DitherActive = effectiveDither;
        t.level1DirectBypassActive = directBypass;
        t.level1PostPcmMaxAbsDiff = postPcmMaxDiff;
        t.level1TotalBlocks = m_level1TotalBlocks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (identical && postPcmLossless) {
            t.level1VerifiedBlocks = m_level1VerifiedBlocks.fetch_add(1, std::memory_order_relaxed) + 1;
        } else {
            t.level1VerifiedBlocks = m_level1VerifiedBlocks.load(std::memory_order_relaxed);
        }

        if (master && !master->isBypassed()) {
            for (size_t i = 0; i < master->nodeCount(); ++i) {
                auto* node = master->node(i);
                if (!node) continue;
                if (auto* comp = dynamic_cast<CompressorNode*>(node)) {
                    t.compressorGainReductionDb = comp->isBypassed() ? 0.0 : comp->gainReductionDb();
                    t.compressorEnabled = !comp->isBypassed() && comp->isEnabled();
                } else if (auto* lim = dynamic_cast<PeakLimiterNode*>(node)) {
                    t.limiterGainReductionDb = lim->isBypassed() ? 0.0 : lim->gainReductionDb();
                    t.limiterLatencyFrames = lim->isBypassed() ? 0 : lim->latencyFrames();
                    t.limiterEnabled = !lim->isBypassed() && lim->isEnabled();
                } else if (auto* rev = dynamic_cast<ReverbNode*>(node)) {
                    t.reverbWetLevel = rev->isBypassed() ? 0.0 : rev->wetLevel();
                    t.reverbEnabled = !rev->isBypassed() && rev->isEnabled();
                }
            }
        } else {
            t.compressorGainReductionDb = 0.0;
            t.limiterGainReductionDb = 0.0;
            t.limiterLatencyFrames = 0;
            t.reverbWetLevel = 0.0;
        }

        uint32_t newState = static_cast<uint32_t>(m_writerTelemetryIndex & 3) | TELEMETRY_NEW_DATA;
        uint32_t oldShared = m_sharedTelemetryState.exchange(newState, std::memory_order_acq_rel);
        m_writerTelemetryIndex = static_cast<int>(oldShared & 3);
    }

    static constexpr uint32_t TELEMETRY_NEW_DATA = 0x4;

    std::atomic<double> m_graphSampleRate;
    std::atomic<OutputPathMode> m_outputMode{OutputPathMode::ListeningPath};
    std::atomic<double> m_listeningVolume{1.0};
    std::atomic<double> m_renderMonitorVolume{1.0};
    std::atomic<bool> m_listeningMuted{false};
    std::atomic<bool> m_renderMonitorMuted{false};
    std::atomic<bool> m_calibrationMonitorMute{false};
    std::atomic<bool> m_liveDitherEnabled{true};

    dsp::ParameterCommandQueue m_paramQueue;
    dsp::DspParameterMailbox m_paramMailbox;

    alignas(64) std::array<DspTelemetry, 3> m_telemetryBuffers{};
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "Telemetry triple-buffer state must be natively lock-free on this platform");
    alignas(64) mutable std::atomic<uint32_t> m_sharedTelemetryState{1};
    int m_writerTelemetryIndex{0};
    mutable int m_readerTelemetryIndex{2};
    mutable DspTelemetry m_cachedTelemetry{};
    mutable std::mutex m_telemetryReadMutex; // UI Thread ONLY
    alignas(64) std::atomic<uint64_t> m_readerExhaustionDrops{0};
    alignas(64) std::atomic<uint64_t> m_commandsConsumed{0};
    alignas(64) std::atomic<uint64_t> m_commandsApplied{0};
    alignas(64) std::atomic<uint64_t> m_mailboxConsumed{0};
    alignas(64) std::atomic<uint64_t> m_mailboxApplied{0};
    alignas(64) std::atomic<uint64_t> m_lastConsumedSequence{0};

    dsp::ParameterSmoother m_listeningVolSmoother{1.0, 10.0, 48000.0};
    dsp::ParameterSmoother m_renderMonVolSmoother{1.0, 10.0, 48000.0};
    dsp::ParameterSmoother m_levelMatchSmoother{1.0, 10.0, 48000.0};
    std::atomic<bool> m_smoothersInitialized{false};
    std::atomic<bool> m_hasProcessedAnyBlock{false};
    alignas(64) std::array<double, MAX_LEVEL1_FRAMES> m_level1RefL{};
    alignas(64) std::array<double, MAX_LEVEL1_FRAMES> m_level1RefR{};
    alignas(64) std::atomic<uint64_t> m_level1VerifiedBlocks{0};
    alignas(64) std::atomic<uint64_t> m_level1TotalBlocks{0};

    mutable EpochReclaimer<AudioChain> m_masterReclaimer;
    mutable EpochReclaimer<AudioChain> m_listeningReclaimer;
};

} // namespace desk32::engine::pipeline
