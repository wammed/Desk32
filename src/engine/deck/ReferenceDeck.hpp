#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <atomic>
#include "engine/decoder/DecoderWorker.hpp"
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
/// RT-Safety: Zero dynamic allocation during renderBlock(), decodes entirely in background DecoderWorker.
class ReferenceDeck {
public:
    static constexpr size_t RT_BUFFER_CAPACITY = 4096;

    explicit ReferenceDeck(char id = 'A');
    ~ReferenceDeck();

    [[nodiscard]] char id() const noexcept { return m_id; }
    [[nodiscard]] TransportState state() const noexcept { return m_state.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t playhead() const noexcept { return m_worker.playhead(); }
    [[nodiscard]] int64_t offsetFrames() const noexcept { return m_offsetFrames.load(std::memory_order_acquire); }
    [[nodiscard]] bool isLocked() const noexcept { return m_isLocked.load(std::memory_order_acquire); }

    [[nodiscard]] TrackInfo trackInfo() const noexcept {
        const auto& wInfo = m_worker.trackInfo();
        TrackInfo t;
        t.path = wInfo.path;
        t.sampleRate = wInfo.sampleRate;
        t.channels = wInfo.channels;
        t.totalFrames = wInfo.totalFrames;
        return t;
    }

    [[nodiscard]] std::shared_ptr<const metadata::TrackMetadata> metadata() const {
        return m_worker.metadata();
    }

    [[nodiscard]] bool checkAndResetTrackChanged() noexcept {
        return m_trackChanged.exchange(false);
    }

    [[nodiscard]] const session::FolderSession& folderSession() const noexcept { return m_folderSession; }
    [[nodiscard]] session::FolderSession& folderSession() noexcept { return m_folderSession; }

    void setLocked(bool locked) noexcept {
        m_isLocked.store(locked, std::memory_order_release);
        if (locked) {
            m_folderSession.stopSession(); // P0-10: Deck lock terminates and discards folder session
        }
    }
    void setOffsetFrames(int64_t offset) noexcept { m_offsetFrames.store(offset, std::memory_order_release); }

    /// Reconfigure graph sample rate across worker and resamplers
    void configureGraphRate(double graphSampleRate) noexcept {
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_worker.configureGraphRate(graphSampleRate);
    }

    /// Load standalone audio track using background DecoderWorker.
    /// Section 2.7.5: Blocked if Deck is locked.
    bool loadTrack(const std::string& filePath, double graphSampleRate) {
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false; // Protected by Deck Lock!
        }

        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_folderSession.stopSession();
        bool ok = m_worker.openTrack(filePath, graphSampleRate);
        if (ok) {
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_trackChanged.store(true, std::memory_order_release);
        }
        return ok;
    }

    /// Synthetic/Benchmark track loading overload (backward compatibility for Phase 0 benchmarks)
    bool loadTrack(const TrackInfo& info, double graphSampleRate) {
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false;
        }

        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_folderSession.stopSession();
        bool ok = m_worker.openSyntheticTrack(info.totalFrames, info.sampleRate, graphSampleRate, info.path);
        if (ok) {
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_trackChanged.store(true, std::memory_order_release);
        }
        return ok;
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
        const auto* firstTrack = m_folderSession.currentTrack();
        if (!firstTrack) return false;

