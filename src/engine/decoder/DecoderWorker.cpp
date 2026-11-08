#include "DecoderWorker.hpp"
#include <chrono>

namespace desk32::engine::decoder {

DecoderWorker::DecoderWorker() {
    m_thread = std::thread(&DecoderWorker::workerLoop, this);
}

DecoderWorker::~DecoderWorker() {
    m_running.store(false, std::memory_order_release);
    m_cmdCv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool DecoderWorker::openTrack(const std::string& filePath, double graphSampleRate) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        seq = ++m_seekSequence;
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::Open;
        cmd.path = filePath;
        cmd.graphRate = graphSampleRate;
        cmd.sequence = seq;
        cmd.openPromise = promise;
        m_pendingCommands.push_back(std::move(cmd));
    }
    m_cmdCv.notify_one();

    if (future.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready) {
        return future.get();
    }
    return false;
}

bool DecoderWorker::openSyntheticTrack(uint64_t totalFrames, double sampleRate, double graphSampleRate, const std::string& path) {
    WorkerTrackInfo tInfo;
    tInfo.path = path;
    tInfo.sampleRate = sampleRate;
    tInfo.channels = 2;
    tInfo.totalFrames = totalFrames;

    metadata::TrackMetadata meta;
    meta.title = "Synthetic Reference Signal";
    meta.artist = "Desk32 DSP Test Engine";
    meta.sampleRate = sampleRate;
    meta.channels = 2;
    meta.totalFrames = totalFrames;
    meta.durationSeconds = (sampleRate > 0) ? (static_cast<double>(totalFrames) / sampleRate) : 0.0;

    {
        std::lock_guard<std::mutex> lock(m_trackMutex);
        m_trackInfo = tInfo;
        m_metadata = std::make_shared<metadata::TrackMetadata>(std::move(meta));
        m_resampler.configure(sampleRate, graphSampleRate, 2);
    }

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        seq = ++m_seekSequence;
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::Open;
        cmd.path = path;
        cmd.graphRate = graphSampleRate;
        cmd.sequence = seq;
        cmd.isSynthetic = true;
        cmd.openPromise = promise;
        m_pendingCommands.push_back(std::move(cmd));
    }
    m_cmdCv.notify_one();

    if (future.wait_for(std::chrono::milliseconds(2000)) == std::future_status::ready) {
        return future.get();
    }
    return false;
}

void DecoderWorker::play() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        m_isPlaying.store(true, std::memory_order_release);
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::Play;
        m_pendingCommands.push_back(cmd);
    }
    m_cmdCv.notify_one();
}

void DecoderWorker::pause() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        m_isPlaying.store(false, std::memory_order_release);
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::Pause;
        m_pendingCommands.push_back(cmd);
    }
    m_cmdCv.notify_one();
}

void DecoderWorker::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        m_isPlaying.store(false, std::memory_order_release);
        m_playhead.store(0, std::memory_order_release);
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::Stop;
        m_pendingCommands.push_back(cmd);
    }
    m_cmdCv.notify_one();
}

void DecoderWorker::seek(int64_t targetFrame) noexcept {
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        m_playhead.store(targetFrame, std::memory_order_release);
        // Note: Do NOT touch m_ringBuffer directly from Control Thread (P0-04)
        seq = ++m_seekSequence;
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::Seek;
        cmd.targetFrame = targetFrame;
        cmd.sequence = seq;
        m_pendingCommands.push_back(cmd);
    }
    m_cmdCv.notify_one();

    // Wait briefly for worker to process seek and prime buffer with at least 256 frames
    auto start = std::chrono::steady_clock::now();
    while (m_seekCompleted.load(std::memory_order_acquire) < seq && !m_isEof.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > 10.0) {
            break;
        }
    }
}

