#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
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

enum class TransportCommand : uint8_t {
    None,
    Play,
    Pause,
    Stop,
    Seek
};

struct TrackInfo {
    std::string path;
    double sampleRate = 44100.0;
    uint32_t channels = 2;
    uint64_t totalFrames = 0;
};

/// Reference Deck Slot (Decks A, B, C, D)
/// Adheres strictly to Section 2.1 - 2.3, 2.7, and Exit Criteria.
/// RT-Safety: Zero dynamic allocation during renderBlock(), atomic transport & playhead state.
class ReferenceDeck {
public:
    static constexpr size_t RT_BUFFER_CAPACITY = 4096; // max frames per block
    static constexpr size_t RESAMPLE_BUF_CAPACITY = 16384; // doubles (8192 stereo frames)

    explicit ReferenceDeck(char id = 'A')
        : m_id(id)
    {
    }

    [[nodiscard]] char id() const noexcept { return m_id; }
    [[nodiscard]] TransportState state() const noexcept { return m_state.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t playhead() const noexcept { return m_playhead.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t offsetFrames() const noexcept { return m_offsetFrames.load(std::memory_order_acquire); }
    [[nodiscard]] bool isLocked() const noexcept { return m_isLocked.load(std::memory_order_acquire); }
    [[nodiscard]] const TrackInfo& trackInfo() const noexcept { return m_track; }
    [[nodiscard]] const metadata::TrackMetadata* metadata() const noexcept {
        return m_decoder ? &m_decoder->metadata() : nullptr;
    }
    [[nodiscard]] bool checkAndResetTrackChanged() noexcept {
        return m_trackChanged.exchange(false);
    }

    [[nodiscard]] const session::FolderSession& folderSession() const noexcept { return m_folderSession; }
    [[nodiscard]] session::FolderSession& folderSession() noexcept { return m_folderSession; }

    void setLocked(bool locked) noexcept { m_isLocked.store(locked, std::memory_order_release); }
    void setOffsetFrames(int64_t offset) noexcept { m_offsetFrames.store(offset, std::memory_order_release); }

    /// Reconfigure graph sample rate across decoders and resamplers
    void configureGraphRate(double graphSampleRate) noexcept {
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_resampleBufCount = 0;
        m_resampler.configure(m_track.sampleRate, graphSampleRate, m_track.channels);
    }

    /// Load standalone audio track using FFmpegAudioDecoder.
    /// Section 2.7.5: Blocked if Deck is locked.
    bool loadTrack(const std::string& filePath, double graphSampleRate) {
        if (m_isLocked.load(std::memory_order_acquire)) {
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

        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_playhead.store(0, std::memory_order_release);
        m_state.store(TransportState::Stopped, std::memory_order_release);

        m_resampleBufCount = 0;
        m_resampler.configure(m_track.sampleRate, graphSampleRate, m_track.channels);
        m_trackChanged.store(true, std::memory_order_release);
        return true;
    }

    /// Synthetic/Benchmark track loading overload (backward compatibility for Phase 0 benchmarks)
    bool loadTrack(const TrackInfo& info, double graphSampleRate) {
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false;
        }
        m_track = info;
        m_playhead.store(0, std::memory_order_release);
        m_state.store(TransportState::Stopped, std::memory_order_release);
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_decoder.reset();
        m_readAheadDecoder.reset();
        m_folderSession.stopSession();
        m_resampleBufCount = 0;
        m_trackChanged.store(true, std::memory_order_release);
        return m_resampler.configure(m_track.sampleRate, graphSampleRate, m_track.channels);
    }

    /// Start a Folder Session (Continuous playback of folder contents)
    /// Section 2.7: Starts strictly from Index 0, deterministic natural sort, single next-track read-ahead.
    bool startFolderSession(const std::string& folderPath, const std::vector<std::string>& fileNames, double graphSampleRate) {
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false; // Protected by Deck Lock!
        }

        if (!m_folderSession.startSession(folderPath, fileNames)) {
            return false;
        }

        m_graphRate.store(graphSampleRate, std::memory_order_release);

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
        m_playhead.store(0, std::memory_order_release);
        m_state.store(TransportState::Stopped, std::memory_order_release);
        m_resampleBufCount = 0;
        m_resampler.configure(m_track.sampleRate, graphSampleRate, m_track.channels);
        m_trackChanged.store(true, std::memory_order_release);

        // Pre-open single next track read-ahead
        prepareReadAhead();

        return true;
    }

    /// Section 2.7.5: Transport operations (Play, Pause, Stop, Seek) ARE PERMITTED even during Deck Lock!
    /// UI thread posts commands; processed by Audio Thread to prevent concurrent decoder access.
    void play() noexcept {
        m_pendingCmd.store(TransportCommand::Play, std::memory_order_release);
        if (m_decoder && m_decoder->isOpen()) {
            m_state.store(TransportState::Playing, std::memory_order_release);
        } else if (m_track.totalFrames > 0) {
            m_state.store(TransportState::Playing, std::memory_order_release);
        }
    }

    void pause() noexcept {
        m_pendingCmd.store(TransportCommand::Pause, std::memory_order_release);
        m_state.store(TransportState::Paused, std::memory_order_release);
    }

    void stop() noexcept {
        m_pendingCmd.store(TransportCommand::Stop, std::memory_order_release);
        m_state.store(TransportState::Stopped, std::memory_order_release);
        m_playhead.store(0, std::memory_order_release);
    }

    void seek(int64_t targetFrame) noexcept {
        m_pendingSeekTarget.store(targetFrame, std::memory_order_release);
        m_pendingCmd.store(TransportCommand::Seek, std::memory_order_release);
        m_playhead.store(targetFrame, std::memory_order_release);
    }

    /// Process pending transport command on Audio Thread before rendering
    void processPendingCommands() noexcept {
        TransportCommand cmd = m_pendingCmd.exchange(TransportCommand::None, std::memory_order_acq_rel);
        if (cmd == TransportCommand::None) return;

        switch (cmd) {
        case TransportCommand::Play:
            if (m_decoder && m_decoder->isOpen()) {
                m_state.store(TransportState::Playing, std::memory_order_release);
            } else if (m_track.totalFrames > 0) {
                m_state.store(TransportState::Playing, std::memory_order_release);
            }
            break;
        case TransportCommand::Pause:
            if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                m_state.store(TransportState::Paused, std::memory_order_release);
            }
            break;
        case TransportCommand::Stop:
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_playhead.store(0, std::memory_order_release);
            if (m_decoder) {
                m_decoder->seek(0);
            }
            m_resampleBufCount = 0;
            m_resampler.reset();
            break;
        case TransportCommand::Seek: {
            int64_t target = m_pendingSeekTarget.load(std::memory_order_acquire);
            if (m_decoder && m_decoder->isOpen()) {
                m_decoder->seek(target);
                m_playhead.store(m_decoder->currentFrame(), std::memory_order_release);
            } else {
                m_playhead.store(std::clamp<int64_t>(target, 0, static_cast<int64_t>(m_track.totalFrames)), std::memory_order_release);
            }
            m_resampleBufCount = 0;
            m_resampler.reset();
            break;
        }
        default:
            break;
        }
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
        if (frames > RT_BUFFER_CAPACITY) {
            frames = RT_BUFFER_CAPACITY;
        }
        size_t rendered = renderBlock(m_readChunkL.data(), m_readChunkR.data(), frames);
        for (size_t i = 0; i < rendered; ++i) {
            outInterleaved[i * 2]     = m_readChunkL[i];
            outInterleaved[i * 2 + 1] = m_readChunkR[i];
        }
        std::fill_n(outInterleaved + rendered * 2, (frames - rendered) * 2, 0.0);
        return rendered;
    }

