#pragma once

#include "AudioChain.hpp"
#include "BuiltInDspNodes.hpp"
#include "engine/dsp/Resampler.hpp"
#include "engine/dsp/ParameterCommandQueue.hpp"
#include "engine/dsp/DspParameterMailbox.hpp"
#include "engine/format/FormatConverter.hpp"
#include "engine/memory/EpochReclaimer.hpp"
#include "engine/routing/OutputPathRouter.hpp"
#include <atomic>
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
};

class AudioPipeline {
public:
    explicit AudioPipeline(double graphSampleRate = 48000.0)
        : m_graphSampleRate(graphSampleRate)
    {
        if (graphSampleRate != 48000.0) {
            m_paramMailbox.setSampleRate(graphSampleRate);
        }
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

    [[nodiscard]] double renderMonitorVolume() const noexcept { return m_renderMonitorVolume.load(); }
    void setRenderMonitorVolume(double vol) noexcept { m_renderMonitorVolume.store(std::clamp(vol, 0.0, 2.0)); }

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

    [[nodiscard]] uint64_t readerExhaustionDrops() const noexcept {
        return m_readerExhaustionDrops.load(std::memory_order_relaxed);
    }

    [[nodiscard]] dsp::ParameterCommandQueue& paramQueue() noexcept { return m_paramQueue; }
    [[nodiscard]] dsp::DspParameterMailbox& paramMailbox() noexcept { return m_paramMailbox; }

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
                      bool tailPreserving, bool enabled) {
        auto guard = m_masterReclaimer.enterRead();
        if (guard) {
            for (size_t i = 0; i < guard->nodeCount(); ++i) {
                if (auto* rev = dynamic_cast<ReverbNode*>(guard->node(i))) {
                    rev->prepareInstance(m_graphSampleRate.load(std::memory_order_relaxed),
                                         roomSize, damping, wetLevel, predelayMs, tailPreserving, enabled);
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
                 void* outPcmBuffer, SampleFormat targetFormat, bool applyDither = true)
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

        // 2. Upstream Master / DSP Chain (before Logical Render Tap)
        if (masterGuard.chain) {
            masterGuard.chain->process(left, right, frameCount);
        }

        // 3. Logical Render Tap Branching (Section 5.11)
        const OutputPathMode currentMode = m_outputMode.load(std::memory_order_relaxed);
        const double listenVol = m_listeningVolume.load(std::memory_order_relaxed);
        const double renderMonVol = m_renderMonitorVolume.load(std::memory_order_relaxed);

        if (currentMode == OutputPathMode::RenderMonitorPath) {
            // Render Monitor Path:
            // Strictly NO Listening Correction. Directly scale by render monitor volume.
            for (size_t i = 0; i < frameCount; ++i) {
                left[i]  *= renderMonVol;
                right[i] *= renderMonVol;
            }
        } else {
            // Listening Path:
            // Route through Listening Correction (PEQ headphone/speaker correction)
            if (listeningGuard.chain) {
                listeningGuard.chain->process(left, right, frameCount);
            }
            // Scale by listening volume
            for (size_t i = 0; i < frameCount; ++i) {
                left[i]  *= listenVol;
                right[i] *= listenVol;
            }
        }

        // 4. Output Boundary: Interleave and convert from planar float64 to target PCM with TPDF dither
        FormatConverter::convertPlanarFloat64ToPcm(
            left, right, frameCount, outPcmBuffer, targetFormat, applyDither
        );

        // 5. Publish Telemetry Snapshot to UI Thread (P0-26)
        publishTelemetry(masterGuard.chain, frameCount);
    }

private:
    void drainParameterCommands(AudioChain* master, AudioChain* listening) noexcept {
        dsp::DspCommand cmd;
        while (m_paramQueue.pop(cmd)) {
            applyDspCommand(cmd, master, listening);
        }
    }

    void applyDspCommand(const dsp::DspCommand& cmd, AudioChain* master, AudioChain* listening) noexcept {
        switch (cmd.type) {
        // P0-26, P0-27: Continuous parameters (GEQ, PEQ, Filter) are updated exclusively
        // via DspParameterMailbox to prevent double-update racing and parameter rewind.
        case dsp::DspCommandType::ApplyPeqPreset:
            // P0-27: Discrete PEQ preset calculation is strictly executed on Control Thread via
            // paramMailbox().setListeningPreset() to prevent heavy coefficient computation in Audio Thread.
            break;
        case dsp::DspCommandType::SetCompressorParams:
            // P0-14: Structural and mathematical configure is handled exclusively on Control Thread
            // via updateCompressor() to maintain strict Audio Thread real-time safety.
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* compNode = dynamic_cast<CompressorNode*>(master->node(i))) {
                        compNode->dsp().setEnabled(cmd.boolFlag1 != 0);
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetLimiterParams:
            // P0-14: Lookahead buffer and ceiling configure handled on Control Thread via updatePeakLimiter()
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* limiterNode = dynamic_cast<PeakLimiterNode*>(master->node(i))) {
                        limiterNode->dsp().setEnabled(cmd.boolFlag1 != 0);
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetReverbParams:
            // P0-14: Reverb comb filter & delay buffer configure handled on Control Thread via updateReverb()
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* reverbNode = dynamic_cast<ReverbNode*>(master->node(i))) {
                        reverbNode->dsp().setEnabled(cmd.boolFlag1 != 0);
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetNodeBypass:
            if (cmd.subIndex == 1) {
                if (listening && cmd.index < listening->nodeCount()) {
                    listening->node(cmd.index)->setBypassed(cmd.boolFlag1 != 0);
                }
            } else {
                if (master && cmd.index < master->nodeCount()) {
                    master->node(cmd.index)->setBypassed(cmd.boolFlag1 != 0);
                }
            }
            break;
        case dsp::DspCommandType::SetGlobalBypass:
            if (cmd.subIndex == 1) {
                if (listening) {
                    listening->setBypassed(cmd.boolFlag1 != 0);
                }
            } else {
                if (master) {
                    master->setBypassed(cmd.boolFlag1 != 0);
                }
            }
            break;
        default:
            break;
        }
    }

    void drainParameterMailbox(AudioChain* master, AudioChain* listening) noexcept {
        dsp::ContinuousDspParameters params;
        if (!m_paramMailbox.fetchUpdates(params)) {
            return;
        }

        if (master) {
            // 1. Graphic EQ: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.geqModifiedMask != 0) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* geqNode = dynamic_cast<GraphicEqNode*>(master->node(i))) {
                        geqNode->dsp().applyAllCoefficients(params.geqCoefficients);
                        break;
                    }
                }
            }

            // 2. Parametric EQ: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.peqLeftModifiedMask != 0 || params.peqRightModifiedMask != 0) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* peqNode = dynamic_cast<ParametricEqNode*>(master->node(i))) {
                        peqNode->dsp().applyCoefficients(
                            params.peqLeftCoefficients,
                            params.peqRightCoefficients,
                            params.peqLeftEnabled,
                            params.peqRightEnabled
                        );
                        break;
                    }
                }
            }

            // 3. Filter parameters: O(1) bulk assignment of precalculated coefficients (P0-27)
            if (params.filterModified) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* filterNode = dynamic_cast<HighLowPassNode*>(master->node(i))) {
                        filterNode->dsp().applyCoefficients(params.filterCoefficients);
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
                        listenNode->dsp().applyCoefficients(
                            params.listeningLeftCoefficients,
                            params.listeningRightCoefficients,
                            params.listeningLeftEnabled,
                            params.listeningRightEnabled
                        );
                        break;
                    }
                }
            }
        }
    }

    /// Audio Thread: Mutex-free, non-blocking lock-free Telemetry publication (P0-29).
    /// Inspects up to N DSP nodes (bounded O(N) traversal, zero dynamic allocation, zero locks)
    /// and publishes the snapshot buffer via a single O(1) atomic exchange.
    void publishTelemetry(AudioChain* master, size_t frames) noexcept {
        DspTelemetry& t = m_telemetryBuffers[static_cast<size_t>(m_writerTelemetryIndex)];
        t.graphSampleRate = m_graphSampleRate.load(std::memory_order_relaxed);
        t.processedFrameCount += frames;

        if (master && !master->isBypassed()) {
            for (size_t i = 0; i < master->nodeCount(); ++i) {
                auto* node = master->node(i);
                if (!node) continue;
                if (auto* comp = dynamic_cast<CompressorNode*>(node)) {
                    t.compressorGainReductionDb = comp->isBypassed() ? 0.0 : comp->gainReductionDb();
                    t.compressorEnabled = !comp->isBypassed() && comp->dsp().isEnabled();
                } else if (auto* lim = dynamic_cast<PeakLimiterNode*>(node)) {
                    t.limiterGainReductionDb = lim->isBypassed() ? 0.0 : lim->gainReductionDb();
                    t.limiterLatencyFrames = lim->isBypassed() ? 0 : lim->latencyFrames();
                    t.limiterEnabled = !lim->isBypassed() && lim->dsp().isEnabled();
                } else if (auto* rev = dynamic_cast<ReverbNode*>(node)) {
                    t.reverbWetLevel = rev->isBypassed() ? 0.0 : rev->wetLevel();
                    t.reverbEnabled = !rev->isBypassed() && rev->dsp().isEnabled();
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

    mutable EpochReclaimer<AudioChain> m_masterReclaimer;
    mutable EpochReclaimer<AudioChain> m_listeningReclaimer;
};

} // namespace desk32::engine::pipeline
