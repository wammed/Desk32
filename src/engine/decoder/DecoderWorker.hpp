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
    bool isSynthetic = false;
    std::shared_ptr<std::promise<bool>> openPromise;
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
    void configureGraphRate(double graphSampleRate) noexcept;

    // Status Queries (Thread-Safe, P0-05)
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen.load(std::memory_order_acquire); }
    [[nodiscard]] bool isPlaying() const noexcept { return m_isPlaying.load(std::memory_order_acquire); }
    [[nodiscard]] bool isEof() const noexcept { return m_isEof.load(std::memory_order_acquire); }
    [[nodiscard]] bool isEofDelivered() const noexcept { return m_eofDelivered.load(std::memory_order_acquire); }
    void clearEofDelivered() noexcept { m_eofDelivered.store(false, std::memory_order_release); }
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

private:
    void workerLoop();
    void processCommand(const WorkerCommand& cmd);
    void doDecodeChunk();

    std::atomic<bool> m_running{true};
    std::atomic<bool> m_isOpen{false};
    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isEof{false};
    std::atomic<bool> m_eofDelivered{false};
    std::atomic<bool> m_isSynthetic{false};
    std::atomic<int64_t> m_playhead{0};
    std::atomic<uint64_t> m_underrunCount{0};
    std::atomic<double> m_graphRate{48000.0};
    std::atomic<uint64_t> m_seekSequence{0};
    std::atomic<uint64_t> m_seekCompleted{0};
    std::atomic<uint64_t> m_nextRequestId{1};
    std::atomic<uint64_t> m_activeRequestId{0};
    std::atomic<uint64_t> m_committedRequestId{0};
    std::atomic<OpenStatus> m_lastOpenStatus{OpenStatus::None};

    mutable std::mutex m_trackMutex;
    WorkerTrackInfo m_trackInfo;
    std::shared_ptr<const metadata::TrackMetadata> m_metadata;

    // Lock-free Ring Buffer between Worker Thread (Producer) and Audio Thread (Consumer)
    buffer::PcmRingBuffer m_ringBuffer{buffer::PcmRingBuffer::DEFAULT_CAPACITY_FRAMES};

    // Worker thread management
    std::thread m_thread;
    std::mutex m_cmdMutex;
    std::condition_variable m_cmdCv;
    std::vector<WorkerCommand> m_pendingCommands;
    std::function<void(uint64_t)> m_preCommitHook;

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
