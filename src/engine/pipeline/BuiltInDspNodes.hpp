#pragma once

#include "AudioNode.hpp"
#include "engine/dsp/ParametricEQ.hpp"
#include "engine/dsp/GraphicEQ.hpp"
#include "engine/dsp/HighLowPassFilter.hpp"
#include "engine/dsp/Compressor.hpp"
#include "engine/dsp/PeakLimiter.hpp"
#include "engine/dsp/AlgorithmicReverb.hpp"
#include <memory>

namespace desk32::engine::pipeline {

/// Node 1: Parametric Equalizer (Master or Listening Correction)
class ParametricEqNode : public AudioNode {
public:
    explicit ParametricEqNode(std::string name = "Parametric EQ", NodeAffiliation affiliation = NodeAffiliation::RenderCommon)
        : AudioNode(std::move(name), affiliation)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_eq.configure(sampleRate);
    }

    void reset() override {
        m_eq.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_eq.process(left[i], right[i]);
        }
    }

    [[nodiscard]] ParametricEQ& dsp() noexcept { return m_eq; }
    [[nodiscard]] const ParametricEQ& dsp() const noexcept { return m_eq; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<ParametricEqNode>(m_name, m_affiliation);
        node->setBypassed(isBypassed());
        node->m_eq = m_eq;
        return node;
    }

private:
    ParametricEQ m_eq;
};

/// Node 2: 32-band Graphic Equalizer (Console-style GEQ)
class GraphicEqNode : public AudioNode {
public:
    explicit GraphicEqNode(std::string name = "Graphic EQ")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_geq.configure(sampleRate);
    }

    void reset() override {
        m_geq.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_geq.process(left[i], right[i]);
        }
    }

    [[nodiscard]] GraphicEQ& dsp() noexcept { return m_geq; }
    [[nodiscard]] const GraphicEQ& dsp() const noexcept { return m_geq; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<GraphicEqNode>(m_name);
        node->setBypassed(isBypassed());
        node->m_geq = m_geq;
        return node;
    }

private:
    GraphicEQ m_geq;
};

/// Node 3: High-Pass / Low-Pass Filter
class HighLowPassNode : public AudioNode {
public:
    explicit HighLowPassNode(std::string name = "HPF / LPF")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_filter.configure(sampleRate);
    }

    void reset() override {
        m_filter.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_filter.process(left[i], right[i]);
        }
    }

    [[nodiscard]] HighLowPassFilter& dsp() noexcept { return m_filter; }
    [[nodiscard]] const HighLowPassFilter& dsp() const noexcept { return m_filter; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<HighLowPassNode>(m_name);
        node->setBypassed(isBypassed());
        node->m_filter = m_filter;
        return node;
    }

private:
    HighLowPassFilter m_filter;
};

/// Node 4: Stereo Compressor
class CompressorNode : public AudioNode {
public:
    explicit CompressorNode(std::string name = "Compressor")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_comp.configure(sampleRate);
    }

    void reset() override {
        m_comp.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_comp.process(left[i], right[i]);
        }
    }

    [[nodiscard]] Compressor& dsp() noexcept { return m_comp; }
    [[nodiscard]] const Compressor& dsp() const noexcept { return m_comp; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<CompressorNode>(m_name);
        node->setBypassed(isBypassed());
        node->dsp().configure(m_comp.sampleRate(), m_comp.thresholdDb(), m_comp.ratio(),
                              m_comp.attackMs(), m_comp.releaseMs(), m_comp.makeupGainDb(),
                              m_comp.kneeDb(), m_comp.isEnabled());
        return node;
    }

private:
    Compressor m_comp;
};

/// Node 5: Sample Peak Limiter (Section 5.5, with lookahead latency reporting)
class PeakLimiterNode : public AudioNode {
public:
    explicit PeakLimiterNode(std::string name = "Peak Limiter")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_limiter.configure(sampleRate);
    }

    void reset() override {
        m_limiter.reset();
    }

    [[nodiscard]] int64_t latencyFrames() const noexcept override {
        return static_cast<int64_t>(m_limiter.lookaheadSamples());
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_limiter.process(left[i], right[i]);
        }
    }

    [[nodiscard]] PeakLimiter& dsp() noexcept { return m_limiter; }
    [[nodiscard]] const PeakLimiter& dsp() const noexcept { return m_limiter; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<PeakLimiterNode>(m_name);
        node->setBypassed(isBypassed());
        node->dsp().configure(m_limiter.sampleRate(), m_limiter.ceilingDb(), m_limiter.releaseMs(),
                              m_limiter.lookaheadMs(), m_limiter.isEnabled());
        return node;
    }

private:
    PeakLimiter m_limiter;
};

/// Node 6: Algorithmic Reverb
class ReverbNode : public AudioNode {
public:
    explicit ReverbNode(std::string name = "Reverb")
        : AudioNode(std::move(name), NodeAffiliation::RenderCommon)
    {}

    void configure(double sampleRate) override {
        AudioNode::configure(sampleRate);
        m_reverb.configure(sampleRate);
    }

    void reset() override {
        m_reverb.reset();
    }

    void process(double* left, double* right, size_t frameCount) override {
        for (size_t i = 0; i < frameCount; ++i) {
            m_reverb.process(left[i], right[i]);
        }
    }

    [[nodiscard]] AlgorithmicReverb& dsp() noexcept { return m_reverb; }
    [[nodiscard]] const AlgorithmicReverb& dsp() const noexcept { return m_reverb; }

    [[nodiscard]] std::shared_ptr<AudioNode> clone() const override {
        auto node = std::make_shared<ReverbNode>(m_name);
        node->setBypassed(isBypassed());
        node->dsp().configure(m_reverb.sampleRate(), m_reverb.roomSize(), m_reverb.damping(),
                              m_reverb.wetLevel(), m_reverb.predelayMs(), m_reverb.isTailPreserving(),
                              m_reverb.isEnabled());
        return node;
    }

private:
    AlgorithmicReverb m_reverb;
};

} // namespace desk32::engine::pipeline
