#include "WaveformExtractor.hpp"
#include "engine/decoder/FFmpegAudioDecoder.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

namespace desk32::engine::audio {

namespace {

struct CacheFileHeader {
    char magic[4] = {'D', 'W', 'F', 'M'};
    uint32_t version = 1;
    uint32_t buckets = WaveformData::DEFAULT_BUCKETS;
    uint32_t reserved = 0;
    int64_t totalFrames = 0;
    double sampleRate = 0.0;
};

} // namespace

WaveformExtractor::WaveformExtractor() {
    m_worker = std::thread(&WaveformExtractor::workerLoop, this);
}

WaveformExtractor::~WaveformExtractor() {
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void WaveformExtractor::setReadyCallback(ReadyCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readyCallback = std::move(cb);
}

std::string WaveformExtractor::canonicalizePath(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    const auto cp = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : cp.string();
}

std::string WaveformExtractor::cacheDirectoryPath() {
    const QByteArray xdg = qgetenv("XDG_CACHE_HOME");
    const QString base = xdg.isEmpty() ? (QDir::homePath() + QStringLiteral("/.cache"))
                                       : QString::fromLocal8Bit(xdg);
    return QDir::cleanPath(base + QStringLiteral("/desk32/waveforms")).toStdString();
}

std::string WaveformExtractor::computeCacheFilePath(const std::string& canonPath) {
    const QFileInfo fi(QString::fromStdString(canonPath));
    const QString key = QStringLiteral("%1|%2|%3|v1")
                            .arg(QString::fromStdString(canonPath))
                            .arg(fi.size())
                            .arg(fi.lastModified().toMSecsSinceEpoch());
    const QByteArray digest = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return cacheDirectoryPath() + "/" + digest.toStdString() + ".wfm";
}

bool WaveformExtractor::loadFromDiskCache(const std::string& cacheFile,
                                          const std::string& canonPath,
                                          WaveformData& outData) {
    QFile f(QString::fromStdString(cacheFile));
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return false;
    }
    CacheFileHeader hdr{};
    if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != static_cast<qint64>(sizeof(hdr))) {
        return false;
    }
    if (std::memcmp(hdr.magic, "DWFM", 4) != 0 || hdr.version != 1 ||
        hdr.buckets == 0 || hdr.buckets > 8192) {
        return false;
    }
    const qint64 expectedPayload = static_cast<qint64>(hdr.buckets) * 4 * static_cast<qint64>(sizeof(float));
    if (f.bytesAvailable() < expectedPayload) {
        return false;
    }

    WaveformData data;
    data.filePath = canonPath;
    data.sampleRate = hdr.sampleRate;
    data.totalFrames = hdr.totalFrames;
    data.bucketCount = hdr.buckets;
    data.minL.resize(hdr.buckets);
    data.maxL.resize(hdr.buckets);
    data.minR.resize(hdr.buckets);
    data.maxR.resize(hdr.buckets);

    const qint64 arrBytes = static_cast<qint64>(hdr.buckets) * static_cast<qint64>(sizeof(float));
    if (f.read(reinterpret_cast<char*>(data.minL.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.maxL.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.minR.data()), arrBytes) != arrBytes ||
        f.read(reinterpret_cast<char*>(data.maxR.data()), arrBytes) != arrBytes) {
        return false;
    }
    data.valid = true;
    outData = std::move(data);
    return true;
}

bool WaveformExtractor::saveToDiskCache(const std::string& cacheFile, const WaveformData& data) {
    if (!data.valid || data.bucketCount == 0) return false;
    const QFileInfo fi(QString::fromStdString(cacheFile));
    QDir().mkpath(fi.absolutePath());

    QSaveFile sf(QString::fromStdString(cacheFile));
    if (!sf.open(QIODevice::WriteOnly)) {
        return false;
    }
    CacheFileHeader hdr{};
    hdr.version = 1;
    hdr.buckets = data.bucketCount;
    hdr.totalFrames = data.totalFrames;
    hdr.sampleRate = data.sampleRate;
    sf.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    const qint64 arrBytes = static_cast<qint64>(data.bucketCount) * static_cast<qint64>(sizeof(float));
    sf.write(reinterpret_cast<const char*>(data.minL.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.maxL.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.minR.data()), arrBytes);
    sf.write(reinterpret_cast<const char*>(data.maxR.data()), arrBytes);
    return sf.commit();
}

