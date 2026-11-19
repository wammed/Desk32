#include "FFmpegAudioDecoder.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/channel_layout.h>
}

namespace desk32::engine::decoder {

FFmpegAudioDecoder::FFmpegAudioDecoder() = default;

FFmpegAudioDecoder::~FFmpegAudioDecoder()
{
    cleanup();
}

void FFmpegAudioDecoder::cleanup()
{
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_decodedFrame) {
        av_frame_free(&m_decodedFrame);
        m_decodedFrame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }

    m_fifoReadPos = 0;
    m_fifoWritePos = 0;
    m_audioStreamIndex = -1;
    m_currentFrame = 0;
    m_isOpen = false;
    m_hasDecodeError = false;
}

void FFmpegAudioDecoder::close()
{
    cleanup();
}

bool FFmpegAudioDecoder::open(const std::string& filePath)
{
    cleanup();
    m_filePath = filePath;
    m_lastError.clear();

    // 1. Open media file
    if (avformat_open_input(&m_formatCtx, filePath.c_str(), nullptr, nullptr) < 0) {
        m_lastError = "Failed to open input file with avformat_open_input: " + filePath;
        return false;
    }

    // 2. Retrieve stream information
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        m_lastError = "Failed to retrieve stream info with avformat_find_stream_info.";
        cleanup();
        return false;
    }

    // 3. Find primary audio stream while strictly skipping video/artwork streams (Section 2.5)
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; ++i) {
        AVStream* st = m_formatCtx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            // Strictly exclude attached picture / album cover streams
            if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
                m_audioStreamIndex = static_cast<int>(i);
                break;
            }
        }
    }

    if (m_audioStreamIndex < 0) {
        m_lastError = "No valid audio stream found in media file.";
        cleanup();
        return false;
    }

    AVStream* audioStream = m_formatCtx->streams[m_audioStreamIndex];

    // 4. Find and initialize codec
    const AVCodec* codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
    if (!codec) {
        m_lastError = "No supported decoder codec found for audio stream.";
        cleanup();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        m_lastError = "Failed to allocate AVCodecContext.";
        cleanup();
        return false;
    }

    if (avcodec_parameters_to_context(m_codecCtx, audioStream->codecpar) < 0) {
        m_lastError = "Failed to copy codec parameters to context.";
        cleanup();
        return false;
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        m_lastError = "Failed to open audio codec with avcodec_open2.";
        cleanup();
        return false;
    }

    // 4.5. AUD-10: Verify strictly stereo (2 channels).
    // Desk32 explicitly rejects mono (1ch), quad (4ch), 5.1ch, 7.1ch, and all other non-stereo media upfront.
    // Implicit downmixing/upmixing via SwrContext is strictly prohibited per Section 2.3 and AUD-10.
    const int channels = m_codecCtx->ch_layout.nb_channels;
    if (channels != 2) {
        m_lastError = "Non-stereo audio format rejected: source has " + std::to_string(channels) +
                      " channel(s). Desk32 strictly supports stereo 2-channel audio only (AUD-10).";
        cleanup();
        return false;
    }

    // 5. Initialize SwrContext to convert from source format to stereo planar double (AV_SAMPLE_FMT_DBLP)
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 2);

    int swrRes = swr_alloc_set_opts2(
        &m_swrCtx,
        &outLayout,
        AV_SAMPLE_FMT_DBLP,
        m_codecCtx->sample_rate,
        &m_codecCtx->ch_layout,
        m_codecCtx->sample_fmt,
        m_codecCtx->sample_rate,
        0,
        nullptr
    );

    if (swrRes < 0 || !m_swrCtx || swr_init(m_swrCtx) < 0) {
        cleanup();
        return false;
    }

    // 6. Allocate reusable packet and frame
    m_decodedFrame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_decodedFrame || !m_packet) {
        cleanup();
        return false;
    }

    // 7. Populate technical format info
    m_formatInfo.sampleRate = static_cast<double>(m_codecCtx->sample_rate);
    m_formatInfo.channels = static_cast<uint32_t>(m_codecCtx->ch_layout.nb_channels);
    m_formatInfo.bitDepth = static_cast<uint32_t>(av_get_bytes_per_sample(m_codecCtx->sample_fmt) * 8);
    m_formatInfo.codecName = codec->name ? codec->name : "Unknown";

    if (audioStream->duration > 0 && audioStream->time_base.den > 0) {
        m_formatInfo.durationSeconds = static_cast<double>(audioStream->duration) * av_q2d(audioStream->time_base);
        m_formatInfo.totalFrames = static_cast<int64_t>(m_formatInfo.durationSeconds * m_formatInfo.sampleRate);
    } else if (m_formatCtx->duration > 0) {
        m_formatInfo.durationSeconds = static_cast<double>(m_formatCtx->duration) / static_cast<double>(AV_TIME_BASE);
        m_formatInfo.totalFrames = static_cast<int64_t>(m_formatInfo.durationSeconds * m_formatInfo.sampleRate);
    }

    // 8. Extract tags (Artwork strictly excluded)
    extractMetadata();

    m_isOpen = true;
    return true;
}

