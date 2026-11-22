#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <memory>
#include "engine/dsp/ParameterSmoother.hpp"

namespace desk32::engine::pipeline {

/// Node affiliation to enforce specification Section 9.2:
/// "Nodes belonging to the Output Path declare Listening-only, Render-common, or Render Monitor-only,
/// structurally verifying that Listening Correction cannot be connected to Render branches."
enum class NodeAffiliation {
    RenderCommon,        ///< General DSP / Mastering (usable in standard master & export chains)
    ListeningCorrection, ///< Listening-only correction (STRICTLY FORBIDDEN from Render / File Output branches)
    RenderMonitorOnly    ///< Render Monitor specific audition path
};

enum class ProcessingSampleFormat {
    Float64Planar
};

enum class NodeState {
    Uninitialized = 0,
    Active = 1,
    Bypassed = 2,
    TailRingout = 3,
    Error = 4
};

enum class BypassBehavior {
    Immediate = 0,
    TailPreserving = 1
};

/// Spec §9.2 explicit AudioNode processing contract descriptor.
struct AudioNodeContract {
    std::string name;
    NodeAffiliation affiliation = NodeAffiliation::RenderCommon;
    uint32_t inputChannels = 2;
    uint32_t outputChannels = 2;
    ProcessingSampleFormat sampleFormat = ProcessingSampleFormat::Float64Planar;
    size_t maxBlockSize = 4096;
    double sampleRate = 48000.0;
    int64_t latencyFrames = 0;
    NodeState state = NodeState::Uninitialized;
    BypassBehavior bypassBehavior = BypassBehavior::Immediate;
};

class AudioNode {
public:
    static constexpr size_t DEFAULT_MAX_BLOCK_SIZE = 4096;

    explicit AudioNode(std::string name, NodeAffiliation affiliation)
        : m_name(std::move(name))
        , m_affiliation(affiliation)
    {}

    virtual ~AudioNode() = default;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] NodeAffiliation affiliation() const noexcept { return m_affiliation; }

    [[nodiscard]] virtual uint32_t inputChannels() const noexcept { return 2; }
    [[nodiscard]] virtual uint32_t outputChannels() const noexcept { return 2; }
    [[nodiscard]] virtual ProcessingSampleFormat sampleFormat() const noexcept {
        return ProcessingSampleFormat::Float64Planar;
    }
    [[nodiscard]] virtual size_t maxBlockSize() const noexcept { return m_maxBlockSize; }
    virtual void setMaxBlockSize(size_t maxFrames) noexcept {
        if (maxFrames > 0) {
            m_maxBlockSize = maxFrames;
        }
    }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    virtual void configure(double sampleRate) {
        m_sampleRate = sampleRate;
        m_inputTrimSmoother.configure(sampleRate, 10.0);
        m_outputTrimSmoother.configure(sampleRate, 10.0);
        m_configured.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] static inline double dbToLinearExact(double db) noexcept {
        const double clamped = std::clamp(db, -12.0, 12.0);
        return (std::abs(clamped) <= 1e-9) ? 1.0 : std::pow(10.0, clamped / 20.0);
    }

    void setInputTrimDb(double db, bool snapImmediate = false) noexcept {
        const double clamped = std::clamp(db, -12.0, 12.0);
        const double cleanDb = (std::abs(clamped) <= 1e-9) ? 0.0 : clamped;
        const double lin = dbToLinearExact(cleanDb);
        m_inputTrimDb.store(cleanDb, std::memory_order_relaxed);
        m_inputTrimTargetLinear.store(lin, std::memory_order_release);
        if (snapImmediate) {
            m_inputTrimSmoother.reset(lin);
            m_trimSmoothersInitialized = true;
        }
    }

    void setOutputTrimDb(double db, bool snapImmediate = false) noexcept {
        const double clamped = std::clamp(db, -12.0, 12.0);
        const double cleanDb = (std::abs(clamped) <= 1e-9) ? 0.0 : clamped;
        const double lin = dbToLinearExact(cleanDb);
        m_outputTrimDb.store(cleanDb, std::memory_order_relaxed);
        m_outputTrimTargetLinear.store(lin, std::memory_order_release);
        if (snapImmediate) {
            m_outputTrimSmoother.reset(lin);
            m_trimSmoothersInitialized = true;
        }
    }