void DecoderWorker::configureGraphRate(double graphSampleRate) noexcept {
    {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        m_graphRate.store(graphSampleRate, std::memory_order_release);
        WorkerCommand cmd;
        cmd.type = WorkerCommandType::ConfigureRate;
        cmd.graphRate = graphSampleRate;
        m_pendingCommands.push_back(cmd);
    }
    m_cmdCv.notify_one();
}

size_t DecoderWorker::readAudioBlock(double* outLeft, double* outRight, size_t frames) noexcept {
    if (!m_isOpen.load(std::memory_order_acquire) || !m_isPlaying.load(std::memory_order_acquire)) {
        std::fill_n(outLeft, frames, 0.0);
        std::fill_n(outRight, frames, 0.0);
        return 0;
    }

    size_t read = m_ringBuffer.readPlanar(outLeft, outRight, frames);
    if (read < frames) {
        // Buffer underrun or EOF
        std::fill_n(outLeft + read, frames - read, 0.0);
        std::fill_n(outRight + read, frames - read, 0.0);

        if (m_isEof.load(std::memory_order_acquire) && m_ringBuffer.availableRead() == 0) {
            m_eofDelivered.store(true, std::memory_order_release);
        } else {
            m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    m_playhead.fetch_add(static_cast<int64_t>(read), std::memory_order_relaxed);
    if (m_ringBuffer.availableWrite() >= 4096) {
        m_cmdCv.notify_one(); // Signal worker thread when space is available
    }
    return read;
}

void DecoderWorker::workerLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        std::vector<WorkerCommand> cmds;
        {
            std::unique_lock<std::mutex> lock(m_cmdMutex);
            if (m_pendingCommands.empty() &&
                (!m_isOpen.load(std::memory_order_relaxed) ||
                 m_isEof.load(std::memory_order_relaxed) ||
                 (m_pendingOutCount == 0 && m_ringBuffer.availableWrite() < 4096))) {
                m_cmdCv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return !m_running.load(std::memory_order_relaxed) ||
                           !m_pendingCommands.empty() ||
                           (m_isOpen.load(std::memory_order_relaxed) &&
                            !m_isEof.load(std::memory_order_relaxed) &&
                            (m_pendingOutCount > 0 || m_ringBuffer.availableWrite() >= 4096));
                });
            }
            cmds.swap(m_pendingCommands);
        }

        if (!m_running.load(std::memory_order_acquire)) break;

        for (const auto& cmd : cmds) {
            processCommand(cmd);
        }

        while (m_running.load(std::memory_order_relaxed) &&
               m_isOpen.load(std::memory_order_relaxed) &&
               !m_isEof.load(std::memory_order_relaxed) &&
               (m_pendingOutCount > 0 || m_ringBuffer.availableWrite() >= 4096)) {

            // Prevent busy spin if ring buffer is temporarily completely full
            if (m_pendingOutCount > 0 && m_ringBuffer.availableWrite() == 0) {
                std::unique_lock<std::mutex> lock(m_cmdMutex);
                m_cmdCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return !m_running.load(std::memory_order_relaxed) ||
                           !m_pendingCommands.empty() ||
                           m_ringBuffer.availableWrite() > 0;
                });
                if (!m_pendingCommands.empty() || !m_running.load(std::memory_order_relaxed)) break;
            }

            doDecodeChunk();
            {
                std::lock_guard<std::mutex> lock(m_cmdMutex);
                if (!m_pendingCommands.empty()) break;
            }
        }
    }
}

