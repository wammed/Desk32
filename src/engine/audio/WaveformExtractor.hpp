#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace desk32::engine::audio {

struct WaveformData {
    static constexpr uint32_t DEFAULT_BUCKETS = 1024;

    std::string filePath;
    double sampleRate = 0.0;
    int64_t totalFrames = 0;
    uint32_t bucketCount = DEFAULT_BUCKETS;
    std::vector<float> minL;
    std::vector<float> maxL;
    std::vector<float> minR;
    std::vector<float> maxR;
    bool valid = false;
};

/// Asynchronous background waveform peak extractor with $XDG_CACHE_HOME/desk32/waveforms/ persistence.
/// Strictly runs on a dedicated non-RT thread using its own FFmpegAudioDecoder instance.
class WaveformExtractor {
public:
    using ReadyCallback = std::function<void(const std::string& filePath, bool success)>;

    WaveformExtractor();
    ~WaveformExtractor();

    WaveformExtractor(const WaveformExtractor&) = delete;
    WaveformExtractor& operator=(const WaveformExtractor&) = delete;

    void setReadyCallback(ReadyCallback cb);

    /// Enqueue background waveform extraction (no-op if already cached in memory).
    void requestWaveform(const std::string& filePath);

    [[nodiscard]] bool isExtracting(const std::string& filePath) const;
    [[nodiscard]] bool hasWaveform(const std::string& filePath) const;
    [[nodiscard]] bool getWaveform(const std::string& filePath, WaveformData& outData) const;

    /// Disk cache helpers ($XDG_CACHE_HOME/desk32/waveforms/).
    [[nodiscard]] static std::string cacheDirectoryPath();
    [[nodiscard]] uint64_t diskCacheSizeBytes() const;
    [[nodiscard]] size_t diskCacheFileCount() const;
    bool clearDiskCache();

private:
    void workerLoop();
    static std::string canonicalizePath(const std::string& path);
    static std::string computeCacheFilePath(const std::string& canonPath);
    static bool loadFromDiskCache(const std::string& cacheFile, const std::string& canonPath, WaveformData& outData);
    static bool saveToDiskCache(const std::string& cacheFile, const WaveformData& data);
    bool extractViaFfmpeg(const std::string& canonPath, WaveformData& outData) const;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    std::unordered_set<std::string> m_queuedOrActive;
    std::unordered_map<std::string, WaveformData> m_memoryCache;
    ReadyCallback m_readyCallback;

    std::atomic<bool> m_stop{false};
    std::thread m_worker;
};

} // namespace desk32::engine::audio
