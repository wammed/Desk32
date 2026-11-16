#pragma once

#include "AudioDecoder.hpp"
#include <vector>
#include <array>
#include <memory>

// Forward declare FFmpeg opaque structs to keep header C++ clean
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace desk32::engine::decoder {

/// Unified high-performance audio decoder based on FFmpeg (libavcodec / libavformat)
/// Handles all 6 target formats: FLAC, WAV, MP3, Vorbis, Opus, and AAC.
/// Enforces Section 2.5: Zero extraction, zero decoding, zero caching of Artwork.
class FFmpegAudioDecoder : public AudioDecoder {
public:
    FFmpegAudioDecoder();
    ~FFmpegAudioDecoder() override;

    bool open(const std::string& filePath) override;
    void close() override;
    [[nodiscard]] bool isOpen() const noexcept override { return m_isOpen; }
    [[nodiscard]] std::string lastError() const override { return m_lastError; }

    [[nodiscard]] const AudioFormatInfo& formatInfo() const noexcept override { return m_formatInfo; }
    [[nodiscard]] const metadata::TrackMetadata& metadata() const noexcept override { return m_metadata; }

    size_t readFrames(double* left, double* right, size_t maxFrames) override;
    bool seek(int64_t targetFrame) override;
    [[nodiscard]] int64_t currentFrame() const noexcept override { return m_currentFrame; }
    [[nodiscard]] int64_t lastSeekActualFrame() const noexcept override { return m_seekActualFrame; }

    // Instrumentation (read-only timing stats for av_read_frame)
    [[nodiscard]] uint64_t lastReadPacketNs() const noexcept { return m_lastReadPacketNs; }
    [[nodiscard]] uint64_t maxReadPacketNs() const noexcept { return m_maxReadPacketNs; }
    [[nodiscard]] uint64_t totalReadPacketNs() const noexcept { return m_totalReadPacketNs; }
    void resetReadPacketStats() noexcept {
        m_lastReadPacketNs = 0;
        m_maxReadPacketNs = 0;
        m_totalReadPacketNs = 0;
    }

private:
    void cleanup();
    bool decodeNextPacket();
    void extractMetadata();

    bool m_isOpen = false;
    std::string m_filePath;
    std::string m_lastError;

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_decodedFrame = nullptr;
    AVPacket* m_packet = nullptr;
    SwrContext* m_swrCtx = nullptr;

    int m_audioStreamIndex = -1;
    int64_t m_currentFrame = 0;
    int64_t m_seekActualFrame = 0;
    bool m_seekPending = false;

    AudioFormatInfo m_formatInfo;
    metadata::TrackMetadata m_metadata;

    // Pre-allocated internal planar double FIFO buffers for smooth, zero-allocation block delivery
    static constexpr size_t FIFO_CAPACITY = 65536;
    alignas(64) std::array<double, FIFO_CAPACITY> m_fifoL{};
    alignas(64) std::array<double, FIFO_CAPACITY> m_fifoR{};
    size_t m_fifoReadPos = 0;
    size_t m_fifoWritePos = 0;

    uint64_t m_lastReadPacketNs = 0;
    uint64_t m_maxReadPacketNs = 0;
    uint64_t m_totalReadPacketNs = 0;
};

} // namespace desk32::engine::decoder

