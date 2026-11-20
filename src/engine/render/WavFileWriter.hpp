#pragma once

#include <QString>
#include <QFile>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>
#include "engine/format/FormatConverter.hpp"

namespace desk32::engine::render {

enum class WavBitDepth {
    Int16,
    Int24,
    Float32
};

/// High-fidelity RIFF/WAV audio file writer.
/// Supports 16-bit PCM (TPDF dither), 24-bit packed PCM (TPDF dither), and 32-bit IEEE float.
class WavFileWriter {
public:
    WavFileWriter() = default;
    ~WavFileWriter() {
        close();
    }

    WavFileWriter(const WavFileWriter&) = delete;
    WavFileWriter& operator=(const WavFileWriter&) = delete;

    [[nodiscard]] bool isOpen() const noexcept {
        return m_file.isOpen();
    }

    [[nodiscard]] uint64_t totalFramesWritten() const noexcept {
        return m_totalFramesWritten;
    }

    [[nodiscard]] const QString& filePath() const noexcept {
        return m_filePath;
    }

    /// Open and initialize WAV file with RIFF header
    bool open(const QString& filePath, double sampleRate, WavBitDepth bitDepth) {
        close();
        m_filePath = filePath;
        m_sampleRate = static_cast<uint32_t>(std::round(sampleRate));
        m_bitDepth = bitDepth;
        m_totalFramesWritten = 0;

        m_file.setFileName(m_filePath);
        if (!m_file.open(QIODevice::WriteOnly)) {
            return false;
        }

        switch (m_bitDepth) {
            case WavBitDepth::Int16:
                m_bytesPerSample = 2;
                m_bitsPerSample = 16;
                m_formatTag = 1; // WAVE_FORMAT_PCM
                break;
            case WavBitDepth::Int24:
                m_bytesPerSample = 3;
                m_bitsPerSample = 24;
                m_formatTag = 1; // WAVE_FORMAT_PCM
                break;
            case WavBitDepth::Float32:
                m_bytesPerSample = 4;
                m_bitsPerSample = 32;
                m_formatTag = 3; // WAVE_FORMAT_IEEE_FLOAT
                break;
        }

        m_channels = 2;
        m_blockAlign = static_cast<uint16_t>(m_channels * m_bytesPerSample);
        m_byteRate = m_sampleRate * m_blockAlign;

        // Write preliminary 44-byte WAV header (sizes updated on finalize)
        char header[44] = {0};
        // RIFF chunk descriptor
        header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
        // bytes 4..7: riffSize (set later)
        header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';

        // "fmt " sub-chunk
        header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
        uint32_t subchunk1Size = 16;
        std::memcpy(&header[16], &subchunk1Size, 4);
        std::memcpy(&header[20], &m_formatTag, 2);
        std::memcpy(&header[22], &m_channels, 2);
        std::memcpy(&header[24], &m_sampleRate, 4);
        std::memcpy(&header[28], &m_byteRate, 4);
        std::memcpy(&header[32], &m_blockAlign, 2);
        std::memcpy(&header[34], &m_bitsPerSample, 2);

        // "data" sub-chunk
        header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
        // bytes 40..43: dataSize (set later)

        qint64 written = m_file.write(header, 44);
        return written == 44;
    }

