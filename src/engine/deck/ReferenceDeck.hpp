#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <atomic>
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/dsp/Resampler.hpp"
#include "engine/session/FolderSession.hpp"
#include "engine/metadata/TrackMetadata.hpp"

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

/// Reference Deck Slot (Decks A, B, C, D)
/// Adheres strictly to Section 2.1 - 2.3, 2.7, and Exit Criteria.
class ReferenceDeck {
public:
    explicit ReferenceDeck(char id = 'A')
        : m_id(id)
    {
        m_resampleInInterleaved.reserve(16384);
        m_readChunkL.reserve(8192);
        m_readChunkR.reserve(8192);
        m_tempOutInterleaved.reserve(8192);
    }

    [[nodiscard]] char id() const noexcept { return m_id; }
    [[nodiscard]] TransportState state() const noexcept { return m_state; }
    [[nodiscard]] int64_t playhead() const noexcept { return m_playhead; }
    [[nodiscard]] int64_t offsetFrames() const noexcept { return m_offsetFrames; }
    [[nodiscard]] bool isLocked() const noexcept { return m_isLocked; }
    [[nodiscard]] const TrackInfo& trackInfo() const noexcept { return m_track; }
    [[nodiscard]] const metadata::TrackMetadata* metadata() const noexcept {
        return m_decoder ? &m_decoder->metadata() : nullptr;
    }
    [[nodiscard]] bool checkAndResetTrackChanged() noexcept {
        return m_trackChanged.exchange(false);
    }

    [[nodiscard]] const session::FolderSession& folderSession() const noexcept { return m_folderSession; }
    [[nodiscard]] session::FolderSession& folderSession() noexcept { return m_folderSession; }

    void setLocked(bool locked) noexcept { m_isLocked = locked; }
    void setOffsetFrames(int64_t offset) noexcept { m_offsetFrames = offset; }

    /// Load standalone audio track using FFmpegAudioDecoder.
    /// Section 2.7.5: Blocked if Deck is locked.
    bool loadTrack(const std::string& filePath, double graphSampleRate) {
        if (m_isLocked) {
            return false; // Protected by Deck Lock!
        }

        auto dec = std::make_unique<decoder::FFmpegAudioDecoder>();
        if (!dec->open(filePath)) {
            return false;
        }

        m_decoder = std::move(dec);
        m_readAheadDecoder.reset();
        m_folderSession.stopSession();

        const auto& info = m_decoder->formatInfo();
        m_track.path = filePath;
        m_track.sampleRate = info.sampleRate;
        m_track.channels = info.channels;
        m_track.totalFrames = static_cast<uint64_t>(std::max<int64_t>(0, info.totalFrames));

        m_graphRate = graphSampleRate;
        m_playhead = 0;
        m_state = TransportState::Stopped;

        m_resampleInInterleaved.clear();
        // Configure SoXR resampler if track rate differs from graph rate
        m_resampler.configure(m_track.sampleRate, m_graphRate, m_track.channels);
        m_trackChanged.store(true, std::memory_order_release);
        return true;
    }

    /// Synthetic/Benchmark track loading overload (backward compatibility for Phase 0 benchmarks)
    bool loadTrack(const TrackInfo& info, double graphSampleRate) {
        if (m_isLocked) {
            return false;
        }
        m_track = info;
        m_playhead = 0;
        m_state = TransportState::Stopped;
        m_graphRate = graphSampleRate;
        m_decoder.reset();
        m_readAheadDecoder.reset();
        m_folderSession.stopSession();
        m_resampleInInterleaved.clear();
        m_trackChanged.store(true, std::memory_order_release);
        return m_resampler.configure(m_track.sampleRate, m_graphRate, m_track.channels);
    }

    /// Start a Folder Session (Continuous playback of folder contents)
    /// Section 2.7: Starts strictly from Index 0, deterministic natural sort, single next-track read-ahead.
    bool startFolderSession(const std::string& folderPath, const std::vector<std::string>& fileNames, double graphSampleRate) {
        if (m_isLocked) {
            return false; // Protected by Deck Lock!
        }

        if (!m_folderSession.startSession(folderPath, fileNames)) {
            return false;
        }

        m_graphRate = graphSampleRate;

        // Immediately load track index 0
        const auto* firstTrack = m_folderSession.currentTrack();
        if (!firstTrack) return false;

        auto dec = std::make_unique<decoder::FFmpegAudioDecoder>();
        if (!dec->open(firstTrack->path)) {
            m_folderSession.stopSession();
            return false;
        }

        m_decoder = std::move(dec);
        const auto& info = m_decoder->formatInfo();
        m_track.path = firstTrack->path;
        m_track.sampleRate = info.sampleRate;
        m_track.channels = info.channels;
        m_track.totalFrames = static_cast<uint64_t>(std::max<int64_t>(0, info.totalFrames));
        m_playhead = 0;
        m_state = TransportState::Stopped;
        m_resampleInInterleaved.clear();
        m_resampler.configure(m_track.sampleRate, m_graphRate, m_track.channels);
        m_trackChanged.store(true, std::memory_order_release);

        // Pre-open single next track read-ahead
        prepareReadAhead();

        return true;
    }