void DecoderWorker::processCommand(const WorkerCommand& cmd) {
    switch (cmd.type) {
    case WorkerCommandType::Open: {
        m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
        m_pendingOutOffset = 0;
        m_pendingOutCount = 0;
        m_syntheticPos = 0;
        m_isEof.store(false, std::memory_order_release);
        m_eofDelivered.store(false, std::memory_order_release);

        bool openSuccess = false;
        if (!cmd.isSynthetic && cmd.path != "synthetic://sine" && cmd.path.find("synthetic") == std::string::npos) {
            auto dec = std::make_unique<FFmpegAudioDecoder>();
            if (dec->open(cmd.path)) {
                const auto& info = dec->formatInfo();
                WorkerTrackInfo tInfo;
                tInfo.path = cmd.path;
                tInfo.sampleRate = info.sampleRate;
                tInfo.channels = info.channels;
                tInfo.totalFrames = static_cast<uint64_t>(std::max<int64_t>(0, info.totalFrames));

                m_resampler.configure(info.sampleRate, cmd.graphRate, info.channels);
                {
                    std::lock_guard<std::mutex> lock(m_trackMutex);
                    m_trackInfo = tInfo;
                    m_metadata = std::make_shared<metadata::TrackMetadata>(dec->metadata());
                }
                m_decoder = std::move(dec);
                m_isSynthetic.store(false, std::memory_order_release);
                m_graphRate.store(cmd.graphRate, std::memory_order_release);
                m_playhead.store(0, std::memory_order_release);
                m_isOpen.store(true, std::memory_order_release);
                m_isPlaying.store(false, std::memory_order_release);

                for (int i = 0; i < 4; ++i) {
                    doDecodeChunk();
                }
                openSuccess = true;
            } else {
                // Open failed: Do NOT mark as open or corrupt track state
                m_isOpen.store(false, std::memory_order_release);
                openSuccess = false;
            }
        } else {
            // Synthetic track
            m_decoder.reset();
            m_isSynthetic.store(true, std::memory_order_release);
            m_graphRate.store(cmd.graphRate, std::memory_order_release);
            m_playhead.store(0, std::memory_order_release);
            m_isOpen.store(true, std::memory_order_release);
            m_isPlaying.store(false, std::memory_order_release);

            for (int i = 0; i < 4; ++i) {
                doDecodeChunk();
            }
            openSuccess = true;
        }

        if (cmd.openPromise) {
            cmd.openPromise->set_value(openSuccess);
        }
        m_seekCompleted.store(cmd.sequence, std::memory_order_release);
        break;
    }
    case WorkerCommandType::Seek: {
        m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
        m_pendingOutOffset = 0;
        m_pendingOutCount = 0;
        m_isEof.store(false, std::memory_order_release);
        m_eofDelivered.store(false, std::memory_order_release);
        if (m_isSynthetic.load(std::memory_order_relaxed)) {
            m_syntheticPos = cmd.targetFrame;
            for (int i = 0; i < 4; ++i) doDecodeChunk();
        } else if (m_decoder) {
            m_decoder->seek(cmd.targetFrame);
            m_resampler.reset();
            for (int i = 0; i < 4; ++i) doDecodeChunk();
        }
        m_seekCompleted.store(cmd.sequence, std::memory_order_release);
        break;
    }
    case WorkerCommandType::Play:
        break;
    case WorkerCommandType::Pause:
        break;
    case WorkerCommandType::Stop:
        m_ringBuffer.resetProducer(); // P0-04: Producer-owned reset
        m_pendingOutOffset = 0;
        m_pendingOutCount = 0;
        m_isEof.store(false, std::memory_order_release);
        m_eofDelivered.store(false, std::memory_order_release);
        m_syntheticPos = 0;
        if (m_decoder) {
            m_decoder->seek(0);
            m_resampler.reset();
        }
        break;
    case WorkerCommandType::ConfigureRate:
        {
            std::lock_guard<std::mutex> lock(m_trackMutex);
            if (m_trackInfo.sampleRate > 0.0 && cmd.graphRate > 0.0) {
                m_resampler.configure(m_trackInfo.sampleRate, cmd.graphRate, m_trackInfo.channels);
                m_ringBuffer.resetProducer();
                m_pendingOutOffset = 0;
                m_pendingOutCount = 0;
            }
        }
        break;
    default:
        break;
    }
}

