#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <cstdint>
#include <cmath>
#include "engine/buffer/PcmRingBuffer.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"
#include "engine/dsp/Resampler.hpp"
#include "engine/metadata/TrackMetadata.hpp"

namespace desk32::engine::decoder {

struct WorkerTrackInfo {
    std::string path;
    double sampleRate = 44100.0;
    uint32_t channels = 2;
    uint64_t totalFrames = 0;
};

enum class OpenStatus : uint8_t {
    None = 0,
    Success,
    Failed,
    TimedOut,
    Cancelled
};

enum class WorkerCommandType : uint8_t {
    None = 0,
    Open,
    OpenSynthetic,
    CloseTrack,
    Seek,
    Play,
    Pause,
    Stop,
    ConfigureRate
};

struct WorkerCommand {
    WorkerCommandType type = WorkerCommandType::None;
    std::string path;
    double sampleRate = 44100.0;
    double graphRate = 48000.0;
    uint64_t totalFrames = 0;
    int64_t targetFrame = 0;
    uint64_t sequence = 0;
    uint64_t requestId = 0;
    uint64_t targetRequestId = 0; // Generation/target Request ID for CloseTrack (P0-26)
    bool isSynthetic = false;
    int64_t requestedNs = 0;
    int64_t notifiedNs = 0;
    std::shared_ptr<std::promise<bool>> openPromise;
};

struct SeekResult {
    bool success = false;
    bool withinBuffer = false;
    int64_t requestedFrame = 0;
    int64_t actualFrame = 0;
    double sampleRateHz = 0.0;
    uint64_t totalFrames = 0;
    int32_t errorCode = 0;
    std::string stage;
    std::string errorMessage;
};

enum class OpenTransitionPhase : uint8_t {
    Idle = 0,
    OpenQueued = 1,
    DecoderOpening = 2,
    StateCommitStart = 3,
    AfterEofClearedBeforePlayCleared = 4,
    AfterPlayCleared = 5,
    Prefilling = 6,
    OpenComplete = 7
};

struct OpenTransitionSnapshot {
    uint64_t openSeq = 0;
    uint64_t requestId = 0;
    int64_t openRequestedNs = 0;
    int64_t cmdNotifiedNs = 0;
    int64_t workerWokeNs = 0;
    int64_t openProcessStartNs = 0;
    int64_t decoderOpenStartNs = 0;
    int64_t decoderOpenEndNs = 0;
    int64_t stateCommitStartNs = 0;
    int64_t ringResetNs = 0;
    int64_t eofClearedNs = 0;
    int64_t resamplerConfigStartNs = 0;
    int64_t resamplerConfigEndNs = 0;
    int64_t playingClearedNs = 0;
    int64_t stateCommitEndNs = 0;
    int64_t prefillStartNs = 0;
    int64_t firstPcmSupplyNs = 0;
    int64_t prefillEndNs = 0;
    int64_t openCompleteNs = 0;
    int64_t playRequestedNs = 0;
    uint64_t prefillInputFrames = 0;
    uint64_t prefillRingAvailAfter = 0;
    bool eofBefore = false;
    bool eofAfter = false;
    bool playingBefore = false;
    bool playingAfter = false;
    bool valid = false;
};

struct UnderrunSnapshot {
    int64_t steadyNs = 0;
    uint64_t underrunTotal = 0;
    uint64_t requestId = 0;
    uint64_t activeRequestId = 0;
    uint64_t openSeq = 0;
    uint64_t eofEpoch = 0;
    uint64_t ringCapacityFrames = 0;
    uint64_t ringAvailBeforeRead = 0;
    uint64_t requestedFrames = 0;
    uint64_t readFrames = 0;
    uint8_t openPhase = 0;
    bool isOpenAtEntry = false;
    bool isPlayingAtEntry = false;
    bool isEofAtCheck = false;
    bool isOpenAtUnderrun = false;
    bool isPlayingAtUnderrun = false;
};

struct DecodePerfStats {
    uint64_t lastChunkTotalNs = 0;
    uint64_t maxChunkTotalNs = 0;
    uint64_t lastReadFramesNs = 0;
    uint64_t maxReadFramesNs = 0;
    uint64_t lastFileReadPacketNs = 0;
    uint64_t maxFileReadPacketNs = 0;
    uint64_t lastResampleNs = 0;
    uint64_t maxResampleNs = 0;
    uint64_t lastWorkerIterIntervalNs = 0;
    uint64_t maxWorkerIterIntervalNs = 0;
    uint64_t totalChunksDecoded = 0;
};

/// Dedicated Background Decoder Worker for ReferenceDeck.
/// Pre-decodes audio from FFmpeg, handles resampling, and feeds a lock-free PcmRingBuffer.
/// Audio Thread NEVER touches FFmpeg, files, heap allocations, or locks.
class DecoderWorker {
public:
    static constexpr size_t CHUNK_SIZE = 2048;

