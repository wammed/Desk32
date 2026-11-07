#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <atomic>

namespace desk32::engine::pipeline {

/// Node affiliation to enforce specification Section 9.2:
/// "Nodes belonging to the Output Path declare Listening-only, Render-common, or Render Monitor-only,
/// structurally verifying that Listening Correction cannot be connected to Render branches."
enum class NodeAffiliation {
    RenderCommon,        ///< General DSP / Mastering (usable in standard master & export chains)
    ListeningCorrection, ///< Listening-only correction (STRICTLY FORBIDDEN from Render / File Output branches)
    RenderMonitorOnly    ///< Render Monitor specific audition path
};

class AudioNode {
public:
    explicit AudioNode(std::string name, NodeAffiliation affiliation)
        : m_name(std::move(name))
        , m_affiliation(affiliation)
    {}

    virtual ~AudioNode() = default;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] NodeAffiliation affiliation() const noexcept { return m_affiliation; }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    virtual void configure(double sampleRate) {
        m_sampleRate = sampleRate;
    }

    [[nodiscard]] bool isBypassed() const noexcept { return m_bypassed.load(std::memory_order_relaxed); }
    virtual void setBypassed(bool bypassed) noexcept { m_bypassed.store(bypassed, std::memory_order_relaxed); }

    [[nodiscard]] virtual int64_t latencyFrames() const noexcept { return 0; }
    virtual void reset() {}

    /// Real-time audio processing callback.
    /// Audio Thread execution guarantees:
    /// - Zero dynamic memory allocations or deallocations
    /// - Zero blocking locks or waits
    /// - Operates on 64-bit float (double) planar/deinterleaved buffers
    virtual void process(double* left, double* right, size_t frameCount) = 0;

protected:
    std::string m_name;
    NodeAffiliation m_affiliation;
    double m_sampleRate = 48000.0;
    std::atomic<bool> m_bypassed{false};
};

} // namespace desk32::engine::pipeline