void DecoderWorker::doDecodeChunk() {
    // 1. Drain any pending resampled output frames from previous chunk first
    if (m_pendingOutCount > 0) {
        size_t written = m_ringBuffer.write(m_tempInterleavedOut.data() + m_pendingOutOffset, m_pendingOutCount);
        m_pendingOutOffset += written;
        m_pendingOutCount -= written;
        if (m_pendingOutCount > 0) {
            return; // Ring buffer full; resume draining on next loop iteration
        }
        m_pendingOutOffset = 0;
    }

    if (m_isSynthetic.load(std::memory_order_relaxed)) {
        if (m_syntheticPos >= static_cast<int64_t>(m_trackInfo.totalFrames)) {
            m_isEof.store(true, std::memory_order_release);
            return;
        }

        size_t framesToGen = std::min(CHUNK_SIZE,
            static_cast<size_t>(static_cast<int64_t>(m_trackInfo.totalFrames) - m_syntheticPos));
        for (size_t i = 0; i < framesToGen; ++i) {
            double t = static_cast<double>(m_syntheticPos + static_cast<int64_t>(i)) / m_trackInfo.sampleRate;
            double s = 0.5 * std::sin(2.0 * M_PI * 440.0 * t);
            m_tempChunkL[i] = s;
            m_tempChunkR[i] = s;
        }
        m_syntheticPos += static_cast<int64_t>(framesToGen);

        size_t written = 0;
        while (written < framesToGen && m_running.load(std::memory_order_relaxed)) {
            size_t w = m_ringBuffer.writePlanar(m_tempChunkL.data() + written, m_tempChunkR.data() + written, framesToGen - written);
            written += w;
            if (written < framesToGen) {
                std::unique_lock<std::mutex> lock(m_cmdMutex);
                if (!m_pendingCommands.empty()) break;
                m_cmdCv.wait_for(lock, std::chrono::milliseconds(2), [this]() {
                    return !m_running.load(std::memory_order_relaxed) || !m_pendingCommands.empty() || m_ringBuffer.availableWrite() > 0;
                });
            }
        }
        return;
    }

    if (!m_decoder || !m_decoder->isOpen()) {
        m_isEof.store(true, std::memory_order_release);
        return;
    }

    size_t read = m_decoder->readFrames(m_tempChunkL.data(), m_tempChunkR.data(), CHUNK_SIZE);
    if (read == 0) {
        m_isEof.store(true, std::memory_order_release);
        return;
    }

    if (m_resampler.isBypass()) {
        size_t written = 0;
        while (written < read && m_running.load(std::memory_order_relaxed)) {
            size_t w = m_ringBuffer.writePlanar(m_tempChunkL.data() + written, m_tempChunkR.data() + written, read - written);
            written += w;
            if (written < read) {
                std::unique_lock<std::mutex> lock(m_cmdMutex);
                if (!m_pendingCommands.empty()) break;
                m_cmdCv.wait_for(lock, std::chrono::milliseconds(2), [this]() {
                    return !m_running.load(std::memory_order_relaxed) || !m_pendingCommands.empty() || m_ringBuffer.availableWrite() > 0;
                });
            }
        }
    } else {
        // Interleave for SoXR
        for (size_t i = 0; i < read; ++i) {
            m_tempInterleavedIn[i * 2]     = m_tempChunkL[i];
            m_tempInterleavedIn[i * 2 + 1] = m_tempChunkR[i];
        }

        size_t inDone = 0, outDone = 0;
        size_t maxOut = CHUNK_SIZE * 8;
        bool ok = m_resampler.process(m_tempInterleavedIn.data(), read, inDone,
                                      reinterpret_cast<double*>(m_tempInterleavedOut.data()), maxOut, outDone);
        if (ok && outDone > 0) {
            size_t written = m_ringBuffer.write(m_tempInterleavedOut.data(), outDone);
            if (written < outDone) {
                m_pendingOutOffset = written;
                m_pendingOutCount = outDone - written;
            }
        }
    }
}

} // namespace desk32::engine::decoder
