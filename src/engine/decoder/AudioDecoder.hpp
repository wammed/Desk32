#pragma once

#include "engine/metadata/TrackMetadata.hpp"
#include <string>
#include <cstdint>
#include <cstddef>

namespace desk32::engine::decoder {

struct AudioFormatInfo {
    double sampleRate = 44100.0;
    uint32_t channels = 2;
    uint32_t bitDepth = 16;
    double durationSeconds = 0.0;
    int64_t totalFrames = 0;
    std::string codecName;
};

/// Abstract Audio Decoder Interface (Section 2.4 & Section 14)
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    /// Open audio file, initialize decoding pipeline, and parse metadata.
    virtual bool open(const std::string& filePath) = 0;

    /// Close file and release all decoding resources.
    virtual void close() = 0;

    [[nodiscard]] virtual bool isOpen() const noexcept = 0;
    [[nodiscard]] virtual std::string lastError() const { return ""; }

    [[nodiscard]] virtual const AudioFormatInfo& formatInfo() const noexcept = 0;
    [[nodiscard]] virtual const metadata::TrackMetadata& metadata() const noexcept = 0;

    /// Read up to maxFrames of stereo audio converted into internal planar float64 [-1.0, 1.0].
    /// Parameters:
    /// - left: target buffer for left channel samples
    /// - right: target buffer for right channel samples
    /// - maxFrames: maximum frames requested
    /// Returns: number of frames actually read. 0 indicates end of file or unrecoverable error.
    virtual size_t readFrames(double* left, double* right, size_t maxFrames) = 0;

    /// Seek to target audio frame.
    virtual bool seek(int64_t targetFrame) = 0;

    /// Seek to timestamp in seconds.
    virtual bool seekSeconds(double seconds) {
        if (formatInfo().sampleRate <= 0.0) return false;
        int64_t targetFrame = static_cast<int64_t>(seconds * formatInfo().sampleRate);
        return seek(targetFrame);
    }

    [[nodiscard]] virtual int64_t currentFrame() const noexcept = 0;
    [[nodiscard]] virtual int64_t lastSeekActualFrame() const noexcept { return currentFrame(); }
};

} // namespace desk32::engine::decoder