void FFmpegAudioDecoder::extractMetadata()
{
    m_metadata = metadata::MetadataReader::parseTags(m_filePath);

    auto getTag = [this](const char* key) -> std::string {
        AVDictionaryEntry* entry = av_dict_get(m_formatCtx->metadata, key, nullptr, AV_DICT_MATCH_CASE);
        if (!entry) {
            entry = av_dict_get(m_formatCtx->metadata, key, nullptr, AV_DICT_IGNORE_SUFFIX);
        }
        return entry && entry->value ? std::string(entry->value) : std::string();
    };

    if (m_metadata.title.empty()) m_metadata.title = getTag("title");
    if (m_metadata.artist.empty()) m_metadata.artist = getTag("artist");
    if (m_metadata.album.empty()) m_metadata.album = getTag("album");
    if (m_metadata.albumArtist.empty()) {
        m_metadata.albumArtist = getTag("album_artist");
        if (m_metadata.albumArtist.empty()) {
            m_metadata.albumArtist = getTag("albumartist");
        }
    }
    if (m_metadata.genre.empty()) m_metadata.genre = getTag("genre");
    if (m_metadata.dateOrYear.empty()) {
        m_metadata.dateOrYear = getTag("date");
        if (m_metadata.dateOrYear.empty()) {
            m_metadata.dateOrYear = getTag("year");
        }
    }
    if (m_metadata.comment.empty()) m_metadata.comment = getTag("comment");
    if (m_metadata.composer.empty()) m_metadata.composer = getTag("composer");

    if (m_metadata.trackNumber <= 0) {
        std::string trackStr = getTag("track");
        if (!trackStr.empty()) {
            try {
                m_metadata.trackNumber = std::stoi(trackStr);
            } catch (...) {
                m_metadata.trackNumber = 0;
            }
        }
    }

    // Stream technical audio parameters from decoded FFmpeg stream
    m_metadata.sampleRate = m_formatInfo.sampleRate;
    m_metadata.channels = m_formatInfo.channels;
    m_metadata.bitDepth = m_formatInfo.bitDepth;
    m_metadata.durationSeconds = m_formatInfo.durationSeconds;
    m_metadata.totalFrames = static_cast<uint64_t>(std::max<int64_t>(0, m_formatInfo.totalFrames));
    m_metadata.codecName = m_formatInfo.codecName;
}

