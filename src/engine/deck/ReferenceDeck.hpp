#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include "engine/dsp/Resampler.hpp"

namespace desk32::engine::deck {

enum class TransportState {
    Stopped,
    Playing,
    Paused
};

struct TrackInfo {
    std::string path;
    double sampleRate = 44100.0;
    uint32_t channels = 2;
    uint64_t totalFrames = 0;
};

class ReferenceDeck {
public:
    explicit ReferenceDeck(char id = 'A')
        : m_id(id)
    {}

    [[nodiscard]] char id() const noexcept { return m_id; }
    [[nodiscard]] TransportState state() const noexcept { return m_state; }
    [[nodiscard]] uint64_t playhead() const noexcept { return m_playhead; }
    [[nodiscard]] int64_t offsetFrames() const noexcept { return m_offsetFrames; }
    [[nodiscard]] bool isLocked() const noexcept { return m_isLocked; }
    [[nodiscard]] const TrackInfo& trackInfo() const noexcept { return m_track; }

    void setLocked(bool locked) noexcept { m_isLocked = locked; }
    void setOffsetFrames(int64_t offset) noexcept { m_offsetFrames = offset; }

    bool loadTrack(const TrackInfo& info, double graphSampleRate) {
        if (m_isLocked) {
            return false;
        }
        m_track = info;
        m_playhead = 0;
        m_state = TransportState::Stopped;

        // Configure resampler for graph rate
        m_graphRate = graphSampleRate;
        return m_resampler.configure(m_track.sampleRate, m_graphRate, m_track.channels);
    }

    void play() noexcept {
        if (m_track.totalFrames > 0) {
            m_state = TransportState::Playing;
        }
    }

    void pause() noexcept {
        if (m_state == TransportState::Playing) {
            m_state = TransportState::Paused;
        }
    }

    void stop() noexcept {
        m_state = TransportState::Stopped;
        m_playhead = 0;
        m_resampler.reset();
    }

    void seek(uint64_t targetFrame) noexcept {
        m_playhead = std::min(targetFrame, m_track.totalFrames);
        m_resampler.reset();
    }

    /// Render next block of audio at the graph sample rate into interleaved output.
    /// @param outInterleaved Buffer of size [frames * channels]
    /// @param frames Number of graph frames requested
    /// @return Number of frames actually rendered
    size_t renderBlock(double* outInterleaved, size_t frames) {
        if (m_state != TransportState::Playing || m_track.totalFrames == 0) {
            std::fill_n(outInterleaved, frames * 2, 0.0);
            return 0;
        }

        if (m_playhead >= m_track.totalFrames) {
            m_state = TransportState::Stopped;
            std::fill_n(outInterleaved, frames * 2, 0.0);
            return 0;
        }

        if (m_resampler.isBypass()) {
            size_t available = static_cast<size_t>(m_track.totalFrames - m_playhead);
            size_t toRender = std::min(frames, available);

            for (size_t i = 0; i < toRender; ++i) {
                double t = static_cast<double>(m_playhead + i) / m_track.sampleRate;
                double val = 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t);
                outInterleaved[i * 2] = val;
                outInterleaved[i * 2 + 1] = val;
            }
            if (toRender < frames) {
                std::fill_n(outInterleaved + toRender * 2, (frames - toRender) * 2, 0.0);
            }
            m_playhead += toRender;
            return toRender;
        }

        // Resampling path: loop until requested output frames are satisfied or track ends
        size_t totalOut = 0;
        while (totalOut < frames && m_playhead < m_track.totalFrames) {
            double ratio = m_track.sampleRate / m_graphRate;
            size_t neededOut = frames - totalOut;
            size_t neededIn = static_cast<size_t>(std::ceil(static_cast<double>(neededOut) * ratio)) + 256;
            size_t availableIn = static_cast<size_t>(m_track.totalFrames - m_playhead);
            size_t inFrames = std::min(neededIn, availableIn);
            if (inFrames == 0) break;

            std::vector<double> inBuffer(inFrames * 2);
            for (size_t i = 0; i < inFrames; ++i) {
                double t = static_cast<double>(m_playhead + i) / m_track.sampleRate;
                double val = 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t);
                inBuffer[i * 2] = val;
                inBuffer[i * 2 + 1] = val;
            }

            size_t inDone = 0;
            size_t outDone = 0;
            bool ok = m_resampler.process(inBuffer.data(), inFrames, inDone,
                                          outInterleaved + totalOut * 2, neededOut, outDone);
            if (!ok) break;

            m_playhead += inDone;
            totalOut += outDone;
            if (inDone == 0 && outDone == 0) {
                break;
            }
        }

        if (totalOut < frames) {
            std::fill_n(outInterleaved + totalOut * 2, (frames - totalOut) * 2, 0.0);
        }
        return (totalOut > 0) ? totalOut : frames;
    }

private:
    char m_id = 'A';
    TransportState m_state = TransportState::Stopped;
    TrackInfo m_track;
    uint64_t m_playhead = 0;
    int64_t m_offsetFrames = 0;
    bool m_isLocked = false;
    double m_graphRate = 48000.0;
    dsp::Resampler m_resampler;
};

} // namespace desk32::engine::deck
