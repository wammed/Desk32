#pragma once

#include "AudioChain.hpp"
#include "BuiltInDspNodes.hpp"
#include "engine/dsp/Resampler.hpp"
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
    void setGraphSampleRate(double sampleRate) {
        m_graphSampleRate.store(sampleRate, std::memory_order_relaxed);
        auto* master = m_masterReclaimer.current();
        if (master) {
            master->configure(sampleRate);
        }
        auto* listening = m_listeningReclaimer.current();
        if (listening) {
            listening->configure(sampleRate);
        }
    }

    [[nodiscard]] OutputPathMode outputMode() const noexcept { return m_outputMode.load(); }
    void setOutputMode(OutputPathMode mode) noexcept { m_outputMode.store(mode); }

    [[nodiscard]] double listeningVolume() const noexcept { return m_listeningVolume.load(); }
    void setListeningVolume(double vol) noexcept { m_listeningVolume.store(std::clamp(vol, 0.0, 2.0)); }

    [[nodiscard]] double renderMonitorVolume() const noexcept { return m_renderMonitorVolume.load(); }
    void setRenderMonitorVolume(double vol) noexcept { m_renderMonitorVolume.store(std::clamp(vol, 0.0, 2.0)); }

    /// Hot-swap Mastering Chain (Control Thread execution).
    /// Safe, lock-free swap verified by EpochReclaimer.
    void swapMasterChain(std::unique_ptr<AudioChain> newChain) {
        if (!newChain || newChain->purpose() != ChainPurpose::MasteringChain) return;
        newChain->configure(m_graphSampleRate);
        m_masterReclaimer.swapChain(std::move(newChain));
    }

    /// Hot-swap Listening Correction Chain (Control Thread execution).
    void swapListeningChain(std::unique_ptr<AudioChain> newChain) {
        if (!newChain || newChain->purpose() != ChainPurpose::ListeningCorrectionChain) return;
        newChain->configure(m_graphSampleRate);
        m_listeningReclaimer.swapChain(std::move(newChain));
    }

    /// Trigger periodic epoch reclamation outside Audio Thread (Control / Worker Thread)
    void reclaimRetiredMemory() {
        m_masterReclaimer.reclaimRetired();
        m_listeningReclaimer.reclaimRetired();
    }

    /// Real-time audio rendering call (Audio Thread execution).
    /// Guarantees:
    /// - Operates on planar double buffers through DSP & Listening Correction.
    /// - Strictly zero heap allocation or blocking locks.
    /// - Branches at Logical Render Tap: Render Monitor bypasses Listening Correction with 0.000000 leakage.
    /// - Converts to target device PCM format with TPDF dither via FormatConverter at the boundary.
    void process(double* left, double* right, size_t frameCount,
                 void* outPcmBuffer, SampleFormat targetFormat, bool applyDither = true)
    {
        // 1. Enter epoch to protect current active chains from destruction during Audio Thread execution
        auto masterGuard = m_masterReclaimer.enterRead();
        auto listeningGuard = m_listeningReclaimer.enterRead();

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
    std::atomic<double> m_graphSampleRate;
    std::atomic<OutputPathMode> m_outputMode{OutputPathMode::ListeningPath};
    std::atomic<double> m_listeningVolume{1.0};
    std::atomic<double> m_renderMonitorVolume{1.0};

    EpochReclaimer<AudioChain> m_masterReclaimer;
    EpochReclaimer<AudioChain> m_listeningReclaimer;
};

} // namespace desk32::engine::pipeline
