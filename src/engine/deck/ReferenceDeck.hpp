#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include "engine/decoder/DecoderWorker.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/session/FolderSession.hpp"
#include "engine/metadata/TrackMetadata.hpp"
#include <QFileInfo>

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
    bool decodeError = false;
};

struct FolderSessionEndEvent {
    char deckId = 'A';
    uint64_t folderSessionId = 0;
    std::string folderPath;
    size_t trackIndex = 0;
    size_t totalTracks = 0;
    std::string currentTrackPath;
    std::string reason;
    std::string detail;
    int32_t errorCode = 0;
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
    [[nodiscard]] double levelMatchGainDb() const noexcept { return m_levelMatchGainDb.load(std::memory_order_acquire); }
    [[nodiscard]] double levelMatchLinearGain() const noexcept {
        return std::pow(10.0, m_levelMatchGainDb.load(std::memory_order_acquire) / 20.0);
    }
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

    void reloadMetadata() {
        m_worker.reloadMetadata();
    }

    using ErrorLogCallback = std::function<void(const std::string& operation, int32_t errorCode, const std::string& target, const std::string& message)>;
    void setErrorLogCallback(ErrorLogCallback cb) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_errorLogCallback = std::move(cb);
    }

    using FolderSessionEndCallback = std::function<void(const FolderSessionEndEvent&)>;
    void setFolderSessionEndCallback(FolderSessionEndCallback cb) {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_folderSessionEndCallback = std::move(cb);
    }

    [[nodiscard]] std::string lastFolderSessionError() const {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        return m_lastFolderSessionError;
    }

    void setPausedByNormalSwitch(bool paused) noexcept {
        m_pausedByNormalSwitch.store(paused, std::memory_order_release);
    }

    [[nodiscard]] bool wasPausedByNormalSwitch() const noexcept {
        return m_pausedByNormalSwitch.load(std::memory_order_acquire);
    }

    void setPreviousRestartThresholdSec(double sec) noexcept {
        m_previousRestartThresholdSec.store(std::max(0.0, sec), std::memory_order_release);
    }

    [[nodiscard]] double previousRestartThresholdSec() const noexcept {
        return m_previousRestartThresholdSec.load(std::memory_order_acquire);
    }

    struct KWeightBiquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0;
        double a1 = 0.0, a2 = 0.0;
        double z1L = 0.0, z2L = 0.0;
        double z1R = 0.0, z2R = 0.0;

        inline void processSample(double inL, double inR, double& outL, double& outR) noexcept {
            outL = (b0 * inL) + z1L;
            z1L  = (b1 * inL) - (a1 * outL) + z2L;
            z2L  = (b2 * inL) - (a2 * outL);

            outR = (b0 * inR) + z1R;
            z1R  = (b1 * inR) - (a1 * outR) + z2R;
            z2R  = (b2 * inR) - (a2 * outR);
        }
    };

    /// Compute ITU-R BS.1770-4 K-weighting stage 1 (high-shelf) & stage 2 (RLB high-pass) coefficients
    /// for the decoder's native sample rate (exact published constants at 44.1k/48k, analog prototype
    /// bilinear transform for 88.2k/96k/176.4k/192k and arbitrary rates).
    static void computeKWeightingStages(double sampleRate, KWeightBiquad& stage1, KWeightBiquad& stage2) noexcept {
        const double fs = std::max(8000.0, sampleRate);
        if (std::abs(fs - 48000.0) < 0.5) {
            stage1 = KWeightBiquad{
                1.53512485958697, -2.69169618940638, 1.19839281085285,
                -1.69065929318241, 0.73248077421585
            };
            stage2 = KWeightBiquad{
                1.0, -2.0, 1.0,
                -1.99004745483398, 0.99007225036621
            };
            return;
        }
        if (std::abs(fs - 44100.0) < 0.5) {
            stage1 = KWeightBiquad{
                1.5308412300503478, -2.6509799951547297, 1.169079079921587,
                -1.6636551132560204, 0.7125954280732254
            };
            stage2 = KWeightBiquad{
                1.0, -2.0, 1.0,
                -1.989169673629796, 0.9891990357870393
            };
            return;
        }

        // Stage 1: ITU-R BS.1770-4 high-shelf pre-filter bilinear transform
        constexpr double f0_1 = 1681.974450955533;
        constexpr double G_1 = 3.999843853973347;
        constexpr double Q_1 = 0.7071752369554196;
        const double K1 = std::tan(std::numbers::pi * f0_1 / fs);
        const double Vh = std::pow(10.0, G_1 / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0_1 = 1.0 + (K1 / Q_1) + (K1 * K1);
        stage1 = KWeightBiquad{
            (Vh + (Vb * K1 / Q_1) + (K1 * K1)) / a0_1,
            2.0 * ((K1 * K1) - Vh) / a0_1,
            (Vh - (Vb * K1 / Q_1) + (K1 * K1)) / a0_1,
            2.0 * ((K1 * K1) - 1.0) / a0_1,
            (1.0 - (K1 / Q_1) + (K1 * K1)) / a0_1
        };

        // Stage 2: ITU-R BS.1770-4 RLB high-pass bilinear transform
        constexpr double f0_2 = 38.13547087602444;
        constexpr double Q_2 = 0.5003270373238773;
        const double K2 = std::tan(std::numbers::pi * f0_2 / fs);
        const double a0_2 = 1.0 + (K2 / Q_2) + (K2 * K2);
        stage2 = KWeightBiquad{
            1.0,
            -2.0,
            1.0,
            2.0 * ((K2 * K2) - 1.0) / a0_2,
            (1.0 - (K2 / Q_2) + (K2 * K2)) / a0_2
        };
    }

    /// Thread-safe ITU-R BS.1770-4 / EBU R128 integrated loudness measurement (LUFS) with
    /// sample-rate-dependent K-weighting and 400 ms dual-stage gating (-70 LUFS abs, -10 LU rel).
    static bool measureTrackLufs(const std::string& filePath,
                                 double& outLufs,
                                 const std::atomic<bool>* cancelFlag = nullptr,
                                 double maxDurationSec = 0.0) {
        if (filePath.empty()) return false;
        if (filePath.rfind("synthetic://", 0) == 0) {
            // Synthetic stereo test sine has peak 0.5 per channel -> total power 0.25 -> -6.71 LUFS
            outLufs = -0.691 + (10.0 * std::log10(0.25));
            return true;
        }
        decoder::FFmpegAudioDecoder dec;
        if (!dec.open(filePath)) {
            return false;
        }
        const auto& fmt = dec.formatInfo();
        const double sr = fmt.sampleRate > 0.0 ? fmt.sampleRate : 48000.0;

        KWeightBiquad stage1, stage2;
        computeKWeightingStages(sr, stage1, stage2);

        const size_t subBlockFrames = std::max<size_t>(1, static_cast<size_t>(std::llround(sr * 0.1))); // 100 ms step
        const size_t maxFrames = (maxDurationSec > 0.0)
                                     ? static_cast<size_t>(std::max(0.4, maxDurationSec) * sr)
                                     : std::numeric_limits<size_t>::max();

        std::array<double, 2048> bufL{};
        std::array<double, 2048> bufR{};
        std::vector<double> subPowers;
        if (fmt.durationSeconds > 0.0) {
            subPowers.reserve(static_cast<size_t>(fmt.durationSeconds * 10.0) + 16);
        }

        double subSq = 0.0;
        size_t subCount = 0;
        double totalSq = 0.0;
        size_t totalFrames = 0;

        while (totalFrames < maxFrames) {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                dec.close();
                return false;
            }
            const size_t remaining = maxFrames - totalFrames;
            const size_t chunk = std::min(bufL.size(), remaining);
            const size_t got = dec.readFrames(bufL.data(), bufR.data(), chunk);
            if (got == 0) break;

            for (size_t i = 0; i < got; ++i) {
                double s1L = 0.0, s1R = 0.0, kwL = 0.0, kwR = 0.0;
                stage1.processSample(bufL[i], bufR[i], s1L, s1R);
                stage2.processSample(s1L, s1R, kwL, kwR);
                const double p = (kwL * kwL) + (kwR * kwR);
                subSq += p;
                totalSq += p;
                ++subCount;
                if (subCount >= subBlockFrames) {
                    subPowers.push_back(subSq / static_cast<double>(subCount));
                    subSq = 0.0;
                    subCount = 0;
                }
            }
            totalFrames += got;
        }
        dec.close();

        if (totalFrames == 0) {
            outLufs = -70.0;
            return false;
        }
        if (subCount >= (subBlockFrames / 2)) {
            subPowers.push_back(subSq / static_cast<double>(subCount));
        }

        // Absolute gate: -70.0 LUFS => power threshold = 10^((-70 + 0.691) / 10)
        const double absGatePower = std::pow(10.0, (-70.0 + 0.691) / 10.0);

        // Short clips (< 400 ms): integrate directly across decoded frames
        if (subPowers.size() < 4) {
            const double p = totalSq / static_cast<double>(totalFrames);
            if (p <= absGatePower) {
                outLufs = -70.0;
                return false;
            }
            outLufs = -0.691 + (10.0 * std::log10(std::max(p, 1e-12)));
            return true;
        }

        // 400 ms sliding gating blocks (4 consecutive 100 ms sub-blocks, 75% overlap)
        std::vector<double> blockPowers;
        blockPowers.reserve(subPowers.size() - 3);
        double absSum = 0.0;
        size_t absCount = 0;
        for (size_t i = 0; i + 3 < subPowers.size(); ++i) {
            const double bp = 0.25 * (subPowers[i] + subPowers[i + 1] + subPowers[i + 2] + subPowers[i + 3]);
            blockPowers.push_back(bp);
            if (bp > absGatePower) {
                absSum += bp;
                ++absCount;
            }
        }
        if (absCount == 0) {
            outLufs = -70.0;
            return false;
        }

        // Relative gate: -10.0 LU below ungated intermediate loudness
        const double absMeanPower = absSum / static_cast<double>(absCount);
        const double relGatePower = absMeanPower * 0.1;
        const double finalGatePower = std::max(absGatePower, relGatePower);

        double gatedSum = 0.0;
        size_t gatedCount = 0;
        for (double bp : blockPowers) {
            if (bp > finalGatePower) {
                gatedSum += bp;
                ++gatedCount;
            }
        }
        if (gatedCount == 0) {
            outLufs = -70.0;
            return false;
        }

        const double gatedPower = gatedSum / static_cast<double>(gatedCount);
        outLufs = -0.691 + (10.0 * std::log10(std::max(gatedPower, 1e-12)));
        return true;
    }

    /// Control-thread offline RMS measurement in dBFS over [startSec, startSec + durationSec].
    static bool measureTrackRmsDb(const std::string& filePath, double startSec, double durationSec, double& outRmsDb) {
        if (filePath.empty()) return false;
        if (filePath.rfind("synthetic://", 0) == 0) {
            // Synthetic test sine wave has peak 0.5 -> RMS 0.5 / sqrt(2)
            outRmsDb = 20.0 * std::log10(0.5 / std::sqrt(2.0));
            return true;
        }
        decoder::FFmpegAudioDecoder dec;
        if (!dec.open(filePath)) {
            return false;
        }
        const auto& fmt = dec.formatInfo();
        const double sr = fmt.sampleRate > 0.0 ? fmt.sampleRate : 48000.0;
        const int64_t startFrame = static_cast<int64_t>(std::max(0.0, startSec) * sr);
        if (startFrame > 0) {
            (void)dec.seek(startFrame);
        }
        const size_t targetFrames = static_cast<size_t>(std::max(0.1, durationSec) * sr);
        std::array<double, 2048> bufL{};
        std::array<double, 2048> bufR{};
        size_t totalRead = 0;
        double sumSq = 0.0;
        while (totalRead < targetFrames) {
            const size_t chunk = std::min(bufL.size(), targetFrames - totalRead);
            const size_t got = dec.readFrames(bufL.data(), bufR.data(), chunk);
            if (got == 0) break;
            for (size_t i = 0; i < got; ++i) {
                sumSq += (bufL[i] * bufL[i]) + (bufR[i] * bufR[i]);
            }
            totalRead += got;
        }
        dec.close();
        if (totalRead == 0) {
            outRmsDb = -96.0;
            return false;
        }
        const double meanSq = sumSq / static_cast<double>(totalRead * 2);
        outRmsDb = 10.0 * std::log10(std::max(meanSq, 1e-12));
        return true;
    }

    [[nodiscard]] bool checkAndResetTrackChanged() noexcept {
        return m_trackChanged.exchange(false);
    }

    [[nodiscard]] const session::FolderSession& folderSession() const noexcept { return m_folderSession; }
    [[nodiscard]] session::FolderSession& folderSession() noexcept { return m_folderSession; }

    void setLocked(bool locked) noexcept {
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            m_isLocked.store(locked, std::memory_order_release);
            if (locked) {
                m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
                m_needsFolderAdvance.store(false, std::memory_order_release);
                discardPendingOverflow();
                m_currentTrackPathHash.store(0, std::memory_order_release);
                emitEnd = captureAndStopSessionLocked("deck_locked", "", 0, endEv, endCb); // P0-10: Deck lock terminates and discards folder session
            }
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
    }

    /// Section 2.7.5: Terminate and discard Folder Session while preserving the currently loaded track context
    void terminateFolderSession(const std::string& reason = "terminated") noexcept {
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (m_folderSession.isActive()) {
                m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
                m_needsFolderAdvance.store(false, std::memory_order_release);
                discardPendingOverflow();
                emitEnd = captureAndStopSessionLocked(reason, "", 0, endEv, endCb);
            }
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
    }
    void setOffsetFrames(int64_t offset) noexcept { m_offsetFrames.store(offset, std::memory_order_release); }
    void setLevelMatchGainDb(double db) noexcept {
        m_levelMatchGainDb.store(std::clamp(db, -24.0, 24.0), std::memory_order_release);
    }

    /// Reconfigure graph sample rate across worker and resamplers
    void configureGraphRate(double graphSampleRate) noexcept {
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        m_worker.configureGraphRate(graphSampleRate);
    }

    /// Load standalone audio track using background DecoderWorker.
    /// Section 2.7.5: Blocked if Deck is locked.
    bool loadTrack(const std::string& filePath, double graphSampleRate) {
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (m_isLocked.load(std::memory_order_acquire)) {
                return false; // Protected by Deck Lock!
            }

            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_lastHandledEofGeneration.store(0, std::memory_order_release);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            m_pausedByNormalSwitch.store(false, std::memory_order_release);
            m_decodeErrorNotified.store(false, std::memory_order_release);
            m_lastFolderSessionError.clear();
            m_graphRate.store(graphSampleRate, std::memory_order_release);
            emitEnd = captureAndStopSessionLocked("track_replaced", filePath, 0, endEv, endCb);
            ok = m_worker.openTrack(filePath, graphSampleRate);
            if (ok) {
                m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(filePath), std::memory_order_release);
                m_state.store(TransportState::Stopped, std::memory_order_release);
                m_trackChanged.store(true, std::memory_order_release);
            }
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
        return ok;
    }

    /// Synthetic/Benchmark track loading overload (backward compatibility for Phase 0 benchmarks)
    bool loadTrack(const TrackInfo& info, double graphSampleRate) {
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (m_isLocked.load(std::memory_order_acquire)) {
                return false;
            }

            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_lastHandledEofGeneration.store(0, std::memory_order_release);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            m_pausedByNormalSwitch.store(false, std::memory_order_release);
            m_decodeErrorNotified.store(false, std::memory_order_release);
            m_lastFolderSessionError.clear();
            m_graphRate.store(graphSampleRate, std::memory_order_release);
            emitEnd = captureAndStopSessionLocked("track_replaced", info.path, 0, endEv, endCb);
            ok = m_worker.openSyntheticTrack(info.totalFrames, info.sampleRate, graphSampleRate, info.path);
            if (ok) {
                m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(info.path), std::memory_order_release);
                m_state.store(TransportState::Stopped, std::memory_order_release);
                m_trackChanged.store(true, std::memory_order_release);
            }
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
        return ok;
    }

    /// Start a Folder Session from the directory containing the currently loaded track (§2.7.2).
    bool startFolderSessionFromCurrentTrack(double graphSampleRate) {
        std::string folderPath;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (m_isLocked.load(std::memory_order_acquire) || !m_worker.isOpen() || m_worker.isSynthetic()) {
                return false;
            }
            const auto info = m_worker.trackInfo();
            if (info.path.empty()) return false;
            folderPath = QFileInfo(QString::fromStdString(info.path)).absolutePath().toStdString();
        }
        if (folderPath.empty()) return false;
        const std::vector<std::string> scanned = session::FolderSession::scanSupportedAudioFiles(folderPath);
        if (scanned.empty()) return false;
        return startFolderSession(folderPath, scanned, graphSampleRate);
    }

    /// Re-validate next track read-ahead candidate (§2.7.4).
    bool prepareNextFolderTrackReadAhead() {
        ErrorLogCallback cbCopy;
        std::string errMsg;
        std::string targetPath;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            ok = validateAndPrepareReadAheadLocked(errMsg, targetPath);
            if (!ok && !errMsg.empty()) {
                cbCopy = m_errorLogCallback;
            }
        }
        if (cbCopy) {
            cbCopy("FolderSessionReadAhead", 1040, targetPath, errMsg);
        }
        return ok;
    }

    /// Start a Folder Session (Continuous playback of folder contents)
    /// Section 2.7: Starts strictly from Index 0, deterministic natural sort, single next-track read-ahead.
    bool startFolderSession(const std::string& folderPath, const std::vector<std::string>& fileNames, double graphSampleRate) {
        ErrorLogCallback cbCopy;
        std::string logOp, logTarget, logMsg;
        int32_t logErrCode = 0;
        FolderSessionEndEvent prevEndEv;
        FolderSessionEndCallback prevEndCb;
        bool emitPrevEnd = false;
        FolderSessionEndEvent failEndEv;
        FolderSessionEndCallback failEndCb;
        bool emitFailEnd = false;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (m_isLocked.load(std::memory_order_acquire)) {
                return false; // Protected by Deck Lock!
            }

            const std::vector<std::string> effectiveFiles =
                fileNames.empty() ? session::FolderSession::scanSupportedAudioFiles(folderPath) : fileNames;

            emitPrevEnd = captureAndStopSessionLocked("new_folder_session", folderPath, 0, prevEndEv, prevEndCb);

            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_lastHandledEofGeneration.store(0, std::memory_order_release);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            m_pausedByNormalSwitch.store(false, std::memory_order_release);
            m_decodeErrorNotified.store(false, std::memory_order_release);
            m_lastFolderSessionError.clear();

            if (m_folderSession.startSession(folderPath, effectiveFiles)) {
                m_graphRate.store(graphSampleRate, std::memory_order_release);
                const auto* firstTrack = m_folderSession.currentTrack();
                if (firstTrack) {
                    ok = m_worker.openTrack(firstTrack->path, graphSampleRate);
                    if (ok) {
                        m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(firstTrack->path), std::memory_order_release);
                        m_state.store(TransportState::Stopped, std::memory_order_release);
                        m_trackChanged.store(true, std::memory_order_release);
                        (void)validateAndPrepareReadAheadLocked(logMsg, logTarget);
                        if (!logMsg.empty()) {
                            cbCopy = m_errorLogCallback;
                            logOp = "FolderSessionReadAhead";
                            logErrCode = 1040;
                        }
                    } else {
                        m_currentTrackPathHash.store(0, std::memory_order_release);
                        logTarget = firstTrack->path;
                        logMsg = "Failed to open initial track in Folder Session: " + logTarget;
                        m_lastFolderSessionError = logMsg;
                        emitFailEnd = captureAndStopSessionLocked("open_initial_failed", logMsg, 1041, failEndEv, failEndCb);
                        logOp = "FolderSessionStart";
                        logErrCode = 1041;
                        cbCopy = m_errorLogCallback;
                    }
                }
            }
        }
        emitFolderSessionEnd(emitPrevEnd, prevEndCb, prevEndEv);
        emitFolderSessionEnd(emitFailEnd, failEndCb, failEndEv);
        if (cbCopy) {
            cbCopy(logOp, logErrCode, logTarget, logMsg);
        }
        return ok;
    }

    /// Advance to next track in Folder Session (manual or direct Control Thread call)
    bool advanceFolderNext() {
        ErrorLogCallback cbCopy;
        std::string logOp, logTarget, logMsg;
        int32_t logErrCode = 0;
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        bool advanced = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (!m_folderSession.isActive()) return false;

            m_transitionInProgress.store(true, std::memory_order_release);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_decodeErrorNotified.store(false, std::memory_order_release);

            if (m_worker.hasDecodeError()) {
                logTarget = m_worker.trackInfo().path;
                logMsg = m_worker.lastDecodeErrorMessage().empty()
                             ? "Unrecoverable mid-stream decode error during Folder Session"
                             : m_worker.lastDecodeErrorMessage();
                m_lastFolderSessionError = logMsg;
                emitEnd = stopLocked("decode_error", logMsg, 1042, endEv, endCb);
                cbCopy = m_errorLogCallback;
                logOp = "FolderSessionDecodeError";
                logErrCode = 1042;
                m_transitionInProgress.store(false, std::memory_order_release);
            } else if (m_folderSession.readAheadStatus() == session::ReadAheadStatus::Failed) {
                const auto* brokenNext = m_folderSession.readAheadTrack();
                logTarget = brokenNext ? brokenNext->path : std::string();
                logMsg = m_folderSession.readAheadError().empty()
                             ? ("Next track unreadable or missing in Folder Session: " + logTarget)
                             : m_folderSession.readAheadError();
                m_lastFolderSessionError = logMsg;
                emitEnd = stopLocked("read_ahead_failed", logMsg, 1040, endEv, endCb);
                cbCopy = m_errorLogCallback;
                logOp = "FolderSessionReadAhead";
                logErrCode = 1040;
                m_transitionInProgress.store(false, std::memory_order_release);
            } else {
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
                            (void)validateAndPrepareReadAheadLocked(logMsg, logTarget);
                            if (!logMsg.empty()) {
                                cbCopy = m_errorLogCallback;
                                logOp = "FolderSessionReadAhead";
                                logErrCode = 1040;
                            }
                            advanced = true;
                        } else {
                            // P0-10: Open track failed. Stop folder session safely without skipping corrupted track.
                            logTarget = nextTrack->path;
                            logMsg = "Failed to open next Folder Session track: " + logTarget;
                            m_lastFolderSessionError = logMsg;
                            emitEnd = stopLocked("open_track_failed", logMsg, 1041, endEv, endCb);
                            cbCopy = m_errorLogCallback;
                            logOp = "FolderSessionAdvance";
                            logErrCode = 1041;
                        }
                    }
                }

                if (!advanced && !emitEnd) {
                    emitEnd = stopLocked("end_of_folder", "", 0, endEv, endCb);
                }
                m_transitionInProgress.store(false, std::memory_order_release);
            }
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
        if (cbCopy) {
            cbCopy(logOp, logErrCode, logTarget, logMsg);
        }
        return advanced;
    }

    /// Validate and execute AutoAdvance for a specific EOF notification on Control Thread.
    AutoAdvanceResult tryAutoAdvance(const EofNotification& notif) {
        ErrorLogCallback cbCopy;
        std::string logOp, logTarget, logMsg;
        int32_t logErrCode = 0;
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        AutoAdvanceResult res;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
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
            if (notif.decodeError || m_worker.hasDecodeError()) {
                res.validNotification = true;
                res.executed = true;
                res.advanced = false;
                res.rejectReason = AutoAdvanceRejectReason::OpenTrackFailed;
                logTarget = res.pathFrom;
                logMsg = m_worker.lastDecodeErrorMessage().empty()
                             ? "Unrecoverable mid-stream decode error during Folder Session"
                             : m_worker.lastDecodeErrorMessage();
                m_lastFolderSessionError = logMsg;
                emitEnd = captureAndStopSessionLocked("decode_error", logMsg, 1042, endEv, endCb);
                m_currentTrackPathHash.store(0, std::memory_order_release);
                m_state.store(TransportState::Stopped, std::memory_order_release);
                m_worker.stop();
                cbCopy = m_errorLogCallback;
                logOp = "FolderSessionDecodeError";
                logErrCode = 1042;
            } else {
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
                if (m_folderSession.readAheadStatus() == session::ReadAheadStatus::Failed) {
                    const auto* brokenNext = m_folderSession.readAheadTrack();
                    logTarget = brokenNext ? brokenNext->path : std::string();
                    logMsg = m_folderSession.readAheadError().empty()
                                 ? ("Next track unreadable or missing in Folder Session: " + logTarget)
                                 : m_folderSession.readAheadError();
                    m_lastFolderSessionError = logMsg;
                    emitEnd = captureAndStopSessionLocked("read_ahead_failed", logMsg, 1040, endEv, endCb);
                    m_currentTrackPathHash.store(0, std::memory_order_release);
                    m_state.store(TransportState::Stopped, std::memory_order_release);
                    m_worker.stop();
                    res.rejectReason = AutoAdvanceRejectReason::OpenTrackFailed;
                    cbCopy = m_errorLogCallback;
                    logOp = "FolderSessionReadAhead";
                    logErrCode = 1040;
                } else if (m_folderSession.advanceNext()) {
                    const auto* nextTrack = m_folderSession.currentTrack();
                    if (nextTrack) {
                        m_currentTrackPathHash.store(session::FolderSession::hashTrackPath(nextTrack->path), std::memory_order_release);
                        bool ok = m_worker.openTrack(nextTrack->path, m_graphRate.load(std::memory_order_relaxed));
                        if (ok) {
                            m_trackChanged.store(true, std::memory_order_release);
                            if (m_state.load(std::memory_order_acquire) == TransportState::Playing) {
                                m_worker.play();
                            }
                            (void)validateAndPrepareReadAheadLocked(logMsg, logTarget);
                            if (!logMsg.empty()) {
                                cbCopy = m_errorLogCallback;
                                logOp = "FolderSessionReadAhead";
                                logErrCode = 1040;
                            }
                            advanced = true;
                        } else {
                            logTarget = nextTrack->path;
                            logMsg = "Failed to open next track in Folder Session: " + logTarget;
                            m_lastFolderSessionError = logMsg;
                            emitEnd = captureAndStopSessionLocked("open_track_failed", logMsg, 1041, endEv, endCb);
                            m_currentTrackPathHash.store(0, std::memory_order_release);
                            m_state.store(TransportState::Stopped, std::memory_order_release);
                            m_worker.stop();
                            res.rejectReason = AutoAdvanceRejectReason::OpenTrackFailed;
                            cbCopy = m_errorLogCallback;
                            logOp = "FolderSessionAdvance";
                            logErrCode = 1041;
                        }
                    }
                } else {
                    emitEnd = captureAndStopSessionLocked("end_of_folder", "", 0, endEv, endCb);
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
            }
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
        if (cbCopy) {
            cbCopy(logOp, logErrCode, logTarget, logMsg);
        }
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

    /// Retreat to previous track in Folder Session (§2.7.3).
    /// Within threshold after playback start (and not at track 0): moves to previous track.
    /// Otherwise (past threshold or at track 0): seeks to frame 0 of current track.
    bool retreatFolderPrevious() {
        ErrorLogCallback cbCopy;
        std::string logOp, logTarget, logMsg;
        int32_t logErrCode = 0;
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        bool retreated = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            if (!m_folderSession.isActive()) return false;

            const double rate = m_graphRate.load(std::memory_order_acquire);
            const double thresholdSec = m_previousRestartThresholdSec.load(std::memory_order_acquire);
            const int64_t posFrames = m_worker.playhead();
            const double elapsedSec = (rate > 0.0 && posFrames > 0) ? (static_cast<double>(posFrames) / rate) : 0.0;

            m_transitionInProgress.store(true, std::memory_order_release);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_decodeErrorNotified.store(false, std::memory_order_release);

            if (m_folderSession.currentIndex() == 0 || elapsedSec >= thresholdSec) {
                const auto seekRes = m_worker.seekWithResult(0);
                m_transitionInProgress.store(false, std::memory_order_release);
                return seekRes.success;
            }

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
                        (void)validateAndPrepareReadAheadLocked(logMsg, logTarget);
                        if (!logMsg.empty()) {
                            cbCopy = m_errorLogCallback;
                            logOp = "FolderSessionReadAhead";
                            logErrCode = 1040;
                        }
                        retreated = true;
                    } else {
                        logTarget = prevTrack->path;
                        logMsg = "Failed to open previous track in Folder Session: " + logTarget;
                        m_lastFolderSessionError = logMsg;
                        emitEnd = stopLocked("open_track_failed", logMsg, 1041, endEv, endCb);
                        cbCopy = m_errorLogCallback;
                        logOp = "FolderSessionPrevious";
                        logErrCode = 1041;
                    }
                }
            }
            m_transitionInProgress.store(false, std::memory_order_release);
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
        if (cbCopy) {
            cbCopy(logOp, logErrCode, logTarget, logMsg);
        }
        return retreated;
    }

    /// Section 2.7.5: Transport operations (Play, Pause, Stop, Seek) ARE PERMITTED even during Deck Lock!
    void play() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_pausedByNormalSwitch.store(false, std::memory_order_release);
        m_decodeErrorNotified.store(false, std::memory_order_release);
        m_state.store(TransportState::Playing, std::memory_order_release);
        m_worker.play();
    }

    void pause() noexcept {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_pausedByNormalSwitch.store(false, std::memory_order_release);
        discardPendingOverflow();
        m_state.store(TransportState::Paused, std::memory_order_release);
        m_worker.pause();
    }

    void stop(const std::string& reason = "user_stop") noexcept {
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            emitEnd = stopLocked(reason, "", 0, endEv, endCb);
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
    }

    void closeTrack() noexcept {
        FolderSessionEndEvent endEv;
        FolderSessionEndCallback endCb;
        bool emitEnd = false;
        {
            std::lock_guard<std::mutex> lock(m_controlMutex);
            m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
            m_needsFolderAdvance.store(false, std::memory_order_release);
            m_currentTrackPathHash.store(0, std::memory_order_release);
            emitEnd = captureAndStopSessionLocked("track_closed", "", 0, endEv, endCb);
            m_worker.stop();
            m_worker.closeTrack();
            m_state.store(TransportState::Stopped, std::memory_order_release);
        }
        emitFolderSessionEnd(emitEnd, endCb, endEv);
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

        if (m_worker.hasDecodeError()) {
            std::fill_n(outLeft, frames, 0.0);
            std::fill_n(outRight, frames, 0.0);
            m_state.store(TransportState::Stopped, std::memory_order_release);
            if (!m_decodeErrorNotified.exchange(true, std::memory_order_acq_rel)) {
                if (m_folderSession.isActive() &&
                    m_playbackGeneration.load(std::memory_order_acquire) == genSnapshot &&
                    !m_transitionInProgress.load(std::memory_order_acquire)) {
                    EofNotification notif;
                    notif.playbackGeneration = genSnapshot;
                    notif.decoderRequestId = m_worker.committedRequestId();
                    notif.folderSessionId = m_folderSession.sessionId();
                    notif.trackIndex = m_folderSession.currentIndex();
                    notif.trackPathHash = m_currentTrackPathHash.load(std::memory_order_acquire);
                    notif.emittedTimestampMs = currentEpochMs();
                    notif.source = EofSource::AudioRender;
                    notif.decodeError = true;
                    if (!m_eofQueue.push(notif)) {
                        recordEofOverflow(notif);
                    }
                    m_needsFolderAdvance.store(true, std::memory_order_release);
                }
            }
            return 0;
        }

        size_t read = m_worker.readAudioBlock(outLeft, outRight, frames);

        if (m_worker.isEofDelivered()) {
            m_worker.clearEofDelivered();
            const bool decErr = m_worker.hasDecodeError();
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
                    notif.decodeError = decErr;
                    if (!m_eofQueue.push(notif)) {
                        recordEofOverflow(notif);
                    }
                    m_needsFolderAdvance.store(true, std::memory_order_release);
                    if (decErr) {
                        m_decodeErrorNotified.store(true, std::memory_order_release);
                        m_state.store(TransportState::Stopped, std::memory_order_release);
                    }
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
        std::atomic<bool> decodeError{false};
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
                slot.decodeError.store(notif.decodeError, std::memory_order_release);
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
                candidate.decodeError = slot.decodeError.load(std::memory_order_acquire);

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

    bool validateAndPrepareReadAheadLocked(std::string& outErrMsg, std::string& outTargetPath) {
        outErrMsg.clear();
        outTargetPath.clear();
        if (!m_folderSession.isActive()) {
            m_folderSession.setReadAheadStatus(session::ReadAheadStatus::None, 0);
            return false;
        }
        const auto* next = m_folderSession.readAheadTrack();
        if (!next) {
            m_folderSession.setReadAheadStatus(session::ReadAheadStatus::None, 0);
            return true; // No next track (final track of folder session)
        }
        outTargetPath = next->path;
        if (next->path.rfind("synthetic://", 0) == 0) {
            m_folderSession.setReadAheadStatus(session::ReadAheadStatus::Validated, next->index);
            return true;
        }
        const QFileInfo fi(QString::fromStdString(next->path));
        if (!fi.exists() || !fi.isFile() || !fi.isReadable()) {
            outErrMsg = "Read-ahead check failed: next track missing or unreadable: " + next->path;
            m_lastFolderSessionError = outErrMsg;
            m_folderSession.setReadAheadStatus(session::ReadAheadStatus::Failed, next->index, outErrMsg);
            return false;
        }
        decoder::FFmpegAudioDecoder probe;
        if (!probe.open(next->path)) {
            outErrMsg = "Read-ahead check failed: cannot decode next track (" +
                        (probe.lastError().empty() ? next->path : probe.lastError()) + ")";
            m_lastFolderSessionError = outErrMsg;
            m_folderSession.setReadAheadStatus(session::ReadAheadStatus::Failed, next->index, outErrMsg);
            return false;
        }
        std::array<double, 64> tmpL{};
        std::array<double, 64> tmpR{};
        (void)probe.readFrames(tmpL.data(), tmpR.data(), tmpL.size());
        const bool decErr = probe.hasDecodeError();
        const std::string decErrMsg = probe.lastError();
        probe.close();
        if (decErr) {
            outErrMsg = "Read-ahead decode error on next track: " + decErrMsg;
            m_lastFolderSessionError = outErrMsg;
            m_folderSession.setReadAheadStatus(session::ReadAheadStatus::Failed, next->index, outErrMsg);
            return false;
        }
        m_folderSession.setReadAheadStatus(session::ReadAheadStatus::Validated, next->index);
        return true;
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

    bool captureAndStopSessionLocked(const std::string& reason,
                                     const std::string& detail,
                                     int32_t errorCode,
                                     FolderSessionEndEvent& outEvent,
                                     FolderSessionEndCallback& outCb) noexcept {
        if (!m_folderSession.isActive() && m_folderSession.sessionId() == 0) {
            return false;
        }
        outEvent.deckId = m_id;
        outEvent.folderSessionId = m_folderSession.sessionId();
        outEvent.folderPath = m_folderSession.folderPath();
        outEvent.trackIndex = m_folderSession.currentIndex();
        outEvent.totalTracks = m_folderSession.totalTracks();
        const auto* cur = m_folderSession.currentTrack();
        outEvent.currentTrackPath = cur ? cur->path : std::string();
        outEvent.reason = reason;
        outEvent.detail = detail;
        outEvent.errorCode = errorCode;
        outCb = m_folderSessionEndCallback;
        m_folderSession.stopSession();
        return static_cast<bool>(outCb) && outEvent.folderSessionId != 0;
    }

    static void emitFolderSessionEnd(bool shouldEmit,
                                     const FolderSessionEndCallback& cb,
                                     const FolderSessionEndEvent& ev) noexcept {
        if (!shouldEmit || !cb) return;
        try {
            cb(ev);
        } catch (...) {
        }
    }

    bool stopLocked(const std::string& reason,
                    const std::string& detail,
                    int32_t errorCode,
                    FolderSessionEndEvent& outEvent,
                    FolderSessionEndCallback& outCb) noexcept {
        m_playbackGeneration.fetch_add(1, std::memory_order_acq_rel);
        m_needsFolderAdvance.store(false, std::memory_order_release);
        m_pausedByNormalSwitch.store(false, std::memory_order_release);
        m_decodeErrorNotified.store(false, std::memory_order_release);
        discardPendingOverflow();
        const bool emitEnd = captureAndStopSessionLocked(reason, detail, errorCode, outEvent, outCb); // v3.7 §2.7.3: Stop terminates and discards Folder Session while preserving loaded track context
        m_currentTrackPathHash.store(0, std::memory_order_release);
        m_state.store(TransportState::Stopped, std::memory_order_release);
        m_worker.stop();
        return emitEnd;
    }

    char m_id;
    std::atomic<TransportState> m_state{TransportState::Stopped};
    std::atomic<bool> m_isLocked{false};
    std::atomic<bool> m_pausedByNormalSwitch{false};
    std::atomic<bool> m_decodeErrorNotified{false};
    std::atomic<double> m_previousRestartThresholdSec{3.0};
    std::atomic<int64_t> m_offsetFrames{0};
    std::atomic<double> m_levelMatchGainDb{0.0};
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

    mutable std::mutex m_controlMutex;
    std::function<void()> m_preTransitionHook;
    ErrorLogCallback m_errorLogCallback;
    FolderSessionEndCallback m_folderSessionEndCallback;
    std::string m_lastFolderSessionError;
    EofNotificationQueue m_eofQueue;
    session::FolderSession m_folderSession;
    decoder::DecoderWorker m_worker;

    alignas(64) std::array<double, RT_BUFFER_CAPACITY> m_scratchL{};
    alignas(64) std::array<double, RT_BUFFER_CAPACITY> m_scratchR{};
};

} // namespace desk32::engine::deck