bool WaveformExtractor::extractViaFfmpeg(const std::string& canonPath, WaveformData& outData) const {
    decoder::FFmpegAudioDecoder dec;
    if (!dec.open(canonPath)) {
        return false;
    }
    const auto fmt = dec.formatInfo();
    const uint32_t buckets = WaveformData::DEFAULT_BUCKETS;
    constexpr size_t CHUNK_FRAMES = 4096;
    std::vector<double> bufL(CHUNK_FRAMES, 0.0);
    std::vector<double> bufR(CHUNK_FRAMES, 0.0);

    WaveformData data;
    data.filePath = canonPath;
    data.sampleRate = fmt.sampleRate;
    data.totalFrames = fmt.totalFrames;
    data.bucketCount = buckets;
    data.minL.assign(buckets, 1.0f);
    data.maxL.assign(buckets, -1.0f);
    data.minR.assign(buckets, 1.0f);
    data.maxR.assign(buckets, -1.0f);

    if (fmt.totalFrames > 0) {
        std::vector<bool> touched(buckets, false);
        int64_t framePos = 0;
        while (!m_stop.load(std::memory_order_acquire)) {
            const size_t n = dec.readFrames(bufL.data(), bufR.data(), CHUNK_FRAMES);
            if (n == 0) break;
            for (size_t i = 0; i < n; ++i) {
                const int64_t f = framePos + static_cast<int64_t>(i);
                const size_t b = static_cast<size_t>(
                    std::clamp<int64_t>((f * static_cast<int64_t>(buckets)) / fmt.totalFrames,
                                        0, static_cast<int64_t>(buckets) - 1));
                const float lv = static_cast<float>(std::clamp(bufL[i], -1.0, 1.0));
                const float rv = static_cast<float>(std::clamp(bufR[i], -1.0, 1.0));
                if (!touched[b]) {
                    data.minL[b] = data.maxL[b] = lv;
                    data.minR[b] = data.maxR[b] = rv;
                    touched[b] = true;
                } else {
                    data.minL[b] = std::min(data.minL[b], lv);
                    data.maxL[b] = std::max(data.maxL[b], lv);
                    data.minR[b] = std::min(data.minR[b], rv);
                    data.maxR[b] = std::max(data.maxR[b], rv);
                }
            }
            framePos += static_cast<int64_t>(n);
        }
        if (m_stop.load(std::memory_order_acquire)) return false;
        for (size_t b = 0; b < buckets; ++b) {
            if (!touched[b]) {
                data.minL[b] = data.maxL[b] = 0.0f;
                data.minR[b] = data.maxR[b] = 0.0f;
            }
        }
        if (framePos > data.totalFrames) {
            data.totalFrames = framePos;
        }
    } else {
        // Unknown totalFrames fallback: accumulate coarse peaks and resample to 1024 buckets.
        struct BlockPeak { float minL, maxL, minR, maxR; };
        std::vector<BlockPeak> coarse;
        int64_t totalRead = 0;
        while (!m_stop.load(std::memory_order_acquire)) {
            const size_t n = dec.readFrames(bufL.data(), bufR.data(), CHUNK_FRAMES);
            if (n == 0) break;
            float mnL = 1.0f, mxL = -1.0f, mnR = 1.0f, mxR = -1.0f;
            for (size_t i = 0; i < n; ++i) {
                const float lv = static_cast<float>(std::clamp(bufL[i], -1.0, 1.0));
                const float rv = static_cast<float>(std::clamp(bufR[i], -1.0, 1.0));
                mnL = std::min(mnL, lv); mxL = std::max(mxL, lv);
                mnR = std::min(mnR, rv); mxR = std::max(mxR, rv);
            }
            coarse.push_back({mnL, mxL, mnR, mxR});
            totalRead += static_cast<int64_t>(n);
        }
        if (m_stop.load(std::memory_order_acquire)) return false;
        data.totalFrames = totalRead;
        if (coarse.empty()) {
            std::fill(data.minL.begin(), data.minL.end(), 0.0f);
            std::fill(data.maxL.begin(), data.maxL.end(), 0.0f);
            std::fill(data.minR.begin(), data.minR.end(), 0.0f);
            std::fill(data.maxR.begin(), data.maxR.end(), 0.0f);
        } else {
            for (size_t b = 0; b < buckets; ++b) {
                const size_t s0 = (b * coarse.size()) / buckets;
                const size_t s1 = std::max(s0 + 1, ((b + 1) * coarse.size()) / buckets);
                float mnL = 1.0f, mxL = -1.0f, mnR = 1.0f, mxR = -1.0f;
                for (size_t s = s0; s < s1 && s < coarse.size(); ++s) {
                    mnL = std::min(mnL, coarse[s].minL); mxL = std::max(mxL, coarse[s].maxL);
                    mnR = std::min(mnR, coarse[s].minR); mxR = std::max(mxR, coarse[s].maxR);
                }
                data.minL[b] = mnL; data.maxL[b] = mxL;
                data.minR[b] = mnR; data.maxR[b] = mxR;
            }
        }
    }

    dec.close();
    data.valid = (data.totalFrames > 0);
    outData = std::move(data);
    return outData.valid;
}