    DecoderWorker();
    ~DecoderWorker();

    DecoderWorker(const DecoderWorker&) = delete;
    DecoderWorker& operator=(const DecoderWorker&) = delete;

    // Control Thread APIs
    bool openTrack(const std::string& filePath, double graphSampleRate);
    bool openSyntheticTrack(uint64_t totalFrames, double sampleRate, double graphSampleRate, const std::string& path = "synthetic://sine");
    void closeTrack();
    void play() noexcept;
    void pause() noexcept;
    void stop() noexcept;
    void seek(int64_t targetFrame) noexcept;
    SeekResult seekWithResult(int64_t targetFrame) noexcept;
    void configureGraphRate(double graphSampleRate) noexcept;

    // Status Queries (Thread-Safe, P0-05)
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen.load(std::memory_order_acquire); }
    [[nodiscard]] bool isPlaying() const noexcept { return m_isPlaying.load(std::memory_order_acquire); }
    [[nodiscard]] bool isEof() const noexcept { return m_isEof.load(std::memory_order_acquire); }
    [[nodiscard]] bool hasDecodeError() const noexcept { return m_hasDecodeError.load(std::memory_order_acquire); }
    [[nodiscard]] bool isEofDelivered() const noexcept { return m_eofDelivered.load(std::memory_order_acquire); }
    void clearEofDelivered() noexcept { m_eofDelivered.store(false, std::memory_order_release); }
    void simulateMidStreamDecodeErrorForTesting(const std::string& msg = "Simulated mid-stream decode error");
    void reloadMetadata();
    [[nodiscard]] std::string lastDecodeErrorMessage() const;
    [[nodiscard]] uint64_t eofEpoch() const noexcept { return m_eofEpoch.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t eofRequestId() const noexcept { return m_eofRequestId.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t eofEmittedTimestampMs() const noexcept { return m_eofEmittedTimestampMs.load(std::memory_order_acquire); }
    [[nodiscard]] int64_t playhead() const noexcept { return m_playhead.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t underrunCount() const noexcept { return m_underrunCount.load(std::memory_order_relaxed); }
    [[nodiscard]] OpenStatus lastOpenStatus() const noexcept { return m_lastOpenStatus.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t activeRequestId() const noexcept { return m_activeRequestId.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t committedRequestId() const noexcept { return m_committedRequestId.load(std::memory_order_acquire); }
    [[nodiscard]] bool isSynthetic() const noexcept { return m_isSynthetic.load(std::memory_order_acquire); }

    void setPreCommitHook(std::function<void(uint64_t)> hook) {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        m_preCommitHook = std::move(hook);
    }

#ifdef DESK32_ENABLE_TEST_HOOKS
    void setInTransitionHook(std::function<void(uint64_t)> hook) {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        m_inTransitionHook = std::move(hook);
    }
    void setInSeekTransitionHook(std::function<void(uint64_t)> hook) {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        m_inSeekTransitionHook = std::move(hook);
    }
    void setBeforeDecodeChunkHook(std::function<void(uint64_t)> hook) {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        m_beforeDecodeChunkHook = std::move(hook);
    }
#endif

    // Lock-free / Seqlock instrumentation readers (Control Thread / Tests)
    [[nodiscard]] OpenTransitionSnapshot readOpenTransitionSnapshot() const noexcept;
    [[nodiscard]] DecodePerfStats readDecodePerfStats() const noexcept;
    bool popUnderrunSnapshot(UnderrunSnapshot& out) noexcept;
    [[nodiscard]] uint64_t underrunSnapshotDropCount() const noexcept {
        return m_underrunSnapshotDropCount.load(std::memory_order_relaxed);
    }

    void setOpenTimeoutMs(uint32_t timeoutMs) noexcept {
        m_openTimeoutMs.store(timeoutMs, std::memory_order_release);
    }

    [[nodiscard]] uint32_t openTimeoutMs() const noexcept {
        return m_openTimeoutMs.load(std::memory_order_acquire);
    }

    /// Thread-safe copy of track info (P0-05)
    [[nodiscard]] WorkerTrackInfo trackInfo() const {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        return m_trackInfo;
    }

    /// Thread-safe shared snapshot of track metadata with guaranteed lifetime (P0-05)
    [[nodiscard]] std::shared_ptr<const metadata::TrackMetadata> metadata() const {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        return m_metadata;
    }

    /// Audio Thread API: Read planar audio block from lock-free PCM ring buffer.
    /// Strictly RT-Safe: Zero dynamic allocation, zero locks, zero blocking, zero FFmpeg calls.
    size_t readAudioBlock(double* outLeft, double* outRight, size_t frames) noexcept;

    /// Direct access to lock-free PCM ring buffer (e.g. for offline testing / inspection)
    [[nodiscard]] buffer::PcmRingBuffer& ringBuffer() noexcept { return m_ringBuffer; }
    [[nodiscard]] const buffer::PcmRingBuffer& ringBuffer() const noexcept { return m_ringBuffer; }

private:
    void workerLoop();
    void processCommand(const WorkerCommand& cmd);
    void doDecodeChunk();

    std::atomic<bool> m_running{true};
    std::atomic<bool> m_isOpen{false};
    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isEof{false};
    std::atomic<bool> m_hasDecodeError{false};
    std::atomic<bool> m_eofDelivered{false};
    std::atomic<uint64_t> m_eofEpoch{1};
    std::atomic<uint64_t> m_eofDeliveredEpoch{0};
    std::atomic<uint64_t> m_eofRequestId{0};
    std::atomic<int64_t> m_eofEmittedTimestampMs{0};
    std::atomic<bool> m_isSynthetic{false};
    std::atomic<int64_t> m_playhead{0};
    std::atomic<uint64_t> m_underrunCount{0};
    std::atomic<double> m_graphRate{48000.0};
    std::atomic<uint64_t> m_seekSequence{0};
    std::atomic<uint64_t> m_seekCompleted{0};
    std::atomic<bool> m_seekSuccess{true};
    std::atomic<int64_t> m_seekActualFrame{0};
    std::atomic<uint64_t> m_nextRequestId{1};
    std::atomic<uint64_t> m_activeRequestId{0};
    std::atomic<uint64_t> m_committedRequestId{0};
    std::atomic<OpenStatus> m_lastOpenStatus{OpenStatus::None};
    std::atomic<uint32_t> m_openTimeoutMs{2000};

    mutable std::mutex m_trackMutex;
    WorkerTrackInfo m_trackInfo;
    std::shared_ptr<const metadata::TrackMetadata> m_metadata;
    std::string m_lastDecodeErrorMessage;

    // Lock-free Ring Buffer between Worker Thread (Producer) and Audio Thread (Consumer)
    buffer::PcmRingBuffer m_ringBuffer{buffer::PcmRingBuffer::DEFAULT_CAPACITY_FRAMES};

    // Worker thread management
    std::thread m_thread;
    std::mutex m_cmdMutex;
    std::condition_variable m_cmdCv;
    std::vector<WorkerCommand> m_pendingCommands;
    std::function<void(uint64_t)> m_preCommitHook;
#ifdef DESK32_ENABLE_TEST_HOOKS
    std::function<void(uint64_t)> m_inTransitionHook;
    std::function<void(uint64_t)> m_inSeekTransitionHook;
    std::function<void(uint64_t)> m_beforeDecodeChunkHook;
#endif

    // Instrumentation state (RT-safe: SPSC ring for Audio->Control, Seqlock for Worker->Control)
    static int64_t steadyNowNs() noexcept;
    void publishOpenTransition(const OpenTransitionSnapshot& snap) noexcept;
    void publishDecodePerf(const DecodePerfStats& stats) noexcept;

    std::atomic<uint8_t> m_openPhase{static_cast<uint8_t>(OpenTransitionPhase::Idle)};
    std::atomic<uint64_t> m_stateTransitionSeq{0};
    std::atomic<uint64_t> m_openSeqCounter{0};
    std::atomic<int64_t> m_lastPlayRequestedNs{0};
    std::atomic<uint64_t> m_lastPlayRequestId{0};
    std::atomic<int64_t> m_lastCmdNotifiedNs{0};

    // Seqlock for OpenTransitionSnapshot (single writer: DecoderWorker thread, TSan-clean atomic fields)
    struct AtomicOpenTransitionSlot {
        std::atomic<uint64_t> openSeq{0};
        std::atomic<uint64_t> requestId{0};
        std::atomic<int64_t> openRequestedNs{0};
        std::atomic<int64_t> cmdNotifiedNs{0};
        std::atomic<int64_t> workerWokeNs{0};
        std::atomic<int64_t> openProcessStartNs{0};
        std::atomic<int64_t> decoderOpenStartNs{0};
        std::atomic<int64_t> decoderOpenEndNs{0};
        std::atomic<int64_t> stateCommitStartNs{0};
        std::atomic<int64_t> ringResetNs{0};
        std::atomic<int64_t> eofClearedNs{0};
        std::atomic<int64_t> resamplerConfigStartNs{0};
        std::atomic<int64_t> resamplerConfigEndNs{0};
        std::atomic<int64_t> playingClearedNs{0};
        std::atomic<int64_t> stateCommitEndNs{0};
        std::atomic<int64_t> prefillStartNs{0};
        std::atomic<int64_t> firstPcmSupplyNs{0};
        std::atomic<int64_t> prefillEndNs{0};
        std::atomic<int64_t> openCompleteNs{0};
        std::atomic<uint64_t> prefillInputFrames{0};
        std::atomic<uint64_t> prefillRingAvailAfter{0};
        std::atomic<bool> eofBefore{false};
        std::atomic<bool> eofAfter{false};
        std::atomic<bool> playingBefore{false};
        std::atomic<bool> playingAfter{false};
        std::atomic<bool> valid{false};
    };
    alignas(64) mutable std::atomic<uint64_t> m_openTelemSeq{0};
    AtomicOpenTransitionSlot m_openTelemData{};
    OpenTransitionSnapshot m_workerOpenTelem{};

    // Seqlock for DecodePerfStats (single writer: DecoderWorker thread, TSan-clean atomic fields)
    struct AtomicDecodePerfSlot {
        std::atomic<uint64_t> lastChunkTotalNs{0};
        std::atomic<uint64_t> maxChunkTotalNs{0};
        std::atomic<uint64_t> lastReadFramesNs{0};
        std::atomic<uint64_t> maxReadFramesNs{0};
        std::atomic<uint64_t> lastFileReadPacketNs{0};
        std::atomic<uint64_t> maxFileReadPacketNs{0};
        std::atomic<uint64_t> lastResampleNs{0};
        std::atomic<uint64_t> maxResampleNs{0};
        std::atomic<uint64_t> lastWorkerIterIntervalNs{0};
        std::atomic<uint64_t> maxWorkerIterIntervalNs{0};
        std::atomic<uint64_t> totalChunksDecoded{0};
    };
    alignas(64) mutable std::atomic<uint64_t> m_perfSeq{0};
    AtomicDecodePerfSlot m_perfData{};
    DecodePerfStats m_workerPerf{};
    int64_t m_lastWorkerIterNs = 0;

    // Lock-free SPSC ring buffer for UnderrunSnapshot (single producer: Audio Thread, single consumer: Control Thread)
    static constexpr size_t UNDERRUN_RING_SIZE = 32;
    alignas(64) std::array<UnderrunSnapshot, UNDERRUN_RING_SIZE> m_underrunRing{};
    alignas(64) std::atomic<uint64_t> m_underrunWriteIdx{0};
    alignas(64) std::atomic<uint64_t> m_underrunReadIdx{0};
    std::atomic<uint64_t> m_underrunSnapshotDropCount{0};

    // Worker thread local resources (NOT touched by Audio Thread)
    std::unique_ptr<FFmpegAudioDecoder> m_decoder;
    dsp::Resampler m_resampler;
    int64_t m_syntheticPos = 0;

    std::array<double, CHUNK_SIZE> m_tempChunkL{};
    std::array<double, CHUNK_SIZE> m_tempChunkR{};
    std::array<double, CHUNK_SIZE * 2> m_tempInterleavedIn{};
    std::array<buffer::StereoFrame, CHUNK_SIZE * 8> m_tempInterleavedOut{};
    size_t m_pendingOutOffset = 0;
    size_t m_pendingOutCount = 0;
};

} // namespace desk32::engine::decoder