    /// Write audio block from planar float64 buffers
    bool writeBlock(const double* left, const double* right, size_t frames, bool applyDither = true) {
        if (!isOpen() || !left || !right || frames == 0) return false;

        switch (m_bitDepth) {
            case WavBitDepth::Int16: {
                m_scratchInt16.resize(frames * 2);
                format::FormatConverter::convertPlanarFloat64ToPcm(
                    left, right, frames, m_scratchInt16.data(), format::SampleFormat::Int16, applyDither
                );
                qint64 bytesToWrite = static_cast<qint64>(frames * 2 * sizeof(int16_t));
                qint64 written = m_file.write(reinterpret_cast<const char*>(m_scratchInt16.data()), bytesToWrite);
                if (written != bytesToWrite) return false;
                break;
            }
            case WavBitDepth::Int24: {
                m_scratchInt32.resize(frames * 2);
                format::FormatConverter::convertPlanarFloat64ToPcm(
                    left, right, frames, m_scratchInt32.data(), format::SampleFormat::Int24_32, applyDither
                );
                // Pack into 3 bytes per sample (little-endian)
                m_scratchPacked24.resize(frames * 2 * 3);
                for (size_t i = 0; i < frames * 2; ++i) {
                    int32_t val = m_scratchInt32[i];
                    m_scratchPacked24[i * 3 + 0] = static_cast<uint8_t>(val & 0xFF);
                    m_scratchPacked24[i * 3 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
                    m_scratchPacked24[i * 3 + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
                }
                qint64 bytesToWrite = static_cast<qint64>(m_scratchPacked24.size());
                qint64 written = m_file.write(reinterpret_cast<const char*>(m_scratchPacked24.data()), bytesToWrite);
                if (written != bytesToWrite) return false;
                break;
            }
            case WavBitDepth::Float32: {
                m_scratchFloat32.resize(frames * 2);
                format::FormatConverter::convertPlanarFloat64ToPcm(
                    left, right, frames, m_scratchFloat32.data(), format::SampleFormat::Float32, false
                );
                qint64 bytesToWrite = static_cast<qint64>(frames * 2 * sizeof(float));
                qint64 written = m_file.write(reinterpret_cast<const char*>(m_scratchFloat32.data()), bytesToWrite);
                if (written != bytesToWrite) return false;
                break;
            }
        }

        m_totalFramesWritten += frames;
        return true;
    }

    [[nodiscard]] const std::string& lastError() const noexcept { return m_lastError; }
    void setForceCloseErrorForTest(bool force) noexcept { m_forceCloseErrorForTest = force; }

    /// Finalize WAV header sizes and close file
    bool close() {
        if (!m_file.isOpen()) return true;

        bool ok = true;
        if (m_forceCloseErrorForTest || m_file.error() != QFileDevice::NoError) {
            ok = false;
            m_lastError = m_forceCloseErrorForTest
                              ? "Injected WAV close I/O error"
                              : m_file.errorString().toStdString();
        }

        const qint64 totalDataBytes = static_cast<qint64>(m_totalFramesWritten) * m_blockAlign;
        const qint64 riffSize = 36 + totalDataBytes;

        // Update sizes in header and check seek/write/flush status
        if (ok) {
            const uint32_t rSize = static_cast<uint32_t>(riffSize);
            if (!m_file.seek(4) || m_file.write(reinterpret_cast<const char*>(&rSize), 4) != 4) {
                ok = false;
                m_lastError = "Failed to seek or write WAV RIFF header size: " +
                              m_file.errorString().toStdString();
            }
        }

        if (ok) {
            const uint32_t dSize = static_cast<uint32_t>(totalDataBytes);
            if (!m_file.seek(40) || m_file.write(reinterpret_cast<const char*>(&dSize), 4) != 4) {
                ok = false;
                m_lastError = "Failed to seek or write WAV data chunk size: " +
                              m_file.errorString().toStdString();
            }
        }

        if (ok && (!m_file.flush() || m_file.error() != QFileDevice::NoError)) {
            ok = false;
            m_lastError = "Failed to flush finalized WAV output: " +
                          m_file.errorString().toStdString();
        }

        m_file.close();
        if (ok && m_file.error() != QFileDevice::NoError) {
            ok = false;
            m_lastError = "WAV file error on close: " + m_file.errorString().toStdString();
        }
        return ok;
    }

private:
    QFile m_file;
    QString m_filePath;
    std::string m_lastError;
    bool m_forceCloseErrorForTest = false;
    uint32_t m_sampleRate = 48000;
    WavBitDepth m_bitDepth = WavBitDepth::Int24;
    uint16_t m_formatTag = 1;
    uint16_t m_channels = 2;
    uint16_t m_bitsPerSample = 24;
    uint16_t m_bytesPerSample = 3;
    uint16_t m_blockAlign = 6;
    uint32_t m_byteRate = 48000 * 6;
    uint64_t m_totalFramesWritten = 0;

    std::vector<int16_t> m_scratchInt16;
    std::vector<int32_t> m_scratchInt32;
    std::vector<uint8_t> m_scratchPacked24;
    std::vector<float> m_scratchFloat32;
};

} // namespace desk32::engine::render