    [[nodiscard]] double inputTrimDb() const noexcept {
        return m_inputTrimDb.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double outputTrimDb() const noexcept {
        return m_outputTrimDb.load(std::memory_order_relaxed);
    }

    void copyTrimFrom(const AudioNode& other, bool snapImmediate = true) noexcept {
        setInputTrimDb(other.inputTrimDb(), snapImmediate);
        setOutputTrimDb(other.outputTrimDb(), snapImmediate);
    }

    /// Apply input trim in-place (remains active even when the node is bypassed).
    /// Bit-exact no-op when target linear gain == 1.0 (0.0 dB) and smoother is settled.
    virtual void applyInputTrim(double* left, double* right, size_t frameCount) noexcept {
        applyTrimImpl(left, right, frameCount, m_inputTrimTargetLinear.load(std::memory_order_acquire),
                      m_inputTrimSmoother, m_inputTrimSmootherInit);
    }

    /// Apply output trim in-place (remains active even when the node is bypassed).
    /// Bit-exact no-op when target linear gain == 1.0 (0.0 dB) and smoother is settled.
    virtual void applyOutputTrim(double* left, double* right, size_t frameCount) noexcept {
        applyTrimImpl(left, right, frameCount, m_outputTrimTargetLinear.load(std::memory_order_acquire),
                      m_outputTrimSmoother, m_outputTrimSmootherInit);
    }

    /// Hook for composite nodes whose internal sub-stages carry trims while bypassed.
    virtual void processBypassedTrimsOnly(double* /*left*/, double* /*right*/, size_t /*frameCount*/) noexcept {}

    /// Static non-advancing versions used on dry scratch during global chain bypass crossfade.
    virtual void applyInputTrimStatic(double* left, double* right, size_t frameCount) const noexcept {
        applyTrimStaticImpl(left, right, frameCount,
                            m_inputTrimSmootherInit ? m_inputTrimSmoother.current()
                                                    : m_inputTrimTargetLinear.load(std::memory_order_acquire));
    }
    virtual void applyOutputTrimStatic(double* left, double* right, size_t frameCount) const noexcept {
        applyTrimStaticImpl(left, right, frameCount,
                            m_outputTrimSmootherInit ? m_outputTrimSmoother.current()
                                                     : m_outputTrimTargetLinear.load(std::memory_order_acquire));
    }
    virtual void processBypassedTrimsStatic(double* /*left*/, double* /*right*/, size_t /*frameCount*/) const noexcept {}

    /// Fade-pair hook for nodes whose internal sub-stages sit between trims and filters.
    virtual void processFadePair(double* wetLeft, double* wetRight,
                                 double* /*dryLeft*/, double* /*dryRight*/,
                                 size_t frameCount) {
        process(wetLeft, wetRight, frameCount);
    }

    [[nodiscard]] virtual bool isBypassed() const noexcept { return m_bypassed.load(std::memory_order_relaxed); }
    virtual void setBypassed(bool bypassed) noexcept { m_bypassed.store(bypassed, std::memory_order_relaxed); }

    [[nodiscard]] virtual BypassBehavior bypassBehavior() const noexcept {
        return m_bypassBehavior.load(std::memory_order_relaxed);
    }
    virtual void setBypassBehavior(BypassBehavior behavior) noexcept {
        m_bypassBehavior.store(behavior, std::memory_order_relaxed);
    }

    /// True if AudioChain should still invoke process() when isBypassed() == true (e.g. TailPreserving Reverb).
    [[nodiscard]] virtual bool wantsProcessWhileBypassed() const noexcept { return false; }

    [[nodiscard]] virtual NodeState nodeState() const noexcept {
        if (m_hasError.load(std::memory_order_relaxed)) return NodeState::Error;
        if (!m_configured.load(std::memory_order_relaxed)) return NodeState::Uninitialized;
        return isBypassed() ? NodeState::Bypassed : NodeState::Active;
    }

    [[nodiscard]] virtual int64_t latencyFrames() const noexcept { return 0; }
    virtual void reset() {
        m_inputTrimSmootherInit = false;
        m_outputTrimSmootherInit = false;
        m_trimSmoothersInitialized = false;
    }

    /// Non-RT reclamation of retired DSP instances (P0-21)
    virtual void reclaimRetired() noexcept {}

    /// Real-time safe deep clone of node state for graph reconstruction (P0-08)
    [[nodiscard]] virtual std::shared_ptr<AudioNode> clone() const = 0;

    [[nodiscard]] virtual AudioNodeContract contract() const {
        AudioNodeContract c;
        c.name = m_name;
        c.affiliation = m_affiliation;
        c.inputChannels = inputChannels();
        c.outputChannels = outputChannels();
        c.sampleFormat = sampleFormat();
        c.maxBlockSize = maxBlockSize();
        c.sampleRate = m_sampleRate;
        c.latencyFrames = latencyFrames();
        c.state = nodeState();
        c.bypassBehavior = bypassBehavior();
        return c;
    }

    /// Real-time audio processing callback.
    /// Audio Thread execution guarantees:
    /// - Zero dynamic memory allocations or deallocations
    /// - Zero blocking locks or waits
    /// - Operates on 64-bit float (double) planar/deinterleaved buffers
    virtual void process(double* left, double* right, size_t frameCount) = 0;

    /// Internal RT-thread-only helper used by AudioChain during ~5ms active->bypass fade-out.
    void setRtFadeForceActive(bool force) noexcept { m_rtFadeForceActive = force; }

protected:
    static inline void applyTrimImpl(double* left, double* right, size_t frameCount,
                                     double targetLin, dsp::ParameterSmoother& smoother,
                                     bool& initialized) noexcept {
        if (!left || !right || frameCount == 0) return;
        if (!initialized) {
            smoother.reset(targetLin);
            initialized = true;
        } else {
            smoother.setTarget(targetLin);
        }
        if (!smoother.isSmoothing()) {
            const double g = smoother.target();
            if (g == 1.0) {
                return;
            }
            for (size_t i = 0; i < frameCount; ++i) {
                left[i] *= g;
                right[i] *= g;
            }
            return;
        }
        for (size_t i = 0; i < frameCount; ++i) {
            const double g = smoother.next();
            left[i] *= g;
            right[i] *= g;
        }
    }

    static inline void applyTrimStaticImpl(double* left, double* right, size_t frameCount,
                                           double gain) noexcept {
        if (!left || !right || frameCount == 0 || gain == 1.0) return;
        for (size_t i = 0; i < frameCount; ++i) {
            left[i] *= gain;
            right[i] *= gain;
        }
    }

    [[nodiscard]] bool isEffectivelyBypassed() const noexcept {
        return isBypassed() && !m_rtFadeForceActive;
    }

    std::string m_name;
    NodeAffiliation m_affiliation;
    double m_sampleRate = 48000.0;
    size_t m_maxBlockSize = DEFAULT_MAX_BLOCK_SIZE;
    std::atomic<bool> m_bypassed{false};
    std::atomic<BypassBehavior> m_bypassBehavior{BypassBehavior::Immediate};
    std::atomic<bool> m_configured{false};
    std::atomic<bool> m_hasError{false};
    bool m_rtFadeForceActive{false};
    std::atomic<double> m_inputTrimDb{0.0};
    std::atomic<double> m_outputTrimDb{0.0};
    std::atomic<double> m_inputTrimTargetLinear{1.0};
    std::atomic<double> m_outputTrimTargetLinear{1.0};
    dsp::ParameterSmoother m_inputTrimSmoother{1.0, 10.0, 48000.0};
    dsp::ParameterSmoother m_outputTrimSmoother{1.0, 10.0, 48000.0};
    bool m_inputTrimSmootherInit{false};
    bool m_outputTrimSmootherInit{false};
    bool m_trimSmoothersInitialized{false};
};

} // namespace desk32::engine::pipeline
