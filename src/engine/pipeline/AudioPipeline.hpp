#pragma once

#include "AudioChain.hpp"
#include "BuiltInDspNodes.hpp"
#include "engine/dsp/Resampler.hpp"
#include "engine/dsp/ParameterCommandQueue.hpp"
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

class AudioPipeline {
public:
    explicit AudioPipeline(double graphSampleRate = 48000.0)
        : m_graphSampleRate(graphSampleRate)
    {
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

    [[nodiscard]] double compressorGainReductionDb() const noexcept {
        auto guard = m_masterReclaimer.enterRead();
        if (!guard || guard->isBypassed()) return 0.0;
        for (size_t i = 0; i < guard->nodeCount(); ++i) {
            if (auto* comp = dynamic_cast<CompressorNode*>(guard->node(i))) {
                return comp->isBypassed() ? 0.0 : comp->dsp().gainReductionDb();
            }
        }
        return 0.0;
    }

    [[nodiscard]] double limiterGainReductionDb() const noexcept {
        auto guard = m_masterReclaimer.enterRead();
        if (!guard || guard->isBypassed()) return 0.0;
        for (size_t i = 0; i < guard->nodeCount(); ++i) {
            if (auto* lim = dynamic_cast<PeakLimiterNode*>(guard->node(i))) {
                return lim->isBypassed() ? 0.0 : lim->dsp().gainReductionDb();
            }
        }
        return 0.0;
    }

    [[nodiscard]] int limiterLatencyFrames() const noexcept {
        auto guard = m_masterReclaimer.enterRead();
        if (!guard || guard->isBypassed()) return 0;
        for (size_t i = 0; i < guard->nodeCount(); ++i) {
            if (auto* lim = dynamic_cast<PeakLimiterNode*>(guard->node(i))) {
                return lim->isBypassed() ? 0 : static_cast<int>(lim->latencyFrames());
            }
        }
        return 0;
    }

    [[nodiscard]] dsp::ParameterCommandQueue& paramQueue() noexcept { return m_paramQueue; }

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

    /// Control Thread DSP configuration methods (P0-14):
    /// Calculations, exponentiation, buffer sizing, and state publication occur strictly on Control Thread.
    void updateCompressor(double thresholdDb, double ratio, double attackMs, double releaseMs,
                          double makeupDb, double kneeDb, bool enabled) {
        auto guard = m_masterReclaimer.enterRead();
        if (guard) {
            for (size_t i = 0; i < guard->nodeCount(); ++i) {
                if (auto* comp = dynamic_cast<CompressorNode*>(guard->node(i))) {
                    comp->dsp().configure(m_graphSampleRate.load(std::memory_order_relaxed),
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
                    lim->dsp().configure(m_graphSampleRate.load(std::memory_order_relaxed),
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
                    rev->dsp().configure(m_graphSampleRate.load(std::memory_order_relaxed),
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

        // 0. Drain pending DSP parameter commands at the block boundary on Audio Thread within active epoch (P0-06)
        drainParameterCommands(masterGuard.chain, listeningGuard.chain);

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
        case dsp::DspCommandType::SetGeqBandGain:
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* geqNode = dynamic_cast<GraphicEqNode*>(master->node(i))) {
                        geqNode->dsp().setBandGain(cmd.index, static_cast<double>(cmd.param1));
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::ResetAllGeqBands:
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* geqNode = dynamic_cast<GraphicEqNode*>(master->node(i))) {
                        geqNode->dsp().resetAllBands();
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetPeqBand:
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* peqNode = dynamic_cast<ParametricEqNode*>(master->node(i))) {
                        ParametricEQ::BandConfig cfg;
                        cfg.frequency = static_cast<double>(cmd.param1);
                        cfg.gainDb = static_cast<double>(cmd.param2);
                        cfg.Q = static_cast<double>(cmd.param3);
                        cfg.type = static_cast<BiquadFilterType>(cmd.intParam1);
                        cfg.enabled = (cmd.boolFlag1 != 0);
                        if (cmd.subIndex == 1) {
                            peqNode->dsp().setRightBand(cmd.index, cfg);
                        } else {
                            peqNode->dsp().setLeftBand(cmd.index, cfg);
                        }
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetPeqBandEnabled:
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* peqNode = dynamic_cast<ParametricEqNode*>(master->node(i))) {
                        if (cmd.subIndex == 1) {
                            auto cfg = peqNode->dsp().rightBand(cmd.index);
                            cfg.enabled = (cmd.boolFlag1 != 0);
                            peqNode->dsp().setRightBand(cmd.index, cfg);
                        } else {
                            auto cfg = peqNode->dsp().leftBand(cmd.index);
                            cfg.enabled = (cmd.boolFlag1 != 0);
                            peqNode->dsp().setLeftBand(cmd.index, cfg);
                        }
                        break;
                    }
                }
            }
            break;
        case dsp::DspCommandType::ApplyPeqPreset:
            {
                const char* presetNames[] = {
                    "Monitor Flat", "Acoustic Clarity", "Harman Target", "Bass Punch", "Vocal Air"
                };
                if (cmd.subIndex == 1) {
                    if (listening) {
                        for (size_t i = 0; i < listening->nodeCount(); ++i) {
                            if (auto* peqNode = dynamic_cast<ParametricEqNode*>(listening->node(i))) {
                                if (cmd.index < 5) {
                                    peqNode->dsp().applyPreset(presetNames[cmd.index]);
                                }
                                break;
                            }
                        }
                    }
                } else {
                    if (master) {
                        for (size_t i = 0; i < master->nodeCount(); ++i) {
                            if (auto* peqNode = dynamic_cast<ParametricEqNode*>(master->node(i))) {
                                if (cmd.index < 5) {
                                    peqNode->dsp().applyPreset(presetNames[cmd.index]);
                                }
                                break;
                            }
                        }
                    }
                }
            }
            break;
        case dsp::DspCommandType::SetFilterParams:
            if (master) {
                for (size_t i = 0; i < master->nodeCount(); ++i) {
                    if (auto* filterNode = dynamic_cast<HighLowPassNode*>(master->node(i))) {
                        filterNode->dsp().setHpfFrequency(static_cast<double>(cmd.param1));
                        filterNode->dsp().setLpfFrequency(static_cast<double>(cmd.param2));
                        filterNode->dsp().setHpfSlope(static_cast<HighLowPassFilter::FilterSlope>(cmd.intParam1));
                        filterNode->dsp().setLpfSlope(static_cast<HighLowPassFilter::FilterSlope>(cmd.intParam2));
                        filterNode->dsp().setHpfEnabled(cmd.boolFlag1 != 0);
                        filterNode->dsp().setLpfEnabled(cmd.boolFlag2 != 0);
                        break;
                    }
                }
            }
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

    std::atomic<double> m_graphSampleRate;
    std::atomic<OutputPathMode> m_outputMode{OutputPathMode::ListeningPath};
    std::atomic<double> m_listeningVolume{1.0};
    std::atomic<double> m_renderMonitorVolume{1.0};

    dsp::ParameterCommandQueue m_paramQueue;

    mutable EpochReclaimer<AudioChain> m_masterReclaimer;
    mutable EpochReclaimer<AudioChain> m_listeningReclaimer;
};

} // namespace desk32::engine::pipeline
