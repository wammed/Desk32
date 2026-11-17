#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include "engine/decoder/DecoderWorker.hpp"
#include "engine/session/FolderSession.hpp"
#include "engine/metadata/TrackMetadata.hpp"

namespace desk32::engine::deck {

enum class TransportState {
    Stopped,
    Playing,
    Paused
};

enum class EofSource : uint8_t {
    AudioRender = 0,
    DecoderWorker = 1,
    SyntheticTest = 2
};

enum class AutoAdvanceRejectReason : uint8_t {
    None = 0,
    DuplicateEof,
    StalePlaybackGeneration,
    StaleDecoderRequest,
    TransitionInProgress,
    SessionInactive,
    TransportNotPlaying,
    TrackIdentityMismatch,
    DeckLocked,
    EndOfFolder,
    OpenTrackFailed
};

[[nodiscard]] inline const char* toReasonString(AutoAdvanceRejectReason r) noexcept {
    switch (r) {
    case AutoAdvanceRejectReason::None: return "";
    case AutoAdvanceRejectReason::DuplicateEof: return "duplicate_eof";
    case AutoAdvanceRejectReason::StalePlaybackGeneration: return "stale_playback_generation";
    case AutoAdvanceRejectReason::StaleDecoderRequest: return "stale_decoder_request";
    case AutoAdvanceRejectReason::TransitionInProgress: return "transition_in_progress";
    case AutoAdvanceRejectReason::SessionInactive: return "session_inactive";
    case AutoAdvanceRejectReason::TransportNotPlaying: return "transport_not_playing";
    case AutoAdvanceRejectReason::TrackIdentityMismatch: return "track_identity_mismatch";
    case AutoAdvanceRejectReason::DeckLocked: return "deck_locked";
    case AutoAdvanceRejectReason::EndOfFolder: return "completed";
    case AutoAdvanceRejectReason::OpenTrackFailed: return "open_track_failed";
    }
    return "unknown";
}

[[nodiscard]] inline const char* toSourceString(EofSource s) noexcept {
    switch (s) {
    case EofSource::AudioRender: return "AudioRender";
    case EofSource::DecoderWorker: return "DecoderWorker";
    case EofSource::SyntheticTest: return "SyntheticTest";
    }
    return "Unknown";
}

/// Trivially copyable EOF notification for lock-free RT -> Control Thread delivery.
struct EofNotification {
    uint64_t playbackGeneration = 0;
    uint64_t decoderRequestId = 0;
    uint64_t folderSessionId = 0;
    size_t trackIndex = 0;
    uint64_t trackPathHash = 0;
    int64_t emittedTimestampMs = 0;
    EofSource source = EofSource::AudioRender;
};

struct AutoAdvanceResult {
    bool executed = false;
    bool advanced = false;
    bool validNotification = false;
    AutoAdvanceRejectReason rejectReason = AutoAdvanceRejectReason::None;
    uint64_t playbackGenerationBefore = 0;
    uint64_t playbackGenerationAfter = 0;
    uint64_t decoderRequestIdBefore = 0;
    uint64_t decoderRequestIdAfter = 0;
    uint64_t folderSessionId = 0;
    int trackIndexFrom = -1;
    int trackIndexTo = -1;
    std::string pathFrom;
    std::string pathTo;
    int64_t eventTimestampMs = 0;
    int64_t receivedTimestampMs = 0;
    int64_t transitionStartTimestampMs = 0;
    int64_t transitionCompleteTimestampMs = 0;
    EofSource source = EofSource::AudioRender;
};

/// Fixed-capacity lock-free ring buffer for EofNotification (zero heap allocation, RT-safe push).
/// Confirmed Production Model (SPSC): 1 RT Audio Thread producer (ReferenceDeck::renderBlock())
/// + 1 Control Thread consumer (ReferenceDeck::processPendingEof()).
/// MAX_CAS_RETRIES bounds execution time if test helpers push concurrently.
class EofNotificationQueue {
public:
    static constexpr size_t CAPACITY = 64;
    static constexpr size_t MASK = CAPACITY - 1;
    static constexpr size_t MAX_CAS_RETRIES = 8;