void WaveformExtractor::requestWaveform(const std::string& filePath) {
    if (filePath.empty()) return;
    const std::string canon = canonicalizePath(filePath);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_memoryCache.find(canon) != m_memoryCache.end()) {
            return;
        }
        if (m_queuedOrActive.find(canon) != m_queuedOrActive.end()) {
            return;
        }
        m_queuedOrActive.insert(canon);
        m_queue.push_back(canon);
    }
    m_cv.notify_one();
}

bool WaveformExtractor::isExtracting(const std::string& filePath) const {
    if (filePath.empty()) return false;
    const std::string canon = canonicalizePath(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queuedOrActive.find(canon) != m_queuedOrActive.end();
}

bool WaveformExtractor::hasWaveform(const std::string& filePath) const {
    if (filePath.empty()) return false;
    const std::string canon = canonicalizePath(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_memoryCache.find(canon);
    return it != m_memoryCache.end() && it->second.valid;
}

bool WaveformExtractor::getWaveform(const std::string& filePath, WaveformData& outData) const {
    if (filePath.empty()) return false;
    const std::string canon = canonicalizePath(filePath);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_memoryCache.find(canon);
    if (it == m_memoryCache.end() || !it->second.valid) {
        return false;
    }
    outData = it->second;
    return true;
}

uint64_t WaveformExtractor::diskCacheSizeBytes() const {
    uint64_t total = 0;
    QDir dir(QString::fromStdString(cacheDirectoryPath()));
    if (!dir.exists()) return 0;
    const auto entries = dir.entryInfoList(QStringList{QStringLiteral("*.wfm")}, QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries) {
        total += static_cast<uint64_t>(fi.size());
    }
    return total;
}

size_t WaveformExtractor::diskCacheFileCount() const {
    QDir dir(QString::fromStdString(cacheDirectoryPath()));
    if (!dir.exists()) return 0;
    return static_cast<size_t>(dir.entryInfoList(QStringList{QStringLiteral("*.wfm")}, QDir::Files | QDir::NoDotAndDotDot).size());
}

bool WaveformExtractor::clearDiskCache() {
    QDir dir(QString::fromStdString(cacheDirectoryPath()));
    if (!dir.exists()) return true;
    const auto entries = dir.entryInfoList(QStringList{QStringLiteral("*.wfm")}, QDir::Files | QDir::NoDotAndDotDot);
    bool allOk = true;
    for (const QFileInfo& fi : entries) {
        if (!QFile::remove(fi.absoluteFilePath())) {
            allOk = false;
        }
    }
    return allOk;
}

void WaveformExtractor::workerLoop() {
    while (true) {
        std::string jobPath;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_stop.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_stop.load(std::memory_order_acquire)) {
                return;
            }
            jobPath = std::move(m_queue.front());
            m_queue.pop_front();
        }

        const std::string cacheFile = computeCacheFilePath(jobPath);
        WaveformData result;
        bool ok = loadFromDiskCache(cacheFile, jobPath, result);
        if (!ok && !m_stop.load(std::memory_order_acquire)) {
            ok = extractViaFfmpeg(jobPath, result);
            if (ok) {
                saveToDiskCache(cacheFile, result);
            }
        }

        ReadyCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (ok) {
                m_memoryCache[jobPath] = result;
            }
            m_queuedOrActive.erase(jobPath);
            cb = m_readyCallback;
        }
        if (cb && !m_stop.load(std::memory_order_acquire)) {
            cb(jobPath, ok);
        }
    }
}

} // namespace desk32::engine::audio
