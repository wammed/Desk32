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
    static constexpr size_t MAX_NODES = 16; ///< Upper bound on DSP nodes per chain for bounded O(N) traversal (P0-29)

    explicit AudioChain(std::string name, ChainPurpose purpose)
        : m_name(std::move(name))
        , m_purpose(purpose)
    {
        m_nodes.reserve(MAX_NODES);
    }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] ChainPurpose purpose() const noexcept { return m_purpose; }

    /// Deep-clone chain and all contained DSP nodes (P0-08)
    [[nodiscard]] std::unique_ptr<AudioChain> clone() const {
        auto copy = std::make_unique<AudioChain>(m_name, m_purpose);
        for (const auto& node : m_nodes) {
            if (node) {
                copy->appendNode(node->clone());
            }
        }
        copy->setBypassed(isBypassed());
        return copy;
    }

    /// Append node to chain with structural verification (Section 9.2).
    /// Returns false and rejects the node if affiliation rules are violated:
    /// - NodeAffiliation::ListeningCorrection can ONLY be added to ChainPurpose::ListeningCorrectionChain.
    /// - NodeAffiliation::RenderMonitorOnly can ONLY be added to ChainPurpose::RenderMonitorChain.
    bool appendNode(std::shared_ptr<AudioNode> node) {
        if (!node) {
            m_rejectedNodeCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (m_nodes.size() >= MAX_NODES) {
            m_rejectedNodeCount.fetch_add(1, std::memory_order_relaxed);
            return false; // Enforce bounded chain size (P0-29)
        }

        if (m_purpose == ChainPurpose::MasteringChain) {
            if (node->affiliation() == NodeAffiliation::ListeningCorrection ||
                node->affiliation() == NodeAffiliation::RenderMonitorOnly) {
                m_rejectedNodeCount.fetch_add(1, std::memory_order_relaxed);
                return false; // Structurally forbidden!
            }
        } else if (m_purpose == ChainPurpose::RenderMonitorChain) {
            if (node->affiliation() == NodeAffiliation::ListeningCorrection) {
                m_rejectedNodeCount.fetch_add(1, std::memory_order_relaxed);
                return false; // Listening Correction must NEVER enter Render Monitor
            }
        }

        m_nodes.push_back(std::move(node));
        return true;
    }

    /// Remove node at index (Control Thread / Chain Builder only, P0-29).
    bool removeNode(size_t index) {
        if (index >= m_nodes.size()) return false;
        m_nodes.erase(m_nodes.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    /// Clear all nodes from chain (Control Thread / Chain Builder only, P0-29).
    void clearNodes() {
        m_nodes.clear();
    }

    [[nodiscard]] uint64_t rejectedNodeCount() const noexcept {
        return m_rejectedNodeCount.load(std::memory_order_relaxed);
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

    /// Reclaim retired DSP instances across all nodes outside Audio Thread (P0-21)
    void reclaimRetiredNodes() noexcept {
        for (auto& node : m_nodes) {
            if (node) {
                node->reclaimRetired();
            }
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
    std::atomic<uint64_t> m_rejectedNodeCount{0};
};

} // namespace desk32::engine::pipeline