    enum class PushResult : uint8_t {
        Success = 0,
        QueueFull = 1,
        CasContention = 2
    };

    PushResult pushDetailed(const EofNotification& notif) noexcept {
        uint64_t writeIdx = m_writeSeq.load(std::memory_order_relaxed);
        for (size_t attempt = 0; attempt < MAX_CAS_RETRIES; ++attempt) {
            const uint64_t readIdx = m_readSeq.load(std::memory_order_acquire);
            if (writeIdx - readIdx >= CAPACITY) {
                m_queueFullCount.fetch_add(1, std::memory_order_relaxed);
                return PushResult::QueueFull;
            }
            if (m_writeSeq.compare_exchange_weak(writeIdx, writeIdx + 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
                Slot& slot = m_slots[writeIdx & MASK];
                slot.data = notif;
                slot.ready.store(true, std::memory_order_release);
                return PushResult::Success;
            }
        }
        m_casContentionCount.fetch_add(1, std::memory_order_relaxed);
        return PushResult::CasContention;
    }

    bool push(const EofNotification& notif) noexcept {
        return pushDetailed(notif) == PushResult::Success;
    }

    bool pop(EofNotification& out) noexcept {
        const uint64_t readIdx = m_readSeq.load(std::memory_order_relaxed);
        if (readIdx >= m_writeSeq.load(std::memory_order_acquire)) {
            return false;
        }
        Slot& slot = m_slots[readIdx & MASK];
        if (!slot.ready.load(std::memory_order_acquire)) {
            return false;
        }
        out = slot.data;
        slot.ready.store(false, std::memory_order_release);
        m_readSeq.store(readIdx + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] uint64_t queueFullCount() const noexcept {
        return m_queueFullCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t casContentionCount() const noexcept {
        return m_casContentionCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t overflowCount() const noexcept {
        return m_queueFullCount.load(std::memory_order_relaxed) +
               m_casContentionCount.load(std::memory_order_relaxed);
    }

private:
    struct Slot {
        std::atomic<bool> ready{false};
        EofNotification data{};
    };
    std::array<Slot, CAPACITY> m_slots{};
    std::atomic<uint64_t> m_writeSeq{0};
    std::atomic<uint64_t> m_readSeq{0};
    std::atomic<uint64_t> m_queueFullCount{0};
    std::atomic<uint64_t> m_casContentionCount{0};
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
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_isLocked.store(locked, std::memory_order_release);
        if (locked) {
            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            discardPendingOverflow();
            m_currentTrackPathHash.store(0, std::memory_order_release);
            m_folderSession.stopSession(); // P0-10: Deck lock terminates and discards folder session
        }
    }

    /// Section 2.7.5: Terminate and discard Folder Session while preserving the currently loaded track context
    void terminateFolderSession() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (m_folderSession.isActive()) {
            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            discardPendingOverflow();
            m_folderSession.stopSession();
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
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false; // Protected by Deck Lock!
        }

        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_lastHandledEofGeneration.store(0, std::memory_order_release);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_folderSession.stopSession();
        bool ok = m_worker.openTrack(filePath, graphSampleRate);
        if (ok) {
            m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(filePath), std::memory_order_release);
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_trackChanged.store(true, std::memory_order_release);
        }
        return ok;
    }

    /// Synthetic/Benchmark track loading overload (backward compatibility for Phase 0 benchmarks)
    bool loadTrack(const TrackInfo& info, double graphSampleRate) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false;
        }

        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_lastHandledEofGeneration.store(0, std::memory_order_release);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_folderSession.stopSession();
        bool ok = m_worker.openSyntheticTrack(info.totalFrames, info.sampleRate, graphSampleRate, info.path);
        if (ok) {
            m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(info.path), std::memory_order_release);
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_trackChanged.store(true, std::memory_order_release);
        }
        return ok;
    }

    /// Start a Folder Session (Continuous playback of folder contents)
    /// Section 2.7: Starts strictly from Index 0, deterministic natural sort, single next-track read-ahead.
    bool startFolderSession(const std::string& folderPath, const std::vector<std::string>& fileNames, double graphSampleRate) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (m_isLocked.load(std::memory_order_acquire)) {
            return false; // Protected by Deck Lock!
        }

        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_lastHandledEofGeneration.store(0, std::memory_order_release);
        m_needsFolderAdvance.store(false, std::memory_order_release);

        if (!m_folderSession.startSession(folderPath, fileNames)) {
            return false;
        }

        m_graphRate.store(graphSampleRate, std::memory_order_release);
        const auto* firstTrack = m_folderSession.currentTrack();
        if (!firstTrack) return false;

        bool ok = m_worker.openTrack(firstTrack->path, graphSampleRate);
        if (ok) {
            m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(firstTrack->path), std::memory_order_release);
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_trackChanged.store(true, std::memory_order_release);
        } else {
            m_currentTrackPathHash.store(0, std::memory_order_release);
            m_folderSession.stopSession();
        }
        return ok;
    }

    /// Advance to next track in Folder Session (manual or direct Control Thread call)
    bool advanceFolderNext() {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (!m_folderSession.isActive()) return false;

        m_transitionInProgress.store(true, std::memory_order_release);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);

        bool advanced = false;
        if (m_folderSession.advanceNext()) {
            const auto* nextTrack = m_folderSession.currentTrack();
            if (nextTrack) {
                m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(nextTrack->path), std::memory_order_release);
                bool ok = m_worker.openTrack(nextTrack->path, m_graphRate.load(std::memory_order_relaxed));
                if (ok) {
                    m_trackChanged.store(true, std::memory_order_release);
                    if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                        m_worker.play();
                    }
                    advanced = true;
                } else {
                    // P0-10: Open track failed. Stop folder session safely without skipping corrupted track.
                    m_folderSession.stopSession();
                    m_currentTrackPathHash.store(0, std::memory_order_release);
                }
            }
        }

        if (!advanced) {
            stopLocked();
        }
        m_transitionInProgress.store(false, std::memory_order_release);
        return advanced;
    }

    /// Validate and execute AutoAdvance for a specific EOF notification on Control Thread.
    AutoAdvanceResult tryAutoAdvance(const EofNotification& notif) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        AutoAdvanceResult res;
        res.receivedTimestampMs = currentEpochMs();
        res.eventTimestampMs = notif.emittedTimestampMs;
        res.source = notif.source;
        res.playbackGenerationBefore = m_playbackGeneration.load(std::memory_order_acquire);
        res.decoderRequestIdBefore = m_worker.committedRequestId();
        res.folderSessionId = m_folderSession.sessionId();
        res.trackIndexFrom = m_folderSession.currentTrack() ? static_cast<int>(m_folderSession.currentIndex()) : -1;
        res.pathFrom = m_folderSession.currentTrack() ? m_folderSession.currentTrack()->path : std::string();

        if (m_isLocked.load(std::memory_order_acquire)) {
            res.rejectReason = AutoAdvanceRejectReason::DeckLocked;
            return res;
        }
        if (!m_folderSession.isActive() ||
            (notif.folderSessionId != 0 && notif.folderSessionId != m_folderSession.sessionId())) {
            res.rejectReason = AutoAdvanceRejectReason::SessionInactive;
            return res;
        }
        if (m_state.load(std::memory_order_acquire) != TransportState::Playing) {
            res.rejectReason = AutoAdvanceRejectReason::TransportNotPlaying;
            return res;
        }
        if (m_transitionInProgress.load(std::memory_order_acquire)) {
            res.rejectReason = AutoAdvanceRejectReason::TransitionInProgress;
            return res;
        }
        if (notif.playbackGeneration != 0 &&
            m_lastHandledEofGeneration.load(std::memory_order_acquire) == notif.playbackGeneration) {
            res.rejectReason = AutoAdvanceRejectReason::DuplicateEof;
            return res;
        }
        if (notif.playbackGeneration != res.playbackGenerationBefore) {
            res.rejectReason = AutoAdvanceRejectReason::StalePlaybackGeneration;
            return res;
        }
        if (notif.decoderRequestId != 0 && notif.decoderRequestId != res.decoderRequestIdBefore) {
            res.rejectReason = AutoAdvanceRejectReason::StaleDecoderRequest;
            return res;
        }
        const uint64_t currentHash = m_currentTrackPathHash.load(std::memory_order_acquire);
        if (notif.trackIndex != m_folderSession.currentIndex() ||
            (notif.trackPathHash != 0 && notif.trackPathHash != currentHash)) {
            res.rejectReason = AutoAdvanceRejectReason::TrackIdentityMismatch;
            return res;
        }

        res.validNotification = true;
        res.transitionStartTimestampMs = currentEpochMs();
        m_transitionInProgress.store(true, std::memory_order_release);
        m_lastHandledEofGeneration.store(notif.playbackGeneration, std::memory_order_release);
        m_needsFolderAdvance.store(false, std::memory_order_release);

        if (m_preTransitionHook) {
            m_preTransitionHook();
        }

        const uint64_t newGen = m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

        bool advanced = false;
        if (m_folderSession.advanceNext()) {
            const auto* nextTrack = m_folderSession.currentTrack();
            if (nextTrack) {
                m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(nextTrack->path), std::memory_order_release);
                bool ok = m_worker.openTrack(nextTrack->path, m_graphRate.load(std::memory_order_relaxed));
                if (ok) {
                    m_trackChanged.store(true, std::memory_order_release);
                    if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                        m_worker.play();
                    }
                    advanced = true;
                } else {
                    m_folderSession.stopSession();
                    m_currentTrackPathHash.store(0, std::memory_order_release);
                    m_state.store(TransportState::Stopped, std::memory_order_release);
                    m_worker.stop();
                    res.rejectReason = AutoAdvanceRejectReason::OpenTrackFailed;
                }
            }
        } else {
            m_folderSession.stopSession();
            m_currentTrackPathHash.store(0, std::memory_order_release);
            m_state.store(TransportState::Stopped, std::memory_order_release);
            m_worker.stop();
            res.rejectReason = AutoAdvanceRejectReason::EndOfFolder;
        }

        m_transitionInProgress.store(false, std::memory_order_release);
        res.transitionCompleteTimestampMs = currentEpochMs();
        res.executed = true;
        res.advanced = advanced;
        res.playbackGenerationAfter = newGen;
        res.decoderRequestIdAfter = m_worker.committedRequestId();
        res.trackIndexTo = m_folderSession.currentTrack() ? static_cast<int>(m_folderSession.currentIndex()) : -1;
        res.pathTo = m_folderSession.currentTrack() ? m_folderSession.currentTrack()->path : std::string();
        return res;
    }

    /// Test-only EOF notification injection (production uses renderBlock() on RT Audio Thread only).
    bool submitEofNotificationForTest(const EofNotification& notif) noexcept {
        if (m_eofQueue.push(notif)) {
            m_needsFolderAdvance.store(true, std::memory_order_release);
            return true;
        }
        recordEofOverflow(notif);
        m_needsFolderAdvance.store(true, std::memory_order_release);
        return false;
    }

    /// Legacy test alias for submitEofNotificationForTest(). Not called in production.
    bool submitEofNotification(const EofNotification& notif) noexcept {
        return submitEofNotificationForTest(notif);
    }

    /// Drain all pending EOF notifications and process them sequentially on Control Thread.
    std::vector<AutoAdvanceResult> processPendingEof() {
        std::vector<AutoAdvanceResult> results;
        EofNotification notif;
        while (m_eofQueue.pop(notif)) {
            results.push_back(tryAutoAdvance(notif));
        }
        EofNotification recovered;
        while (tryPopOverflow(recovered)) {
            results.push_back(tryAutoAdvance(recovered));
        }
        m_needsFolderAdvance.store(false, std::memory_order_release);
        return results;
    }

    [[nodiscard]] uint64_t eofQueueFullCount() const noexcept {
        return m_eofQueue.queueFullCount();
    }

    [[nodiscard]] uint64_t eofQueueCasContentionCount() const noexcept {
        return m_eofQueue.casContentionCount();
    }

    [[nodiscard]] uint64_t eofQueueOverflowCount() const noexcept {
        return m_eofQueueOverflowCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t eofOverflowCasContentionCount() const noexcept {
        return m_overflowCasContentionCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t eofOverflowDropCount() const noexcept {
        return m_eofOverflowDropCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] EofNotificationQueue& eofQueueForTest() noexcept { return m_eofQueue; }
    [[nodiscard]] bool recordEofOverflowForTest(const EofNotification& notif) noexcept {
        return recordEofOverflow(notif);
    }
    [[nodiscard]] bool tryPopOverflowForTest(EofNotification& out) noexcept {
        return tryPopOverflow(out);
    }

    void setPreTransitionHook(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_preTransitionHook = std::move(hook);
    }

    [[nodiscard]] uint64_t playbackGeneration() const noexcept { return m_playbackGeneration.load(std::memory_order_acquire); }
    [[nodiscard]] bool isTransitionInProgress() const noexcept { return m_transitionInProgress.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t currentTrackPathHash() const noexcept { return m_currentTrackPathHash.load(std::memory_order_acquire); }

    /// Retreat to previous track in Folder Session (executed on Control Thread)
    bool retreatFolderPrevious() {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        if (!m_folderSession.isActive()) return false;

        m_transitionInProgress.store(true, std::memory_order_release);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);

        bool retreated = false;
        if (m_folderSession.retreatPrevious()) {
            const auto* prevTrack = m_folderSession.currentTrack();
            if (prevTrack) {
                m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(prevTrack->path), std::memory_order_release);
                bool ok = m_worker.openTrack(prevTrack->path, m_graphRate.load(std::memory_order_relaxed));
                if (ok) {
                    m_trackChanged.store(true, std::memory_order_release);
                    if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                        m_worker.play();
                    }
                    retreated = true;
                }
            }
        }
        m_transitionInProgress.store(false, std::memory_order_release);
        return retreated;
    }

    /// Section 2.7.5: Transport operations (Play, Pause, Stop, Seek) ARE PERMITTED even during Deck Lock!
    void play() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_state.store(TransportState::Playing, std::memory_order_release);
        m_worker.play();
    }

    void pause() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        discardPendingOverflow();
        m_state.store(TransportState::Paused, std::memory_order_release);
        m_worker.pause();
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        stopLocked();
    }

    void closeTrack() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_currentTrackPathHash.store(0, std::memory_order_release);
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
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        discardPendingOverflow();
        m_worker.seek(targetFrame);
    }

    decoder::SeekResult seekWithResult(int64_t targetFrame) noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        discardPendingOverflow();
        return m_worker.seekWithResult(targetFrame);
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

        const uint64_t genSnapshot = m_playbackGeneration.load(std::memory_order_acquire);
        size_t read = m_worker.readAudioBlock(outLeft, outRight, frames);

        if (m_worker.isEofDelivered()) {
            m_worker.clearEofDelivered();
            if (m_playbackGeneration.load(std::memory_order_acquire) == genSnapshot &&
                !m_transitionInProgress.load(std::memory_order_acquire)) {
                if (m_folderSession.isActive()) {
                    EofNotification notif;
                    notif.playbackGeneration = genSnapshot;
                    notif.decoderRequestId = m_worker.eofRequestId();
                    notif.folderSessionId = m_folderSession.sessionId();
                    notif.trackIndex = m_folderSession.currentIndex();
                    notif.trackPathHash = m_currentTrackPathHash.load(std::memory_order_acquire);
                    notif.emittedTimestampMs = m_worker.eofEmittedTimestampMs();
                    notif.source = EofSource::AudioRender;
                    if (!m_eofQueue.push(notif)) {
                        recordEofOverflow(notif);
                    }
                    m_needsFolderAdvance.store(true, std::memory_order_release);
                } else {
                    m_state.store(TransportState::Stopped, std::memory_order_release);
                }
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
    static int64_t currentEpochMs() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /// Lock-free sequence-stamped overflow fallback bank (OVERFLOW_SLOT_COUNT Seqlock slots over atomic fields).
    /// Zero locks, zero heap allocation, bounded CAS retries, TSan-clean (no atomic_thread_fence), torn-read-free.
    static constexpr size_t OVERFLOW_SLOT_COUNT = 8;

    struct OverflowSlot {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint64_t> lastConsumedSeq{0};
        std::atomic<uint64_t> generation{0};
        std::atomic<uint64_t> requestId{0};
        std::atomic<uint64_t> sessionId{0};
        std::atomic<size_t> trackIndex{0};
        std::atomic<uint64_t> trackPathHash{0};
        std::atomic<int64_t> timestampMs{0};
        std::atomic<uint8_t> source{0};
    };

    bool recordEofOverflow(const EofNotification& notif) noexcept {
        m_eofQueueOverflowCount.fetch_add(1, std::memory_order_relaxed);
        const size_t startIdx =
            static_cast<size_t>(m_overflowWriteCursor.fetch_add(1, std::memory_order_relaxed)) % OVERFLOW_SLOT_COUNT;

        for (size_t attempt = 0; attempt < EofNotificationQueue::MAX_CAS_RETRIES; ++attempt) {
            OverflowSlot& slot = m_overflowSlots[(startIdx + attempt) % OVERFLOW_SLOT_COUNT];
            uint64_t seq = slot.seq.load(std::memory_order_acquire);
            if ((seq & 1ULL) != 0ULL) {
                m_overflowCasContentionCount.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const uint64_t lastConsumed = slot.lastConsumedSeq.load(std::memory_order_acquire);
            if (seq > lastConsumed) {
                // Unread notification present in this slot: never overwrite.
                continue;
            }
            if (slot.seq.compare_exchange_weak(seq, seq + 1ULL,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                slot.generation.store(notif.playbackGeneration, std::memory_order_release);
                slot.requestId.store(notif.decoderRequestId, std::memory_order_release);
                slot.sessionId.store(notif.folderSessionId, std::memory_order_release);
                slot.trackIndex.store(notif.trackIndex, std::memory_order_release);
                slot.trackPathHash.store(notif.trackPathHash, std::memory_order_release);
                slot.timestampMs.store(notif.emittedTimestampMs, std::memory_order_release);
                slot.source.store(static_cast<uint8_t>(notif.source), std::memory_order_release);
                slot.seq.store(seq + 2ULL, std::memory_order_release);
                return true;
            }
            m_overflowCasContentionCount.fetch_add(1, std::memory_order_relaxed);
        }

        m_eofOverflowDropCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool tryPopOverflow(EofNotification& out) noexcept {
        static constexpr size_t MAX_READ_RETRIES = 16;
        const size_t startIdx = m_overflowReadCursor % OVERFLOW_SLOT_COUNT;
        for (size_t offset = 0; offset < OVERFLOW_SLOT_COUNT; ++offset) {
            const size_t idx = (startIdx + offset) % OVERFLOW_SLOT_COUNT;
            OverflowSlot& slot = m_overflowSlots[idx];
            for (size_t attempt = 0; attempt < MAX_READ_RETRIES; ++attempt) {
                const uint64_t seq1 = slot.seq.load(std::memory_order_acquire);
                uint64_t lastConsumed = slot.lastConsumedSeq.load(std::memory_order_acquire);
                if ((seq1 & 1ULL) != 0ULL) {
                    continue; // Writer currently updating overflow fields
                }
                if (seq1 <= lastConsumed) {
                    break; // No unconsumed overflow notification in this slot
                }

                EofNotification candidate;
                candidate.playbackGeneration = slot.generation.load(std::memory_order_acquire);
                candidate.decoderRequestId = slot.requestId.load(std::memory_order_acquire);
                candidate.folderSessionId = slot.sessionId.load(std::memory_order_acquire);
                candidate.trackIndex = slot.trackIndex.load(std::memory_order_acquire);
                candidate.trackPathHash = slot.trackPathHash.load(std::memory_order_acquire);
                candidate.emittedTimestampMs = slot.timestampMs.load(std::memory_order_acquire);
                candidate.source = static_cast<EofSource>(slot.source.load(std::memory_order_acquire));

                const uint64_t seq2 = slot.seq.load(std::memory_order_acquire);
                if (seq1 == seq2) {
                    if (slot.lastConsumedSeq.compare_exchange_strong(lastConsumed, seq1,
                                                                     std::memory_order_acq_rel,
                                                                     std::memory_order_acquire)) {
                        m_overflowReadCursor = (idx + 1) % OVERFLOW_SLOT_COUNT;
                        out = candidate;
                        return true;
                    }
                    break;
                }
            }
        }
        return false;
    }

    void discardPendingOverflow() noexcept {
        for (OverflowSlot& slot : m_overflowSlots) {
            const uint64_t seq = slot.seq.load(std::memory_order_acquire);
            const uint64_t evenSeq = (seq + 1ULL) & ~1ULL;
            uint64_t cur = slot.lastConsumedSeq.load(std::memory_order_relaxed);
            while (cur < evenSeq &&
                   !slot.lastConsumedSeq.compare_exchange_weak(cur, evenSeq,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_relaxed)) {
            }
        }
    }

    void stopLocked() noexcept {
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        discardPendingOverflow();
        m_folderSession.stopSession(); // v3.7 §2.7.3: Stop terminates and discards Folder Session while preserving loaded track context
        m_currentTrackPathHash.store(0, std::memory_order_release);
        m_state.store(TransportState::Stopped, std::memory_order_release);
        m_worker.stop();
    }

    char m_id;
    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<bool> m_isLocked{false};
    std::atomic<int64_t> m_offsetFrames{0};
    std::atomic<double> m_graphRate{48000.0};
    std::atomic<bool> m_trackChanged{false};
    std::atomic<bool> m_needsFolderAdvance{false};
    std::atomic<uint64_t> m_playbackGeneration{0};
    std::atomic<uint64_t> m_lastHandledEofGeneration{0};
    std::atomic<bool> m_transitionInProgress{false};
    std::atomic<uint64_t> m_currentTrackPathHash{0};

    std::atomic<uint64_t> m_eofQueueOverflowCount{0};
    std::atomic<uint64_t> m_overflowCasContentionCount{0};
    std::atomic<uint64_t> m_eofOverflowDropCount{0};
    std::atomic<uint64_t> m_overflowWriteCursor{0};
    size_t m_overflowReadCursor{0};
    std::array<OverflowSlot, OVERFLOW_SLOT_COUNT> m_overflowSlots{};

    std::mutex m_controlMutex;
    std::function<void()> m_preTransitionHook;
    EofNotificationQueue m_eofQueue;
    session::FolderSession m_folderSession;
    decoder::DecoderWorker m_worker;

    alignas(64) std::array<double, RT_BUFFER_CAPACITY> m_scratchL{};
    alignas(64) std::array<double, RT_BUFFER_CAPACITY> m_scratchR{};
};

} // namespace desk32::engine::deck
