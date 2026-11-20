#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>

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
        m_configured.store(true, std::memory_order_relaxed);
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
    virtual void reset() {}

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
};

} // namespace desk32::engine::pipeline