        bool ok = m_worker.openTrack(firstTrack->path, graphSampleRate);
        if (ok) {
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_trackChanged.store(true, std::memory_order_release);
        } else {
            m_folderSession.stopSession();
        }
        return ok;
    }

    /// Advance to next track in Folder Session (executed on Control Thread)
    bool advanceFolderNext() {
        if (!m_folderSession.isActive()) return false;

        if (m_folderSession.advanceNext()) {
            const auto* nextTrack = m_folderSession.currentTrack();
            if (nextTrack) {
                bool ok = m_worker.openTrack(nextTrack->path, m_graphRate.load(std::memory_order_relaxed));
                if (ok) {
                    m_trackChanged.store(true, std::memory_order_release);
                    if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                        m_worker.play();
                    }
                    return true;
                }
                // P0-10: Open track failed. Stop folder session safely without skipping corrupted track.
                m_folderSession.stopSession();
            }
        }

        stop();
        return false;
    }

    /// Retreat to previous track in Folder Session (executed on Control Thread)
    bool retreatFolderPrevious() {
        if (!m_folderSession.isActive()) return false;

        if (m_folderSession.retreatPrevious()) {
            const auto* prevTrack = m_folderSession.currentTrack();
            if (prevTrack) {
                bool ok = m_worker.openTrack(prevTrack->path, m_graphRate.load(std::memory_order_relaxed));
                if (ok) {
                    m_trackChanged.store(true, std::memory_order_release);
                    if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                        m_worker.play();
                    }
                    return true;
                }
            }
        }
        return false;
    }

    /// Section 2.7.5: Transport operations (Play, Pause, Stop, Seek) ARE PERMITTED even during Deck Lock!
    void play() noexcept {
        m_state.store(TransportState::Playing, std::memory_order_release);
        m_worker.play();
    }

    void pause() noexcept {
        m_state.store(TransportState::Paused, std::memory_order_release);
        m_worker.pause();
    }

    void stop() noexcept {
        m_state.store(TransportState::Stopped, std::memory_order_release);
        m_worker.stop();
    }

    void closeTrack() noexcept {
        m_folderSession.stopSession();
        m_worker.stop();
        m_worker.closeTrack();
        m_state.store(TransportState::Stopped, std::memory_order_release);
    }

    [[nodiscard]] bool isOpen() const noexcept { return m_worker.isOpen(); }
    [[nodiscard]] bool isPlaying() const noexcept { return m_state.load(std::memory_order_acquire) == TransportState::Playing; }
    [[nodiscard]] decoder::DecoderWorker& worker() noexcept { return m_worker; }
    [[nodiscard]] const decoder::DecoderWorker& worker() const noexcept { return m_worker; }

    void seek(int64_t targetFrame) noexcept {
        m_worker.seek(targetFrame);
    }

    [[nodiscard]] bool needsFolderAdvance() const noexcept {
        return m_needsFolderAdvance.load(std::memory_order_acquire);
    }

    void clearNeedsFolderAdvance() noexcept {
        m_needsFolderAdvance.store(false, std::memory_order_release);
    }

    /// Render planar 64-bit float audio block [left, right] at graphSampleRate.
    /// Strictly RT-Safe: zero dynamic heap allocation, zero FFmpeg calls, zero file I/O, zero locks.
    size_t renderBlock(double* outLeft, double* outRight, size_t frames) noexcept {
        if (m_state.load(std::memory_order_acquire) != TransportState::Playing) {
            std::fill_n(outLeft, frames, 0.0);
            std::fill_n(outRight, frames, 0.0);
            return 0;
        }

        size_t read = m_worker.readAudioBlock(outLeft, outRight, frames);

        if (m_worker.isEofDelivered()) {
            m_worker.clearEofDelivered();
            if (m_folderSession.isActive()) {
                m_needsFolderAdvance.store(true, std::memory_order_release);
            } else {
                m_state.store(TransportState::Stopped, std::memory_order_release);
            }
        }

        return read;
    }

    /// Backward-compatible interleaved rendering for Phase 0 benchmarks
    size_t renderBlock(double* outInterleaved, size_t frames) noexcept {
        if (frames > RT_BUFFER_CAPACITY) frames = RT_BUFFER_CAPACITY;
        size_t read = renderBlock(m_scratchL.data(), m_scratchR.data(), frames);
        for (size_t i = 0; i < read; ++i) {
            outInterleaved[i * 2]     = m_scratchL[i];
            outInterleaved[i * 2 + 1] = m_scratchR[i];
        }
        std::fill_n(outInterleaved + read * 2, (frames - read) * 2, 0.0);
        return read;
    }

private:
    char m_id;
    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<bool> m_isLocked{false};
    std::atomic<int64_t> m_offsetFrames{0};
    std::atomic<double> m_graphRate{48000.0};
    std::atomic<bool> m_trackChanged{false};
    std::atomic<bool> m_needsFolderAdvance{false};

    session::FolderSession m_folderSession;
    decoder::DecoderWorker m_worker;

    alignas(64) std::array<double, RT_BUFFER_CAPACITY> m_scratchL{};
    alignas(64) std::array<double, RT_BUFFER_CAPACITY> m_scratchR{};
};

} // namespace desk32::engine::deck