    /// Section 2.7.5: Transport operations (Play, Pause, Stop, Seek) ARE PERMITTED even during Deck Lock!
    void play() noexcept {
        if (m_decoder && m_decoder->isOpen()) {
            m_state = TransportState::Playing;
        } else if (m_track.totalFrames > 0) {
            m_state = TransportState::Playing; // Synthetic track support
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
        if (m_decoder) {
            m_decoder->seek(0);
        }
        m_resampleInInterleaved.clear();
        m_resampler.reset();
    }

    void seek(int64_t targetFrame) noexcept {
        if (m_decoder && m_decoder->isOpen()) {
            m_decoder->seek(targetFrame);
            m_playhead = m_decoder->currentFrame();
        } else {
            m_playhead = std::clamp<int64_t>(targetFrame, 0, static_cast<int64_t>(m_track.totalFrames));
        }
        m_resampleInInterleaved.clear();
        m_resampler.reset();
    }

    /// Advance to next track in Folder Session
    bool advanceFolderNext() {
        if (!m_folderSession.isActive()) return false;

        if (m_folderSession.advanceNext()) {
            promoteReadAheadOrLoadCurrent();
            return true;
        }

        // End of folder reached
        stop();
        return false;
    }

    /// Retreat to previous track in Folder Session
    bool retreatFolderPrevious() {
        if (!m_folderSession.isActive()) return false;

        if (m_folderSession.retreatPrevious()) {
            const auto* prevTrack = m_folderSession.currentTrack();
            if (prevTrack) {
                return loadTrackInternal(prevTrack->path);
            }
        }
        return false;
    }

    /// Render interleaved 64-bit float audio block (backward compatibility for Phase 0 benchmarks)
    size_t renderBlock(double* outInterleaved, size_t frames) {
        std::vector<double> l(frames);
        std::vector<double> r(frames);
        size_t rendered = renderBlock(l.data(), r.data(), frames);
        for (size_t i = 0; i < rendered; ++i) {
            outInterleaved[i * 2]     = l[i];
            outInterleaved[i * 2 + 1] = r[i];
        }
        std::fill_n(outInterleaved + rendered * 2, (frames - rendered) * 2, 0.0);
        return rendered;
    }

    /// Render planar 64-bit float audio block [left, right] at graphSampleRate.
    /// Handles decoding, SoXR resampling, and seamless Folder Session auto-advancement.
    size_t renderBlock(double* outLeft, double* outRight, size_t frames) {
        if (m_state != TransportState::Playing) {
            std::fill_n(outLeft, frames, 0.0);
            std::fill_n(outRight, frames, 0.0);
            return 0;
        }

        // Fallback for synthetic/benchmark track without real file decoder
        if (!m_decoder || !m_decoder->isOpen()) {
            if (m_track.totalFrames == 0 || m_playhead >= static_cast<int64_t>(m_track.totalFrames)) {
                stop();
                std::fill_n(outLeft, frames, 0.0);
                std::fill_n(outRight, frames, 0.0);
                return 0;
            }

            if (m_resampler.isBypass()) {
                size_t available = static_cast<size_t>(static_cast<int64_t>(m_track.totalFrames) - m_playhead);
                size_t toRender = std::min(frames, available);
                for (size_t i = 0; i < toRender; ++i) {
                    double t = static_cast<double>(m_playhead + static_cast<int64_t>(i)) / m_track.sampleRate;
                    double val = 0.5 * std::sin(2.0 * M_PI * 440.0 * t);
                    outLeft[i] = val;
                    outRight[i] = val;
                }
                m_playhead += static_cast<int64_t>(toRender);
                std::fill_n(outLeft + toRender, frames - toRender, 0.0);
                std::fill_n(outRight + toRender, frames - toRender, 0.0);
                return toRender;
            } else {
                // Resample synthetic track
                size_t totalOut = 0;
                while (totalOut < frames && m_playhead < static_cast<int64_t>(m_track.totalFrames)) {
                    double ratio = m_track.sampleRate / m_graphRate;
                    size_t neededOut = frames - totalOut;
                    size_t neededIn = static_cast<size_t>(std::ceil(static_cast<double>(neededOut) * ratio)) + 256;
                    size_t availableIn = static_cast<size_t>(static_cast<int64_t>(m_track.totalFrames) - m_playhead);
                    size_t inFrames = std::min(neededIn, availableIn);
                    if (inFrames == 0) break;

                    std::vector<double> inBuffer(inFrames * 2);
                    for (size_t i = 0; i < inFrames; ++i) {
                        double t = static_cast<double>(m_playhead + static_cast<int64_t>(i)) / m_track.sampleRate;
                        double val = 0.5 * std::sin(2.0 * M_PI * 440.0 * t);
                        inBuffer[i * 2] = val;
                        inBuffer[i * 2 + 1] = val;
                    }

                    std::vector<double> outBuffer(neededOut * 2);
                    size_t inDone = 0;
                    size_t outDone = 0;
                    bool ok = m_resampler.process(inBuffer.data(), inFrames, inDone,
                                                  outBuffer.data(), neededOut, outDone);
                    if (!ok) break;

                    for (size_t i = 0; i < outDone; ++i) {
                        outLeft[totalOut + i] = outBuffer[i * 2];
                        outRight[totalOut + i] = outBuffer[i * 2 + 1];
                    }

                    m_playhead += static_cast<int64_t>(inDone);
                    totalOut += outDone;
                    if (inDone == 0 && outDone == 0) {
                        break;
                    }
                }

                if (totalOut < frames) {
                    std::fill_n(outLeft + totalOut, frames - totalOut, 0.0);
                    std::fill_n(outRight + totalOut, frames - totalOut, 0.0);
                }
                return (totalOut > 0) ? totalOut : frames;
            }
        }

        // Real file decoding path
        size_t framesDelivered = 0;

        while (framesDelivered < frames) {
            size_t remaining = frames - framesDelivered;

            if (m_resampler.isBypass()) {
                // Direct rate match: decode directly into output buffers
                size_t read = m_decoder->readFrames(outLeft + framesDelivered, outRight + framesDelivered, remaining);
                framesDelivered += read;
                m_playhead += static_cast<int64_t>(read);

                if (read == 0) {
                    // Current track ended
                    if (m_folderSession.isActive() && m_folderSession.advanceNext()) {
                        promoteReadAheadOrLoadCurrent();
                        continue; // Continue rendering from next track
                    } else {
                        // End of playback
                        stop();
                        std::fill_n(outLeft + framesDelivered, remaining, 0.0);
                        std::fill_n(outRight + framesDelivered, remaining, 0.0);
                        break;
                    }
                }
            } else {
                // Resample real audio stream through SoXR with persistent streaming buffer
                double ratio = m_track.sampleRate / m_graphRate;
                size_t neededIn = static_cast<size_t>(std::ceil(static_cast<double>(remaining) * ratio)) + 256;
                size_t bufferedIn = m_resampleInInterleaved.size() / 2;

                if (bufferedIn < neededIn) {
                    size_t toRead = neededIn - bufferedIn;
                    if (m_readChunkL.size() < toRead) {
                        m_readChunkL.resize(toRead);
                        m_readChunkR.resize(toRead);
                    }
                    size_t read = m_decoder->readFrames(m_readChunkL.data(), m_readChunkR.data(), toRead);
                    if (read > 0) {
                        size_t oldSize = m_resampleInInterleaved.size();
                        m_resampleInInterleaved.resize(oldSize + read * 2);
                        for (size_t i = 0; i < read; ++i) {
                            m_resampleInInterleaved[oldSize + i * 2]     = m_readChunkL[i];
                            m_resampleInInterleaved[oldSize + i * 2 + 1] = m_readChunkR[i];
                        }
                    }
                }

                size_t inFramesAvail = m_resampleInInterleaved.size() / 2;
                if (inFramesAvail > 0) {
                    if (m_tempOutInterleaved.size() < remaining * 2) {
                        m_tempOutInterleaved.resize(remaining * 2);
                    }
                    size_t inDone = 0, outDone = 0;
                    bool ok = m_resampler.process(m_resampleInInterleaved.data(), inFramesAvail, inDone,
                                                  m_tempOutInterleaved.data(), remaining, outDone);
                    if (ok && outDone > 0) {
                        for (size_t i = 0; i < outDone; ++i) {
                            outLeft[framesDelivered + i]  = m_tempOutInterleaved[i * 2];
                            outRight[framesDelivered + i] = m_tempOutInterleaved[i * 2 + 1];
                        }
                        framesDelivered += outDone;
                    }

                    if (inDone > 0) {
                        m_playhead += static_cast<int64_t>(inDone);
                        m_resampleInInterleaved.erase(m_resampleInInterleaved.begin(),
                                                      m_resampleInInterleaved.begin() + static_cast<ptrdiff_t>(inDone * 2));
                    }

                    if (outDone == 0 && framesDelivered < frames) {
                        // SoXR primed or needing more samples to emit next phase
                        size_t toReadMore = 256;
                        if (m_readChunkL.size() < toReadMore) {
                            m_readChunkL.resize(toReadMore);
                            m_readChunkR.resize(toReadMore);
                        }
                        size_t readMore = m_decoder->readFrames(m_readChunkL.data(), m_readChunkR.data(), toReadMore);
                        if (readMore > 0) {
                            size_t oldSize = m_resampleInInterleaved.size();
                            m_resampleInInterleaved.resize(oldSize + readMore * 2);
                            for (size_t i = 0; i < readMore; ++i) {
                                m_resampleInInterleaved[oldSize + i * 2]     = m_readChunkL[i];
                                m_resampleInInterleaved[oldSize + i * 2 + 1] = m_readChunkR[i];
                            }
                            continue;
                        } else if (inDone == 0) {
                            // Reached EOF and SoXR has no more output
                            if (m_folderSession.isActive() && m_folderSession.advanceNext()) {
                                promoteReadAheadOrLoadCurrent();
                                continue;
                            } else {
                                stop();
                                std::fill_n(outLeft + framesDelivered, remaining, 0.0);
                                std::fill_n(outRight + framesDelivered, remaining, 0.0);
                                break;
                            }
                        }
                    }
                } else {
                    // No input available and decoder read 0 -> End of File
                    if (m_folderSession.isActive() && m_folderSession.advanceNext()) {
                        promoteReadAheadOrLoadCurrent();
                        continue;
                    } else {
                        stop();
                        std::fill_n(outLeft + framesDelivered, remaining, 0.0);
                        std::fill_n(outRight + framesDelivered, remaining, 0.0);
                        break;
                    }
                }
            }
        }

        return framesDelivered;
    }

private:
    void prepareReadAhead() {
        const auto* nextTrack = m_folderSession.readAheadTrack();
        if (nextTrack) {
            auto dec = std::make_unique<decoder::FFmpegAudioDecoder>();
            if (dec->open(nextTrack->path)) {
                m_readAheadDecoder = std::move(dec);
                return;
            }
        }
        m_readAheadDecoder.reset();
    }

    void promoteReadAheadOrLoadCurrent() {
        const auto* cur = m_folderSession.currentTrack();
        if (!cur) return;

        if (m_readAheadDecoder && m_readAheadDecoder->isOpen()) {
            m_decoder = std::move(m_readAheadDecoder);
        } else {
            auto dec = std::make_unique<decoder::FFmpegAudioDecoder>();
            dec->open(cur->path);
            m_decoder = std::move(dec);
        }

        if (m_decoder && m_decoder->isOpen()) {
            const auto& info = m_decoder->formatInfo();
            m_track.path = cur->path;
            m_track.sampleRate = info.sampleRate;
            m_track.channels = info.channels;
            m_track.totalFrames = static_cast<uint64_t>(std::max<int64_t>(0, info.totalFrames));
            m_playhead = 0;
            m_resampleInInterleaved.clear();
            m_resampler.configure(m_track.sampleRate, m_graphRate, m_track.channels);
            m_trackChanged.store(true, std::memory_order_release);
        }

        prepareReadAhead();
    }

    bool loadTrackInternal(const std::string& path) {
        auto dec = std::make_unique<decoder::FFmpegAudioDecoder>();
        if (!dec->open(path)) return false;

        m_decoder = std::move(dec);
        const auto& info = m_decoder->formatInfo();
        m_track.path = path;
        m_track.sampleRate = info.sampleRate;
        m_track.channels = info.channels;
        m_track.totalFrames = static_cast<uint64_t>(std::max<int64_t>(0, info.totalFrames));
        m_playhead = 0;
        m_resampleInInterleaved.clear();
        m_resampler.configure(m_track.sampleRate, m_graphRate, m_track.channels);
        m_trackChanged.store(true, std::memory_order_release);

        prepareReadAhead();
        return true;
    }

    char m_id;
    TransportState m_state = TransportState::Stopped;
    bool m_isLocked = false;
    int64_t m_playhead = 0;
    int64_t m_offsetFrames = 0;
    double m_graphRate = 48000.0;

    TrackInfo m_track;
    dsp::Resampler m_resampler;
    session::FolderSession m_folderSession;

    std::unique_ptr<decoder::FFmpegAudioDecoder> m_decoder;
    std::unique_ptr<decoder::FFmpegAudioDecoder> m_readAheadDecoder;

    std::atomic<bool> m_trackChanged{false};
    std::vector<double> m_resampleInInterleaved;
    std::vector<double> m_readChunkL;
    std::vector<double> m_readChunkR;
    std::vector<double> m_tempOutInterleaved;
};

} // namespace desk32::engine::deck
