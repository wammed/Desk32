#pragma once

#include "AudioNode.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace desk32::engine::pipeline {

enum class ChainPurpose {
    MasteringChain,          ///< Upstream master chain before Logical Render Tap
    ListeningCorrectionChain,///< Downstream listening path (headphone/speaker correction)
    RenderMonitorChain       ///< Downstream render monitor audition path
};

class AudioChain {
public:
    static constexpr size_t MAX_NODES = 16; ///< Upper bound on DSP nodes per chain for bounded O(N) traversal (P0-29)
    static constexpr size_t DRY_SCRATCH_FRAMES = AudioNode::DEFAULT_MAX_BLOCK_SIZE;

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
        m_hasProcessedBlock = false;
        m_chainFadeRemaining = 0;
        m_nodeFadeRemaining.fill(0);
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

    /// Real-time audio processing (zero heap allocations).
    /// Executes sequentially across active nodes and applies ~5ms linear dry/wet smoothing on bypass transitions (§5.9).
    inline void process(double* left, double* right, size_t frameCount) {
        const bool curChainBypass = m_bypassed.load(std::memory_order_relaxed);
        const size_t nNodes = std::min(m_nodes.size(), MAX_NODES);

        if (!m_hasProcessedBlock) {
            m_prevChainBypassed = curChainBypass;
            for (size_t i = 0; i < nNodes; ++i) {
                m_prevNodeBypassed[i] = m_nodes[i]->isBypassed();
            }
            m_hasProcessedBlock = true;
            if (curChainBypass) {
                return;
            }
            for (size_t i = 0; i < nNodes; ++i) {
                auto& node = m_nodes[i];
                if (!node->isBypassed() || node->wantsProcessWhileBypassed()) {
                    node->process(left, right, frameCount);
                }
            }
            return;
        }

        const size_t fadeLen = static_cast<size_t>(std::clamp(m_sampleRate * 0.005, 16.0, 512.0));
        if (curChainBypass != m_prevChainBypassed) {
            m_chainFadeTotal = fadeLen;
            m_chainFadeRemaining = fadeLen;
            m_chainFadeToBypass = curChainBypass;
            m_prevChainBypassed = curChainBypass;
        }

        if (curChainBypass && m_chainFadeRemaining == 0) {
            return;
        }

        const bool applyChainFade = (m_chainFadeRemaining > 0) && (frameCount <= DRY_SCRATCH_FRAMES);
        if (applyChainFade) {
            std::memcpy(m_chainDryL.data(), left, frameCount * sizeof(double));
            std::memcpy(m_chainDryR.data(), right, frameCount * sizeof(double));
        } else if (curChainBypass) {
            return;
        }

        for (size_t i = 0; i < nNodes; ++i) {
            auto& node = m_nodes[i];
            const bool nodeBypass = node->isBypassed();
            const bool tailBypass = (node->bypassBehavior() == BypassBehavior::TailPreserving);

            if (nodeBypass != m_prevNodeBypassed[i]) {
                m_prevNodeBypassed[i] = nodeBypass;
                if (!tailBypass && !curChainBypass) {
                    m_nodeFadeTotal[i] = fadeLen;
                    m_nodeFadeRemaining[i] = fadeLen;
                    m_nodeFadeToBypass[i] = nodeBypass;
                } else {
                    m_nodeFadeRemaining[i] = 0;
                }
            }

            const bool fadingNode = (m_nodeFadeRemaining[i] > 0) && (frameCount <= DRY_SCRATCH_FRAMES);
            if (!nodeBypass) {
                if (fadingNode) {
                    std::memcpy(m_nodeDryL.data(), left, frameCount * sizeof(double));
                    std::memcpy(m_nodeDryR.data(), right, frameCount * sizeof(double));
                    node->process(left, right, frameCount);
                    applyCrossfade(left, right, m_nodeDryL.data(), m_nodeDryR.data(),
                                   frameCount, m_nodeFadeRemaining[i], m_nodeFadeTotal[i], m_nodeFadeToBypass[i]);
                } else {
                    m_nodeFadeRemaining[i] = 0;
                    node->process(left, right, frameCount);
                }
            } else if (tailBypass && node->wantsProcessWhileBypassed()) {
                m_nodeFadeRemaining[i] = 0;
                node->process(left, right, frameCount);
            } else if (fadingNode && m_nodeFadeToBypass[i]) {
                std::memcpy(m_nodeDryL.data(), left, frameCount * sizeof(double));
                std::memcpy(m_nodeDryR.data(), right, frameCount * sizeof(double));
                node->setRtFadeForceActive(true);
                node->process(left, right, frameCount);
                node->setRtFadeForceActive(false);
                applyCrossfade(left, right, m_nodeDryL.data(), m_nodeDryR.data(),
                               frameCount, m_nodeFadeRemaining[i], m_nodeFadeTotal[i], true);
            } else {
                m_nodeFadeRemaining[i] = 0;
            }
        }

        if (applyChainFade) {
            applyCrossfade(left, right, m_chainDryL.data(), m_chainDryR.data(),
                           frameCount, m_chainFadeRemaining, m_chainFadeTotal, m_chainFadeToBypass);
        }
    }

private:
    static inline void applyCrossfade(double* wetL, double* wetR,
                                      const double* dryL, const double* dryR,
                                      size_t frameCount,
                                      size_t& fadeRemaining, size_t fadeTotal,
                                      bool fadeToBypass) noexcept
    {
        if (fadeTotal == 0 || fadeRemaining == 0) {
            fadeRemaining = 0;
            return;
        }
        const double invTotal = 1.0 / static_cast<double>(fadeTotal);
        for (size_t i = 0; i < frameCount; ++i) {
            if (fadeRemaining > 0) {
                const size_t done = fadeTotal - fadeRemaining;
                const double progress = static_cast<double>(done) * invTotal;
                const double wetWeight = fadeToBypass ? (1.0 - progress) : progress;
                const double dryWeight = 1.0 - wetWeight;
                wetL[i] = wetL[i] * wetWeight + dryL[i] * dryWeight;
                wetR[i] = wetR[i] * wetWeight + dryR[i] * dryWeight;
                --fadeRemaining;
            } else if (fadeToBypass) {
                wetL[i] = dryL[i];
                wetR[i] = dryR[i];
            }
        }
    }

    std::string m_name;
    ChainPurpose m_purpose;
    std::vector<std::shared_ptr<AudioNode>> m_nodes;
    double m_sampleRate = 48000.0;
    std::atomic<bool> m_bypassed{false};
    std::atomic<uint64_t> m_rejectedNodeCount{0};

    bool m_hasProcessedBlock{false};
    bool m_prevChainBypassed{false};
    size_t m_chainFadeRemaining{0};
    size_t m_chainFadeTotal{0};
    bool m_chainFadeToBypass{false};

    std::array<bool, MAX_NODES> m_prevNodeBypassed{};
    std::array<size_t, MAX_NODES> m_nodeFadeRemaining{};
    std::array<size_t, MAX_NODES> m_nodeFadeTotal{};
    std::array<bool, MAX_NODES> m_nodeFadeToBypass{};

    alignas(64) std::array<double, DRY_SCRATCH_FRAMES> m_chainDryL{};
    alignas(64) std::array<double, DRY_SCRATCH_FRAMES> m_chainDryR{};
    alignas(64) std::array<double, DRY_SCRATCH_FRAMES> m_nodeDryL{};
    alignas(64) std::array<double, DRY_SCRATCH_FRAMES> m_nodeDryR{};
};

} // namespace desk32::engine::pipeline