bool FFmpegAudioDecoder::decodeNextPacket()
{
    while (true) {
        int ret = avcodec_receive_frame(m_codecCtx, m_decodedFrame);
        if (ret == 0) {
            // Successfully retrieved a decoded frame. Convert to planar double without heap allocation.
            const int inSamples = m_decodedFrame->nb_samples;
            if (inSamples <= 0) continue;

            if (m_seekPending) {
                if (m_decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE &&
                    m_audioStreamIndex >= 0 && m_formatInfo.sampleRate > 0.0) {
                    AVStream* st = m_formatCtx->streams[m_audioStreamIndex];
                    const int64_t ptsFrame = av_rescale_q(
                        m_decodedFrame->best_effort_timestamp,
                        st->time_base,
                        AVRational{1, static_cast<int>(m_formatInfo.sampleRate)}
                    );
                    m_seekActualFrame = std::max<int64_t>(0, ptsFrame);
                    m_currentFrame = m_seekActualFrame;
                }
                m_seekPending = false;
            }

            // Compact FIFO if read pointer has advanced
            if (m_fifoReadPos > 0) {
                size_t unread = (m_fifoWritePos > m_fifoReadPos) ? (m_fifoWritePos - m_fifoReadPos) : 0;
                if (unread > 0) {
                    std::memmove(m_fifoL.data(), m_fifoL.data() + m_fifoReadPos, unread * sizeof(double));
                    std::memmove(m_fifoR.data(), m_fifoR.data() + m_fifoReadPos, unread * sizeof(double));
                }
                m_fifoReadPos = 0;
                m_fifoWritePos = unread;
            }

            size_t availableCapacity = (FIFO_CAPACITY > m_fifoWritePos) ? (FIFO_CAPACITY - m_fifoWritePos) : 0;
            if (availableCapacity == 0) {
                av_frame_unref(m_decodedFrame);
                return true; // FIFO full
            }

            int outCapacity = static_cast<int>(std::min(availableCapacity, size_t(8192)));
            uint8_t* outData[2] = {
                reinterpret_cast<uint8_t*>(m_fifoL.data() + m_fifoWritePos),
                reinterpret_cast<uint8_t*>(m_fifoR.data() + m_fifoWritePos)
            };

            int convertedFrames = swr_convert(
                m_swrCtx,
                outData,
                outCapacity,
                const_cast<const uint8_t**>(m_decodedFrame->data),
                inSamples
            );

            if (convertedFrames < 0) {
                av_frame_unref(m_decodedFrame);
                m_hasDecodeError = true;
                m_lastError = "Sample conversion error during mid-stream decode";
                return false;
            }

            av_frame_unref(m_decodedFrame);

            if (convertedFrames > 0) {
                m_fifoWritePos += static_cast<size_t>(convertedFrames);
                return true;
            }
            continue;
        }

        if (ret == AVERROR_EOF) {
            return false; // Normal end of stream
        }

        if (ret == AVERROR(EAGAIN)) {
            // Need to feed more packets to decoder
            const auto tReadStart = std::chrono::steady_clock::now();
            int readRet = av_read_frame(m_formatCtx, m_packet);
            const auto tReadEnd = std::chrono::steady_clock::now();
            const uint64_t readNs = static_cast<uint64_t>(std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::nanoseconds>(tReadEnd - tReadStart).count()));
            m_lastReadPacketNs = readNs;
            if (readNs > m_maxReadPacketNs) {
                m_maxReadPacketNs = readNs;
            }
            m_totalReadPacketNs += readNs;

            if (readRet == AVERROR_EOF) {
                // Flush decoder on normal EOF
                avcodec_send_packet(m_codecCtx, nullptr);
                continue;
            }
            if (readRet < 0) {
                char errBuf[AV_ERROR_MAX_STRING_SIZE]{};
                av_strerror(readRet, errBuf, sizeof(errBuf));
                m_hasDecodeError = true;
                m_lastError = std::string("Unrecoverable mid-stream read error: ") + errBuf;
                return false;
            }

            if (m_packet->stream_index == m_audioStreamIndex) {
                int sendRet = avcodec_send_packet(m_codecCtx, m_packet);
                if (sendRet < 0 && sendRet != AVERROR(EAGAIN) && sendRet != AVERROR_EOF) {
                    char errBuf[AV_ERROR_MAX_STRING_SIZE]{};
                    av_strerror(sendRet, errBuf, sizeof(errBuf));
                    av_packet_unref(m_packet);
                    m_hasDecodeError = true;
                    m_lastError = std::string("Unrecoverable mid-stream packet decode error: ") + errBuf;
                    return false;
                }
            }
            av_packet_unref(m_packet);
            continue;
        }

        // Unrecoverable decode error
        {
            char errBuf[AV_ERROR_MAX_STRING_SIZE]{};
            av_strerror(ret, errBuf, sizeof(errBuf));
            m_hasDecodeError = true;
            m_lastError = std::string("Unrecoverable mid-stream decode error: ") + errBuf;
        }
        return false;
    }
}

size_t FFmpegAudioDecoder::readFrames(double* left, double* right, size_t maxFrames)
{
    if (!m_isOpen || maxFrames == 0) return 0;

    size_t framesRead = 0;

    while (framesRead < maxFrames) {
        size_t availableInFifo = (m_fifoWritePos > m_fifoReadPos) ? (m_fifoWritePos - m_fifoReadPos) : 0;

        if (availableInFifo > 0) {
            size_t toCopy = std::min(availableInFifo, maxFrames - framesRead);
            std::copy_n(m_fifoL.data() + m_fifoReadPos, toCopy, left + framesRead);
            std::copy_n(m_fifoR.data() + m_fifoReadPos, toCopy, right + framesRead);
            m_fifoReadPos += toCopy;
            framesRead += toCopy;
            m_currentFrame += static_cast<int64_t>(toCopy);
        } else {
            // Feed more frames
            if (!decodeNextPacket()) {
                break; // End of file
            }
        }
    }

    return framesRead;
}

bool FFmpegAudioDecoder::seek(int64_t targetFrame)
{
    if (!m_isOpen || m_audioStreamIndex < 0) return false;

    AVStream* st = m_formatCtx->streams[m_audioStreamIndex];
    if (st->time_base.den <= 0 || m_formatInfo.sampleRate <= 0.0) return false;

    // Convert target frame to stream time_base units
    int64_t ts = av_rescale_q(
        targetFrame,
        AVRational{1, static_cast<int>(m_formatInfo.sampleRate)},
        st->time_base
    );

    if (av_seek_frame(m_formatCtx, m_audioStreamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }

    avcodec_flush_buffers(m_codecCtx);
    m_hasDecodeError = false;
    m_fifoReadPos = 0;
    m_fifoWritePos = 0;
    m_currentFrame = targetFrame;
    m_seekActualFrame = targetFrame;
    m_seekPending = true;

    return true;
}

} // namespace desk32::engine::decoder