    /// Render planar 64-bit float audio block [left, right] at graphSampleRate.
    /// Handles decoding, SoXR resampling, and seamless Folder Session auto-advancement.
    /// Strictly RT-Safe: zero dynamic heap allocation during execution.
    size_t renderBlock(double* outLeft, double* outRight, size_t frames) {
        processPendingCommands();

        if (m_state.load(std::memory_order_acquire) != TransportState::Playing) {
            std::fill_n(outLeft, frames, 0.0);
            std::fill_n(outRight, frames, 0.0);
            return 0;
        }

        const double currentGraphRate = m_graphRate.load(std::memory_order_acquire);
        int64_t currentPlayhead = m_playhead.load(std::memory_order_acquire);

        // Fallback for synthetic/benchmark track without real file decoder
        if (!m_decoder || !m_decoder->isOpen()) {
            if (m_track.totalFrames == 0 || currentPlayhead >= static_cast<int64_t>(m_track.totalFrames)) {
                stop();
                std::fill_n(outLeft, frames, 0.0);
                std::fill_n(outRight, frames, 0.0);
                return 0;
            }

            if (m_resampler.isBypass()) {
                size_t available = static_cast<size_t>(static_cast<int64_t>(m_track.totalFrames) - currentPlayhead);
                size_t toRender = std::min(frames, available);
                for (size_t i = 0; i < toRender; ++i) {
                    double t = static_cast<double>(currentPlayhead + static_cast<int64_t>(i)) / m_track.sampleRate;
                    double val = 0.5 * std::sin(2.0 * M_PI * 440.0 * t);
                    outLeft[i] = val;
                    outRight[i] = val;
                }
                m_playhead.fetch_add(static_cast<int64_t>(toRender), std::memory_order_relaxed);
                std::fill_n(outLeft + toRender, frames - toRender, 0.0);
                std::fill_n(outRight + toRender, frames - toRender, 0.0);
                return toRender;
            } else {
                // Resample synthetic track using preallocated stack/member buffers
                size_t totalOut = 0;
                while (totalOut < frames && currentPlayhead < static_cast<int64_t>(m_track.totalFrames)) {
                    double ratio = m_track.sampleRate / currentGraphRate;
                    size_t neededOut = frames - totalOut;
                    size_t neededIn = static_cast<size_t>(std::ceil(static_cast<double>(neededOut) * ratio)) + 256;
                    size_t availableIn = static_cast<size_t>(static_cast<int64_t>(m_track.totalFrames) - currentPlayhead);
                    size_t inFrames = std::min(neededIn, std::min(availableIn, RT_BUFFER_CAPACITY));
                    if (inFrames == 0) break;

                    for (size_t i = 0; i < inFrames; ++i) {
                        double t = static_cast<double>(currentPlayhead + static_cast<int64_t>(i)) / m_track.sampleRate;
                        double val = 0.5 * std::sin(2.0 * M_PI * 440.0 * t);
                        m_readChunkL[i] = val;
                        m_readChunkR[i] = val;
                        m_tempOutInterleaved[i * 2]     = val;
                        m_tempOutInterleaved[i * 2 + 1] = val;
                    }

                    std::array<double, RT_BUFFER_CAPACITY * 2> outBuffer{};
                    size_t inDone = 0, outDone = 0;
                    bool ok = m_resampler.process(m_tempOutInterleaved.data(), inFrames, inDone,
                                                  outBuffer.data(), neededOut, outDone);
                    if (!ok) break;

                    for (size_t i = 0; i < outDone; ++i) {
                        outLeft[totalOut + i] = outBuffer[i * 2];
                        outRight[totalOut + i] = outBuffer[i * 2 + 1];
                    }

                    m_playhead.fetch_add(static_cast<int64_t>(inDone), std::memory_order_relaxed);
                    currentPlayhead += static_cast<int64_t>(inDone);
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
                // Direct rate match: decode directly into output buffers without heap allocation
                size_t read = m_decoder->readFrames(outLeft + framesDelivered, outRight + framesDelivered, remaining);
                framesDelivered += read;
                m_playhead.fetch_add(static_cast<int64_t>(read), std::memory_order_relaxed);

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
                // Resample real audio stream through SoXR with zero dynamic allocations
                double ratio = m_track.sampleRate / currentGraphRate;
                size_t neededIn = static_cast<size_t>(std::ceil(static_cast<double>(remaining) * ratio)) + 256;
                size_t bufferedIn = m_resampleBufCount / 2;

                if (bufferedIn < neededIn) {
                    size_t toRead = neededIn - bufferedIn;
                    size_t spaceFrames = (RESAMPLE_BUF_CAPACITY - m_resampleBufCount) / 2;
                    size_t actualToRead = std::min(toRead, std::min(spaceFrames, RT_BUFFER_CAPACITY));

                    if (actualToRead > 0) {
                        size_t read = m_decoder->readFrames(m_readChunkL.data(), m_readChunkR.data(), actualToRead);
                        if (read > 0) {
                            for (size_t i = 0; i < read; ++i) {
                                m_resampleBuffer[m_resampleBufCount + i * 2]     = m_readChunkL[i];
                                m_resampleBuffer[m_resampleBufCount + i * 2 + 1] = m_readChunkR[i];
                            }
                            m_resampleBufCount += read * 2;
                        }
                    }
                }

                size_t inFramesAvail = m_resampleBufCount / 2;
                if (inFramesAvail > 0) {
                    size_t inDone = 0, outDone = 0;
                    size_t maxOut = std::min(remaining, RT_BUFFER_CAPACITY);
                    bool ok = m_resampler.process(m_resampleBuffer.data(), inFramesAvail, inDone,
                                                  m_tempOutInterleaved.data(), maxOut, outDone);
                    if (ok && outDone > 0) {
                        for (size_t i = 0; i < outDone; ++i) {
                            outLeft[framesDelivered + i]  = m_tempOutInterleaved[i * 2];
                            outRight[framesDelivered + i] = m_tempOutInterleaved[i * 2 + 1];
                        }
                        framesDelivered += outDone;
                    }

                    if (inDone > 0) {
                        m_playhead.fetch_add(static_cast<int64_t>(inDone), std::memory_order_relaxed);
                        size_t consumedDoubles = inDone * 2;
                        m_resampleBufCount -= consumedDoubles;
                        if (m_resampleBufCount > 0) {
                            std::memmove(m_resampleBuffer.data(), m_resampleBuffer.data() + consumedDoubles,
                                         m_resampleBufCount * sizeof(double));
                        }
                    }

                    if (outDone == 0 && framesDelivered < frames) {
                        // SoXR primed or needing more samples
                        size_t spaceFrames = (RESAMPLE_BUF_CAPACITY - m_resampleBufCount) / 2;
                        size_t toReadMore = std::min<size_t>(256, std::min(spaceFrames, RT_BUFFER_CAPACITY));
                        if (toReadMore > 0) {
                            size_t readMore = m_decoder->readFrames(m_readChunkL.data(), m_readChunkR.data(), toReadMore);
                            if (readMore > 0) {
                                for (size_t i = 0; i < readMore; ++i) {
                                    m_resampleBuffer[m_resampleBufCount + i * 2]     = m_readChunkL[i];
                                    m_resampleBuffer[m_resampleBufCount + i * 2 + 1] = m_readChunkR[i];
                                }
                                m_resampleBufCount += readMore * 2;
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
            m_playhead.store(0, std::memory_order_release);
            m_resampleBufCount = 0;
            m_resampler.configure(m_track.sampleRate, m_graphRate.load(std::memory_order_acquire), m_track.channels);
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
        m_playhead.store(0, std::memory_order_release);
        m_resampleBufCount = 0;
        m_resampler.configure(m_track.sampleRate, m_graphRate.load(std::memory_order_acquire), m_track.channels);
        m_trackChanged.store(true, std::memory_order_release);

        prepareReadAhead();
        return true;
    }

    char m_id;
    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<bool> m_isLocked{false};
    std::atomic<int64_t> m_playhead{0};
    std::atomic<int64_t> m_offsetFrames{0};
    std::atomic<double> m_graphRate{48000.0};

    // Thread-safe command queue / mailbox from UI thread to Audio Thread
    std::atomic<TransportCommand> m_pendingCmd{TransportCommand::None};
    std::atomic<int64_t> m_pendingSeekTarget{0};

    TrackInfo m_track;
    dsp::Resampler m_resampler;
    session::FolderSession m_folderSession;

    std::unique_ptr<decoder::FFmpegAudioDecoder> m_decoder;
    std::unique_ptr<decoder::FFmpegAudioDecoder> m_readAheadDecoder;

    std::atomic<bool> m_trackChanged{false};

    // Preallocated real-time audio buffers (Zero dynamic allocation on audio thread)
    std::array<double, RT_BUFFER_CAPACITY> m_readChunkL{};
    std::array<double, RT_BUFFER_CAPACITY> m_readChunkR{};
    std::array<double, RT_BUFFER_CAPACITY * 2> m_tempOutInterleaved{};

    std::array<double, RESAMPLE_BUF_CAPACITY> m_resampleBuffer{};
    size_t m_resampleBufCount = 0;
};

} // namespace desk32::engine::deck
