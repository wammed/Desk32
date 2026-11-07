#pragma once

#include "AudioNode.hpp"
#include <vector>
#include <memory>
#include <string>
#include <atomic>

namespace desk32::engine::pipeline {

enum class ChainPurpose {
    MasteringChain,          ///< Upstream master chain before Logical Render Tap
    ListeningCorrectionChain,///< Downstream listening path (headphone/speaker correction)
    RenderMonitorChain       ///< Downstream render monitor audition path
};

class AudioChain {
public:
    explicit AudioChain(std::string name, ChainPurpose purpose)
        : m_name(std::move(name))
        , m_purpose(purpose)
    {}

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] ChainPurpose purpose() const noexcept { return m_purpose; }

    /// Append node to chain with structural verification (Section 9.2).
    /// Returns false and rejects the node if affiliation rules are violated:
    /// - NodeAffiliation::ListeningCorrection can ONLY be added to ChainPurpose::ListeningCorrectionChain.
    /// - NodeAffiliation::RenderMonitorOnly can ONLY be added to ChainPurpose::RenderMonitorChain.
    bool appendNode(std::shared_ptr<AudioNode> node) {
        if (!node) return false;

        if (m_purpose == ChainPurpose::MasteringChain) {
            if (node->affiliation() == NodeAffiliation::ListeningCorrection ||
                node->affiliation() == NodeAffiliation::RenderMonitorOnly) {
                return false; // Structurally forbidden!
            }
        } else if (m_purpose == ChainPurpose::RenderMonitorChain) {
            if (node->affiliation() == NodeAffiliation::ListeningCorrection) {
                return false; // Listening Correction must NEVER enter Render Monitor
            }
        }

        m_nodes.push_back(std::move(node));
        return true;
    }

    void configure(double sampleRate) {
        m_sampleRate = sampleRate;
        for (auto& node : m_nodes) {
            node->configure(sampleRate);
        }
    }

    void reset() {
        for (auto& node : m_nodes) {
            node->reset();
        }
    }

    [[nodiscard]] size_t nodeCount() const noexcept { return m_nodes.size(); }
    [[nodiscard]] const std::vector<std::shared_ptr<AudioNode>>& nodes() const noexcept { return m_nodes; }
    [[nodiscard]] AudioNode* node(size_t index) noexcept { return index < m_nodes.size() ? m_nodes[index].get() : nullptr; }
    [[nodiscard]] const AudioNode* node(size_t index) const noexcept { return index < m_nodes.size() ? m_nodes[index].get() : nullptr; }
    [[nodiscard]] std::shared_ptr<AudioNode> nodeShared(size_t index) const noexcept { return index < m_nodes.size() ? m_nodes[index] : nullptr; }

    [[nodiscard]] int64_t totalLatencyFrames() const noexcept {
        int64_t total = 0;
        for (const auto& node : m_nodes) {
            if (!node->isBypassed()) {
                total += node->latencyFrames();
            }
        }
        return total;
    }

    [[nodiscard]] bool isBypassed() const noexcept { return m_bypassed.load(std::memory_order_relaxed); }
    void setBypassed(bool bypassed) noexcept { m_bypassed.store(bypassed, std::memory_order_relaxed); }

    /// Real-time audio processing.
    /// Executes sequentially across all active nodes in the chain.
    inline void process(double* left, double* right, size_t frameCount) {
        if (m_bypassed.load(std::memory_order_relaxed)) return;

        for (auto& node : m_nodes) {
            if (!node->isBypassed()) {
                node->process(left, right, frameCount);
            }
        }
    }

private:
    std::string m_name;
    ChainPurpose m_purpose;
    std::vector<std::shared_ptr<AudioNode>> m_nodes;
    double m_sampleRate = 48000.0;
    std::atomic<bool> m_bypassed{false};
};

} // namespace desk32::engine::pipeline

