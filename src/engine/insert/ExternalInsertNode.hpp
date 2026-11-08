#pragma once

#include "engine/pipeline/AudioNode.hpp"
#include "ExternalInsertBridge.hpp"
#include "DelayCompensationBuffer.hpp"
#include <memory>

namespace desk32::engine::insert {

/// External Insert Node on the AudioChain.
/// Provides a single serial external insert connection point via PipeWire/JACK ports to external plugin hosts (e.g., Carla).
/// Features:
/// - Feedback loop detection & automatic bypass
/// - Real-time safe delay compensation (PDC)
/// - Strict separation of reported vs measured latency
/// - Smooth disconnect and recovery handling without clicks or XRUNs
/// - Strictly ZERO dynamic heap allocations in real-time audio callback
class ExternalInsertNode : public pipeline::AudioNode {
public:
    explicit ExternalInsertNode(std::string name = "External Insert",
                                pipeline::NodeAffiliation affiliation = pipeline::NodeAffiliation::RenderCommon,
                                std::shared_ptr<ExternalInsertBridge> bridge = nullptr)
        : pipeline::AudioNode(std::move(name), affiliation)
        , m_bridge(bridge ? std::move(bridge) : std::make_shared<ExternalInsertBridge>())
        , m_delayBuffer(std::make_unique<DelayCompensationBuffer>())
    {
        // Default to bypassed until configured and activated
        setBypassed(true);
    }

    ~ExternalInsertNode() override = default;

    ExternalInsertNode(const ExternalInsertNode&) = delete;
    ExternalInsertNode& operator=(const ExternalInsertNode&) = delete;

    void configure(double sampleRate) override {
        pipeline::AudioNode::configure(sampleRate);
        m_sampleRate = sampleRate;
    }

    void reset() override {
        m_delayBuffer->reset();
    }

    [[nodiscard]] int64_t latencyFrames() const noexcept override {
        return static_cast<int64_t>(m_bridge->reportedLatencyFrames() + m_delayBuffer->delayFrames());
    }

    [[nodiscard]] ExternalInsertBridge& bridge() noexcept { return *m_bridge; }
    [[nodiscard]] const ExternalInsertBridge& bridge() const noexcept { return *m_bridge; }
    [[nodiscard]] std::shared_ptr<ExternalInsertBridge> bridgeShared() const noexcept { return m_bridge; }

    [[nodiscard]] DelayCompensationBuffer& delayBuffer() noexcept { return *m_delayBuffer; }
    [[nodiscard]] const DelayCompensationBuffer& delayBuffer() const noexcept { return *m_delayBuffer; }

    void setBypassed(bool bypassed) noexcept override {
        pipeline::AudioNode::setBypassed(bypassed);
        m_bridge->setBypass(bypassed);
    }

    [[nodiscard]] bool isBypassed() const noexcept override {
        return pipeline::AudioNode::isBypassed() || m_bridge->isBypassed();
    }

    [[nodiscard]] std::shared_ptr<pipeline::AudioNode> clone() const override {
        auto node = std::make_shared<ExternalInsertNode>(m_name, m_affiliation, m_bridge);
        node->setBypassed(isBypassed());
        node->configure(m_sampleRate);
        node->delayBuffer().setDelayFrames(m_delayBuffer->delayFrames());
        return node;
    }

    /// Real-time audio processing callback.
    /// Audio Thread execution guarantees:
    /// - Zero dynamic memory allocations or deallocations
    /// - Zero blocking locks or waits
    /// - Planar double float buffers
    void process(double* left, double* right, size_t frameCount) override {
        if (!left || !right || frameCount == 0) return;

        // 1. If bypassed, pass through untouched (dry)
        if (isBypassed()) {
            return;
        }

        // 2. Process through External Insert Bridge (send to host / receive from host)
        m_bridge->process(left, right, frameCount);

        // 3. Apply Plugin Delay Compensation (PDC) alignment if configured
        m_delayBuffer->process(left, right, frameCount);
    }

private:
    std::shared_ptr<ExternalInsertBridge> m_bridge;
    std::unique_ptr<DelayCompensationBuffer> m_delayBuffer;
};

} // namespace desk32::engine::insert

