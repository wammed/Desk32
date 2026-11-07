#pragma once

#include "AudioDecoder.hpp"
#include <vector>
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

    [[nodiscard]] const AudioFormatInfo& formatInfo() const noexcept override { return m_formatInfo; }
    [[nodiscard]] const metadata::TrackMetadata& metadata() const noexcept override { return m_metadata; }

    size_t readFrames(double* left, double* right, size_t maxFrames) override;
    bool seek(int64_t targetFrame) override;
    [[nodiscard]] int64_t currentFrame() const noexcept override { return m_currentFrame; }

private:
    void cleanup();
    bool decodeNextPacket();
    void extractMetadata();

    bool m_isOpen = false;
    std::string m_filePath;

    AVFormatContext* m_formatCtx = nullptr;
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_decodedFrame = nullptr;
    AVPacket* m_packet = nullptr;
    SwrContext* m_swrCtx = nullptr;

    int m_audioStreamIndex = -1;
    int64_t m_currentFrame = 0;

    AudioFormatInfo m_formatInfo;
    metadata::TrackMetadata m_metadata;

    // Internal planar double FIFO buffers for smooth block delivery
    std::vector<double> m_fifoL;
    std::vector<double> m_fifoR;
    size_t m_fifoReadPos = 0;
};

} // namespace desk32::engine::decoder

